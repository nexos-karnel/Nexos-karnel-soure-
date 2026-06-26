// virtio_blk.c — NexOS virtio-blk driver implementation
// Block device = storage persistent (data tidak hilang saat reboot)
//
// Flow per operasi (read/write):
//   1. Isi BlkReqHdr  → desc[0]: device-readable, 16 bytes
//   2. Data buffer    → desc[1]: READ=device-writable, WRITE=device-readable
//   3. Status byte    → desc[2]: device-writable, 1 byte
//   4. Chain desc[0]→[1]→[2] via .next
//   5. Taruh desc[0] di avail ring, bump avail.idx
//   6. QUEUE_NOTIFY → device mulai proses
//   7. Polling used ring sampai device selesai
//   8. Cek status byte == VIRTIO_BLK_S_OK

#include "virtio_blk.h"

// ─── FORWARD DECL UART (dari kernel_v0_3.c) ─────────────
extern void uart_print(const char *s);
extern void uart_println(const char *s);
extern void uart_putc(char c);

// ─── MMIO HELPERS ───────────────────────────────────────

static unsigned int blk_mmio_read(volatile unsigned int *base, unsigned int off) {
    return *(volatile unsigned int*)((unsigned char*)base + off);
}
static void blk_mmio_write(volatile unsigned int *base, unsigned int off, unsigned int v) {
    *(volatile unsigned int*)((unsigned char*)base + off) = v;
}

// ─── MEMORY HELPERS ─────────────────────────────────────

static void blk_memset(void *p, unsigned char v, unsigned int n) {
    unsigned char *d = (unsigned char*)p;
    while (n--) *d++ = v;
}
static void blk_memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char*)dst;
    const unsigned char *s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
}

static void print_dec_blk(unsigned int n) {
    char buf[12]; int i = 0;
    if (!n) { uart_putc('0'); return; }
    while (n) { buf[i++] = '0' + n % 10; n /= 10; }
    while (i--) uart_putc(buf[i]);
}

// ─── INIT ───────────────────────────────────────────────

