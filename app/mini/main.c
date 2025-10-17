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
#include <errno.h>
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
#define DEFAULT_ICMP_INTERVAL_MS 1000
#define DEFAULT_ICMP_PAYLOAD_LEN 32
#define MAX_ICMP_PAYLOAD_LEN 1400

static volatile bool force_quit = false;

struct icmp_gen_cfg {
    bool enabled;
    uint16_t port_id;
    struct rte_mempool *pool;
    struct rte_ether_addr src_mac;
    struct rte_ether_addr dst_mac;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t ident;
    uint16_t seq;
    uint16_t payload_len;
    uint64_t interval_cycles;
    uint64_t next_send;
};

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

static bool parse_mac_addr(const char *str, struct rte_ether_addr *mac)
{
    if (str == NULL || mac == NULL)
        return false;

    unsigned int bytes[6];
    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x",
               &bytes[0], &bytes[1], &bytes[2],
               &bytes[3], &bytes[4], &bytes[5]) != 6)
        return false;

    for (int i = 0; i < 6; i++)
        mac->addr_bytes[i] = (uint8_t)bytes[i];

    return true;
}

static bool parse_ipv4_addr(const char *str, uint32_t *addr_out)
{
    if (str == NULL || addr_out == NULL)
        return false;

    struct in_addr tmp;
    if (inet_pton(AF_INET, str, &tmp) != 1)
        return false;

    *addr_out = tmp.s_addr;
    return true;
}

static uint16_t icmp_checksum(const void *data, uint16_t len)
{
    const uint16_t *words = data;
    uint32_t sum = 0;
    uint16_t length = len;

    while (length > 1) {
        sum += *words++;
        length -= 2;
    }

    if (length == 1)
        sum += *((const uint8_t *)words) << 8;

    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);

    return (uint16_t)(~sum);
}

static void setup_icmp_generator(struct icmp_gen_cfg *cfg,
                                 struct rte_mempool *pool,
                                 uint16_t nb_ports,
                                 const struct rte_ether_addr *port_macs)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->pool = pool;

    const char *port_env = getenv("ICMP_GEN_PORT");
    const char *src_ip_env = getenv("ICMP_GEN_SRC_IP");
    const char *dst_ip_env = getenv("ICMP_GEN_DST_IP");
    const char *dst_mac_env = getenv("ICMP_GEN_DST_MAC");

    if (!port_env || !src_ip_env || !dst_ip_env || !dst_mac_env)
        return;

    errno = 0;
    char *end = NULL;
    unsigned long port = strtoul(port_env, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0' || port >= nb_ports)
        goto invalid;
    cfg->port_id = (uint16_t)port;

    if (!parse_ipv4_addr(src_ip_env, &cfg->src_ip))
        goto invalid;
    if (!parse_ipv4_addr(dst_ip_env, &cfg->dst_ip))
        goto invalid;
    if (!parse_mac_addr(dst_mac_env, &cfg->dst_mac))
        goto invalid;

    cfg->src_mac = port_macs[cfg->port_id];

    const char *interval_env = getenv("ICMP_GEN_INTERVAL_MS");
    unsigned long interval_ms = DEFAULT_ICMP_INTERVAL_MS;
    if (interval_env && *interval_env) {
        errno = 0;
        interval_ms = strtoul(interval_env, &end, 10);
        if (errno != 0 || end == NULL || *end != '\0' || interval_ms == 0)
            goto invalid;
    }

    const char *payload_env = getenv("ICMP_GEN_PAYLOAD_LEN");
    unsigned long payload_len = DEFAULT_ICMP_PAYLOAD_LEN;
    if (payload_env && *payload_env) {
        errno = 0;
        payload_len = strtoul(payload_env, &end, 10);
        if (errno != 0 || end == NULL || *end != '\0')
            goto invalid;
    }
    if (payload_len > MAX_ICMP_PAYLOAD_LEN)
        payload_len = MAX_ICMP_PAYLOAD_LEN;

    cfg->payload_len = (uint16_t)payload_len;
    cfg->ident = (uint16_t)getpid();

    uint64_t hz = rte_get_timer_hz();
    cfg->interval_cycles = (interval_ms * hz) / 1000;
    if (cfg->interval_cycles == 0)
        cfg->interval_cycles = hz / 1000; // minimum 1 ms
    cfg->next_send = rte_get_timer_cycles();
    cfg->enabled = true;

    char src_ip_buf[INET_ADDRSTRLEN];
    char dst_ip_buf[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &(struct in_addr){ .s_addr = cfg->src_ip }, src_ip_buf, sizeof(src_ip_buf)))
        snprintf(src_ip_buf, sizeof(src_ip_buf), "(invalid)");
    if (!inet_ntop(AF_INET, &(struct in_addr){ .s_addr = cfg->dst_ip }, dst_ip_buf, sizeof(dst_ip_buf)))
        snprintf(dst_ip_buf, sizeof(dst_ip_buf), "(invalid)");

    printf("ICMP generator enabled: port=%u %s -> %s dst-mac=%02x:%02x:%02x:%02x:%02x:%02x interval=%lums payload=%uB\n",
           cfg->port_id,
           src_ip_buf,
           dst_ip_buf,
           cfg->dst_mac.addr_bytes[0], cfg->dst_mac.addr_bytes[1], cfg->dst_mac.addr_bytes[2],
           cfg->dst_mac.addr_bytes[3], cfg->dst_mac.addr_bytes[4], cfg->dst_mac.addr_bytes[5],
           interval_ms,
           cfg->payload_len);
    return;

