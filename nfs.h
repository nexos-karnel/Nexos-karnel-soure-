// nfs.h — NexOS Filesystem (NFS) — persistent storage di atas virtio-blk
//
// Format disk sederhana, custom untuk NexOS:
//
//   Sector 0        : SuperBlock (signature, versi, metadata)
//   Sector 1        : Inode Table (MAX_INODES entri, masing2 64 bytes)
//   Sector 2–N      : Data blocks (512 bytes per blok)
//
// Inode menyimpan: nama file, ukuran, first_block, type (file/dir)
// Data blocks saling terhubung via linked list (last_block = 0xFFFFFFFF)
//
// Kapasitas dengan disk 32MB:
//   32MB / 512 = 65536 sektor
//   MAX_INODES = 64 → inode table = 8 sektor
//   Sisa ≈ 65528 sektor × 512 = ~32MB data
//
// API design: mirip POSIX tapi minimal — nfs_open, nfs_read, nfs_write, dll.

#ifndef NFS_H
#define NFS_H

#include "virtio_blk.h"

// ─── KONFIGURASI ────────────────────────────────────────
#define NFS_MAGIC           0x4E465300U  // "NFS\0"
#define NFS_VERSION         1
#define NFS_SECTOR_SIZE     512
#define MAX_INODES          64
#define NFS_NAME_LEN        48           // max panjang nama file/dir
#define NFS_BLOCKS_PER_INODE 8           // blok langsung per inode (4KB per file max direct)
                                         // lebih dari itu → indirect block
#define NFS_DATA_START_LBA  16           // sektor 0-15 = superblock + inode table
#define NFS_INVALID_BLK     0xFFFFFFFFU  // sentinel: tidak ada blok berikutnya

// ─── NODE TYPE ──────────────────────────────────────────
#define NFS_TYPE_FREE       0
#define NFS_TYPE_FILE       1
#define NFS_TYPE_DIR        2

// ─── SUPERBLOCK ─────────────────────────────────────────
// Selalu di sektor 0. Total = 512 bytes.
typedef struct {
    unsigned int    magic;          // NFS_MAGIC
    unsigned int    version;        // NFS_VERSION
    unsigned int    total_sectors;  // total sektor disk
    unsigned int    data_start;     // LBA sektor data pertama
    unsigned int    inode_count;    // MAX_INODES
    unsigned int    free_blocks;    // berapa blok data yang tersisa
    unsigned int    next_free_lba;  // LBA blok data bebas berikutnya (bitmap sederhana)
    unsigned char   _pad[512 - 7*4];
} __attribute__((packed)) NfsSuperBlock;

// ─── INODE ──────────────────────────────────────────────
// 64 bytes per inode. Inode table = MAX_INODES × 64 = 4096 bytes = 8 sektor.
typedef struct {
    unsigned char   type;                       // NFS_TYPE_*
    unsigned char   _pad1[3];
    char            name[NFS_NAME_LEN];         // nama file/dir (null-terminated)
    unsigned int    size;                       // ukuran dalam bytes
    unsigned int    parent_inode;               // inode directory parent (0 = root)
    unsigned int    direct[NFS_BLOCKS_PER_INODE]; // LBA blok data langsung
    unsigned int    indirect_lba;               // LBA blok yang isinya array LBA (opsional)
} __attribute__((packed)) NfsInode;             // total = 1+3+48+4+4+32+4 = 96 bytes
                                                // kita pack ke 96, inode table = 96*64 = 6144 bytes = 12 sektor

// ─── FILE DESCRIPTOR (in-memory, tidak disimpan ke disk) ─
typedef struct {
    int             valid;          // 1 = slot terpakai
    int             inode_idx;      // index di inode table
    unsigned int    pos;            // posisi read/write saat ini (bytes)
    int             writable;       // 0=read-only, 1=read-write
} NfsFile;

#define NFS_MAX_FD      8           // max file terbuka bersamaan

// ─── NFS DEVICE (global state) ──────────────────────────
typedef struct {
    VirtioBlk      *blk;            // block device
    NfsSuperBlock   sb;             // superblock (cache di RAM)
    NfsInode        inodes[MAX_INODES]; // inode table (cache di RAM)
    NfsFile         fds[NFS_MAX_FD];
    int             mounted;        // 1 = filesystem siap
} NfsDev;

// ─── API ────────────────────────────────────────────────

// Format disk baru. HAPUS SEMUA DATA. Hanya jalankan sekali.
int  nfs_format(NfsDev *nfs, VirtioBlk *blk);

// Mount filesystem yang sudah ada.
// Return 0 sukses, -1 gagal (disk kosong → perlu nfs_format dulu).
int  nfs_mount(NfsDev *nfs, VirtioBlk *blk);

// Unmount: flush semua cache ke disk.
void nfs_sync(NfsDev *nfs);

// Buat file baru. Return inode index atau -1 gagal.
int  nfs_create(NfsDev *nfs, const char *path, int type);

// Buka file. Return fd (0–7) atau -1 gagal.
int  nfs_open(NfsDev *nfs, const char *path, int writable);

// Tutup file descriptor.
void nfs_close(NfsDev *nfs, int fd);

// Baca dari file. Return bytes yang berhasil dibaca.
int  nfs_read(NfsDev *nfs, int fd, void *buf, unsigned int len);

// Tulis ke file. Return bytes yang berhasil ditulis.
int  nfs_write(NfsDev *nfs, int fd, const void *buf, unsigned int len);

// Hapus file/dir.
int  nfs_unlink(NfsDev *nfs, const char *path);

// Buat directory.
int  nfs_mkdir(NfsDev *nfs, const char *path);

// List isi directory. Callback dipanggil untuk setiap entry.
void nfs_ls(NfsDev *nfs, const char *path,
            void (*cb)(const char *name, int type, unsigned int size));

// Cari inode berdasarkan path. Return index atau -1.
int  nfs_find(NfsDev *nfs, const char *path);

// Flush inode table ke disk.
void nfs_flush_inodes(NfsDev *nfs);

// Flush superblock ke disk.
void nfs_flush_sb(NfsDev *nfs);

#endif // NFS_H
