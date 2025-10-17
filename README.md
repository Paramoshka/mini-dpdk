# mini-dpdk: Примитивное DPDK приложение для Pod

Это минимальное DPDK‑приложение, которое инициализирует EAL, конфигурирует все доступные порты и выполняет простую L2 пересылку: порт 0 <-> 1, 2 <-> 3 и т.д. Если доступен только один порт — пакеты просто читаются и сбрасываются. В логе показывается MAC/драйвер и периодическая статистика.

## Сборка локально (хост)

Требуется установленный DPDK (dev-пакеты) и pkg-config.
sudo apt-get install -y dpdk dpdk-dev libnuma-dev build-essential pkg-config
- Ubuntu: ``
- Сборка: `make`
- Запуск (root + заранее забайнденные VF под vfio-pci):
  - Пример: `sudo ./build/mini-dpdk -l 0 -a 0000:af:00.1 -a 0000:af:00.2 --`

## Сборка контейнера

- `docker build -t mini-dpdk:local .`

В образе используется многоэтапная сборка на Ubuntu 22.04, устанавливаются пакеты `dpdk`/`dpdk-dev`, затем собирается бинарник.

## Запуск в Docker (быстрый тест)

Нужно, чтобы на хосте были заранее:
- Включён IOMMU и загружен `vfio-pci` (`sudo modprobe vfio-pci`)
- VF интерфейсы привязаны к `vfio-pci` (например `dpdk-devbind.py -b vfio-pci 0000:af:00.1 0000:af:00.2`)

Минимальный запуск (пример, замените BDF):

```
docker run --rm -it \
  --privileged \
  --ulimit memlock=-1 \
  --mount type=tmpfs,destination=/dev/hugepages,tmpfs-mode=1770 \
  -e DPDK_ALLOW="0000:af:00.1,0000:af:00.2" \
  -e LCORES=0 -e DPDK_MEM=1024 \
  -v /dev/vfio:/dev/vfio \
  mini-dpdk:local --
```

Переменная `DPDK_ALLOW` — список PCI BDF через запятую. По умолчанию приложение запустится с EAL `-l ${LCORES:-0} -m ${DPDK_MEM:-1024}`. Любые дополнительные EAL‑аргументы можно передать через `EAL_EXTRA`, например `--file-prefix=pod1`.

## Запуск в Kubernetes (Pod)

1) Убедитесь, что на ноде настроены hugepages (например `hugepages-2Mi`) и VF‑интерфейсы привязаны к `vfio-pci`.
2) Отредактируйте `k8s/pod-vfio.yaml` и подставьте ваши BDF в `env: DPDK_ALLOW`.
3) Задеплойте Pod:

```
kubectl apply -f k8s/pod-vfio.yaml
kubectl logs -f pod/mini-dpdk-vfio
```

Манифест запускает контейнер с `privileged: true`, монтирует `/dev/vfio` и `hugepages` (2Mi) в `/dev/hugepages`. Значения hugepages в `resources.requests/limits` при необходимости скорректируйте под вашу кластерную политику.

Если вы используете SR-IOV Network Device Plugin, вместо привилегированного пода лучше запросить VF как ресурс и использовать CNI‑сеть. Данное приложение совместимо, так как для DPDK достаточно, чтобы внутри контейнера были доступны VF (через vfio) и hugepages.

## Полезные заметки

- В DPDK EAL используйте `-a <BDF>`/`--allow <BDF>` для явного разрешения нужных устройств. Можно оставить пустым, если хотите открыть все доступные PCI‑девайсы — но в pod‑среде это часто избыточно.
- Для нескольких пар портов трафик идёт попарно: 0<->1, 2<->3, и т.д. Если порт один — пакеты будут сбрасываться.
- Для корректного выделения hugepages в Kubernetes важны и кворум ноды, и настройки kubelet (`--feature-gates=HugePages`).
- Для быстрой диагностики смотрите логи и `stats: rx=... tx=... drop=...` каждые ~1 секунду.
- kubectl get nodes -o custom-columns=NAME:.metadata.name,HP2Mi:.status.allocatable.hugepages-2Mi,HP1Gi:.status.allocatable.hugepages-1Gi
- kubectl describe node networklab-sriov-0 | grep -A3 -i hugepages

## Структура

- `app/mini/main.c` — исходник минимального приложения.
- `Makefile` — сборка локально через pkg-config (`libdpdk`).
- `Dockerfile` — многоэтапная сборка образа.
- `docker/entrypoint.sh` — простой wrapper для сборки EAL‑аргументов из env.
- `k8s/pod-vfio.yaml` — пример Pod‑манифеста для VFIO.



# Прочее

# 1) Включить IOMMU (проверьте; если нет — добавить в загрузку и перезагрузить):
# в /proc/cmdline должны быть: intel_iommu=on iommu=pt   # (или amd_iommu=on)
cat /proc/cmdline

# 2) Загрузить vfio
sudo modprobe vfio-pci

# 3) Отвязать VF от mlx5_core и привязать к vfio-pci (BDF вашей VF: 0000:17:00.2)
sudo /usr/local/bin/dpdk-devbind.py -s || true         # посмотреть список (если есть)
sudo /usr/local/bin/dpdk-devbind.py -b vfio-pci 0000:17:00.0
# Если скрипта нет:
# echo 0000:17:00.2 | sudo tee /sys/bus/pci/drivers/mlx5_core/unbind
# echo 15b3 1018 | sudo tee /sys/bus/pci/drivers/vfio-pci/new_id   # (VID:DID пример для Mellanox)
# echo 0000:17:00.2 | sudo tee /sys/bus/pci/drivers/vfio-pci/bind

# 4) Проверьте, что устройство теперь под vfio-pci
readlink /sys/bus/pci/devices/0000:17:00.2/driver
# -> должен указывать на .../vfio-pci

# 5) Убедитесь, что /dev/vfio/* существует
ls -l /dev/vfio
echo "vm.nr_hugepages = 256" | sudo tee /etc/sysctl.d/99-hugepages.conf
sudo sysctl --system
sudo systemctl restart kubelet  # по необходимости
kubectl describe node <имя-ноды> | sed -n '/Allocatable:/,/Events:/p'


# вариант через dpdk-devbind.py
sudo /usr/local/bin/dpdk-devbind.py -b mlx5_core 0000:17:00.0

# или руками:
echo 0000:17:00.2 | sudo tee /sys/bus/pci/drivers/vfio-pci/unbind
echo 0000:17:00.2 | sudo tee /sys/bus/pci/drivers/mlx5_core/bind


/usr/local/bin/dpdk-devbind.py -s | grep 17:00.2
# должно быть: drv=mlx5_core (а не vfio-pci)

