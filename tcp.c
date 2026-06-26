// tcp.c — NexOS minimal TCP implementation
// Retransmit: TX buffer + exponential backoff RTO via nexos_ticks()

#include "tcp.h"

// ─── FORWARD DECLARES dari virtio_net.c ─────────────────
extern void uart_print(const char *s);
extern void uart_println(const char *s);
extern void uart_putc(char c);
extern unsigned short ip_checksum(const void *data, unsigned int len);

// ─── FORWARD DECLARE ────────────────────────────────────
static int ip_eq4(const unsigned char *a, const unsigned char *b);

// ─── HELPERS ─────────────────────────────────────────────

static void tcp_memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
}

static void tcp_memset(void *dst, unsigned char v, unsigned int n) {
    unsigned char *d = dst;
    while (n--) *d++ = v;
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

static unsigned int htonl32(unsigned int x) {
    return ((x>>24)&0xFF) | (((x>>16)&0xFF)<<8)
         | (((x>>8)&0xFF)<<16) | ((x&0xFF)<<24);
}

static unsigned int ntohl32(unsigned int x) { return htonl32(x); }
static unsigned short ntohs16(unsigned short x) { return htons16(x); }

// Pseudo-random ISN — pakai alamat conn sebagai seed sederhana
static unsigned int gen_isn(TcpConn *conn) {
    unsigned int seed = (unsigned int)(unsigned long)conn;
    seed ^= nexos_ticks();
    seed ^= (unsigned int)conn->local_port << 16;
    seed ^= (unsigned int)conn->remote_port;
    seed *= 0x6c62272eU;
    return seed;
}

// ─── TCP CHECKSUM ────────────────────────────────────────
static unsigned short tcp_checksum(
    const unsigned char src_ip[4], const unsigned char dst_ip[4],
    const void *tcphdr, unsigned int tcplen)
{
    unsigned char pseudo[12];
    tcp_memcpy(pseudo,     src_ip, 4);
    tcp_memcpy(pseudo + 4, dst_ip, 4);
    pseudo[8]  = 0;
    pseudo[9]  = 6;  // IP_PROTO_TCP
    pseudo[10] = (unsigned char)(tcplen >> 8);
    pseudo[11] = (unsigned char)(tcplen & 0xFF);

    unsigned int sum = 0;
    const unsigned short *p = (const unsigned short*)pseudo;
    for (int i = 0; i < 6; i++) sum += p[i];

    p = (const unsigned short*)tcphdr;
    unsigned int len = tcplen;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(unsigned char*)p;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)(~sum);
}

