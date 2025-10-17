# Multi-stage build for the minimal DPDK app

FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
    build-essential pkg-config make \
    dpdk dpdk-dev libnuma-dev ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Debug: show where libfdt is in builder (for troubleshooting)
RUN find /usr/lib -maxdepth 2 -name 'libfdt.so*' -print || true

WORKDIR /src
COPY . .
RUN make all

FROM ubuntu:22.04 AS runner
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
    libnuma1 libbsd0 libfdt1 rdma-core ibverbs-providers libmlx5-1 \
    libnl-3-200 libnl-route-3-200 libnl-genl-3-200 \
    python3 pciutils ethtool ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/mini-dpdk /usr/local/bin/mini-dpdk
COPY --from=builder /usr/lib/x86_64-linux-gnu/librte_*.so* /usr/local/lib/
COPY --from=builder /usr/lib/x86_64-linux-gnu/dpdk /usr/local/lib/dpdk
COPY docker/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# Default EAL parameters can be overridden via env/args
ENV LCORES=0 \
    DPDK_MEM=1024 \
    DPDK_ALLOW="" \
    EAL_EXTRA=""

RUN echo "/usr/local/lib" > /etc/ld.so.conf.d/local-dpdk.conf \
 && echo "/usr/local/lib/dpdk" >> /etc/ld.so.conf.d/local-dpdk.conf \
 && ldconfig
ENV LD_LIBRARY_PATH=/usr/local/lib:/usr/local/lib/dpdk
RUN ldconfig
ENTRYPOINT ["/entrypoint.sh"]
CMD ["--"]
