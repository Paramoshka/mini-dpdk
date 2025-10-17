// Minimal DPDK app: probes/initializes ports and does simple L2 RX->TX forwarding
// If only one port is available, it drops received packets.

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <rte_common.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_log.h>
#include <rte_cycles.h>
#include <rte_branch_prediction.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_icmp.h>

#define RX_RING_SIZE 512
#define TX_RING_SIZE 512
#define NUM_MBUFS 8192
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

static volatile bool force_quit = false;

static void handle_signal(int sig)
{
    (void)sig;
    force_quit = true;
}

static void inspect_for_icmp(uint16_t port_id, const struct rte_mbuf *mbuf)
{
    const uint16_t pkt_len = rte_pktmbuf_pkt_len(mbuf);
    const uint16_t data_len = rte_pktmbuf_data_len(mbuf);
    if (pkt_len < sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_icmp_hdr))
        return;
    if (data_len < sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr))
        return;

    const struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbuf, const struct rte_ether_hdr *);
    if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
        return;

    const struct rte_ipv4_hdr *ipv4 = (const struct rte_ipv4_hdr *)(eth + 1);
    const uint16_t ip_hdr_len = (ipv4->version_ihl & 0x0f) * 4;
    if (ip_hdr_len < sizeof(struct rte_ipv4_hdr))
        return;
    if (pkt_len < sizeof(struct rte_ether_hdr) + ip_hdr_len + sizeof(struct rte_icmp_hdr))
        return;
    if (data_len < sizeof(struct rte_ether_hdr) + ip_hdr_len)
        return;
    if (ipv4->next_proto_id != IPPROTO_ICMP)
        return;

    const uint8_t *l4 = (const uint8_t *)ipv4 + ip_hdr_len;
    const struct rte_icmp_hdr *icmp = (const struct rte_icmp_hdr *)l4;

    char src[INET_ADDRSTRLEN];
    char dst[INET_ADDRSTRLEN];
    struct in_addr src_addr = { .s_addr = ipv4->src_addr };
    struct in_addr dst_addr = { .s_addr = ipv4->dst_addr };

    if (inet_ntop(AF_INET, &src_addr, src, sizeof(src)) == NULL)
        snprintf(src, sizeof(src), "(invalid)");
    if (inet_ntop(AF_INET, &dst_addr, dst, sizeof(dst)) == NULL)
        snprintf(dst, sizeof(dst), "(invalid)");

    printf("port %u ICMP type=%u code=%u %s -> %s len=%u\n",
           port_id,
           icmp->icmp_type,
           icmp->icmp_code,
           src,
           dst,
           pkt_len);
}

static void print_port_info(uint16_t port_id)
{
    struct rte_eth_dev_info info;
    rte_eth_dev_info_get(port_id, &info);

    struct rte_ether_addr mac;
    rte_eth_macaddr_get(port_id, &mac);

    printf("Port %u: driver=%s, if_index=%u, MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
           port_id,
           info.driver_name ? info.driver_name : "(unknown)",
           info.if_index,
           mac.addr_bytes[0], mac.addr_bytes[1], mac.addr_bytes[2],
           mac.addr_bytes[3], mac.addr_bytes[4], mac.addr_bytes[5]);
}

static unsigned parse_uint_arg(const char *arg, const char *name, unsigned def)
{
    size_t len = strlen(name);
    if (strncmp(arg, name, len) == 0) {
        const char *eq = arg + len;
        if (*eq == '=') {
            unsigned v = (unsigned)strtoul(eq + 1, NULL, 10);
            if (v > 0)
                return v;
        }
    }
    return def;
}