int vblk_init(VirtioBlk *dev) {
    uart_println("[vblk] Scanning virtio-blk device...");
    blk_memset(dev, 0, sizeof(VirtioBlk));

    // Scan 8 MMIO slots — cari device ID 2 (blk)
    int found = 0;
    volatile unsigned int *mmio = (void*)0;

    for (int slot = 0; slot < 8; slot++) {
        volatile unsigned int *base =
            (volatile unsigned int*)(VIRTIO_MMIO_BASE + slot * VIRTIO_MMIO_SIZE);

        unsigned int magic  = blk_mmio_read(base, VIRTIO_MMIO_MAGIC);
        unsigned int ver    = blk_mmio_read(base, VIRTIO_MMIO_VERSION);
        unsigned int dev_id = blk_mmio_read(base, VIRTIO_MMIO_DEVICE_ID);

        if (magic != 0x74726976) continue;  // "virt"
        if (ver   != 1)          continue;  // legacy only
        if (dev_id != VIRTIO_DEVICE_BLK) continue;

        uart_print("[vblk] Found at slot ");
        print_dec_blk(slot);
        uart_print(" (0x0a000");
        // print slot offset hex
        unsigned int off = slot * VIRTIO_MMIO_SIZE;
        for (int i = 4; i >= 0; i -= 4) {
            unsigned char nib = (off >> i) & 0xF;
            uart_putc(nib < 10 ? '0' + nib : 'a' + nib - 10);
        }
        uart_println(")");

        mmio  = base;
        found = 1;
        break;
    }

    if (!found) {
        uart_println("[vblk] ERR: tidak ditemukan. Pastikan QEMU punya:");
        uart_println("[vblk]   -drive file=nexos_disk.img,format=raw,if=none,id=blk0");
        uart_println("[vblk]   -device virtio-blk-device,drive=blk0");
        return -1;
    }

    dev->mmio = mmio;

    // ── Step 1: Reset ────────────────────────────────────
    blk_mmio_write(mmio, VIRTIO_MMIO_STATUS, 0);

    // ── Step 2: ACKNOWLEDGE ──────────────────────────────
    blk_mmio_write(mmio, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    // ── Step 3: DRIVER ───────────────────────────────────
    blk_mmio_write(mmio, VIRTIO_MMIO_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // ── Step 4: Page size (legacy requirement) ───────────
    blk_mmio_write(mmio, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);

    // ── Step 5: Feature negotiation ──────────────────────
    // Untuk blk kita tidak butuh feature spesial (FLUSH opsional)
    blk_mmio_write(mmio, VIRTIO_MMIO_GUEST_FEATURES, 0);

    // ── Step 6: Setup virtqueue (queue 0 = request queue) ─
    blk_mmio_write(mmio, VIRTIO_MMIO_QUEUE_SEL, 0);
    unsigned int qmax = blk_mmio_read(mmio, VIRTIO_MMIO_QUEUE_NUM_MAX);
    unsigned int qsize = BLK_QUEUE_SIZE;
    if (qmax < qsize) qsize = qmax;

    blk_mmio_write(mmio, VIRTIO_MMIO_QUEUE_NUM,   qsize);
    blk_mmio_write(mmio, VIRTIO_MMIO_QUEUE_ALIGN, 4096);

    // PFN = physical page number dari desc table
    // desc table harus 4096-byte aligned (kita pakai __attribute__((aligned(4096))))
    unsigned int pfn = (unsigned int)dev->desc / 4096;
    blk_mmio_write(mmio, VIRTIO_MMIO_QUEUE_PFN, pfn);

    // ── Step 7: DRIVER_OK ────────────────────────────────
    blk_mmio_write(mmio, VIRTIO_MMIO_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    // Baca capacity dari config space (offset 0x100 dari MMIO base)
    // virtio-blk config: [capacity:u64][size_max:u32][seg_max:u32]...
    volatile unsigned int *cfg = (volatile unsigned int*)((unsigned char*)mmio + 0x100);
    dev->capacity = cfg[0]; // low 32 bit dari capacity (cukup untuk 2TB)

    dev->next_desc    = 0;
    dev->last_used_idx = 0;
    dev->ready        = 1;

    uart_print("[vblk] Ready. Capacity: ");
    print_dec_blk(dev->capacity / 2048);
    uart_println(" MB");

    return 0;
}

// ─── INTERNAL: DO REQUEST ───────────────────────────────
// Kirim satu request (read atau write) ke device, poll sampai selesai.
// lba    = logical block address (sector number)
// count  = jumlah sektor
// buf    = buffer data
// type   = VIRTIO_BLK_T_IN (read) atau VIRTIO_BLK_T_OUT (write)

static int vblk_do_request(VirtioBlk *dev, unsigned int lba,
                            unsigned int count, void *buf, unsigned int type) {
    if (!dev->ready) return -1;

    volatile unsigned int *mmio = dev->mmio;

    // Request header — harus static/global karena device akses via DMA
    // (stack address bisa bermasalah kalau MMU aktif, tapi di baremetal ini ok)
    static BlkReqHdr  req;
    static unsigned char status_byte;

    req.type     = type;
    req.reserved = 0;
    req.sector   = lba;
    status_byte  = 0xFF; // sentinel — device akan overwrite jadi 0 jika OK

    // ── Descriptor 0: request header (device baca) ───────
    unsigned int d0 = dev->next_desc % BLK_QUEUE_SIZE;
    dev->desc[d0].addr  = (unsigned long long)(unsigned int)&req;
    dev->desc[d0].len   = sizeof(BlkReqHdr);
    dev->desc[d0].flags = VRING_DESC_F_NEXT;
    dev->desc[d0].next  = (d0 + 1) % BLK_QUEUE_SIZE;

    // ── Descriptor 1: data buffer ────────────────────────
    unsigned int d1 = (d0 + 1) % BLK_QUEUE_SIZE;
    dev->desc[d1].addr  = (unsigned long long)(unsigned int)buf;
    dev->desc[d1].len   = count * BLK_SECTOR_SIZE;
    // READ: device nulis ke buffer kita → WRITE flag
    // WRITE: device baca dari buffer kita → tidak ada flag
    dev->desc[d1].flags = VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    dev->desc[d1].next  = (d1 + 1) % BLK_QUEUE_SIZE;

    // ── Descriptor 2: status byte (device tulis) ─────────
    unsigned int d2 = (d1 + 1) % BLK_QUEUE_SIZE;
    dev->desc[d2].addr  = (unsigned long long)(unsigned int)&status_byte;
    dev->desc[d2].len   = 1;
    dev->desc[d2].flags = VRING_DESC_F_WRITE; // device tulis status
    dev->desc[d2].next  = 0;

    dev->next_desc = (d2 + 1) % BLK_QUEUE_SIZE;

    // ── Taruh d0 di available ring ───────────────────────
    unsigned short avail_idx = dev->avail.idx % BLK_QUEUE_SIZE;
    dev->avail.ring[avail_idx] = (unsigned short)d0;
    // Memory barrier: pastikan descriptor sudah tertulis sebelum bump idx
    __asm__ volatile("dsb" ::: "memory");
    dev->avail.idx++;
    __asm__ volatile("dsb" ::: "memory");

    // ── Notify device ────────────────────────────────────
    blk_mmio_write(mmio, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    // ── Poll used ring sampai device selesai ─────────────
    // Timeout loop (~5 juta iterasi) supaya tidak hang selamanya
    unsigned int timeout = 5000000;
    while (dev->used.idx == dev->last_used_idx) {
        __asm__ volatile("" ::: "memory"); // prevent optimization
        if (--timeout == 0) {
            uart_println("[vblk] ERR: timeout waiting for device");
            return -1;
        }
    }

    // Ack interrupt
    blk_mmio_write(mmio, VIRTIO_MMIO_INTERRUPT_ACK,
        blk_mmio_read(mmio, VIRTIO_MMIO_INTERRUPT_STATUS) & 3);

    dev->last_used_idx = dev->used.idx;

    // ── Cek status ───────────────────────────────────────
    if (status_byte != VIRTIO_BLK_S_OK) {
        uart_print("[vblk] ERR: device returned status=");
        print_dec_blk(status_byte);
        uart_println("");
        return -1;
    }

    return 0;
}

// ─── PUBLIC API ─────────────────────────────────────────

int vblk_read(VirtioBlk *dev, unsigned int lba, unsigned int count, void *buf) {
    return vblk_do_request(dev, lba, count, buf, VIRTIO_BLK_T_IN);
}

int vblk_write(VirtioBlk *dev, unsigned int lba, unsigned int count, const void *buf) {
    return vblk_do_request(dev, lba, count, (void*)buf, VIRTIO_BLK_T_OUT);
}

unsigned int vblk_capacity(VirtioBlk *dev) {
    return dev->capacity;
}
