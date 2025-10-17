# mini-dpdk — минимальное DPDK‑приложение для Kubernetes/Docker

Минимальный L2 форвардер на DPDK: инициализирует EAL, поднимает доступные порты и пересылает трафик попарно 0↔1, 2↔3 и т.д. При одном порте пакеты читаются и сбрасываются. В логах отображаются драйвер, MAC и периодическая статистика.

## Быстрый старт

- Сборка локально (Ubuntu):
  - Зависимости: `sudo apt-get install -y dpdk dpdk-dev libnuma-dev build-essential pkg-config`
  - Сборка бинарника: `make`
- Сборка контейнера: `docker build -t mini-dpdk:local .`
- Запуск в Kubernetes: отредактируйте `k8s/pod-vfio.yaml` (см. ниже) и примените:
  - `kubectl apply -f k8s/pod-vfio.yaml`
  - `kubectl logs -f pod/mini-dpdk-vfio`

## Kubernetes: манифест и переменные

- Манифест `k8s/pod-vfio.yaml` включает:
  - hugepages 2Mi (`limits/requests: hugepages-2Mi` + `emptyDir: HugePages-2Mi`)
  - `hostNetwork: true` и монтирование `/dev/infiniband` (важно для Mellanox mlx5)
  - `privileged: true` для упрощения диагностики
- Переменные окружения:
  - `LCORES`: список lcores (например `0`)
  - `DPDK_MEM`: объём памяти EAL в МБ (например `32` для 64Mi hugepages)
  - `EAL_EXTRA`: дополнительные флаги EAL (например `--file-prefix=pod1`)
  - Для Mellanox (рекомендуется):
    - `MLX5_IFNAMES`: имена интерфейсов через запятую, например `eth4,eth5`
    - `MLX5_DEVARGS_EXTRA`: доп. devargs, например `dv_flow_en=0,dv_esw_en=0`
  - Для отладки: `DEBUG_SLEEP=300` или `DEBUG_HOLD=1` (задерживает старт для exec)

Применение:

```
kubectl apply -f k8s/pod-vfio.yaml
kubectl logs -f pod/mini-dpdk-vfio
```

## Docker (локальная проверка)

Для VFIO используйте привязку к `vfio-pci` (актуально для многих PMD, но не для Mellanox). Для Mellanox (mlx5) VFIO не требуется — PMD работает поверх `mlx5_core` и RDMA.

Пример запуска (замените интерфейсы под вашу систему):

```
docker run --rm -it \
  --privileged \
  --ulimit memlock=-1 \
  --mount type=tmpfs,destination=/dev/hugepages,tmpfs-mode=1770 \
  -e LCORES=0 -e DPDK_MEM=128 \
  -e MLX5_IFNAMES="eth4,eth5" \
  mini-dpdk:local --
```

## Заметки по Mellanox (mlx5)

- Для mlx5 не нужно привязывать VF к `vfio-pci` — используйте драйвер ядра `mlx5_core` и доступ к `/dev/infiniband`.
- Не смешивайте PF и VF в одном запуске; лучше использовать пару VF (например `eth4,eth5`).
- Если указываете BDF, добавляйте устройства отдельно: несколько `-a` или через `DPDK_ALLOW="0000:bb:dd.f,0000:bb:dd.f"` (entrypoint развернёт в `-a ... -a ...`).
- Для CX‑4 Lx часто помогает отключить DV/ESW: `MLX5_DEVARGS_EXTRA="dv_flow_en=0,dv_esw_en=0"`.

## Отладка и полезные команды

- Проверка hugepages на ноде:
  - `kubectl get nodes -o custom-columns=NAME:.metadata.name,HP2Mi:.status.allocatable.hugepages-2Mi,HP1Gi:.status.allocatable.hugepages-1Gi`
  - `kubectl describe node <node> | grep -A3 -i hugepages`
- Внутри контейнера:
  - `env | grep -E 'RTE_|LD_LIBRARY_PATH'`
  - `find /usr/local/lib /usr/local/lib/dpdk -name 'librte_net_mlx5*'`
  - `dpdk-devbind.py -s` (если присутствует) или `lspci -nn | grep -i mellanox`
- Логи приложения: раз в ~1 секунду печатается `stats: rx=.. tx=.. drop=..`. При 1 порте `tx=0`, `drop=rx` — ожидаемо.

## Структура репозитория

- `app/mini/main.c` — исходник (L2 0↔1, 2↔3; при 1 порте — дроп).
- `Makefile` — сборка через `pkg-config libdpdk` (динамическая линковка, явные шины и mempool ring).
- `Dockerfile` — многоэтапная сборка, рантайм с `rdma-core`, `libmlx5` и т.д.
- `docker/entrypoint.sh` — собирает EAL‑аргументы из env, подгружает PMD/плагины, поддерживает отладочные задержки.
- `k8s/pod-vfio.yaml` — пример Pod‑манифеста (hostNetwork, hugepages 2Mi, /dev/infiniband).

## Частые проблемы

- “No available Ethernet ports” — не подгрузился PMD или интерфейсы не видны контейнеру. Проверьте `hostNetwork: true`, `/dev/infiniband`, и что в логах виден `librte_net_mlx5`.
- “Cannot create mbuf pool … No such file or directory” — не подхватился mempool driver. В образе есть `librte_mempool_ring.so`, entrypoint его подгружает (`-d`).
- “Could not find bus \"pci\"” — решается явной линковкой с `-lrte_bus_pci` (сделано в Makefile) и присутствием `librte_bus_pci.so` в образе.