// ─── SEND RAW TCP SEGMENT ────────────────────────────────
// seq_override: kalau non-zero, pakai nilai ini sebagai SEQ number
// (dipakai retransmit supaya pakai snd_una bukan snd_nxt).
// Untuk kirim normal, pass 0 (pakai conn->snd_nxt).
//
// PENTING: fungsi ini TIDAK advance snd_nxt — caller yang urus.
// Ini supaya retransmit bisa kirim ulang dengan seq yang sama.
static int tcp_send_raw(TcpConn *conn,
    unsigned char flags,
    unsigned int  seq_val,      // seq number yang dipakai di header
    const void   *payload,
    unsigned int  plen)
{
    if (!conn->dev || !conn->dev->ready) return -1;

    unsigned char next_hop[4];
    vnet_route_lookup(conn->dev, conn->remote_ip, next_hop);

    ArpEntry *ae = arp_resolve(conn->dev, next_hop);
    if (!ae) return -1;
    if (ae->state != ARP_RESOLVED) {
        uart_println("[tcp] ARP pending, tunggu resolve...");
        return -1;
    }

    unsigned int total = sizeof(EthHdr) + sizeof(IpHdr)
                       + sizeof(TcpHdr) + plen;
    if (total > ETH_FRAME_MAX) return -1;

    unsigned char frame[ETH_FRAME_MAX];
    tcp_memset(frame, 0, total);

    EthHdr *eth = (EthHdr*)frame;
    tcp_memcpy(eth->dst, ae->mac,        ETH_ALEN);
    tcp_memcpy(eth->src, conn->dev->mac, ETH_ALEN);
    eth->ethertype = htons16(0x0800);

    IpHdr *ip = (IpHdr*)(frame + sizeof(EthHdr));
    ip->ihl_ver   = 0x45;
    ip->tos       = 0;
    ip->total_len = htons16((unsigned short)(sizeof(IpHdr) + sizeof(TcpHdr) + plen));
    ip->id        = htons16((unsigned short)(seq_val & 0xFFFF));
    ip->frag_off  = 0;
    ip->ttl       = 64;
    ip->proto     = 6;
    ip->checksum  = 0;
    tcp_memcpy(ip->src, conn->dev->ip,   4);
    tcp_memcpy(ip->dst, conn->remote_ip, 4);
    ip->checksum  = ip_checksum(ip, sizeof(IpHdr));

    TcpHdr *tcp = (TcpHdr*)(frame + sizeof(EthHdr) + sizeof(IpHdr));
    tcp->src_port  = htons16(conn->local_port);
    tcp->dst_port  = htons16(conn->remote_port);
    tcp->seq       = htonl32(seq_val);
    tcp->ack       = htonl32(conn->ack);
    tcp->data_off  = (sizeof(TcpHdr) / 4) << 4;
    tcp->flags     = flags;
    tcp->window    = htons16(TCP_RX_BUF_SIZE);
    tcp->checksum  = 0;
    tcp->urgent    = 0;

    if (plen > 0)
        tcp_memcpy((unsigned char*)tcp + sizeof(TcpHdr), payload, plen);

    tcp->checksum = tcp_checksum(
        conn->dev->ip, conn->remote_ip,
        tcp, sizeof(TcpHdr) + plen);

    return vnet_send(conn->dev, frame, total);
}

// ─── HELPERS: arm/disarm retransmit timer ────────────────

static void rto_arm(TcpConn *conn) {
    conn->last_send_tick = nexos_ticks();
    conn->rto_armed      = 1;
}

static void rto_disarm(TcpConn *conn) {
    conn->rto_armed         = 0;
    conn->retransmit_count  = 0;
    conn->rto_ticks         = TCP_RTO_INIT;
}

// ─── PUBLIC API ──────────────────────────────────────────

void tcp_init(TcpConn *conn, VirtioNet *dev, unsigned short local_port) {
    tcp_memset(conn, 0, sizeof(TcpConn));
    conn->dev        = dev;
    conn->local_port = local_port;
    conn->state      = TCP_CLOSED;
    conn->rto_ticks  = TCP_RTO_INIT;
}

int tcp_connect(TcpConn *conn, const unsigned char ip[4], unsigned short port) {
    if (conn->state != TCP_CLOSED) {
        uart_println("[tcp] ERR: koneksi sudah aktif.");
        return -1;
    }

    tcp_memcpy(conn->remote_ip, ip, 4);
    conn->remote_port       = port;
    conn->isn               = gen_isn(conn);
    conn->snd_una           = conn->isn;
    conn->snd_nxt           = conn->isn;
    conn->ack               = 0;
    conn->rx_head           = 0;
    conn->rx_tail           = 0;
    conn->rx_len            = 0;
    conn->tx_len            = 0;
    conn->remote_fin        = 0;
    conn->rto_ticks         = TCP_RTO_INIT;
    conn->retransmit_count  = 0;
    conn->rto_armed         = 0;

    uart_print("[tcp] Connecting to ");
    for (int i = 0; i < 4; i++) {
        print_dec(ip[i]);
        if (i < 3) uart_putc('.');
    }
    uart_putc(':');
    print_dec(port);
    uart_print("\r\n");

    // Kirim SYN — seq = ISN, SYN consume 1 seq
    int r = tcp_send_raw(conn, TCP_SYN, conn->snd_nxt, (void*)0, 0);
    if (r < 0) {
        uart_println("[tcp] ERR: gagal kirim SYN (ARP belum resolved?)");
        return -1;
    }
    conn->snd_nxt++;        // SYN consume 1 seq
    // Arm retransmit untuk SYN
    rto_arm(conn);
    conn->state = TCP_SYN_SENT;
    uart_println("[tcp] SYN terkirim, menunggu SYN-ACK...");
    return 0;
}

