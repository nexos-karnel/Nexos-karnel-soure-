// virtio_net.c — NexOS virtio-net driver implementation
// Pasang di kernel_v0_3.c dengan: #include "virtio_net.h"
// (atau compile terpisah dan link bareng)

#include "virtio_net.h"

// ─── UART (reuse dari kernel_v0_3.c) ────────────────────
// Forward declare supaya bisa print tanpa duplikasi kode
extern void uart_print(const char *s);
extern void uart_println(const char *s);
extern void uart_putc(char c);

// ─── MMIO HELPER ─────────────────────────────────────────

static unsigned int mmio_read(volatile unsigned int *base, unsigned int offset) {
    return *(volatile unsigned int*)((unsigned char*)base + offset);
}

static void mmio_write(volatile unsigned int *base, unsigned int offset, unsigned int val) {
    *(volatile unsigned int*)((unsigned char*)base + offset) = val;
}

// ─── BYTE ORDER ──────────────────────────────────────────
// ARM adalah little-endian, network adalah big-endian

static unsigned short htons(unsigned short x) {
    return (unsigned short)((x >> 8) | (x << 8));
}

static unsigned short ntohs(unsigned short x) {
    return htons(x);
}

static unsigned int htonl(unsigned int x) {
    return ((x >> 24) & 0xFF)
         | (((x >> 16) & 0xFF) << 8)
         | (((x >>  8) & 0xFF) << 16)
         | ((x & 0xFF) << 24);
}

// ─── MEMORY HELPERS ──────────────────────────────────────

static void net_memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char*)dst;
    const unsigned char *s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
}

static void net_memset(void *dst, unsigned char val, unsigned int n) {
    unsigned char *d = (unsigned char*)dst;
    while (n--) *d++ = val;
}

// ─── PRINT HELPERS ───────────────────────────────────────

static void print_hex_byte(unsigned char b) {
    const char *hex = "0123456789abcdef";
    uart_putc(hex[b >> 4]);
    uart_putc(hex[b & 0xF]);
}

static void print_dec(unsigned int n) {
    char buf[12];
    int i = 0;
    if (n == 0) { uart_putc('0'); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) uart_putc(buf[i]);
}

void vnet_print_mac(const unsigned char mac[ETH_ALEN]) {
    for (int i = 0; i < ETH_ALEN; i++) {
        if (i) uart_putc(':');
        print_hex_byte(mac[i]);
    }
}

void vnet_print_ip(const unsigned char ip[4]) {
    for (int i = 0; i < 4; i++) {
        if (i) uart_putc('.');
        print_dec(ip[i]);
    }
}

// ─── CHECKSUM ────────────────────────────────────────────
// Standard IP/ICMP checksum (ones complement sum)

unsigned short ip_checksum(const void *data, unsigned int len) {
    const unsigned short *p = (const unsigned short*)data;
    unsigned int sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len) sum += *(unsigned char*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)(~sum);
}

// ─── VIRTQUEUE SETUP ─────────────────────────────────────

// Inisialisasi satu virtqueue
// slot = 0 (RX) atau 1 (TX)
// Butuh VQueue yang sudah dialokasi dan zero-filled
static int vq_setup(volatile unsigned int *mmio, unsigned int slot, VQueue *vq) {
    // Pilih queue
    mmio_write(mmio, VIRTIO_MMIO_QUEUE_SEL, slot);

    // Cek max size
    unsigned int qmax = mmio_read(mmio, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0) {
        uart_println("[vnet] ERR: queue not available");
        return -1;
    }
    if (qmax < VQUEUE_SIZE) {
        uart_println("[vnet] ERR: queue max terlalu kecil");
        return -1;
    }

    // Set ukuran queue
    mmio_write(mmio, VIRTIO_MMIO_QUEUE_NUM,   VQUEUE_SIZE);
    mmio_write(mmio, VIRTIO_MMIO_QUEUE_ALIGN,  4096);

    // Set page frame number (PFN) — legacy virtio pakai PFN bukan address langsung
    // PFN = physical_address / page_size
    unsigned int pfn = (unsigned int)vq / 4096;
    mmio_write(mmio, VIRTIO_MMIO_QUEUE_PFN, pfn);

    return 0;
}