int main(int argc, char **argv)
{
    int ret;
    uint16_t port_id;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    argc -= ret;
    argv += ret;

    const uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0)
        rte_exit(EXIT_FAILURE, "No available Ethernet ports. Did you bind VFs to vfio-pci and allow them?\n");

    printf("EAL OK. lcores=%u, ports=%u\n", rte_lcore_count(), nb_ports);

    // Defaults, allow override via simple app args after EAL '--':
    // --total-num-mbufs=N, --mbuf-size=N, --mbuf-cache=N
    unsigned total_mbufs = NUM_MBUFS * (nb_ports > 0 ? nb_ports : 1);
    uint16_t data_room = RTE_MBUF_DEFAULT_BUF_SIZE;
    unsigned cache_sz = MBUF_CACHE_SIZE;

    for (int i = 0; i < argc; i++) {
        total_mbufs = parse_uint_arg(argv[i], "--total-num-mbufs", total_mbufs);
        data_room  = (uint16_t)parse_uint_arg(argv[i], "--mbuf-size", data_room);
        cache_sz   = parse_uint_arg(argv[i], "--mbuf-cache", cache_sz);
    }

    if (cache_sz >= total_mbufs)
        cache_sz = RTE_MIN(250U, total_mbufs / 4);

    // Create a single mempool shared across ports
    unsigned socket_id = rte_socket_id();
    char pool_name[64];
    snprintf(pool_name, sizeof(pool_name), "MBUF_POOL_%u", socket_id);

    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create(
        pool_name, total_mbufs, cache_sz, 0, data_room, socket_id);
    if (mbuf_pool == NULL) {
        fprintf(stderr, "Failed to create mbuf pool on socket %u: %s\n",
                socket_id, rte_strerror(rte_errno));
        // Retry with ANY socket
        snprintf(pool_name, sizeof(pool_name), "MBUF_POOL_ANY");
        mbuf_pool = rte_pktmbuf_pool_create(
            pool_name, total_mbufs, cache_sz, 0, data_room, SOCKET_ID_ANY);
        if (mbuf_pool == NULL)
            rte_exit(EXIT_FAILURE, "Cannot create mbuf pool even with SOCKET_ID_ANY: %s\n",
                     rte_strerror(rte_errno));
    }

    // Configure each detected port
    RTE_ETH_FOREACH_DEV(port_id) {
        struct rte_eth_conf port_conf;
        memset(&port_conf, 0, sizeof(port_conf));
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

        int rc = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
        if (rc < 0)
            rte_exit(EXIT_FAILURE, "rte_eth_dev_configure(%u) failed: %d\n", port_id, rc);

        rc = rte_eth_rx_queue_setup(port_id, 0, RX_RING_SIZE,
                                    rte_eth_dev_socket_id(port_id), NULL, mbuf_pool);
        if (rc < 0)
            rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup(%u) failed: %d\n", port_id, rc);

        rc = rte_eth_tx_queue_setup(port_id, 0, TX_RING_SIZE,
                                    rte_eth_dev_socket_id(port_id), NULL);
        if (rc < 0)
            rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup(%u) failed: %d\n", port_id, rc);

        rc = rte_eth_dev_start(port_id);
        if (rc < 0)
            rte_exit(EXIT_FAILURE, "rte_eth_dev_start(%u) failed: %d\n", port_id, rc);

        rte_eth_promiscuous_enable(port_id);
        print_port_info(port_id);
    }

    printf("Running. Ctrl-C to quit.\n");

    // Simple single-threaded loop: for each port, RX burst and forward
    const uint64_t stat_interval = rte_get_timer_hz();
    uint64_t next_stat = rte_get_timer_cycles() + stat_interval;
    uint64_t rx_pkts_total = 0, tx_pkts_total = 0, drop_pkts_total = 0;

    while (likely(!force_quit)) {
        RTE_ETH_FOREACH_DEV(port_id) {
            struct rte_mbuf *bufs[BURST_SIZE];
            const uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, bufs, BURST_SIZE);
            if (unlikely(nb_rx == 0))
                continue;
            rx_pkts_total += nb_rx;

            uint16_t peer = port_id ^ 1; // 0<->1, 2<->3, ...
            uint16_t sent = 0;

            for (uint16_t i = 0; i < nb_rx; i++)
                inspect_for_icmp(port_id, bufs[i]);

            if (nb_ports >= 2 && peer < nb_ports) {
                // In L2 forward, swap src/dst MAC if needed (not strictly required)
                // For simplicity, we just transmit as-is.
                sent = rte_eth_tx_burst(peer, 0, bufs, nb_rx);
            }

            if (sent < nb_rx) {
                // free unsent or all if single port
                for (uint16_t i = sent; i < nb_rx; i++)
                    rte_pktmbuf_free(bufs[i]);
                drop_pkts_total += (nb_rx - sent);
            } else {
                tx_pkts_total += sent;
            }
        }

        uint64_t now = rte_get_timer_cycles();
        if (now >= next_stat) {
            printf("stats: rx=%" PRIu64 " tx=%" PRIu64 " drop=%" PRIu64 "\n",
                   rx_pkts_total, tx_pkts_total, drop_pkts_total);
            next_stat = now + stat_interval;
        }
    }

    printf("Exiting... stopping ports\n");
    RTE_ETH_FOREACH_DEV(port_id) {
        rte_eth_dev_stop(port_id);
        rte_eth_dev_close(port_id);
    }

    rte_eal_cleanup();
    return 0;
}