int tcp_send(TcpConn *conn, const void *data, unsigned int len) {
    if (conn->state != TCP_ESTABLISHED) {
        uart_println("[tcp] ERR: belum ESTABLISHED.");
        return -1;
    }
    if (len == 0) return 0;

    // Simpan ke TX buffer (untuk retransmit)
    if (len > TCP_TX_BUF_SIZE) len = TCP_TX_BUF_SIZE;  // clamp
    tcp_memcpy(conn->tx_buf, data, len);
    conn->tx_len = len;

    // Kirim dalam chunks sesuai MSS
    unsigned int max_seg = ETH_MTU - sizeof(IpHdr) - sizeof(TcpHdr);
    unsigned int sent = 0;

    while (sent < len) {
        unsigned int chunk = len - sent;
        if (chunk > max_seg) chunk = max_seg;

        int r = tcp_send_raw(conn, TCP_ACK | TCP_PSH,
                             conn->snd_nxt,
                             (const unsigned char*)data + sent, chunk);
        if (r < 0) break;
        conn->snd_nxt += chunk;
        sent          += chunk;
    }

    if (sent > 0) rto_arm(conn);   // arm timer untuk data ini
    return (int)sent;
}

void tcp_close(TcpConn *conn) {
    if (conn->state == TCP_CLOSED) return;
    tcp_send_raw(conn, TCP_FIN | TCP_ACK, conn->snd_nxt, (void*)0, 0);
    conn->snd_nxt++;
    conn->state = TCP_FIN_WAIT;
    uart_println("[tcp] FIN terkirim.");
}

// ─── RETRANSMIT POLL ─────────────────────────────────────
// Dipanggil tiap loop iterasi saat polling RX.
// Cek apakah ada unACKed data yang sudah melewati RTO.
// Return 0 = ok, -1 = max retry tercapai (koneksi di-close).
int tcp_poll(TcpConn *conn) {
    // Gak perlu poll kalau gak ada unACKed data atau timer belum di-arm
    if (!conn->rto_armed) return 0;
    if (conn->state == TCP_CLOSED) return 0;

    // Belum timeout
    if (!nexos_elapsed(conn->last_send_tick, conn->rto_ticks)) return 0;

    // Timeout! Cek apakah sudah max retry
    conn->retransmit_count++;
    if (conn->retransmit_count > TCP_RETRANSMIT_MAX) {
        uart_print("[tcp] RETRANSMIT FAILED: max retry (");
        print_dec(TCP_RETRANSMIT_MAX);
        uart_println(") tercapai. Koneksi direset.");
        // Kirim RST ke remote (best-effort, gak retry RST)
        tcp_send_raw(conn, TCP_RST | TCP_ACK, conn->snd_nxt, (void*)0, 0);
        conn->state     = TCP_CLOSED;
        conn->rto_armed = 0;
        return -1;
    }

    uart_print("[tcp] RTO timeout, retransmit #");
    print_dec(conn->retransmit_count);
    uart_print(" (RTO=");
    print_dec(conn->rto_ticks);
    uart_println(" ticks)");

    // Exponential backoff: double RTO, cap di TCP_RTO_MAX
    conn->rto_ticks *= 2;
    if (conn->rto_ticks > TCP_RTO_MAX || conn->rto_ticks == 0)
        conn->rto_ticks = TCP_RTO_MAX;

    // ── Pilih apa yang di-retransmit ──
    if (conn->state == TCP_SYN_SENT) {
        // Retransmit SYN: seq = ISN (snd_una), snd_nxt sudah advance ke ISN+1
        uart_println("[tcp] Retransmit SYN...");
        tcp_send_raw(conn, TCP_SYN, conn->snd_una, (void*)0, 0);

    } else if (conn->state == TCP_ESTABLISHED && conn->tx_len > 0) {
        // Retransmit data dari snd_una
        uart_print("[tcp] Retransmit data: ");
        print_dec(conn->tx_len);
        uart_println(" bytes dari snd_una");

        unsigned int max_seg = ETH_MTU - sizeof(IpHdr) - sizeof(TcpHdr);
        unsigned int sent = 0;
        unsigned int seq  = conn->snd_una;

        while (sent < conn->tx_len) {
            unsigned int chunk = conn->tx_len - sent;
            if (chunk > max_seg) chunk = max_seg;
            tcp_send_raw(conn, TCP_ACK | TCP_PSH,
                         seq, conn->tx_buf + sent, chunk);
            seq  += chunk;
            sent += chunk;
        }

    } else if (conn->state == TCP_FIN_WAIT) {
        // Retransmit FIN
        uart_println("[tcp] Retransmit FIN...");
        tcp_send_raw(conn, TCP_FIN | TCP_ACK, conn->snd_una, (void*)0, 0);
    }

    // Reset timer untuk attempt berikutnya
    conn->last_send_tick = nexos_ticks();
    return 0;
}