invalid:
    fprintf(stderr, "ICMP generator disabled: invalid environment configuration\n");
}

static uint16_t icmp_generator_try_send(struct icmp_gen_cfg *cfg, uint64_t now)
{
    if (!cfg->enabled || cfg->pool == NULL || now < cfg->next_send)
        return 0;

    const uint16_t icmp_len = sizeof(struct rte_icmp_hdr) + cfg->payload_len;
    const uint16_t ip_len = sizeof(struct rte_ipv4_hdr) + icmp_len;
    const uint16_t total_len = sizeof(struct rte_ether_hdr) + ip_len;

    if (total_len > rte_pktmbuf_data_room_size(cfg->pool) - RTE_PKTMBUF_HEADROOM) {
        fprintf(stderr, "ICMP generator: total frame %u exceeds mbuf room, disabling\n", total_len);
        cfg->enabled = false;
        return 0;
    }

    cfg->next_send = now + cfg->interval_cycles;

    struct rte_mbuf *pkt = rte_pktmbuf_alloc(cfg->pool);
    if (pkt == NULL)
        return 0;

    if (rte_pktmbuf_append(pkt, total_len) == NULL) {
        rte_pktmbuf_free(pkt);
        return 0;
    }

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    eth->dst_addr = cfg->dst_mac;
    eth->src_addr = cfg->src_mac;
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    struct rte_ipv4_hdr *ipv4 = (struct rte_ipv4_hdr *)(eth + 1);
    ipv4->version_ihl = RTE_IPV4_VHL_DEF;
    ipv4->type_of_service = 0;
    ipv4->total_length = rte_cpu_to_be_16(ip_len);
    struct rte_icmp_hdr *icmp = (struct rte_icmp_hdr *)(ipv4 + 1);
    uint16_t seq = cfg->seq;
    ipv4->packet_id = rte_cpu_to_be_16(seq);
    ipv4->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
    ipv4->time_to_live = 64;
    ipv4->next_proto_id = IPPROTO_ICMP;
    ipv4->src_addr = cfg->src_ip;
    ipv4->dst_addr = cfg->dst_ip;
    ipv4->hdr_checksum = 0;

    icmp->icmp_type = RTE_IP_ICMP_ECHO_REQUEST;
    icmp->icmp_code = 0;
    icmp->icmp_ident = rte_cpu_to_be_16(cfg->ident);
    icmp->icmp_seq_nb = rte_cpu_to_be_16(seq);
    icmp->icmp_cksum = 0;

    uint8_t *payload = (uint8_t *)(icmp + 1);
    for (uint16_t i = 0; i < cfg->payload_len; i++)
        payload[i] = (uint8_t)(i + seq);

    icmp->icmp_cksum = icmp_checksum(icmp, icmp_len);
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);

    pkt->nb_segs = 1;
    pkt->pkt_len = total_len;
    pkt->data_len = total_len;
    pkt->ol_flags = 0;
    pkt->l2_len = sizeof(*eth);
    pkt->l3_len = sizeof(*ipv4);
    pkt->l4_len = sizeof(*icmp) + cfg->payload_len;

    struct rte_mbuf *pkts[1] = { pkt };
    uint16_t sent = rte_eth_tx_burst(cfg->port_id, 0, pkts, 1);
    if (sent == 0) {
        rte_pktmbuf_free(pkt);
        return 0;
    }

    cfg->seq++;
    printf("ICMP generator: sent echo seq=%u on port %u\n", seq, cfg->port_id);
    return sent;
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
    struct rte_ether_addr port_macs[RTE_MAX_ETHPORTS];
    memset(port_macs, 0, sizeof(port_macs));

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
        rte_eth_macaddr_get(port_id, &port_macs[port_id]);
        print_port_info(port_id);
    }

    struct icmp_gen_cfg icmp_gen;
    setup_icmp_generator(&icmp_gen, mbuf_pool, nb_ports, port_macs);

    printf("Running. Ctrl-C to quit.\n");

    // Simple single-threaded loop: for each port, RX burst and forward
    const uint64_t stat_interval = rte_get_timer_hz();
    uint64_t next_stat = rte_get_timer_cycles() + stat_interval;
    uint64_t rx_pkts_total = 0, tx_pkts_total = 0, drop_pkts_total = 0;

    while (likely(!force_quit)) {
        uint64_t now = rte_get_timer_cycles();
        tx_pkts_total += icmp_generator_try_send(&icmp_gen, now);

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

        now = rte_get_timer_cycles();
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
