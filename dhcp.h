// dhcp.h — NexOS minimal DHCP client (DORA: Discover→Offer→Request→Ack)
// Scope: satu lease, satu interface, blocking poll loop
// QEMU -netdev user menyediakan DHCP server di 10.0.2.2 → assign 10.0.2.15

#ifndef DHCP_H
#define DHCP_H

#include "virtio_net.h"

// ─── DHCP CONSTANTS ──────────────────────────────────────
#define DHCP_SERVER_PORT    67
#define DHCP_CLIENT_PORT    68
#define DHCP_MAGIC_COOKIE   0x63825363U  // big-endian: 99.130.83.99

// BOOTP op codes
#define DHCP_BOOTREQUEST    1
#define DHCP_BOOTREPLY      2

// DHCP message types (option 53)
#define DHCP_DISCOVER       1
#define DHCP_OFFER          2
#define DHCP_REQUEST        3
#define DHCP_ACK            5
#define DHCP_NAK            6

// DHCP options
#define DHCP_OPT_MSGTYPE    53
#define DHCP_OPT_SERVERID   54
#define DHCP_OPT_REQIP      50
#define DHCP_OPT_SUBNET     1
#define DHCP_OPT_ROUTER     3
#define DHCP_OPT_DNS        6
#define DHCP_OPT_LEASE      51
#define DHCP_OPT_END        255
#define DHCP_OPT_PAD        0

// ─── DHCP PACKET ─────────────────────────────────────────
// Fixed-length BOOTP header (236 bytes) + options (variable, kita alokasi 312)
#define DHCP_OPTIONS_LEN    312
#define DHCP_PKT_LEN        (236 + DHCP_OPTIONS_LEN)

typedef struct {
    unsigned char   op;             // BOOTREQUEST / BOOTREPLY
    unsigned char   htype;          // 1 = Ethernet
    unsigned char   hlen;           // 6 = MAC len
    unsigned char   hops;           // 0
    unsigned int    xid;            // transaction ID (random)
    unsigned short  secs;           // seconds elapsed
    unsigned short  flags;          // 0x8000 = broadcast
    unsigned char   ciaddr[4];      // client IP (0.0.0.0 kalau belum punya)
    unsigned char   yiaddr[4];      // "your" IP — diisi server (OFFER/ACK)
    unsigned char   siaddr[4];      // server IP
    unsigned char   giaddr[4];      // relay agent (0)
    unsigned char   chaddr[16];     // client MAC (6 bytes + 10 bytes padding)
    unsigned char   sname[64];      // server hostname (kosong)
    unsigned char   file[128];      // boot filename (kosong)
    unsigned int    magic;          // DHCP magic cookie
    unsigned char   options[DHCP_OPTIONS_LEN];
} __attribute__((packed)) DhcpPkt;

// ─── DHCP STATE MACHINE ──────────────────────────────────
typedef enum {
    DHCP_STATE_IDLE = 0,
    DHCP_STATE_DISCOVER,    // sudah kirim DISCOVER, tunggu OFFER
    DHCP_STATE_REQUEST,     // sudah kirim REQUEST, tunggu ACK
    DHCP_STATE_BOUND,       // punya lease aktif
    DHCP_STATE_FAILED,      // timeout / NAK
} DhcpState;

// ─── DHCP CLIENT CONTEXT ─────────────────────────────────
typedef struct {
    DhcpState       state;
    VirtioNet       *dev;

    unsigned int    xid;            // transaction ID sesi ini

    // Hasil dari OFFER / ACK
    unsigned char   offered_ip[4];
    unsigned char   server_ip[4];
    unsigned char   subnet[4];
    unsigned char   router[4];
    unsigned char   dns[4];
    unsigned int    lease_secs;

    // Retransmit
    unsigned int    last_send_tick;
    int             retries;
} DhcpClient;

// ─── PUBLIC API ──────────────────────────────────────────

// Init DHCP client (ikat ke VirtioNet device)
void dhcp_init(DhcpClient *c, VirtioNet *dev);

// Mulai DORA — kirim DISCOVER
// Return 0 kalau paket terkirim, -1 kalau device belum ready
int  dhcp_start(DhcpClient *c);

// Poll: cek state, handle timeout/retransmit
// Dipanggil tiap loop saat nunggu response
// Return state saat ini (BOUND = sukses, FAILED = gagal)
DhcpState dhcp_poll(DhcpClient *c);

// Input handler — dipanggil dari vnet_handle_frame untuk UDP port 68
void dhcp_input(DhcpClient *c, const unsigned char *payload, unsigned int len);

// Apply lease ke VirtioNet device (set IP/GW/netmask) + DNS ke g_dns
// Dipanggil otomatis setelah ACK diterima
void dhcp_apply(DhcpClient *c);

// Print lease info ke UART
void dhcp_print(DhcpClient *c);

#endif // DHCP_H
