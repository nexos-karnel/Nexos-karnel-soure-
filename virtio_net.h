// virtio_net.h — NexOS virtio-net driver (MMIO, ARM virt)
// Spec: virtio 1.0 legacy (yang QEMU pakai by default)
//
// QEMU flags yang diperlukan:
//   -netdev user,id=net0
//   -device virtio-net-device,netdev=net0
//
// virtio-net-device (bukan virtio-net-pci) = MMIO, cocok untuk ARM baremetal

#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

// ─── VIRTIO MMIO BASE ────────────────────────────────────
// QEMU ARM virt: virtio MMIO devices dimulai dari 0x0a000000
// Setiap device punya window 0x200 bytes
// Device 0 = 0x0a000000, device 1 = 0x0a000200, dst
// Cek: qemu-system-arm -machine virt -machine dumpdtb=virt.dtb
//      && dtc -I dtb -O dts virt.dtb | grep virtio

#define VIRTIO_MMIO_BASE        0x0a000000U
#define VIRTIO_MMIO_SIZE        0x200

// ─── VIRTIO MMIO REGISTERS (offset dari base) ────────────
#define VIRTIO_MMIO_MAGIC           0x000  // R   should be 0x74726976 ("virt")
#define VIRTIO_MMIO_VERSION         0x004  // R   1 = legacy, 2 = modern
#define VIRTIO_MMIO_DEVICE_ID       0x008  // R   1 = net, 2 = blk, dst
#define VIRTIO_MMIO_VENDOR_ID       0x00C  // R
#define VIRTIO_MMIO_HOST_FEATURES   0x010  // R   features yg device support
#define VIRTIO_MMIO_HOST_FEATURES_SEL 0x014 // W  pilih feature page (0 atau 1)
#define VIRTIO_MMIO_GUEST_FEATURES  0x020  // W   features yg driver mau
#define VIRTIO_MMIO_GUEST_FEATURES_SEL 0x024 // W
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028  // W   page size (legacy: harus 4096)
#define VIRTIO_MMIO_QUEUE_SEL       0x030  // W   pilih virtqueue mana
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034  // R   max queue size
#define VIRTIO_MMIO_QUEUE_NUM       0x038  // W   actual queue size yg kita mau
#define VIRTIO_MMIO_QUEUE_ALIGN     0x03C  // W   alignment (legacy: 4096)
#define VIRTIO_MMIO_QUEUE_PFN       0x040  // RW  physical page number queue
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050  // W   notify device ada descriptor baru
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060 // R   bit 0 = used ring updated
#define VIRTIO_MMIO_INTERRUPT_ACK   0x064  // W   clear interrupt
#define VIRTIO_MMIO_STATUS          0x070  // RW  device status

// Device Status bits
#define VIRTIO_STATUS_ACKNOWLEDGE   (1 << 0)
#define VIRTIO_STATUS_DRIVER        (1 << 1)
#define VIRTIO_STATUS_DRIVER_OK     (1 << 2)
#define VIRTIO_STATUS_FEATURES_OK   (1 << 3)
#define VIRTIO_STATUS_FAILED        (1 << 7)

// Device ID
#define VIRTIO_DEVICE_NET           1

// Net feature bits
#define VIRTIO_NET_F_MAC            (1 << 5)   // device punya MAC address

// ─── VIRTQUEUE LAYOUT (legacy) ───────────────────────────
// Virtqueue = ring buffer shared antara driver dan device
// Layout di memory (kontigu, page-aligned):
//
//   [Descriptor Table]  — 16 bytes per descriptor × QUEUE_SIZE
//   [Available Ring]    — 6 bytes header + 2 bytes×QUEUE_SIZE + 2 bytes footer
//   [padding ke page]
//   [Used Ring]         — 8 bytes header + 8 bytes×QUEUE_SIZE + 2 bytes footer

#define VQUEUE_SIZE     64      // harus power of 2, max = QUEUE_NUM_MAX