unsigned int tcp_rx_available(TcpConn *conn) {
    return conn->rx_len;
}

unsigned int tcp_read(TcpConn *conn, void *buf, unsigned int maxlen) {
    if (conn->rx_len == 0) return 0;
    unsigned int n = conn->rx_len < maxlen ? conn->rx_len : maxlen;
    unsigned char *out = (unsigned char*)buf;
    for (unsigned int i = 0; i < n; i++) {
        out[i] = conn->rx_buf[conn->rx_tail];
        conn->rx_tail = (conn->rx_tail + 1) % TCP_RX_BUF_SIZE;
    }
    conn->rx_len -= n;
    return n;
}

// ─── TCP INPUT (state machine) ───────────────────────────

void tcp_input(TcpConn *conn, const IpHdr *iph, const TcpHdr *tcph,
               const unsigned char *payload, unsigned int plen)
{
    if (ntohs16(tcph->dst_port) != conn->local_port)  return;
    if (ntohs16(tcph->src_port) != conn->remote_port) return;
    if (!ip_eq4(iph->src, conn->remote_ip))           return;

    unsigned char flags   = tcph->flags;
    unsigned int  seg_seq = ntohl32(tcph->seq);
    unsigned int  seg_ack = ntohl32(tcph->ack);

    switch (conn->state) {

    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            // Verifikasi ACK = ISN + 1 (ACK untuk SYN kita)
            if (seg_ack != conn->isn + 1) {
                uart_println("[tcp] ERR: ACK number tidak valid di SYN-ACK");
                return;
            }
            // SYN-ACK diterima — update state
            conn->snd_una = seg_ack;    // SYN kita sudah di-ACK
            conn->ack     = seg_seq + 1;
            conn->state   = TCP_ESTABLISHED;
            rto_disarm(conn);           // SYN sudah di-ACK, matiin timer
            uart_println("[tcp] ESTABLISHED!");
            // Kirim ACK untuk SYN-ACK
            tcp_send_raw(conn, TCP_ACK, conn->snd_nxt, (void*)0, 0);

        } else if (flags & TCP_RST) {
            uart_println("[tcp] Connection refused (RST).");
            conn->state = TCP_CLOSED;
            rto_disarm(conn);
        }
        break;

    case TCP_ESTABLISHED:
        if (flags & TCP_RST) {
            uart_println("[tcp] Connection reset by peer.");
            conn->state = TCP_CLOSED;
            rto_disarm(conn);
            return;
        }

        // ── Proses ACK: geser snd_una ──
        if (flags & TCP_ACK) {
            // seg_ack harus antara snd_una dan snd_nxt (valid ACK window)
            // Cek: snd_una < seg_ack <= snd_nxt (unsigned arithmetic)
            if ((seg_ack - conn->snd_una - 1) < (conn->snd_nxt - conn->snd_una)) {
                unsigned int newly_acked = seg_ack - conn->snd_una;

                // Geser TX buffer: buang bytes yang sudah di-ACK
                if (newly_acked >= conn->tx_len) {
                    // Semua data yang dikirim sudah di-ACK
                    conn->tx_len  = 0;
                    conn->snd_una = seg_ack;
                    rto_disarm(conn);   // gak ada unACKed data lagi
                } else {
                    // Sebagian di-ACK — geser tx_buf ke depan
                    unsigned int remaining = conn->tx_len - newly_acked;
                    tcp_memcpy(conn->tx_buf, conn->tx_buf + newly_acked, remaining);
                    conn->tx_len  = remaining;
                    conn->snd_una = seg_ack;
                    // Timer tetap jalan untuk sisa data yang belum di-ACK
                    rto_arm(conn);
                }

                conn->remote_win = ntohs16(tcph->window);
            }
            // ACK duplikat atau di luar window: abaikan
        }

        // ── FIN dari remote ──
        if (flags & TCP_FIN) {
            conn->ack = seg_seq + 1;
            conn->remote_fin = 1;
            tcp_send_raw(conn, TCP_ACK, conn->snd_nxt, (void*)0, 0);
            conn->state = TCP_CLOSE_WAIT;
            uart_println("[tcp] Remote closed connection (FIN received).");
            // Balas FIN (passive close)
            tcp_send_raw(conn, TCP_FIN | TCP_ACK, conn->snd_nxt, (void*)0, 0);
            conn->snd_nxt++;
            conn->state = TCP_TIME_WAIT;
            return;
        }

        // ── Data segment ──
        if (plen > 0) {
            if (seg_seq == conn->ack) {
                unsigned int space = TCP_RX_BUF_SIZE - conn->rx_len;
                unsigned int copy  = plen < space ? plen : space;
                for (unsigned int i = 0; i < copy; i++) {
                    conn->rx_buf[conn->rx_head] = payload[i];
                    conn->rx_head = (conn->rx_head + 1) % TCP_RX_BUF_SIZE;
                }
                conn->rx_len += copy;
                conn->ack    += copy;
                tcp_send_raw(conn, TCP_ACK, conn->snd_nxt, (void*)0, 0);
            }
            // Out-of-order: drop (no reorder buffer)
        }
        break;

    case TCP_FIN_WAIT:
        if (flags & TCP_ACK) {
            // ACK untuk FIN kita — geser snd_una
            if (seg_ack == conn->snd_nxt) {
                conn->snd_una = seg_ack;
                rto_disarm(conn);
            }
        }
        if (flags & TCP_FIN) {
            conn->ack = seg_seq + 1;
            tcp_send_raw(conn, TCP_ACK, conn->snd_nxt, (void*)0, 0);
            conn->state = TCP_TIME_WAIT;
            uart_println("[tcp] Connection closed.");
        }
        break;

    case TCP_TIME_WAIT:
        conn->state = TCP_CLOSED;
        break;

    default:
        break;
    }
}

