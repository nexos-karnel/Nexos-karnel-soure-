// dhcp.c — NexOS minimal DHCP client
// Flow: DISCOVER (broadcast) → OFFER → REQUEST (broadcast) → ACK → BOUND
// Setelah BOUND, panggil dhcp_apply() untuk set IP ke VirtioNet

#include "dhcp.h"

// ─── FORWARD DECLARES (dari virtio_net.c) ───────────────
extern void uart_print(const char *s);
extern void uart_println(const char *s);
extern void uart_putc(char c);
extern unsigned short ip_checksum(const void *data, unsigned int len);
extern int  udp_send(VirtioNet *dev,
                     const unsigned char src_ip[4],
                     const unsigned char dst_ip[4],
                     unsigned short src_port, unsigned short dst_port,
                     const void *payload, unsigned int plen);
extern unsigned int nexos_ticks(void);
extern int          nexos_elapsed(unsigned int start, unsigned int timeout);

// ─── HELPERS ─────────────────────────────────────────────

static void dhcp_memset(void *d, unsigned char v, unsigned int n) {
    unsigned char *p = d;
    while (n--) *p++ = v;
}
static void dhcp_memcpy(void *d, const void *s, unsigned int n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    while (n--) *dd++ = *ss++;
}
static int dhcp_memcmp(const void *a, const void *b, unsigned int n) {
    const unsigned char *aa = a, *bb = b;
    while (n--) { if (*aa != *bb) return *aa - *bb; aa++; bb++; }
    return 0;
}

static void print_ip(const unsigned char ip[4]) {
    char buf[4]; int i, n;
    for (i = 0; i < 4; i++) {
        n = ip[i];
        int j = 0;
        if (!n) { uart_putc('0'); }
        else {
            char tmp[3]; int ti = 0;
            while (n) { tmp[ti++] = '0' + n % 10; n /= 10; }
            while (ti--) uart_putc(tmp[ti]);
        }
        if (i < 3) uart_putc('.');
    }
    (void)buf;
}

static void print_dec(unsigned int n) {
    if (!n) { uart_putc('0'); return; }
    char tmp[10]; int i = 0;
    while (n) { tmp[i++] = '0' + n % 10; n /= 10; }
    while (i--) uart_putc(tmp[i]);
}

// Network byte order helpers
static unsigned short htons16(unsigned short x) {
    return (unsigned short)((x >> 8) | (x << 8));
}
static unsigned int htonl32(unsigned int x) {
    return ((x>>24)&0xFF) | (((x>>16)&0xFF)<<8)
         | (((x>>8)&0xFF)<<16) | ((x&0xFF)<<24);
}
static unsigned int ntohl32(unsigned int x) { return htonl32(x); }

// Simple XID generator dari MAC + ticks
static unsigned int make_xid(VirtioNet *dev) {
    unsigned int x = nexos_ticks();
    x ^= (unsigned int)dev->mac[2] << 24;
    x ^= (unsigned int)dev->mac[3] << 16;
    x ^= (unsigned int)dev->mac[4] << 8;
    x ^= (unsigned int)dev->mac[5];
    x *= 0x6c62272eU;
    return x ? x : 0xdeadbeefU;
}

// ─── BUILD DHCP PACKET ───────────────────────────────────

static unsigned int dhcp_build(DhcpClient *c, unsigned char msgtype,
                                unsigned char *out, unsigned int maxlen)
{
    if (maxlen < sizeof(DhcpPkt)) return 0;
    DhcpPkt *pkt = (DhcpPkt *)out;
    dhcp_memset(pkt, 0, sizeof(DhcpPkt));

    pkt->op     = DHCP_BOOTREQUEST;
    pkt->htype  = 1;
    pkt->hlen   = 6;
    pkt->hops   = 0;
    pkt->xid    = htonl32(c->xid);
    pkt->secs   = 0;
    pkt->flags  = htons16(0x8000);  // broadcast flag

    // ciaddr = 0 untuk DISCOVER/REQUEST awal
    dhcp_memset(pkt->ciaddr, 0, 4);

    // chaddr = MAC kita (6 byte, sisa zero-pad)
    dhcp_memcpy(pkt->chaddr, c->dev->mac, 6);

    // Magic cookie
    pkt->magic = htonl32(DHCP_MAGIC_COOKIE);

    // ── Options ──
    unsigned char *opt = pkt->options;
    unsigned int   oi  = 0;

    // Option 53: DHCP Message Type
    opt[oi++] = DHCP_OPT_MSGTYPE;
    opt[oi++] = 1;
    opt[oi++] = msgtype;

    if (msgtype == DHCP_REQUEST) {
        // Option 50: Requested IP Address (dari OFFER)
        opt[oi++] = DHCP_OPT_REQIP;
        opt[oi++] = 4;
        dhcp_memcpy(&opt[oi], c->offered_ip, 4); oi += 4;

        // Option 54: Server Identifier
        opt[oi++] = DHCP_OPT_SERVERID;
        opt[oi++] = 4;
        dhcp_memcpy(&opt[oi], c->server_ip, 4); oi += 4;
    }

    // Option 55: Parameter Request List
    opt[oi++] = 55;
    opt[oi++] = 4;
    opt[oi++] = DHCP_OPT_SUBNET;   // 1
    opt[oi++] = DHCP_OPT_ROUTER;   // 3
    opt[oi++] = DHCP_OPT_DNS;      // 6
    opt[oi++] = DHCP_OPT_LEASE;    // 51

    // End
    opt[oi++] = DHCP_OPT_END;

    return sizeof(DhcpPkt);
}