// ─── RX SETUP ────────────────────────────────────────────
// Pre-populate RX queue dengan buffer yang siap diisi device

static void rxq_fill(VirtioNet *dev) {
    VQueue *q = dev->rxq;

    for (int i = 0; i < NET_RX_BUFS; i++) {
        // Setup descriptor: buffer yang bisa ditulis device
        q->desc[i].addr  = (unsigned long long)(unsigned int)dev->rx_buf[i];
        q->desc[i].len   = NET_BUF_SIZE;
        q->desc[i].flags = VRING_DESC_F_WRITE;  // device boleh write
        q->desc[i].next  = 0;

        // Masukkan ke available ring
        q->avail.ring[q->avail.idx % VQUEUE_SIZE] = (unsigned short)i;
        q->avail.idx++;
    }

    // Notify device: ada buffer baru di RX queue
    // Memory barrier sebelum notify
    __asm__ volatile ("dmb" ::: "memory");
    mmio_write(dev->mmio, VIRTIO_MMIO_QUEUE_NOTIFY, 0); // 0 = RX queue
}

// ─── SCAN & INIT ─────────────────────────────────────────

// Alokasi static virtqueue (2 buah: RX + TX)
// Harus 4096-byte aligned
static VQueue vq_rx __attribute__((aligned(4096)));
static VQueue vq_tx __attribute__((aligned(4096)));