void tcp_print_state(TcpConn *conn) {
    uart_print("\r\n TCP Connection:\r\n");
    uart_print("  State    : ");
    switch (conn->state) {
        case TCP_CLOSED:      uart_println("CLOSED");      break;
        case TCP_SYN_SENT:    uart_println("SYN_SENT");    break;
        case TCP_ESTABLISHED: uart_println("ESTABLISHED"); break;
        case TCP_FIN_WAIT:    uart_println("FIN_WAIT");    break;
        case TCP_CLOSE_WAIT:  uart_println("CLOSE_WAIT");  break;
        case TCP_TIME_WAIT:   uart_println("TIME_WAIT");   break;
        default:              uart_println("UNKNOWN");      break;
    }
    uart_print("  Remote   : ");
    for (int i = 0; i < 4; i++) {
        print_dec(conn->remote_ip[i]);
        if (i < 3) uart_putc('.');
    }
    uart_putc(':');
    print_dec(conn->remote_port);
    uart_print("\r\n");
    uart_print("  snd_una  : "); print_dec(conn->snd_una); uart_print("\r\n");
    uart_print("  snd_nxt  : "); print_dec(conn->snd_nxt); uart_print("\r\n");
    uart_print("  ack      : "); print_dec(conn->ack);      uart_print("\r\n");
    uart_print("  tx_pend  : "); print_dec(conn->tx_len);   uart_println(" bytes unACKed");
    uart_print("  rx_avail : "); print_dec(conn->rx_len);   uart_println(" bytes");
    uart_print("  retries  : "); print_dec((unsigned int)conn->retransmit_count); uart_print("\r\n");
    uart_print("  rto_ticks: "); print_dec(conn->rto_ticks); uart_print("\r\n\r\n");
}

