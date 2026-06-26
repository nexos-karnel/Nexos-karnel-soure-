// dns.h — NexOS minimal DNS resolver
// Scope: query A record doang (IPv4), satu pertanyaan per request,
// UDP/53, no caching antar sesi (cache cuma hidup selama nunggu resolve).
// Cukup buat keperluan "wget <hostname>".

#ifndef DNS_H
#define DNS_H

#include "virtio_net.h"

// ─── DNS HEADER (RFC 1035) ───────────────────────────────
typedef struct {
    unsigned short  id;        // dipakai match request<->response
    unsigned short  flags;     // QR, opcode, AA, TC, RD, RA, Z, RCODE
    unsigned short  qdcount;   // jumlah pertanyaan
    unsigned short  ancount;   // jumlah jawaban
    unsigned short  nscount;   // jumlah authority record
    unsigned short  arcount;   // jumlah additional record
} __attribute__((packed)) DnsHdr;

// DNS flags
#define DNS_FLAG_RD     (1 << 8)   // Recursion Desired (kita selalu minta ini)
#define DNS_FLAG_QR     (1 << 15) // 1 = response, 0 = query
#define DNS_RCODE_MASK  0x000F

#define DNS_TYPE_A      1   // A record (IPv4 address)
#define DNS_CLASS_IN    1   // Internet class

#define DNS_SERVER_PORT 53

// ─── RESOLVER STATE ──────────────────────────────────────
typedef enum {
    DNS_IDLE = 0,
    DNS_WAITING,    // query udah dikirim, nunggu response
    DNS_RESOLVED,   // dapat IP
    DNS_FAILED,     // NXDOMAIN / timeout abis retry / error lain
} DnsState;

#define DNS_MAX_RETRY    3
// Timeout per-attempt dalam "tick" CPU cycle kasar (lihat nexos_time.h)
#define DNS_TIMEOUT_TICKS  3000000U

typedef struct {
    DnsState        state;
    unsigned short  query_id;       // ID query yang sedang ditunggu
    unsigned short  local_port;     // source port kita buat query ini
    unsigned char   resolved_ip[4]; // hasil kalau DNS_RESOLVED
    int             retry_count;    // sudah retry berapa kali
    unsigned int    last_send_tick; // kapan terakhir kirim query (buat timeout)
    char            hostname[128];  // hostname yang sedang di-resolve
    VirtioNet       *dev;
    unsigned char   dns_server[4];  // IP DNS server (biasanya = gateway di QEMU usernet)
} DnsResolver;

// ─── FUNGSI PUBLIK ───────────────────────────────────────

// Init resolver, ikat ke device dan DNS server tujuan
void dns_init(DnsResolver *res, VirtioNet *dev, const unsigned char dns_server[4]);

// Mulai resolve hostname (kirim query pertama). Return 0 kalau query
// terkirim, -1 kalau hostname invalid / device belum ready.
int  dns_resolve_start(DnsResolver *res, const char *hostname);

// Dipanggil tiap loop (polling) — cek timeout, kirim retry kalau perlu.
// Return: DNS_WAITING (masih proses), DNS_RESOLVED, atau DNS_FAILED.
DnsState dns_poll(DnsResolver *res);

// Dipanggil dari UDP dispatcher saat ada DNS response masuk
void dns_input(DnsResolver *res, const unsigned char *payload, unsigned int plen);

// Print status resolver ke UART (debug)
void dns_print_state(DnsResolver *res);

// Helper: apakah string ini sudah berupa IP literal "a.b.c.d"?
// (dipakai wget supaya bisa skip DNS kalau user kasih IP langsung)
int  dns_is_ip_literal(const char *s);

// Helper: parse "a.b.c.d" jadi 4 byte. Return 0 kalau sukses.
int  dns_parse_ip_literal(const char *s, unsigned char out[4]);

#endif // DNS_H