// Descriptor flags
#define VRING_DESC_F_NEXT       (1 << 0)   // ada next descriptor
#define VRING_DESC_F_WRITE      (1 << 1)   // device boleh write (RX buffer)

// Virtqueue descriptor
typedef struct {
    unsigned long long  addr;   // physical address buffer
    unsigned int        len;    // panjang buffer
    unsigned short      flags;  // VRING_DESC_F_*
    unsigned short      next;   // index descriptor berikutnya (kalau F_NEXT)
} __attribute__((packed)) VRingDesc;

// Available ring (driver → device: "ini descriptor yang siap")
typedef struct {
    unsigned short  flags;              // 0 = normal
    unsigned short  idx;                // index berikutnya yang akan ditulis
    unsigned short  ring[VQUEUE_SIZE];  // array descriptor index
    unsigned short  used_event;         // optional
} __attribute__((packed)) VRingAvail;

// Used ring element (device → driver: "ini descriptor yang sudah selesai")
typedef struct {
    unsigned int    id;     // descriptor index
    unsigned int    len;    // berapa bytes yang ditulis
} __attribute__((packed)) VRingUsedElem;

// Used ring (device → driver)
typedef struct {
    unsigned short      flags;
    unsigned short      idx;
    VRingUsedElem       ring[VQUEUE_SIZE];
    unsigned short      avail_event;
} __attribute__((packed)) VRingUsed;

// Satu virtqueue lengkap
typedef struct {
    VRingDesc       desc[VQUEUE_SIZE];
    VRingAvail      avail;
    // padding supaya used ring mulai di page boundary
    unsigned char   _pad[4096
        - (sizeof(VRingDesc) * VQUEUE_SIZE)
        - sizeof(VRingAvail)];
    VRingUsed       used;
} __attribute__((packed, aligned(4096))) VQueue;

// ─── VIRTIO-NET HEADER ───────────────────────────────────
// Setiap packet (TX maupun RX) harus diawali header ini
typedef struct {
    unsigned char   flags;          // 0 = no checksum needed
    unsigned char   gso_type;       // 0 = VIRTIO_NET_HDR_GSO_NONE
    unsigned short  hdr_len;
    unsigned short  gso_size;
    unsigned short  csum_start;
    unsigned short  csum_offset;
    // NOTE: legacy virtio-net tidak ada num_buffers field di sini
} __attribute__((packed)) VirtioNetHdr;

// ─── ETHERNET ────────────────────────────────────────────
#define ETH_ALEN        6       // MAC address length
#define ETH_MTU         1500
#define ETH_FRAME_MAX   (ETH_MTU + 14)  // MTU + 14 byte Ethernet header

#define ETH_TYPE_ARP    0x0806
#define ETH_TYPE_IP     0x0800

typedef struct {
    unsigned char   dst[ETH_ALEN];
    unsigned char   src[ETH_ALEN];
    unsigned short  ethertype;      // big-endian!
} __attribute__((packed)) EthHdr;

// ─── ARP ─────────────────────────────────────────────────
#define ARP_HW_ETH      1
#define ARP_PROTO_IP    0x0800
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

typedef struct {
    unsigned short  hw_type;        // 1 = Ethernet
    unsigned short  proto_type;     // 0x0800 = IP
    unsigned char   hw_len;         // 6
    unsigned char   proto_len;      // 4
    unsigned short  op;             // ARP_OP_*
    unsigned char   sender_mac[ETH_ALEN];
    unsigned char   sender_ip[4];
    unsigned char   target_mac[ETH_ALEN];
    unsigned char   target_ip[4];
} __attribute__((packed)) ArpPkt;

// ─── IP ──────────────────────────────────────────────────
#define IP_PROTO_ICMP   1
#define IP_PROTO_UDP    17
#define IP_PROTO_TCP    6