// Helper: compare 4-byte IP
static int ip_eq4(const unsigned char *a, const unsigned char *b) {
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

// ─────────────────────────────────────────────────────────
// SERVER MODE: TcpListener
// ─────────────────────────────────────────────────────────

int tcp_listen(TcpListener *lst, VirtioNet *dev, unsigned short port) {
    if (!dev || !dev->ready) return -1;
    tcp_memset(lst, 0, sizeof(TcpListener));
    lst->dev        = dev;
    lst->local_port = port;
    lst->active     = 1;
    uart_print("[tcp] Listening on port ");
    print_dec(port);
    uart_println("");
    return 0;
}

// Cari slot backlog kosong
static TcpBacklogEntry *backlog_alloc(TcpListener *lst) {
    if (lst->backlog_count >= TCP_BACKLOG_SIZE) return (void*)0;
    for (int i = 0; i < TCP_BACKLOG_SIZE; i++) {
        if (!lst->backlog[i].used) {
            lst->backlog[i].used = 1;
            lst->backlog_count++;
            return &lst->backlog[i];
        }
    }
    return (void*)0;
}

// Cari backlog entry berdasarkan remote IP+port (untuk match ACK yang masuk)
static TcpBacklogEntry *backlog_find(TcpListener *lst,
                                      const unsigned char remote_ip[4],
                                      unsigned short remote_port)
{
    for (int i = 0; i < TCP_BACKLOG_SIZE; i++) {
        if (!lst->backlog[i].used) continue;
        TcpConn *c = &lst->backlog[i].conn;
        if (c->remote_port == remote_port && ip_eq4(c->remote_ip, remote_ip))
            return &lst->backlog[i];
    }
    return (void*)0;
}

void tcp_listener_input(TcpListener *lst, const IpHdr *iph, const TcpHdr *tcph,
                         const unsigned char *payload, unsigned int plen)
{
    if (!lst->active) return;

    unsigned char  flags    = tcph->flags;
    unsigned short src_port = ntohs16(tcph->src_port);
    unsigned int   seg_seq  = ntohl32(tcph->seq);
    unsigned int   seg_ack  = ntohl32(tcph->ack);

    // ── Incoming SYN → buka slot backlog, kirim SYN-ACK ──
    if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
        // Cek apakah remote ini sudah ada di backlog (retransmit SYN)
        TcpBacklogEntry *existing = backlog_find(lst, iph->src, src_port);
        if (existing) {
            // Retransmit SYN-ACK
            uart_println("[tcp] Retransmit SYN-ACK (duplikat SYN)");
            TcpConn *c = &existing->conn;
            tcp_send_raw(c, TCP_SYN | TCP_ACK, c->snd_una, (void*)0, 0);
            return;
        }

        TcpBacklogEntry *entry = backlog_alloc(lst);
        if (!entry) {
            uart_println("[tcp] Backlog penuh, drop SYN.");
            return;
        }

        TcpConn *c = &entry->conn;
        tcp_memset(c, 0, sizeof(TcpConn));
        c->dev         = lst->dev;
        c->local_port  = lst->local_port;
        c->remote_port = src_port;
        tcp_memcpy(c->remote_ip, iph->src, 4);
        c->rto_ticks   = TCP_RTO_INIT;

        // ISN server: pseudo-random dari ticks + remote info
        c->isn    = nexos_ticks() ^ ((unsigned int)src_port << 16)
                  ^ (unsigned int)iph->src[3];
        c->isn   *= 0x6c62272eU;
        c->snd_una = c->isn;
        c->snd_nxt = c->isn;
        c->ack     = seg_seq + 1;   // remote ISN + 1 (consume SYN)
        c->state   = TCP_SYN_RCVD;

        uart_print("[tcp] SYN dari ");
        for (int i = 0; i < 4; i++) {
            print_dec(iph->src[i]);
            if (i < 3) uart_putc('.');
        }
        uart_putc(':');
        print_dec(src_port);
        uart_println(" — kirim SYN-ACK");

        // Kirim SYN-ACK: seq = ISN, ack = remote ISN + 1
        tcp_send_raw(c, TCP_SYN | TCP_ACK, c->snd_nxt, (void*)0, 0);
        c->snd_nxt++;   // SYN consume 1 seq
        rto_arm(c);
        return;
    }

    // ── Paket lain: cari di backlog berdasarkan remote IP+port ──
    TcpBacklogEntry *entry = backlog_find(lst, iph->src, src_port);
    if (!entry) return;

    TcpConn *c = &entry->conn;

    // RST → buang entry
    if (flags & TCP_RST) {
        uart_println("[tcp] RST diterima, hapus backlog entry.");
        tcp_memset(entry, 0, sizeof(TcpBacklogEntry));
        lst->backlog_count--;
        return;
    }

    // ── SYN_RCVD: tunggu ACK untuk SYN-ACK kita ──
    if (c->state == TCP_SYN_RCVD) {
        if (flags & TCP_ACK) {
            // Verifikasi ACK number = ISN + 1
            if (seg_ack == c->isn + 1) {
                c->snd_una = seg_ack;
                c->state   = TCP_ESTABLISHED;
                rto_disarm(c);
                uart_print("[tcp] ESTABLISHED (server) dari ");
                for (int i = 0; i < 4; i++) {
                    print_dec(c->remote_ip[i]);
                    if (i < 3) uart_putc('.');
                }
                uart_putc(':');
                print_dec(c->remote_port);
                uart_println("");
                // Kalau ada data sekaligus di paket ini, proses juga
                if (plen > 0 && (flags & TCP_PSH)) {
                    unsigned int space = TCP_RX_BUF_SIZE - c->rx_len;
                    unsigned int copy  = plen < space ? plen : space;
                    for (unsigned int i = 0; i < copy; i++) {
                        c->rx_buf[c->rx_head] = payload[i];
                        c->rx_head = (c->rx_head + 1) % TCP_RX_BUF_SIZE;
                    }
                    c->rx_len += copy;
                    c->ack    += copy;
                    tcp_send_raw(c, TCP_ACK, c->snd_nxt, (void*)0, 0);
                }
            }
        }
        return;
    }

    // ── ESTABLISHED: pakai tcp_input biasa ──
    if (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT ||
        c->state == TCP_CLOSE_WAIT  || c->state == TCP_TIME_WAIT) {
        tcp_input(c, iph, tcph, payload, plen);
        // Kalau koneksi tutup, bersihkan slot
        if (c->state == TCP_CLOSED || c->state == TCP_TIME_WAIT) {
            c->state = TCP_CLOSED;
        }
    }
}

