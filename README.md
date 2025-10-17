# mini-dpdk — минимальное DPDK‑приложение для Kubernetes/Docker

Минимальный L2 форвардер на DPDK: инициализирует EAL, поднимает доступные порты и пересылает трафик попарно 0↔1, 2↔3 и т.д. При одном порте пакеты читаются и сбрасываются. В логах отображаются драйвер, MAC и периодическая статистика.

## Быстрый старт

- Сборка локально (Ubuntu):
  - Зависимости: `sudo apt-get install -y dpdk dpdk-dev libnuma-dev build-essential pkg-config`
  - Сборка бинарника: `make`
- Сборка контейнера: `docker build -t mini-dpdk:local .`
- Запуск в Kubernetes: отредактируйте `k8s/pod-vfio.yaml` (см. ниже) и примените:
  - `kubectl create -f k8s/pod-vfio.yaml`
  - `kubectl logs -f pod/mini-dpdk-vfio`

## Kubernetes: манифест и переменные

- Манифест `k8s/pod-vfio.yaml` включает:
  - hugepages 2Mi (`limits/requests: hugepages-2Mi` + `emptyDir: HugePages-2Mi`)
  - `privileged: true` для упрощения диагностики
- Переменные окружения:
  - `LCORES`: список lcores (например `0`)
  - `DPDK_MEM`: объём памяти EAL в МБ (например `32` для 64Mi hugepages)
  - `EAL_EXTRA`: дополнительные флаги EAL (например `--file-prefix=pod1`)
  - Для Mellanox (рекомендуется):
    - `MLX5_DEVARGS_EXTRA`: доп. devargs, например `dv_flow_en=0,dv_esw_en=0`
  - Для отладки: `DEBUG_SLEEP=300` или `DEBUG_HOLD=1` (задерживает старт для exec)
  - ICMP-генератор (опционально — создаёт ICMP Echo Request на выбранном порту):
    - `ICMP_GEN_PORT`: индекс DPDK-порта, с которого отправлять ICMP.
    - `ICMP_GEN_SRC_IP`: исходный IPv4-адрес в пакетах (например `192.0.2.1`).
    - `ICMP_GEN_DST_IP`: целевой IPv4-адрес (например `192.0.2.2`).
    - `ICMP_GEN_DST_MAC`: MAC-адрес назначения в формате `aa:bb:cc:dd:ee:ff`.
    - `ICMP_GEN_INTERVAL_MS`: (опционально) период отправки в миллисекундах, по умолчанию `1000`.
    - `ICMP_GEN_PAYLOAD_LEN`: (опционально) размер полезной нагрузки ICMP в байтах, по умолчанию `32`.
    - Источник L2 (MAC) берётся автоматически с выбранного порта; первый пакет отправляется сразу после запуска, далее — с заданным интервалом.

Применение:

```
kubectl create -f k8s/pod-vfio.yaml
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

Пример проверки связи между двумя хостами с прямым подключением (каждый запускает `mini-dpdk` на своём VF/интерфейсе):

```
# Хост A
export ICMP_GEN_PORT=0
export ICMP_GEN_SRC_IP=192.0.2.1
export ICMP_GEN_DST_IP=192.0.2.2
export ICMP_GEN_DST_MAC=aa:bb:cc:dd:ee:01  # MAC хоста B

# Хост B
export ICMP_GEN_PORT=0
export ICMP_GEN_SRC_IP=192.0.2.2
export ICMP_GEN_DST_IP=192.0.2.1
export ICMP_GEN_DST_MAC=aa:bb:cc:dd:ee:02  # MAC хоста A
```

После запуска оба процесса будут раз в секунду рассылать ICMP Echo Request; ответы можно увидеть в логах (`inspect_for_icmp` печатает входящие ICMP), а также проверять результирующий трафик на целевых интерфейсах.

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

### SR‑IOV: проверка и управление VF (пример для PF `eth2`)

- Проверить возможности и текущее число VF:

```
cat /sys/class/net/eth2/device/sriov_totalvfs
cat /sys/class/net/eth2/device/sriov_numvfs
```

- Пересоздать 2 VF (сначала обнулить, затем задать желаемое количество):

```
echo 0 | tee /sys/class/net/eth2/device/sriov_numvfs
echo 2 | tee /sys/class/net/eth2/device/sriov_numvfs
```

- Посмотреть их BDF (symlink `virtfn*` указывает на адрес устройства):

```
ls -l /sys/class/net/eth2/device/virtfn*
# → …virtfn0 -> 0000:17:00.2, virtfn1 -> 0000:17:00.3
```

- (Опционально) задать MAC/доверие/antispoof для конкретных VF:

```
ip link set eth2 vf 0 mac 02:11:22:33:44:55 trust on spoofchk off
ip link set eth2 vf 1 mac 02:11:22:33:44:66 trust on spoofchk off
```

Примечания:
- Замените `eth2` на имя вашего PF.
- Команды требуют прав root (используйте `sudo` при необходимости).

#### Привязка драйверов (bind/unbind): PF на `mlx5_core`, VF на `vfio-pci`

- Пример: PF `0000:17:00.0` оставить на `mlx5_core`, VF `0000:17:00.2` привязать к `vfio-pci`:

```
# (опционально) снять текущее привязки
echo 0000:17:00.2 | sudo tee /sys/bus/pci/drivers/vfio-pci/unbind 2>/dev/null || true
echo 0000:17:00.2 | sudo tee /sys/bus/pci/devices/0000:17:00.2/driver/unbind 2>/dev/null || true

# привязать VF к vfio-pci
echo 0000:17:00.2 | sudo tee /sys/bus/pci/drivers/vfio-pci/bind
или
/usr/local/bin/dpdk-devbind.py -b vfio-pci 0000:17:00.2

# убедиться, что PF на mlx5_core (при необходимости — привязать)
echo 0000:17:00.0 | sudo tee /sys/bus/pci/drivers/mlx5_core/bind

# проверить
lspci -k -s 17:00.2
lspci -k -s 17:00.0
```

Заметки:
- Общий случай для Intel/других NIC: PF остаётся в «родном» драйвере, VF — в `vfio-pci`.
- Для Mellanox (`mlx5`) обычно VFIO не требуется: и PF, и VF работают с `mlx5_core` (PMD mlx5 использует драйвер ядра и RDMA).

## Структура репозитория

- `app/mini/main.c` — исходник (L2 0↔1, 2↔3; при 1 порте — дроп).
- `Makefile` — сборка через `pkg-config libdpdk` (динамическая линковка, явные шины и mempool ring).
- `Dockerfile` — многоэтапная сборка, рантайм с `rdma-core`, `libmlx5` и т.д.
- `docker/entrypoint.sh` — собирает EAL‑аргументы из env, подгружает PMD/плагины, поддерживает отладочные задержки.
- `k8s/pod-vfio.yaml` — пример Pod‑манифеста (hostNetwork, hugepages 2Mi, /dev/infiniband).

## Частые проблемы
- При запуске в Kubernetes не забывайте включить `hostNetwork: true` в Pod — без него VF не видны внутри контейнера и DPDK не находит порты.