// Forward declare TcpHdr supaya vnet_handle_frame bisa dispatch TCP
// Definisi lengkap ada di tcp.h
typedef struct {
    unsigned short  src_port;
    unsigned short  dst_port;
    unsigned int    seq;
    unsigned int    ack;
    unsigned char   data_off;
    unsigned char   flags;
    unsigned short  window;
    unsigned short  checksum;
    unsigned short  urgent;
} __attribute__((packed)) TcpHdr;

typedef struct {
    unsigned char   ihl_ver;        // version=4, ihl=5 → 0x45
    unsigned char   tos;
    unsigned short  total_len;
    unsigned short  id;
    unsigned short  frag_off;
    unsigned char   ttl;
    unsigned char   proto;
    unsigned short  checksum;
    unsigned char   src[4];
    unsigned char   dst[4];
} __attribute__((packed)) IpHdr;

// ─── UDP ─────────────────────────────────────────────────
typedef struct {
    unsigned short  src_port;
    unsigned short  dst_port;
    unsigned short  length;
    unsigned short  checksum;
} __attribute__((packed)) UdpHdr;

// ─── ICMP (buat ping reply) ───────────────────────────────
#define ICMP_ECHO_REQUEST   8
#define ICMP_ECHO_REPLY     0

typedef struct {
    unsigned char   type;
    unsigned char   code;
    unsigned short  checksum;
    unsigned short  id;
    unsigned short  seq;
} __attribute__((packed)) IcmpHdr;

// ─── ARP TABLE ───────────────────────────────────────────
#define ARP_TABLE_SIZE  16
#define ARP_TIMEOUT     30

typedef enum {
    ARP_FREE = 0,
    ARP_PENDING,        // sudah kirim request, belum dapat reply
    ARP_RESOLVED,       // MAC sudah diketahui
} ArpState;

typedef struct {
    unsigned char   ip[4];
    unsigned char   mac[ETH_ALEN];
    ArpState        state;
    unsigned int    age;
} ArpEntry;

typedef struct {
    ArpEntry        entries[ARP_TABLE_SIZE];
    int             count;
} ArpTable;

// ─── DRIVER STATE ────────────────────────────────────────
#define NET_RX_BUFS     16
#define NET_BUF_SIZE    (sizeof(VirtioNetHdr) + ETH_FRAME_MAX)

typedef struct {
    // Apakah driver sudah berhasil init
    int             ready;

    // Base address device ini (bisa scan beberapa slot)
    volatile unsigned int *mmio;

    // MAC address
    unsigned char   mac[ETH_ALEN];

    // IP config (static, set manual)
    unsigned char   ip[4];
    unsigned char   gateway[4];
    unsigned char   netmask[4];

    // Virtqueues: 0 = RX, 1 = TX
    VQueue          *rxq;
    VQueue          *txq;

    // RX buffer pool
    unsigned char   rx_buf[NET_RX_BUFS][NET_BUF_SIZE];
    unsigned short  rx_last_seen;   // used ring index terakhir yang diproses

    // TX buffer (simple: satu buffer, kirim sekali)
    unsigned char   tx_buf[NET_BUF_SIZE];

    // Statistik
    unsigned int    tx_count;
    unsigned int    rx_count;
    unsigned int    rx_dropped;

    // ARP table
    ArpTable        arp;

} VirtioNet;

// ─── FUNGSI PUBLIK ───────────────────────────────────────

// Checksum IP/ICMP standard
unsigned short ip_checksum(const void *data, unsigned int len);

// Init driver — scan MMIO, setup virtqueue, return 0 kalau sukses
// gateway/netmask dipakai untuk routing: kalau target IP beda subnet
// dari ip/netmask, frame dikirim ke MAC gateway, bukan MAC target.
int  vnet_init(VirtioNet *dev, unsigned char ip[4],
               const unsigned char gateway[4], const unsigned char netmask[4]);

// Kirim raw Ethernet frame
// data = pointer ke payload (tanpa virtio header, header diisi otomatis)
// len  = ukuran data (termasuk Ethernet header, maks ETH_FRAME_MAX)
int  vnet_send(VirtioNet *dev, const void *data, unsigned int len);