// ─── SEND HELPERS ────────────────────────────────────────

static const unsigned char BROADCAST_IP[4]  = {255, 255, 255, 255};
static const unsigned char ZERO_IP[4]       = {0, 0, 0, 0};

static int dhcp_send_discover(DhcpClient *c) {
    unsigned char buf[sizeof(DhcpPkt)];
    unsigned int  len = dhcp_build(c, DHCP_DISCOVER, buf, sizeof(buf));
    if (!len) return -1;
    uart_println("[dhcp] Sending DISCOVER (broadcast)...");
    return udp_send(c->dev,
                    ZERO_IP, BROADCAST_IP,
                    DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                    buf, len);
}

static int dhcp_send_request(DhcpClient *c) {
    unsigned char buf[sizeof(DhcpPkt)];
    unsigned int  len = dhcp_build(c, DHCP_REQUEST, buf, sizeof(buf));
    if (!len) return -1;
    uart_print("[dhcp] Sending REQUEST for ");
    print_ip(c->offered_ip);
    uart_println("...");
    return udp_send(c->dev,
                    ZERO_IP, BROADCAST_IP,
                    DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                    buf, len);
}

// ─── PARSE OPTIONS ───────────────────────────────────────

static void dhcp_parse_options(DhcpClient *c, const unsigned char *opts,
                                unsigned int olen, unsigned char *out_msgtype)
{
    unsigned int i = 0;
    *out_msgtype = 0;

    while (i < olen) {
        unsigned char tag = opts[i++];
        if (tag == DHCP_OPT_END) break;
        if (tag == DHCP_OPT_PAD) continue;
        if (i >= olen) break;
        unsigned char len = opts[i++];
        if (i + len > olen) break;

        switch (tag) {
        case DHCP_OPT_MSGTYPE:
            if (len >= 1) *out_msgtype = opts[i];
            break;
        case DHCP_OPT_SUBNET:
            if (len >= 4) dhcp_memcpy(c->subnet, &opts[i], 4);
            break;
        case DHCP_OPT_ROUTER:
            if (len >= 4) dhcp_memcpy(c->router, &opts[i], 4);
            break;
        case DHCP_OPT_DNS:
            if (len >= 4) dhcp_memcpy(c->dns, &opts[i], 4);
            break;
        case DHCP_OPT_SERVERID:
            if (len >= 4) dhcp_memcpy(c->server_ip, &opts[i], 4);
            break;
        case DHCP_OPT_LEASE:
            if (len >= 4) {
                c->lease_secs = ntohl32(
                    ((unsigned int)opts[i]   << 24) |
                    ((unsigned int)opts[i+1] << 16) |
                    ((unsigned int)opts[i+2] << 8)  |
                    ((unsigned int)opts[i+3]));
            }
            break;
        default:
            break;
        }
        i += len;
    }
}

// ─── PUBLIC API ──────────────────────────────────────────

void dhcp_init(DhcpClient *c, VirtioNet *dev) {
    dhcp_memset(c, 0, sizeof(DhcpClient));
    c->dev   = dev;
    c->state = DHCP_STATE_IDLE;
}

int dhcp_start(DhcpClient *c) {
    if (!c->dev || !c->dev->ready) return -1;
    c->xid            = make_xid(c->dev);
    c->retries        = 0;
    c->state          = DHCP_STATE_DISCOVER;
    dhcp_memset(c->offered_ip, 0, 4);
    dhcp_memset(c->server_ip,  0, 4);
    dhcp_memset(c->subnet,     0, 4);
    dhcp_memset(c->router,     0, 4);
    dhcp_memset(c->dns,        0, 4);
    c->lease_secs     = 0;
    c->last_send_tick = nexos_ticks();
    return dhcp_send_discover(c);
}