int vnet_init(VirtioNet *dev, unsigned char ip[4],
              const unsigned char gateway[4], const unsigned char netmask[4]) {
    uart_println("[vnet] Scanning virtio-net device...");

    // Zero out struct
    net_memset(dev, 0, sizeof(VirtioNet));

    // Coba scan sampai 8 virtio MMIO slot
    volatile unsigned int *mmio = (volatile unsigned int *)VIRTIO_MMIO_BASE;
    int found = 0;

    for (int slot = 0; slot < 8; slot++) {
        volatile unsigned int *base =
            (volatile unsigned int*)(VIRTIO_MMIO_BASE + slot * VIRTIO_MMIO_SIZE);

        unsigned int magic   = mmio_read(base, VIRTIO_MMIO_MAGIC);
        unsigned int version = mmio_read(base, VIRTIO_MMIO_VERSION);
        unsigned int dev_id  = mmio_read(base, VIRTIO_MMIO_DEVICE_ID);

        // Magic harus 0x74726976 ("virt" little-endian)
        if (magic != 0x74726976) continue;
        // Version 1 = legacy (yang QEMU pakai)
        if (version != 1) continue;
        // Device ID 1 = network
        if (dev_id != VIRTIO_DEVICE_NET) continue;

        uart_print("[vnet] Found virtio-net at slot ");
        print_dec(slot);
        uart_print(" addr=0x0a00");
        print_hex_byte((slot * 2) & 0xFF);
        uart_println("00");

        mmio  = base;
        found = 1;
        break;
    }

    if (!found) {
        uart_println("[vnet] ERR: virtio-net tidak ditemukan!");
        uart_println("[vnet] Pastikan QEMU dijalankan dengan:");
        uart_println("[vnet]   -netdev user,id=net0");
        uart_println("[vnet]   -device virtio-net-device,netdev=net0");
        return -1;
    }

    dev->mmio = mmio;

    // ── Step 1: Reset device ──────────────────────────────
    mmio_write(mmio, VIRTIO_MMIO_STATUS, 0);

    // ── Step 2: Set ACKNOWLEDGE ───────────────────────────
    mmio_write(mmio, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    // ── Step 3: Set DRIVER ────────────────────────────────
    mmio_write(mmio, VIRTIO_MMIO_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // ── Step 4: Set page size (legacy) ───────────────────
    mmio_write(mmio, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);

    // ── Step 5: Negotiate features ───────────────────────
    // Kita cuma minta MAC address (bit 5)
    unsigned int host_feat = mmio_read(mmio, VIRTIO_MMIO_HOST_FEATURES);
    unsigned int our_feat  = 0;

    if (host_feat & VIRTIO_NET_F_MAC) {
        our_feat |= VIRTIO_NET_F_MAC;
        uart_println("[vnet] Feature: MAC address");
    }

    mmio_write(mmio, VIRTIO_MMIO_GUEST_FEATURES, our_feat);

    // ── Step 6: Baca MAC address ─────────────────────────
    // Di legacy virtio-net, MAC ada di config space (setelah register area)
    // Offset dari base: 0x100 + byte index
    volatile unsigned char *cfg = (volatile unsigned char*)mmio + 0x100;
    for (int i = 0; i < ETH_ALEN; i++)
        dev->mac[i] = cfg[i];

    uart_print("[vnet] MAC: ");
    vnet_print_mac(dev->mac);
    uart_print("\r\n");

    // ── Step 7: Setup virtqueues ─────────────────────────
    net_memset(&vq_rx, 0, sizeof(VQueue));
    net_memset(&vq_tx, 0, sizeof(VQueue));

    dev->rxq = &vq_rx;
    dev->txq = &vq_tx;

    if (vq_setup(mmio, 0, dev->rxq) < 0) goto fail;
    if (vq_setup(mmio, 1, dev->txq) < 0) goto fail;

    // ── Step 8: Driver OK ────────────────────────────────
    mmio_write(mmio, VIRTIO_MMIO_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    // ── Step 9: Pre-fill RX buffers ──────────────────────
    rxq_fill(dev);

    // ── Step 10: Set IP, gateway, netmask ────────────────
    net_memcpy(dev->ip,      ip,      4);
    net_memcpy(dev->gateway, gateway, 4);
    net_memcpy(dev->netmask, netmask, 4);

    dev->ready = 1;

    uart_print("[vnet] IP : ");
    vnet_print_ip(dev->ip);
    uart_print("\r\n");
    uart_print("[vnet] GW : ");
    vnet_print_ip(dev->gateway);
    uart_print("\r\n");
    uart_print("[vnet] Mask: ");
    vnet_print_ip(dev->netmask);
    uart_print("\r\n");
    uart_println("[vnet] virtio-net ready!");

    return 0;

fail:
    mmio_write(mmio, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
    return -1;
}

// ─── SEND ────────────────────────────────────────────────

int vnet_send(VirtioNet *dev, const void *data, unsigned int len) {
    if (!dev->ready) return -1;
    if (len > ETH_FRAME_MAX) return -1;

    VQueue *q = dev->txq;

    // Pakai dua descriptor berantai:
    // [0] = VirtioNetHdr
    // [1] = Ethernet frame data

    // Cari dua descriptor bebas
    // Untuk simplicity: TX selalu pakai slot 0 dan 1 (satu TX sekaligus)
    // Production harusnya round-robin atau lock-free queue
    // tapi untuk NexOS ini cukup

    // Virtio-net header
    VirtioNetHdr *hdr = (VirtioNetHdr*)dev->tx_buf;
    net_memset(hdr, 0, sizeof(VirtioNetHdr));
    // flags=0, gso_type=0 → no offload needed

    // Copy data setelah header
    net_memcpy(dev->tx_buf + sizeof(VirtioNetHdr), data, len);

    // Descriptor 0: header
    unsigned short desc_hdr = 0;
    q->desc[desc_hdr].addr  = (unsigned long long)(unsigned int)dev->tx_buf;
    q->desc[desc_hdr].len   = sizeof(VirtioNetHdr);
    q->desc[desc_hdr].flags = VRING_DESC_F_NEXT;
    q->desc[desc_hdr].next  = 1;

    // Descriptor 1: data
    unsigned short desc_dat = 1;
    q->desc[desc_dat].addr  = (unsigned long long)
                               ((unsigned int)dev->tx_buf + sizeof(VirtioNetHdr));
    q->desc[desc_dat].len   = len;
    q->desc[desc_dat].flags = 0;   // tidak WRITE, tidak NEXT
    q->desc[desc_dat].next  = 0;

    // Masukkan ke available ring
    unsigned short avail_idx = q->avail.idx % VQUEUE_SIZE;
    q->avail.ring[avail_idx] = desc_hdr;
    __asm__ volatile ("dmb" ::: "memory");
    q->avail.idx++;
    __asm__ volatile ("dmb" ::: "memory");

    // Notify device: ada TX baru
    mmio_write(dev->mmio, VIRTIO_MMIO_QUEUE_NOTIFY, 1); // 1 = TX queue

    dev->tx_count++;
    return 0;
}

// ─── RECEIVE POLL ────────────────────────────────────────

void vnet_poll(VirtioNet *dev, void (*callback)(const void *frame, unsigned int len)) {
    if (!dev->ready) return;

    VQueue *q = dev->rxq;

    // Cek interrupt status
    unsigned int isr = mmio_read(dev->mmio, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (isr == 0) return;

    // Clear interrupt
    mmio_write(dev->mmio, VIRTIO_MMIO_INTERRUPT_ACK, isr);

    // Memory barrier
    __asm__ volatile ("dmb" ::: "memory");

    // Proses semua entry di used ring yang belum kita proses
    while (dev->rx_last_seen != q->used.idx) {
        unsigned short idx = dev->rx_last_seen % VQUEUE_SIZE;
        VRingUsedElem *e   = &q->used.ring[idx];

        unsigned int desc_id = e->id;
        unsigned int pkt_len = e->len;

        if (desc_id < NET_RX_BUFS && pkt_len > sizeof(VirtioNetHdr)) {
            // Skip VirtioNetHdr, kasih ke callback frame aslinya
            unsigned char *frame = dev->rx_buf[desc_id] + sizeof(VirtioNetHdr);
            unsigned int   flen  = pkt_len - sizeof(VirtioNetHdr);

            dev->rx_count++;
            if (callback) callback(frame, flen);
        } else {
            dev->rx_dropped++;
        }

        // Recycle descriptor: kembalikan buffer ke available ring
        q->desc[desc_id].addr  = (unsigned long long)(unsigned int)dev->rx_buf[desc_id];
        q->desc[desc_id].len   = NET_BUF_SIZE;
        q->desc[desc_id].flags = VRING_DESC_F_WRITE;
        q->desc[desc_id].next  = 0;

        q->avail.ring[q->avail.idx % VQUEUE_SIZE] = (unsigned short)desc_id;
        __asm__ volatile ("dmb" ::: "memory");
        q->avail.idx++;
        __asm__ volatile ("dmb" ::: "memory");

        dev->rx_last_seen++;
    }

    // Notify device ada buffer RX baru
    mmio_write(dev->mmio, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
}

// ─── ARP TABLE ───────────────────────────────────────────

static int ip4_eq(const unsigned char *a, const unsigned char *b) {
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

// ─── ROUTING ─────────────────────────────────────────────
// Cek apakah ip_a dan ip_b berada di subnet yang sama, berdasarkan netmask.
// Caranya: AND-kan kedua IP dengan netmask, bandingkan hasilnya (network address).
int ip4_same_subnet(const unsigned char ip_a[4], const unsigned char ip_b[4],
                     const unsigned char netmask[4]) {
    for (int i = 0; i < 4; i++) {
        if ((ip_a[i] & netmask[i]) != (ip_b[i] & netmask[i]))
            return 0;
    }
    return 1;
}

// Tentukan IP mana yang harus di-ARP-resolve untuk bisa kirim frame
// ke target_ip:
//   - Kalau target_ip satu subnet dengan dev->ip → resolve target_ip langsung
//     (dia ada di LAN yang sama, bisa dijangkau via ARP biasa)
//   - Kalau beda subnet → resolve dev->gateway, lalu frame dikirim ke MAC
//     gateway itu (gateway yang nanti routing packet keluar subnet)
void vnet_route_lookup(VirtioNet *dev, const unsigned char target_ip[4],
                        unsigned char out_ip[4]) {
    if (ip4_same_subnet(dev->ip, target_ip, dev->netmask)) {
        net_memcpy(out_ip, target_ip, 4);
    } else {
        net_memcpy(out_ip, dev->gateway, 4);
    }
}

ArpEntry* arp_lookup(ArpTable *t, const unsigned char ip[4]) {
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (t->entries[i].state != ARP_FREE && ip4_eq(t->entries[i].ip, ip))
            return &t->entries[i];
    }
    return (ArpEntry*)0;
}

ArpEntry* arp_update(ArpTable *t, const unsigned char ip[4], const unsigned char mac[ETH_ALEN]) {
    // Kalau sudah ada, update aja
    ArpEntry *e = arp_lookup(t, ip);
    if (!e) {
        // Cari slot kosong
        for (int i = 0; i < ARP_TABLE_SIZE; i++) {
            if (t->entries[i].state == ARP_FREE) {
                e = &t->entries[i];
                t->count++;
                break;
            }
        }
    }
    if (!e) return (ArpEntry*)0; // tabel penuh

    net_memcpy(e->ip,  ip,  4);
    net_memcpy(e->mac, mac, ETH_ALEN);
    e->state = ARP_RESOLVED;
    e->age   = 0;
    return e;
}

ArpEntry* arp_resolve(VirtioNet *dev, const unsigned char ip[4]) {
    ArpEntry *e = arp_lookup(&dev->arp, ip);

    if (e && e->state == ARP_RESOLVED) {
        e->age++;
        return e;    // sudah ada MAC-nya
    }

    if (e && e->state == ARP_PENDING) {
        e->age++;
        return e;    // masih nunggu reply
    }

    // Belum ada entry — cari slot kosong, kirim ARP request
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (dev->arp.entries[i].state == ARP_FREE) {
            e = &dev->arp.entries[i];
            net_memcpy(e->ip, ip, 4);
            net_memset(e->mac, 0, ETH_ALEN);
            e->state = ARP_PENDING;
            e->age   = 0;
            dev->arp.count++;
            break;
        }
    }

    vnet_arp_request(dev, ip);
    return e;
}

void arp_print_table(ArpTable *t) {
    uart_println("\r\n ARP Table:\r\n");
    uart_println("  IP               MAC                STATE");
    uart_println("  ──────────────────────────────────────────────");
    int found = 0;
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        ArpEntry *e = &t->entries[i];
        if (e->state == ARP_FREE) continue;
        uart_print("  ");
        vnet_print_ip(e->ip);
        // padding IP ke 17 char
        int iplen = 0;
        unsigned char tmp[4]; net_memcpy(tmp, e->ip, 4);
        // hitung panjang IP string
        for (int o = 0; o < 4; o++) {
            unsigned char b = e->ip[o];
            if (b >= 100) iplen += 3;
            else if (b >= 10) iplen += 2;
            else iplen += 1;
            if (o < 3) iplen++;
        }
        for (int p = iplen; p < 17; p++) uart_putc(' ');
        vnet_print_mac(e->mac);
        uart_print("   ");
        if (e->state == ARP_RESOLVED) uart_print("RESOLVED");
        else                           uart_print("PENDING ");
        uart_print("  age=");
        print_dec(e->age);
        uart_print("\r\n");
        found++;
    }
    if (!found) uart_println("  (kosong)");
    uart_print("\r\n");
}

// ─── ARP ─────────────────────────────────────────────────

static const unsigned char broadcast_mac[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

void vnet_arp_request(VirtioNet *dev, const unsigned char target_ip[4]) {
    unsigned char frame[sizeof(EthHdr) + sizeof(ArpPkt)];

    // Ethernet header
    EthHdr *eth = (EthHdr*)frame;
    net_memcpy(eth->dst, broadcast_mac, ETH_ALEN);
    net_memcpy(eth->src, dev->mac, ETH_ALEN);
    eth->ethertype = htons(ETH_TYPE_ARP);

    // ARP payload
    ArpPkt *arp = (ArpPkt*)(frame + sizeof(EthHdr));
    arp->hw_type    = htons(ARP_HW_ETH);
    arp->proto_type = htons(ARP_PROTO_IP);
    arp->hw_len     = ETH_ALEN;
    arp->proto_len  = 4;
    arp->op         = htons(ARP_OP_REQUEST);
    net_memcpy(arp->sender_mac, dev->mac,   ETH_ALEN);
    net_memcpy(arp->sender_ip,  dev->ip,    4);
    net_memset(arp->target_mac, 0,          ETH_ALEN);
    net_memcpy(arp->target_ip,  target_ip,  4);

    uart_print("[arp] Who has ");
    vnet_print_ip(target_ip);
    uart_print("? Tell ");
    vnet_print_ip(dev->ip);
    uart_print("\r\n");

    vnet_send(dev, frame, sizeof(frame));
}

void vnet_arp_reply(VirtioNet *dev, const ArpPkt *req) {
    unsigned char frame[sizeof(EthHdr) + sizeof(ArpPkt)];

    EthHdr *eth = (EthHdr*)frame;
    net_memcpy(eth->dst, req->sender_mac, ETH_ALEN);
    net_memcpy(eth->src, dev->mac, ETH_ALEN);
    eth->ethertype = htons(ETH_TYPE_ARP);

    ArpPkt *arp = (ArpPkt*)(frame + sizeof(EthHdr));
    arp->hw_type    = htons(ARP_HW_ETH);
    arp->proto_type = htons(ARP_PROTO_IP);
    arp->hw_len     = ETH_ALEN;
    arp->proto_len  = 4;
    arp->op         = htons(ARP_OP_REPLY);
    net_memcpy(arp->sender_mac, dev->mac,        ETH_ALEN);
    net_memcpy(arp->sender_ip,  dev->ip,         4);
    net_memcpy(arp->target_mac, req->sender_mac, ETH_ALEN);
    net_memcpy(arp->target_ip,  req->sender_ip,  4);

    uart_print("[arp] Reply: ");
    vnet_print_ip(dev->ip);
    uart_print(" is at ");
    vnet_print_mac(dev->mac);
    uart_print("\r\n");

    vnet_send(dev, frame, sizeof(frame));
}

// ─── UDP SEND ────────────────────────────────────────────
// Kirim UDP datagram. src_ip boleh 0.0.0.0 (untuk DHCP DISCOVER/REQUEST
// sebelum kita punya IP). dst_ip boleh 255.255.255.255 (broadcast).
// Broadcast → dst MAC = ff:ff:ff:ff:ff:ff, tidak perlu ARP.
// Unicast   → ARP resolve normal via vnet_route_lookup + arp_resolve.

int udp_send(VirtioNet *dev,
             const unsigned char src_ip[4],
             const unsigned char dst_ip[4],
             unsigned short src_port, unsigned short dst_port,
             const void *payload, unsigned int plen)
{
    if (!dev || !dev->ready) return -1;

    unsigned int total = sizeof(EthHdr) + sizeof(IpHdr)
                       + sizeof(UdpHdr) + plen;
    if (total > ETH_FRAME_MAX) return -1;

    // Pilih dst MAC: broadcast atau resolve ARP
    static const unsigned char bcast_mac[ETH_ALEN] =
        {0xff,0xff,0xff,0xff,0xff,0xff};
    static const unsigned char bcast_ip[4] =
        {255,255,255,255};

    unsigned char dst_mac[ETH_ALEN];
    int is_bcast = (dst_ip[0]==255 && dst_ip[1]==255 &&
                    dst_ip[2]==255 && dst_ip[3]==255);
    if (is_bcast) {
        net_memcpy(dst_mac, bcast_mac, ETH_ALEN);
    } else {
        unsigned char next_hop[4];
        vnet_route_lookup(dev, dst_ip, next_hop);
        ArpEntry *ae = arp_resolve(dev, next_hop);
        if (!ae || ae->state != ARP_RESOLVED) return -1;
        net_memcpy(dst_mac, ae->mac, ETH_ALEN);
    }

    // Pilih src IP: kalau 0.0.0.0 (DHCP sebelum lease), pakai apa adanya
    const unsigned char *actual_src = src_ip;

    unsigned char frame[ETH_FRAME_MAX];
    net_memset(frame, 0, total);

    // Ethernet
    EthHdr *eth = (EthHdr*)frame;
    net_memcpy(eth->dst, dst_mac,  ETH_ALEN);
    net_memcpy(eth->src, dev->mac, ETH_ALEN);
    eth->ethertype = htons(0x0800);

    // IP
    IpHdr *ip = (IpHdr*)(frame + sizeof(EthHdr));
    ip->ihl_ver   = 0x45;
    ip->tos       = 0;
    ip->total_len = htons((unsigned short)(sizeof(IpHdr) + sizeof(UdpHdr) + plen));
    ip->id        = htons((unsigned short)(src_port ^ dst_port));
    ip->frag_off  = 0;
    ip->ttl       = 64;
    ip->proto     = IP_PROTO_UDP;
    ip->checksum  = 0;
    net_memcpy(ip->src, actual_src, 4);
    net_memcpy(ip->dst, dst_ip,     4);
    ip->checksum  = ip_checksum(ip, sizeof(IpHdr));

    // UDP
    UdpHdr *udp = (UdpHdr*)(frame + sizeof(EthHdr) + sizeof(IpHdr));
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons((unsigned short)(sizeof(UdpHdr) + plen));
    udp->checksum = 0;  // checksum opsional untuk UDP, kita skip

    // Payload
    if (plen > 0)
        net_memcpy((unsigned char*)udp + sizeof(UdpHdr), payload, plen);

    return vnet_send(dev, frame, total);
}

// ─── ICMP ────────────────────────────────────────────────

void vnet_icmp_reply(VirtioNet *dev, const IpHdr *iph, const IcmpHdr *icmph,
                     const unsigned char *payload, unsigned int plen) {
    unsigned int total = sizeof(EthHdr) + sizeof(IpHdr) + sizeof(IcmpHdr) + plen;
    if (total > ETH_FRAME_MAX) return;

    unsigned char frame[ETH_FRAME_MAX];
    net_memset(frame, 0, total);

    // Ethernet
    EthHdr *eth = (EthHdr*)frame;
    // Lookup MAC sender di ARP table
    ArpEntry *ae = arp_lookup(&dev->arp, iph->src);
    if (ae && ae->state == ARP_RESOLVED) {
        net_memcpy(eth->dst, ae->mac, ETH_ALEN);
    } else {
        // Fallback broadcast — QEMU user-net masih bisa terima
        net_memcpy(eth->dst, broadcast_mac, ETH_ALEN);
    }
    net_memcpy(eth->src, dev->mac, ETH_ALEN);
    eth->ethertype = htons(ETH_TYPE_IP);

    // IP
    IpHdr *ip = (IpHdr*)(frame + sizeof(EthHdr));
    ip->ihl_ver   = 0x45;
    ip->tos       = 0;
    ip->total_len = htons((unsigned short)(sizeof(IpHdr) + sizeof(IcmpHdr) + plen));
    ip->id        = 0;
    ip->frag_off  = 0;
    ip->ttl       = 64;
    ip->proto     = IP_PROTO_ICMP;
    ip->checksum  = 0;
    net_memcpy(ip->src, dev->ip,  4);
    net_memcpy(ip->dst, iph->src, 4);
    ip->checksum  = ip_checksum(ip, sizeof(IpHdr));

    // ICMP
    IcmpHdr *icmp = (IcmpHdr*)(frame + sizeof(EthHdr) + sizeof(IpHdr));
    icmp->type     = ICMP_ECHO_REPLY;
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = icmph->id;
    icmp->seq      = icmph->seq;

    // Copy ping payload
    if (plen > 0)
        net_memcpy((unsigned char*)icmp + sizeof(IcmpHdr), payload, plen);

    // ICMP checksum mencakup header + payload
    icmp->checksum = ip_checksum(icmp, sizeof(IcmpHdr) + plen);

    uart_print("[icmp] Echo reply → ");
    vnet_print_ip(iph->src);
    uart_print("\r\n");

    vnet_send(dev, frame, total);
}

// ─── FRAME DISPATCHER ────────────────────────────────────

void vnet_handle_frame(VirtioNet *dev, const void *frame, unsigned int len) {
    if (len < sizeof(EthHdr)) return;

    const EthHdr *eth = (const EthHdr*)frame;
    unsigned short etype = ntohs(eth->ethertype);

    if (etype == ETH_TYPE_ARP) {
        // ── ARP ──
        if (len < sizeof(EthHdr) + sizeof(ArpPkt)) return;
        const ArpPkt *arp = (const ArpPkt*)((const unsigned char*)frame + sizeof(EthHdr));

        if (ntohs(arp->op) == ARP_OP_REQUEST) {
            // Cek apakah ARP ini untuk IP kita
            if (ip4_eq(arp->target_ip, dev->ip)) {
                // Simpan sender ke ARP table sekalian
                arp_update(&dev->arp, arp->sender_ip, arp->sender_mac);
                vnet_arp_reply(dev, arp);
            }
        } else if (ntohs(arp->op) == ARP_OP_REPLY) {
            // Simpan ke ARP table
            ArpEntry *e = arp_update(&dev->arp, arp->sender_ip, arp->sender_mac);
            uart_print("[arp] Resolved: ");
            vnet_print_ip(arp->sender_ip);
            uart_print(" → ");
            vnet_print_mac(arp->sender_mac);
            uart_print("\r\n");
            (void)e;
        }

    } else if (etype == ETH_TYPE_IP) {
        // ── IP ──
        if (len < sizeof(EthHdr) + sizeof(IpHdr)) return;
        const IpHdr *ip = (const IpHdr*)((const unsigned char*)frame + sizeof(EthHdr));

        // Cek apakah packet untuk kita atau broadcast
        int for_us = (ip->dst[0] == dev->ip[0] && ip->dst[1] == dev->ip[1] &&
                      ip->dst[2] == dev->ip[2] && ip->dst[3] == dev->ip[3]);
        int is_bcast = (ip->dst[0]==255 && ip->dst[1]==255 &&
                        ip->dst[2]==255 && ip->dst[3]==255);
        // Juga terima kalau IP kita masih 0.0.0.0 (belum punya lease DHCP)
        int ip_unset = (dev->ip[0]==0 && dev->ip[1]==0 &&
                        dev->ip[2]==0 && dev->ip[3]==0);
        if (!for_us && !is_bcast && !ip_unset) return;

        unsigned int ihl = (ip->ihl_ver & 0x0F) * 4;
        const unsigned char *ip_payload = (const unsigned char*)ip + ihl;
        unsigned int ip_payload_len = ntohs(ip->total_len) - ihl;

        if (ip->proto == IP_PROTO_ICMP) {
            // ── ICMP ──
            if (ip_payload_len < sizeof(IcmpHdr)) return;
            const IcmpHdr *icmp = (const IcmpHdr*)ip_payload;

            if (icmp->type == ICMP_ECHO_REQUEST) {
                uart_print("[icmp] Echo request from ");
                vnet_print_ip(ip->src);
                uart_print("\r\n");
                const unsigned char *ping_data = ip_payload + sizeof(IcmpHdr);
                unsigned int ping_len = ip_payload_len - sizeof(IcmpHdr);
                vnet_icmp_reply(dev, ip, icmp, ping_data, ping_len);
            }

        } else if (ip->proto == IP_PROTO_UDP) {
            // ── UDP ──
            if (ip_payload_len < sizeof(UdpHdr)) return;
            const UdpHdr *udp = (const UdpHdr*)ip_payload;
            unsigned short dport = ntohs(udp->dst_port);
            unsigned short sport = ntohs(udp->src_port);

            uart_print("[udp] port=");
            print_dec(dport);
            uart_print(" len=");
            print_dec(ntohs(udp->length));
            uart_print("\r\n");

            if (sport == 53) {
                // DNS response — dispatch ke resolver global (didefinisikan
                // di kernel_v0_3.c, sama pola seperti tcp_input_dispatch)
                const unsigned char *dns_payload = ip_payload + sizeof(UdpHdr);
                unsigned int dns_plen = ip_payload_len > sizeof(UdpHdr)
                                      ? ip_payload_len - sizeof(UdpHdr) : 0;
                dns_input_dispatch((void*)0, dns_payload, dns_plen);
            }
            // TODO: port handler lain (DHCP, dst) kalau dibutuhkan nanti
            // ── DHCP reply (server → client, dst port 68) ──
            if (dport == 68) {
                const unsigned char *dhcp_payload = ip_payload + sizeof(UdpHdr);
                unsigned int dhcp_plen = ip_payload_len > sizeof(UdpHdr)
                                       ? ip_payload_len - sizeof(UdpHdr) : 0;
                dhcp_input_dispatch(dev, dhcp_payload, dhcp_plen);
            }

        } else if (ip->proto == IP_PROTO_TCP) {
            // ── TCP — dispatch ke tcp_input via weak handler ──
            if (ip_payload_len < sizeof(TcpHdr)) return;
            const TcpHdr *tcph = (const TcpHdr*)ip_payload;
            unsigned int  tcph_len = ((tcph->data_off >> 4) & 0xF) * 4;
            const unsigned char *tcp_payload = ip_payload + tcph_len;
            unsigned int  tcp_plen = ip_payload_len > tcph_len
                                   ? ip_payload_len - tcph_len : 0;
            // Dispatch ke kernel handler (didefinisikan di kernel_v0_3.c)
            tcp_input_dispatch((void*)0, ip, tcph, tcp_payload, tcp_plen);
        }
    }
}