// Poll RX: proses semua packet yang masuk, panggil callback per packet
// callback(buf, len) dipanggil untuk setiap Ethernet frame yang diterima
void vnet_poll(VirtioNet *dev, void (*callback)(const void *frame, unsigned int len));

// Helper: print MAC address ke UART
void vnet_print_mac(const unsigned char mac[ETH_ALEN]);

// Helper: print IP address ke UART
void vnet_print_ip(const unsigned char ip[4]);

// Kirim ARP request untuk ip tertentu
void vnet_arp_request(VirtioNet *dev, const unsigned char target_ip[4]);

// Reply ke ARP request yang masuk
void vnet_arp_reply(VirtioNet *dev, const ArpPkt *req);

// ARP table: lookup IP → dapat ArpEntry (NULL kalau tidak ada)
ArpEntry* arp_lookup(ArpTable *t, const unsigned char ip[4]);

// ARP table: simpan/update entry dari reply yang masuk
ArpEntry* arp_update(ArpTable *t, const unsigned char ip[4], const unsigned char mac[ETH_ALEN]);

// ARP table: cari atau kirim request, return entry (state bisa PENDING)
ArpEntry* arp_resolve(VirtioNet *dev, const unsigned char ip[4]);

// Routing: tentukan IP mana yang harus di-ARP-resolve untuk mengirim
// ke target_ip. Kalau target_ip satu subnet dengan dev->ip (berdasarkan
// dev->netmask) → return target_ip itu sendiri (resolve langsung).
// Kalau beda subnet → return dev->gateway (kirim lewat gateway/router).
// Hasil ditulis ke out_ip[4].
void vnet_route_lookup(VirtioNet *dev, const unsigned char target_ip[4],
                        unsigned char out_ip[4]);

// Helper: cek apakah dua IP satu subnet, berdasarkan netmask yang diberikan
int ip4_same_subnet(const unsigned char ip_a[4], const unsigned char ip_b[4],
                     const unsigned char netmask[4]);

// ARP table: print semua entry ke UART
void arp_print_table(ArpTable *t);

// Handle satu Ethernet frame (dispatch ke ARP/IP/etc)
void vnet_handle_frame(VirtioNet *dev, const void *frame, unsigned int len);

// Kirim ICMP echo reply (balas ping)
void vnet_icmp_reply(VirtioNet *dev, const IpHdr *iph, const IcmpHdr *icmph,
                     const unsigned char *payload, unsigned int plen);

// tcp_input dipanggil dari vnet_handle_frame untuk TCP segments
// Dideclare di sini supaya virtio_net.c bisa dispatch tanpa circular include
struct TcpConn;
void tcp_input_dispatch(struct TcpConn *conn, const IpHdr *iph, const TcpHdr *tcph,
                        const unsigned char *payload, unsigned int plen);

// dns_input dipanggil dari vnet_handle_frame untuk UDP packet dari port 53
// (DNS response). Dideclare di sini dengan alasan sama seperti TCP di atas.
struct DnsResolver;
void dns_input_dispatch(struct DnsResolver *res, const unsigned char *dns_payload,
                        unsigned int dns_plen);

// dhcp_input_dispatch — dipanggil dari vnet_handle_frame untuk UDP port 68
// (DHCP reply dari server). Dispatch ke DhcpClient global di kernel.
struct DhcpClient;
void dhcp_input_dispatch(VirtioNet *dev, const unsigned char *payload,
                         unsigned int len);

// udp_send — kirim UDP datagram arbitrary
// src_ip boleh 0.0.0.0 (DHCP sebelum lease), dst_ip boleh 255.255.255.255
// Return 0 sukses, -1 gagal
int udp_send(VirtioNet *dev,
             const unsigned char src_ip[4],
             const unsigned char dst_ip[4],
             unsigned short src_port, unsigned short dst_port,
             const void *payload, unsigned int plen);

#endif // VIRTIO_NET_H
