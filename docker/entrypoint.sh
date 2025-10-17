#!/usr/bin/env bash
set -euo pipefail

export LD_LIBRARY_PATH="/usr/local/lib:/usr/local/lib/dpdk:${LD_LIBRARY_PATH:-}"
export RTE_EAL_PMD_PATH="/usr/local/lib:/usr/local/lib/dpdk"
export DPDK_PLUGIN_PATH="${RTE_EAL_PMD_PATH}"
# Ensure default mempool ops is ring
export RTE_MBUF_DEFAULT_MEMPOOL_OPS="ring_mp_mc"

# Optional debug sleep controls
if [[ "${DEBUG_HOLD:-0}" = "1" ]]; then
  echo "DEBUG_HOLD=1 set: sleeping forever for debugging..."
  while true; do sleep 3600; done
fi

if [[ -n "${DEBUG_SLEEP:-}" ]]; then
  if [[ "${DEBUG_SLEEP}" =~ ^(inf|infinity)$ ]]; then
    echo "DEBUG_SLEEP=${DEBUG_SLEEP}: sleeping indefinitely..."
    while true; do sleep 3600; done
  else
    echo "DEBUG_SLEEP=${DEBUG_SLEEP}: sleeping before start..."
    sleep "${DEBUG_SLEEP}"
  fi
fi

# Expand PMD path to include nested pmd directories if present
for base in /usr/local/lib /usr/local/lib/dpdk; do
  if [[ -d "$base" ]]; then
    PMD_DIRS=$(find "$base" -maxdepth 3 -type f -name 'librte_*so*' -printf '%h\n' 2>/dev/null | sort -u | tr '\n' ':' || true)
    if [[ -n "${PMD_DIRS:-}" ]]; then
      RTE_EAL_PMD_PATH="${PMD_DIRS%:}:${RTE_EAL_PMD_PATH}"
    fi
  fi
done
export RTE_EAL_PMD_PATH
export DPDK_PLUGIN_PATH="${RTE_EAL_PMD_PATH}"

echo "PMD path: ${RTE_EAL_PMD_PATH}"
echo "DPDK libs present:" && (ls -1 /usr/local/lib | grep -E '^librte_' || true)
echo "PMDs present:" && (find /usr/local/lib /usr/local/lib/dpdk -maxdepth 3 -type f -name 'librte_*so*' -printf '%f\n' 2>/dev/null | sort -u || true)

# Toggle to use unified devargs (bus=pci,addr=...). Default off for compatibility.
USE_PCI_DEVARGS=${USE_PCI_DEVARGS:-0}

ALLOW_FLAGS=()
DEVARGS_FLAGS=()
VISIBLE_BDFS=()
if [[ -n "${DPDK_ALLOW:-}" ]]; then
  IFS=',' read -ra ADDR <<< "${DPDK_ALLOW}"
  for a in "${ADDR[@]}"; do
    a_trim=$(echo "$a" | xargs)
    [[ -z "$a_trim" ]] && continue
    if [[ -e "/sys/bus/pci/devices/${a_trim}" ]]; then
      VISIBLE_BDFS+=("$a_trim")
      if [[ "$USE_PCI_DEVARGS" = "1" ]]; then
        ALLOW_FLAGS+=("-a" "bus=pci,addr=$a_trim")
      else
        ALLOW_FLAGS+=("-a" "$a_trim")
      fi
    else
      echo "WARN: PCI device ${a_trim} not visible inside container; skipping from allow-list" >&2
    fi
  done
fi

EAL_ARGS=("-l" "${LCORES:-0}" "-m" "${DPDK_MEM:-1024}")

if [[ -n "${EAL_EXTRA:-}" ]]; then
  # shellcheck disable=SC2206
  EXTRA_ARR=( ${EAL_EXTRA} )
  EAL_ARGS+=("${EXTRA_ARR[@]}")
fi

EAL_ARGS+=("${ALLOW_FLAGS[@]}")

# Optional: add mlx5 devargs by interface names
if [[ -n "${MLX5_IFNAMES:-}" ]]; then
  IFS=',' read -ra IFN <<< "${MLX5_IFNAMES}"
  for n in "${IFN[@]}"; do
    n_trim=$(echo "$n" | xargs)
    [[ -z "$n_trim" ]] && continue
    devstr="net_mlx5,ifname=${n_trim}"
    if [[ -n "${MLX5_DEVARGS_EXTRA:-}" ]]; then
      if [[ "${MLX5_DEVARGS_EXTRA}" == ,* ]]; then
        devstr+="${MLX5_DEVARGS_EXTRA}"
      else
        devstr+=","${MLX5_DEVARGS_EXTRA}
      fi
    fi
    DEVARGS_FLAGS+=("-a" "$devstr")
  done
fi

EAL_ARGS+=("${DEVARGS_FLAGS[@]}")

# Auto-load critical DPDK drivers once (deduplicated)
add_first_lib() {
  local base="$1"
  local try
  # Prefer unversioned .so in /usr/local/lib
  try="/usr/local/lib/${base}.so"
  if [[ -e "$try" ]]; then
    AUTO_DRIVERS+=("-d" "$try")
    return 0
  fi
  # Else pick first versioned match from known locations
  try=$(find /usr/local/lib /usr/local/lib/dpdk -maxdepth 3 -type f -name "${base}.so*" 2>/dev/null | sort -u | head -n1)
  if [[ -n "$try" && -e "$try" ]]; then
    AUTO_DRIVERS+=("-d" "$try")
    return 0
  fi
  return 1
}

AUTO_DRIVERS=()
add_first_lib librte_mempool_ring
add_first_lib librte_common_mlx5
add_first_lib librte_net_mlx5
if [[ ${#AUTO_DRIVERS[@]} -gt 0 ]]; then
  echo "Auto-loading drivers (deduped): ${AUTO_DRIVERS[*]}"
  EAL_ARGS+=("${AUTO_DRIVERS[@]}")
fi
echo "Launching mini-dpdk with EAL args: ${EAL_ARGS[*]} -- $*"

# Try run; if fails and we had allow-list, retry without it for discovery/debug
if /usr/local/bin/mini-dpdk "${EAL_ARGS[@]}" -- "$@"; then
  exit 0
fi

if [[ ${#ALLOW_FLAGS[@]} -gt 0 ]]; then
  echo "mini-dpdk failed with explicit PCI allow-list (${VISIBLE_BDFS[*]}). Retrying without -a filters..." >&2
  EAL_ARGS_NO_ALLOW=( )
  # strip any -a flags
  for arg in "${EAL_ARGS[@]}"; do
    if [[ "$arg" == "-a" ]]; then
      skip_next=1; continue
    fi
    if [[ ${skip_next:-0} -eq 1 ]]; then
      unset skip_next; continue
    fi
    EAL_ARGS_NO_ALLOW+=("$arg")
  done
  echo "Retry EAL args: ${EAL_ARGS_NO_ALLOW[*]} -- $*"
  exec /usr/local/bin/mini-dpdk "${EAL_ARGS_NO_ALLOW[@]}" -- "$@"
fi

exit 1
