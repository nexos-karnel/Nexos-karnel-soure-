// dns.c — NexOS minimal DNS resolver implementation
// Query A record via UDP/53, dengan retry + timeout (lihat dns.h untuk scope)

#include "dns.h"
#include "nexos_time.h"

// ─── FORWARD DECLARES (dari virtio_net.c) ───────────────
extern void uart_print(const char *s);
extern void uart_println(const char *s);
extern void uart_putc(char c);
extern unsigned short ip_checksum(const void *data, unsigned int len);

// ─── HELPERS (gaya sama seperti tcp.c/virtio_net.c) ─────

static void dns_memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
}

static void dns_memset(void *dst, unsigned char v, unsigned int n) {
    unsigned char *d = dst;
    while (n--) *d++ = v;
}

static unsigned int dns_strlen(const char *s) {
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static void print_dec(unsigned int n) {
    char buf[12]; int i = 0;
    if (!n) { uart_putc('0'); return; }
    while (n) { buf[i++] = '0' + n % 10; n /= 10; }
    while (i--) uart_putc(buf[i]);
}

static unsigned short htons16(unsigned short x) {
    return (unsigned short)((x >> 8) | (x << 8));
}
static unsigned short ntohs16(unsigned short x) { return htons16(x); }

// ─── IP LITERAL HELPER ──────────────────────────────────

int dns_is_ip_literal(const char *s) {
    // Cek sederhana: cuma berisi digit dan titik, dan ada tepat 3 titik
    int dots = 0;
    for (unsigned int i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '.') { dots++; continue; }
        if (c < '0' || c > '9') return 0;
    }
    return dots == 3;
}

int dns_parse_ip_literal(const char *s, unsigned char out[4]) {
    int i = 0, oct = 0;
    while (s[i] && oct < 4) {
        unsigned int n = 0;
        int digits = 0;
        while (s[i] >= '0' && s[i] <= '9') {
            n = n * 10 + (unsigned int)(s[i] - '0');
            i++; digits++;
        }
        if (digits == 0 || n > 255) return -1;
        out[oct++] = (unsigned char)n;
        if (s[i] == '.') i++;
    }
    return (oct == 4) ? 0 : -1;
}

// ─── PSEUDO-RANDOM QUERY ID ──────────────────────────────
// Gak ada RNG hardware — pakai tick counter sebagai sumber "random".
// Cukup karena cuma butuh beda antar query, bukan keamanan kriptografis.
static unsigned short dns_gen_id(DnsResolver *res) {
    unsigned int t = nexos_ticks();
    return (unsigned short)((t ^ (unsigned int)(unsigned long)res) & 0xFFFF);
}

// ─── BUILD DNS QUERY ─────────────────────────────────────
// Encode hostname jadi label format DNS: "example.com" ->
//   [7]example[3]com[0]
// Return panjang label-encoded (termasuk byte 0 terminator), -1 kalau gagal.
static int dns_encode_name(const char *hostname, unsigned char *out, unsigned int outmax) {
    unsigned int hlen = dns_strlen(hostname);
    if (hlen == 0 || hlen > 253) return -1;
    if (hlen + 2 > outmax) return -1; // +2 = leading length byte pertama + terminator

    unsigned int out_i = 0;
    unsigned int label_start = 0;

    for (unsigned int i = 0; i <= hlen; i++) {
        if (hostname[i] == '.' || hostname[i] == 0) {
            unsigned int label_len = i - label_start;
            if (label_len == 0 || label_len > 63) return -1; // label kosong/kepanjangan invalid
            out[out_i++] = (unsigned char)label_len;
            for (unsigned int k = 0; k < label_len; k++)
                out[out_i++] = (unsigned char)hostname[label_start + k];
            label_start = i + 1;
        }
    }
    out[out_i++] = 0; // root terminator
    return (int)out_i;
}

