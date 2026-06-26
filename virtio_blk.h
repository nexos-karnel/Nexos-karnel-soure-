// virtio_blk.h — NexOS virtio-blk driver (MMIO, ARM virt)
// Persistent block storage — data tidak hilang saat reboot
//
// QEMU flags yang diperlukan (tambah ke Makefile):
//   -drive file=nexos_disk.img,format=raw,if=none,id=blk0
//   -device virtio-blk-device,drive=blk0
//
// Buat disk image (jalankan sekali):
//   dd if=/dev/zero of=nexos_disk.img bs=1M count=32
//
// Device ID virtio-blk = 2 (beda dari net = 1)
// MMIO: sama seperti net, scan slot 0x0a000000 + slot*0x200

#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

// ─── VIRTIO MMIO (reuse definisi dari virtio_net.h) ─────
// Kalau virtio_net.h sudah di-include duluan, definisi ini tidak perlu.
// Tapi kita redeclare sebagai lokal supaya file ini standalone.
#ifndef VIRTIO_MMIO_BASE
#define VIRTIO_MMIO_BASE        0x0a000000U
#define VIRTIO_MMIO_SIZE        0x200

#define VIRTIO_MMIO_MAGIC           0x000
#define VIRTIO_MMIO_VERSION         0x004
#define VIRTIO_MMIO_DEVICE_ID       0x008
#define VIRTIO_MMIO_VENDOR_ID       0x00C
#define VIRTIO_MMIO_HOST_FEATURES   0x010
#define VIRTIO_MMIO_GUEST_FEATURES  0x020
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
#define VIRTIO_MMIO_QUEUE_NUM       0x038
#define VIRTIO_MMIO_QUEUE_ALIGN     0x03C
#define VIRTIO_MMIO_QUEUE_PFN       0x040
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK   0x064
#define VIRTIO_MMIO_STATUS          0x070

#define VIRTIO_STATUS_ACKNOWLEDGE   (1 << 0)
#define VIRTIO_STATUS_DRIVER        (1 << 1)
#define VIRTIO_STATUS_DRIVER_OK     (1 << 2)
#define VIRTIO_STATUS_FEATURES_OK   (1 << 3)
#define VIRTIO_STATUS_FAILED        (1 << 7)
#endif

// ─── VIRTIO BLK SPECIFICS ───────────────────────────────
#define VIRTIO_DEVICE_BLK           2       // Device ID untuk block device

// Request types
#define VIRTIO_BLK_T_IN             0       // Read dari disk ke memory
#define VIRTIO_BLK_T_OUT            1       // Write dari memory ke disk
#define VIRTIO_BLK_T_FLUSH          4       // Flush write cache

// Status dari device setelah operasi
#define VIRTIO_BLK_S_OK             0
#define VIRTIO_BLK_S_IOERR          1
#define VIRTIO_BLK_S_UNSUPP         2

// Ukuran sector (standard)
#define BLK_SECTOR_SIZE             512

// ─── VIRTQUEUE ──────────────────────────────────────────
#define BLK_QUEUE_SIZE              16      // Cukup untuk baremetal single-threaded

typedef struct {
    unsigned long long  addr;
    unsigned int        len;
    unsigned short      flags;
    unsigned short      next;
} __attribute__((packed)) BlkVRingDesc;

#define VRING_DESC_F_NEXT           (1 << 0)
#define VRING_DESC_F_WRITE          (1 << 1)

typedef struct {
    unsigned short flags;
    unsigned short idx;
    unsigned short ring[BLK_QUEUE_SIZE];
} __attribute__((packed)) BlkVRingAvail;

typedef struct {
    unsigned int    id;
    unsigned int    len;
} __attribute__((packed)) BlkVRingUsedElem;

typedef struct {
    unsigned short          flags;
    unsigned short          idx;
    BlkVRingUsedElem        ring[BLK_QUEUE_SIZE];
} __attribute__((packed)) BlkVRingUsed;

// ─── BLK REQUEST HEADER ─────────────────────────────────
// Harus dikirim sebagai descriptor pertama (device-readable)
typedef struct {
    unsigned int        type;       // VIRTIO_BLK_T_IN / T_OUT
    unsigned int        reserved;   // harus 0
    unsigned long long  sector;     // sektor yang dibaca/ditulis
} __attribute__((packed)) BlkReqHdr;

// ─── VIRTIO BLK DEVICE ──────────────────────────────────
// Semua state driver ada di sini. Pakai satu global di kernel.
typedef struct {
    volatile unsigned int  *mmio;           // base MMIO address
    unsigned int            capacity;       // total sectors (dari config space)

    // Virtqueue (harus 4096-byte aligned di physical memory)
    BlkVRingDesc    __attribute__((aligned(4096))) desc[BLK_QUEUE_SIZE];
    BlkVRingAvail   avail;
    unsigned char   _pad[4096 - sizeof(BlkVRingAvail) - sizeof(BlkVRingDesc)*BLK_QUEUE_SIZE % 4096];
    BlkVRingUsed    used;

    unsigned short  next_desc;      // index descriptor berikutnya
    unsigned short  last_used_idx;  // tracking used ring

    int             ready;          // 1 = driver OK
} VirtioBlk;

// ─── API ────────────────────────────────────────────────

// Init driver — scan MMIO slots, cari device ID 2.
// Return 0 sukses, -1 tidak ditemukan.
int  vblk_init(VirtioBlk *dev);

// Baca 'count' sektor dari 'lba' ke buffer 'buf'.
// buf harus punya ruang minimal count * 512 bytes.
// Return 0 sukses, -1 gagal.
int  vblk_read(VirtioBlk *dev, unsigned int lba, unsigned int count, void *buf);

// Tulis 'count' sektor dari 'buf' ke 'lba'.
// Return 0 sukses, -1 gagal.
int  vblk_write(VirtioBlk *dev, unsigned int lba, unsigned int count, const void *buf);

// Capacity dalam sektor
unsigned int vblk_capacity(VirtioBlk *dev);

#endif // VIRTIO_BLK_H