int tcp_accept(TcpListener *lst, TcpConn *out_conn) {
    for (int i = 0; i < TCP_BACKLOG_SIZE; i++) {
        TcpBacklogEntry *e = &lst->backlog[i];
        if (!e->used) continue;
        if (e->conn.state == TCP_ESTABLISHED) {
            tcp_memcpy(out_conn, &e->conn, sizeof(TcpConn));
            // Bersihkan slot (koneksi sekarang dipegang caller)
            tcp_memset(e, 0, sizeof(TcpBacklogEntry));
            lst->backlog_count--;
            uart_print("[tcp] accept() → koneksi dari ");
            for (int j = 0; j < 4; j++) {
                print_dec(out_conn->remote_ip[j]);
                if (j < 3) uart_putc('.');
            }
            uart_putc(':');
            print_dec(out_conn->remote_port);
            uart_println("");
            return 1;
        }
    }
    return 0;
}

void tcp_listener_poll(TcpListener *lst) {
    if (!lst->active) return;
    for (int i = 0; i < TCP_BACKLOG_SIZE; i++) {
        TcpBacklogEntry *e = &lst->backlog[i];
        if (!e->used) continue;
        TcpConn *c = &e->conn;

        // Retransmit SYN-ACK kalau SYN_RCVD timeout
        if (c->state == TCP_SYN_RCVD && c->rto_armed) {
            if (nexos_elapsed(c->last_send_tick, c->rto_ticks)) {
                c->retransmit_count++;
                if (c->retransmit_count > TCP_RETRANSMIT_MAX) {
                    uart_println("[tcp] SYN-ACK max retry, drop koneksi.");
                    tcp_memset(e, 0, sizeof(TcpBacklogEntry));
                    lst->backlog_count--;
                    continue;
                }
                c->rto_ticks *= 2;
                if (c->rto_ticks > TCP_RTO_MAX) c->rto_ticks = TCP_RTO_MAX;
                uart_print("[tcp] Retransmit SYN-ACK #");
                print_dec(c->retransmit_count);
                uart_println("");
                tcp_send_raw(c, TCP_SYN | TCP_ACK, c->snd_una, (void*)0, 0);
                c->last_send_tick = nexos_ticks();
            }
        }

        // Poll koneksi ESTABLISHED (retransmit data kalau ada)
        if (c->state == TCP_ESTABLISHED) {
            tcp_poll(c);
        }
    }
}

void tcp_listener_print(TcpListener *lst) {
    uart_print("\r\n[tcp] Listener port ");
    print_dec(lst->local_port);
    uart_print(" — ");
    if (lst->active) uart_println("ACTIVE");
    else             uart_println("INACTIVE");
    uart_print("  Backlog: ");
    print_dec(lst->backlog_count);
    uart_print("/");
    print_dec(TCP_BACKLOG_SIZE);
    uart_println("");
    for (int i = 0; i < TCP_BACKLOG_SIZE; i++) {
        TcpBacklogEntry *e = &lst->backlog[i];
        if (!e->used) continue;
        uart_print("  [");
        print_dec(i);
        uart_print("] ");
        for (int j = 0; j < 4; j++) {
            print_dec(e->conn.remote_ip[j]);
            if (j < 3) uart_putc('.');
        }
        uart_putc(':');
        print_dec(e->conn.remote_port);
        uart_print(" → ");
        switch (e->conn.state) {
            case TCP_SYN_RCVD:    uart_println("SYN_RCVD");    break;
            case TCP_ESTABLISHED: uart_println("ESTABLISHED");  break;
            default:              uart_println("...");           break;
        }
    }
    uart_println("");
}