// ─── SEND DNS QUERY ──────────────────────────────────────
static int dns_send_query(DnsResolver *res) {
    if (!res->dev || !res->dev->ready) return -1;

    // Resolve MAC DNS server dulu (biasanya = gateway, satu subnet kita)
    unsigned char next_hop[4];
    vnet_route_lookup(res->dev, res->dns_server, next_hop);
    ArpEntry *ae = arp_resolve(res->dev, next_hop);
    if (!ae || ae->state != ARP_RESOLVED) {
        // Masih nunggu ARP resolve next_hop — biarin dns_poll yang retry nanti
        return -1;
    }

    // ── Build DNS payload (header + question) ──
    unsigned char qbuf[256];
    DnsHdr *qh = (DnsHdr*)qbuf;
    dns_memset(qh, 0, sizeof(DnsHdr));
    qh->id      = htons16(res->query_id);
    qh->flags   = htons16(DNS_FLAG_RD); // standard query, recursion desired
    qh->qdcount = htons16(1);

    unsigned char *qname = qbuf + sizeof(DnsHdr);
    int namelen = dns_encode_name(res->hostname, qname, sizeof(qbuf) - sizeof(DnsHdr) - 4);
    if (namelen < 0) return -1;

    unsigned char *qtail = qname + namelen;
    qtail[0] = 0; qtail[1] = DNS_TYPE_A;   // QTYPE = A (16-bit big-endian, MSB=0)
    qtail[2] = 0; qtail[3] = DNS_CLASS_IN; // QCLASS = IN

    unsigned int dns_payload_len = sizeof(DnsHdr) + (unsigned int)namelen + 4;

    // ── Build full frame: Eth + IP + UDP + DNS ──
    unsigned int total = sizeof(EthHdr) + sizeof(IpHdr) + sizeof(UdpHdr) + dns_payload_len;
    if (total > ETH_FRAME_MAX) return -1;

    unsigned char frame[ETH_FRAME_MAX];
    dns_memset(frame, 0, total);

    EthHdr *eth = (EthHdr*)frame;
    dns_memcpy(eth->dst, ae->mac, ETH_ALEN);
    dns_memcpy(eth->src, res->dev->mac, ETH_ALEN);
    eth->ethertype = htons16(ETH_TYPE_IP);

    IpHdr *ip = (IpHdr*)(frame + sizeof(EthHdr));
    ip->ihl_ver   = 0x45;
    ip->tos       = 0;
    ip->total_len = htons16((unsigned short)(sizeof(IpHdr) + sizeof(UdpHdr) + dns_payload_len));
    ip->id        = htons16((unsigned short)res->query_id);
    ip->frag_off  = 0;
    ip->ttl       = 64;
    ip->proto     = IP_PROTO_UDP;
    ip->checksum  = 0;
    dns_memcpy(ip->src, res->dev->ip,    4);
    dns_memcpy(ip->dst, res->dns_server, 4);
    ip->checksum  = ip_checksum(ip, sizeof(IpHdr));

    UdpHdr *udp = (UdpHdr*)(frame + sizeof(EthHdr) + sizeof(IpHdr));
    udp->src_port = htons16(res->local_port);
    udp->dst_port = htons16(DNS_SERVER_PORT);
    udp->length   = htons16((unsigned short)(sizeof(UdpHdr) + dns_payload_len));
    udp->checksum = 0; // optional di IPv4, 0 = tidak dipakai (valid sesuai RFC 768)

    dns_memcpy((unsigned char*)udp + sizeof(UdpHdr), qbuf, dns_payload_len);

    uart_print("[dns] Query ");
    uart_print(res->hostname);
    uart_print(" -> ");
    vnet_print_ip(res->dns_server);
    uart_print(" (id=");
    print_dec(res->query_id);
    uart_println(")");

    return vnet_send(res->dev, frame, total);
}

// ─── PUBLIC API ──────────────────────────────────────────

void dns_init(DnsResolver *res, VirtioNet *dev, const unsigned char dns_server[4]) {
    dns_memset(res, 0, sizeof(DnsResolver));
    res->dev   = dev;
    res->state = DNS_IDLE;
    dns_memcpy(res->dns_server, dns_server, 4);
}

int dns_resolve_start(DnsResolver *res, const char *hostname) {
    if (!res->dev || !res->dev->ready) {
        uart_println("[dns] ERR: network belum ready.");
        return -1;
    }
    unsigned int hlen = dns_strlen(hostname);
    if (hlen == 0 || hlen >= sizeof(res->hostname)) {
        uart_println("[dns] ERR: hostname invalid/kepanjangan.");
        return -1;
    }

    dns_memcpy(res->hostname, hostname, hlen + 1);
    res->query_id      = dns_gen_id(res);
    res->local_port    = 33000 + (res->query_id % 1000); // ephemeral port kasar
    res->retry_count   = 0;
    res->state         = DNS_WAITING;
    res->last_send_tick = nexos_ticks();

    if (dns_send_query(res) < 0) {
        // Gagal kirim (misal ARP masih pending) — tetap WAITING,
        // dns_poll() akan coba retry.
        uart_println("[dns] Query pertama gagal kirim, akan diretry...");
    }
    return 0;
}