void dhcp_input(DhcpClient *c, const unsigned char *payload, unsigned int len) {
    if (len < sizeof(DhcpPkt)) return;

    const DhcpPkt *pkt = (const DhcpPkt *)payload;

    // Validasi: harus BOOTREPLY, xid cocok, magic cookie benar
    if (pkt->op != DHCP_BOOTREPLY)                        return;
    if (ntohl32(pkt->xid) != c->xid)                     return;
    if (ntohl32(pkt->magic) != DHCP_MAGIC_COOKIE)        return;

    // Pastikan ini untuk MAC kita
    if (dhcp_memcmp(pkt->chaddr, c->dev->mac, 6) != 0)   return;

    // Parse options
    unsigned char msgtype = 0;
    unsigned int  opts_len = len - (unsigned int)((unsigned char*)pkt->options - (unsigned char*)pkt);
    if (opts_len > DHCP_OPTIONS_LEN) opts_len = DHCP_OPTIONS_LEN;
    dhcp_parse_options(c, pkt->options, opts_len, &msgtype);

    switch (c->state) {

    case DHCP_STATE_DISCOVER:
        if (msgtype == DHCP_OFFER) {
            dhcp_memcpy(c->offered_ip, pkt->yiaddr, 4);
            uart_print("[dhcp] OFFER received: ");
            print_ip(c->offered_ip);
            uart_print(" from ");
            print_ip(c->server_ip);
            uart_println("");
            // Kirim REQUEST
            c->state = DHCP_STATE_REQUEST;
            c->retries = 0;
            c->last_send_tick = nexos_ticks();
            dhcp_send_request(c);
        }
        break;

    case DHCP_STATE_REQUEST:
        if (msgtype == DHCP_ACK) {
            dhcp_memcpy(c->offered_ip, pkt->yiaddr, 4);
            uart_print("[dhcp] ACK! Lease: ");
            print_ip(c->offered_ip);
            uart_println("");
            c->state = DHCP_STATE_BOUND;
            dhcp_apply(c);
        } else if (msgtype == DHCP_NAK) {
            uart_println("[dhcp] NAK received. Restarting DISCOVER...");
            c->state   = DHCP_STATE_IDLE;
            c->retries = 0;
            dhcp_start(c);
        }
        break;

    default:
        break;
    }
}

DhcpState dhcp_poll(DhcpClient *c) {
    if (c->state == DHCP_STATE_BOUND || c->state == DHCP_STATE_FAILED
        || c->state == DHCP_STATE_IDLE) {
        return c->state;
    }

    // Timeout per-state: ~3 detik di QEMU (pakai ticks, approx)
    // 62.5MHz × 3s = 187.5M ticks; 1GHz × 3s = 3G (overflow-safe karena unsigned)
    unsigned int timeout = 187500000U;

    if (!nexos_elapsed(c->last_send_tick, timeout)) return c->state;

    // Timeout — retry atau fail
    c->retries++;
    if (c->retries > 4) {
        uart_println("[dhcp] FAILED: no response after 4 retries.");
        c->state = DHCP_STATE_FAILED;
        return c->state;
    }

    uart_print("[dhcp] Timeout, retry #");
    print_dec(c->retries);
    uart_println("");

    c->last_send_tick = nexos_ticks();

    if (c->state == DHCP_STATE_DISCOVER) {
        dhcp_send_discover(c);
    } else if (c->state == DHCP_STATE_REQUEST) {
        dhcp_send_request(c);
    }

    return c->state;
}

void dhcp_apply(DhcpClient *c) {
    // Subnet default kalau server gak kasih
    if (c->subnet[0] == 0 && c->subnet[1] == 0 &&
        c->subnet[2] == 0 && c->subnet[3] == 0) {
        c->subnet[0] = 255; c->subnet[1] = 255;
        c->subnet[2] = 255; c->subnet[3] = 0;
    }

    dhcp_memcpy(c->dev->ip,      c->offered_ip, 4);
    dhcp_memcpy(c->dev->gateway, c->router,     4);
    dhcp_memcpy(c->dev->netmask, c->subnet,     4);

    uart_println("[dhcp] Applied lease to interface.");
    dhcp_print(c);
}

void dhcp_print(DhcpClient *c) {
    uart_print("  IP      : "); print_ip(c->offered_ip); uart_println("");
    uart_print("  Subnet  : "); print_ip(c->subnet);     uart_println("");
    uart_print("  Router  : "); print_ip(c->router);     uart_println("");
    uart_print("  DNS     : "); print_ip(c->dns);        uart_println("");
    uart_print("  Server  : "); print_ip(c->server_ip);  uart_println("");
    uart_print("  Lease   : "); print_dec(c->lease_secs);uart_println(" seconds");
}