DnsState dns_poll(DnsResolver *res) {
    if (res->state != DNS_WAITING) return res->state;

    if (nexos_elapsed(res->last_send_tick, DNS_TIMEOUT_TICKS)) {
        res->retry_count++;
        if (res->retry_count > DNS_MAX_RETRY) {
            uart_print("[dns] FAILED: timeout setelah ");
            print_dec(DNS_MAX_RETRY);
            uart_println(" retry.");
            res->state = DNS_FAILED;
            return res->state;
        }

        uart_print("[dns] Timeout, retry #");
        print_dec(res->retry_count);
        uart_println("...");

        res->last_send_tick = nexos_ticks();
        dns_send_query(res); // kalau gagal kirim lagi, tetap WAITING, dicoba lagi tick berikutnya
    }

    return res->state;
}

void dns_input(DnsResolver *res, const unsigned char *payload, unsigned int plen) {
    if (res->state != DNS_WAITING) return;
    if (plen < sizeof(DnsHdr)) return;

    const DnsHdr *rh = (const DnsHdr*)payload;
    unsigned short id    = ntohs16(rh->id);
    unsigned short flags = ntohs16(rh->flags);

    if (id != res->query_id) return; // bukan response buat query kita

    if (!(flags & DNS_FLAG_QR)) return; // bukan response (harusnya gak mungkin sih)

    unsigned int rcode = flags & DNS_RCODE_MASK;
    if (rcode != 0) {
        uart_print("[dns] FAILED: server return RCODE=");
        print_dec(rcode);
        uart_println(rcode == 3 ? " (NXDOMAIN)" : "");
        res->state = DNS_FAILED;
        return;
    }

    unsigned short qdcount = ntohs16(rh->qdcount);
    unsigned short ancount = ntohs16(rh->ancount);
    if (ancount == 0) {
        uart_println("[dns] FAILED: tidak ada answer record.");
        res->state = DNS_FAILED;
        return;
    }

    // ── Skip question section ──
    // Format tiap question: [label-encoded name][QTYPE 2B][QCLASS 2B]
    const unsigned char *p   = payload + sizeof(DnsHdr);
    const unsigned char *end = payload + plen;

    for (int q = 0; q < qdcount; q++) {
        while (p < end && *p != 0) {
            if (*p & 0xC0) { p += 2; break; } // pointer compression (gak wajar di question, tapi jaga2)
            p += (*p) + 1;
        }
        if (p < end && *p == 0) p++; // skip terminator byte
        p += 4; // skip QTYPE + QCLASS
    }

    // ── Parse answer records, cari yang TYPE=A ──
    for (int a = 0; a < ancount; a++) {
        if (p >= end) break;

        // NAME: bisa label biasa atau pointer compression (0xC0 prefix)
        if ((*p & 0xC0) == 0xC0) {
            p += 2;
        } else {
            while (p < end && *p != 0) {
                if (*p & 0xC0) { p += 2; goto name_done; }
                p += (*p) + 1;
            }
            if (p < end) p++; // skip terminator
        }
        name_done:;

        if (p + 10 > end) break; // TYPE(2) CLASS(2) TTL(4) RDLENGTH(2)

        unsigned short rtype  = ntohs16(*(const unsigned short*)p); p += 2;
        /* class */                                                  p += 2;
        /* ttl   */                                                  p += 4;
        unsigned short rdlen  = ntohs16(*(const unsigned short*)p);  p += 2;

        if (p + rdlen > end) break;

        if (rtype == DNS_TYPE_A && rdlen == 4) {
            dns_memcpy(res->resolved_ip, p, 4);
            res->state = DNS_RESOLVED;

            uart_print("[dns] Resolved ");
            uart_print(res->hostname);
            uart_print(" -> ");
            vnet_print_ip(res->resolved_ip);
            uart_println("");
            return;
        }

        p += rdlen; // bukan A record (misal CNAME) — skip, lanjut ke answer berikutnya
    }

    if (res->state != DNS_RESOLVED) {
        uart_println("[dns] FAILED: tidak ada A record di response.");
        res->state = DNS_FAILED;
    }
}

void dns_print_state(DnsResolver *res) {
    uart_print("[dns] state=");
    switch (res->state) {
        case DNS_IDLE:     uart_print("IDLE"); break;
        case DNS_WAITING:  uart_print("WAITING"); break;
        case DNS_RESOLVED: uart_print("RESOLVED"); break;
        case DNS_FAILED:   uart_print("FAILED"); break;
    }
    uart_print(" host=");
    uart_print(res->hostname);
    if (res->state == DNS_RESOLVED) {
        uart_print(" ip=");
        vnet_print_ip(res->resolved_ip);
    }
    uart_println("");
}
