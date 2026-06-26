// nfs.c — NexOS Filesystem implementation

#include "nfs.h"

extern void uart_print(const char *s);
extern void uart_println(const char *s);
extern void uart_putc(char c);

// ─── HELPERS ────────────────────────────────────────────

static unsigned int nfs_strlen(const char *s) {
    unsigned int n = 0; while (s[n]) n++; return n;
}
static int nfs_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static void nfs_strcpy(char *d, const char *s, unsigned int maxn) {
    unsigned int i = 0;
    while (i < maxn - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void nfs_memset(void *p, unsigned char v, unsigned int n) {
    unsigned char *d = (unsigned char*)p; while (n--) *d++ = v;
}
static void nfs_memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char*)dst;
    const unsigned char *s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
}

static void print_num(unsigned int n) {
    char buf[12]; int i = 0;
    if (!n) { uart_putc('0'); return; }
    while (n) { buf[i++] = '0' + n % 10; n /= 10; }
    while (i--) uart_putc(buf[i]);
}

// Sector buffer (global, 512 bytes)
static unsigned char g_sector_buf[512];

// Inode table sectors: MAX_INODES * sizeof(NfsInode) / 512
// sizeof(NfsInode) = 96 bytes → 64 inodes = 6144 bytes = 12 sectors
#define INODE_TABLE_LBA     1
#define INODE_TABLE_SECTORS ((MAX_INODES * sizeof(NfsInode) + 511) / 512)

// ─── PATH PARSING ───────────────────────────────────────

// Ambil komponen terakhir dari path (/a/b/c → "c")
static void path_basename(const char *path, char *out, unsigned int maxn) {
    int last_slash = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') last_slash = i;
    nfs_strcpy(out, path + last_slash + 1, maxn);
}

// Ambil direktori parent (/a/b/c → "/a/b")
static void path_dirname(const char *path, char *out, unsigned int maxn) {
    int last_slash = 0;
    for (int i = 0; path[i]; i++) if (path[i] == '/') last_slash = i;
    if (last_slash == 0) { out[0] = '/'; out[1] = 0; return; }
    unsigned int n = (unsigned int)last_slash;
    if (n >= maxn) n = maxn - 1;
    nfs_memcpy(out, path, n);
    out[n] = 0;
}

// ─── DISK I/O (sector-level) ────────────────────────────

static int nfs_read_sector(NfsDev *nfs, unsigned int lba, void *buf) {
    return vblk_read(nfs->blk, lba, 1, buf);
}
static int nfs_write_sector(NfsDev *nfs, unsigned int lba, const void *buf) {
    return vblk_write(nfs->blk, lba, 1, (void*)buf);
}

// ─── SUPERBLOCK ─────────────────────────────────────────

void nfs_flush_sb(NfsDev *nfs) {
    nfs_memset(g_sector_buf, 0, 512);
    nfs_memcpy(g_sector_buf, &nfs->sb, sizeof(NfsSuperBlock));
    nfs_write_sector(nfs, 0, g_sector_buf);
}

static int nfs_load_sb(NfsDev *nfs) {
    if (nfs_read_sector(nfs, 0, g_sector_buf) < 0) return -1;
    nfs_memcpy(&nfs->sb, g_sector_buf, sizeof(NfsSuperBlock));
    return 0;
}

// ─── INODE TABLE ────────────────────────────────────────

void nfs_flush_inodes(NfsDev *nfs) {
    // Tulis inode table ke disk (bisa multi-sektor)
    unsigned char *p = (unsigned char*)nfs->inodes;
    unsigned int bytes = MAX_INODES * sizeof(NfsInode);
    unsigned int lba = INODE_TABLE_LBA;
    while (bytes > 0) {
        unsigned int chunk = bytes > 512 ? 512 : bytes;
        nfs_memset(g_sector_buf, 0, 512);
        nfs_memcpy(g_sector_buf, p, chunk);
        nfs_write_sector(nfs, lba, g_sector_buf);
        p += chunk; bytes -= chunk; lba++;
    }
}

static int nfs_load_inodes(NfsDev *nfs) {
    unsigned char *p = (unsigned char*)nfs->inodes;
    unsigned int bytes = MAX_INODES * sizeof(NfsInode);
    unsigned int lba = INODE_TABLE_LBA;
    while (bytes > 0) {
        if (nfs_read_sector(nfs, lba, g_sector_buf) < 0) return -1;
        unsigned int chunk = bytes > 512 ? 512 : bytes;
        nfs_memcpy(p, g_sector_buf, chunk);
        p += chunk; bytes -= chunk; lba++;
    }
    return 0;
}

// ─── ALLOCATE DATA BLOCK ────────────────────────────────
// Return LBA blok baru, atau 0 kalau penuh.
static unsigned int nfs_alloc_block(NfsDev *nfs) {
    if (nfs->sb.free_blocks == 0) return 0;
    unsigned int lba = nfs->sb.next_free_lba;
    // Cari LBA berikutnya yang bebas (simple sequential scan)
    // Track used LBAs dari inode direct blocks
    unsigned int next = lba + 1;
    // Simple: just bump. For production: use bitmap.
    nfs->sb.next_free_lba = next;
    nfs->sb.free_blocks--;
    nfs_flush_sb(nfs);
    return lba;
}

// ─── FORMAT ─────────────────────────────────────────────

int nfs_format(NfsDev *nfs, VirtioBlk *blk) {
    uart_println("[nfs] Formatting disk...");
    nfs->blk = blk;
    nfs_memset(nfs->inodes, 0, sizeof(nfs->inodes));
    nfs_memset(nfs->fds,    0, sizeof(nfs->fds));

    unsigned int total = vblk_capacity(blk);

    // Superblock
    nfs->sb.magic          = NFS_MAGIC;
    nfs->sb.version        = NFS_VERSION;
    nfs->sb.total_sectors  = total;
    nfs->sb.data_start     = NFS_DATA_START_LBA;
    nfs->sb.inode_count    = MAX_INODES;
    nfs->sb.free_blocks    = total - NFS_DATA_START_LBA;
    nfs->sb.next_free_lba  = NFS_DATA_START_LBA;
    nfs_flush_sb(nfs);

    // Buat root directory (inode 0)
    nfs->inodes[0].type         = NFS_TYPE_DIR;
    nfs->inodes[0].name[0]      = '/';
    nfs->inodes[0].name[1]      = 0;
    nfs->inodes[0].size         = 0;
    nfs->inodes[0].parent_inode = 0;
    for (int i = 0; i < NFS_BLOCKS_PER_INODE; i++)
        nfs->inodes[0].direct[i] = NFS_INVALID_BLK;
    nfs->inodes[0].indirect_lba = NFS_INVALID_BLK;

    nfs_flush_inodes(nfs);
    nfs->mounted = 1;

    uart_print("[nfs] Format OK. Capacity: ");
    print_num((total - NFS_DATA_START_LBA) / 2);
    uart_println(" KB usable");
    return 0;
}

// ─── MOUNT ──────────────────────────────────────────────

int nfs_mount(NfsDev *nfs, VirtioBlk *blk) {
    nfs->blk = blk;
    nfs_memset(nfs->fds, 0, sizeof(nfs->fds));

    if (nfs_load_sb(nfs) < 0) {
        uart_println("[nfs] ERR: cannot read superblock");
        return -1;
    }
    if (nfs->sb.magic != NFS_MAGIC) {
        uart_println("[nfs] ERR: disk tidak diformat. Ketik: nfs format");
        return -1;
    }
    if (nfs_load_inodes(nfs) < 0) {
        uart_println("[nfs] ERR: cannot read inode table");
        return -1;
    }

    nfs->mounted = 1;
    uart_print("[nfs] Mounted. ");
    print_num(nfs->sb.free_blocks);
    uart_println(" blocks free");
    return 0;
}

void nfs_sync(NfsDev *nfs) {
    nfs_flush_sb(nfs);
    nfs_flush_inodes(nfs);
}

// ─── FIND INODE ─────────────────────────────────────────

int nfs_find(NfsDev *nfs, const char *path) {
    // Root
    if (path[0] == '/' && path[1] == 0) return 0;

    char bname[NFS_NAME_LEN];
    char dname[64];
    path_basename(path, bname, NFS_NAME_LEN);
    path_dirname(path, dname, 64);

    // Cari parent dir dulu
    int parent_idx = 0;
    if (!(dname[0] == '/' && dname[1] == 0)) {
        parent_idx = nfs_find(nfs, dname);
        if (parent_idx < 0) return -1;
    }

    // Scan inode yang parent-nya cocok dan namanya cocok
    for (int i = 1; i < MAX_INODES; i++) {
        if (nfs->inodes[i].type == NFS_TYPE_FREE) continue;
        if ((int)nfs->inodes[i].parent_inode != parent_idx) continue;
        if (nfs_strcmp(nfs->inodes[i].name, bname) == 0) return i;
    }
    return -1;
}

// ─── CREATE ─────────────────────────────────────────────

int nfs_create(NfsDev *nfs, const char *path, int type) {
    if (!nfs->mounted) return -1;
    if (nfs_find(nfs, path) >= 0) return -1; // sudah ada

    // Cari slot inode kosong
    int slot = -1;
    for (int i = 1; i < MAX_INODES; i++) {
        if (nfs->inodes[i].type == NFS_TYPE_FREE) { slot = i; break; }
    }
    if (slot < 0) { uart_println("[nfs] ERR: inode table penuh"); return -1; }

    // Cari parent dir
    char dname[64];
    path_dirname(path, dname, 64);
    int parent = nfs_find(nfs, dname);
    if (parent < 0) { uart_println("[nfs] ERR: parent dir tidak ada"); return -1; }

    char bname[NFS_NAME_LEN];
    path_basename(path, bname, NFS_NAME_LEN);

    NfsInode *nd = &nfs->inodes[slot];
    nfs_memset(nd, 0, sizeof(NfsInode));
    nd->type         = (unsigned char)type;
    nd->parent_inode = (unsigned int)parent;
    nd->size         = 0;
    nfs_strcpy(nd->name, bname, NFS_NAME_LEN);
    for (int i = 0; i < NFS_BLOCKS_PER_INODE; i++) nd->direct[i] = NFS_INVALID_BLK;
    nd->indirect_lba = NFS_INVALID_BLK;

    nfs_flush_inodes(nfs);
    return slot;
}

int nfs_mkdir(NfsDev *nfs, const char *path) {
    return nfs_create(nfs, path, NFS_TYPE_DIR);
}

// ─── UNLINK ─────────────────────────────────────────────

int nfs_unlink(NfsDev *nfs, const char *path) {
    int idx = nfs_find(nfs, path);
    if (idx < 0) return -1;
    nfs->inodes[idx].type = NFS_TYPE_FREE;
    nfs_flush_inodes(nfs);
    return 0;
}

// ─── OPEN / CLOSE ───────────────────────────────────────

int nfs_open(NfsDev *nfs, const char *path, int writable) {
    if (!nfs->mounted) return -1;
    int inode = nfs_find(nfs, path);
    if (inode < 0) return -1;
    if (nfs->inodes[inode].type != NFS_TYPE_FILE) return -1;

    for (int i = 0; i < NFS_MAX_FD; i++) {
        if (!nfs->fds[i].valid) {
            nfs->fds[i].valid     = 1;
            nfs->fds[i].inode_idx = inode;
            nfs->fds[i].pos       = 0;
            nfs->fds[i].writable  = writable;
            return i;
        }
    }
    uart_println("[nfs] ERR: fd table penuh");
    return -1;
}

void nfs_close(NfsDev *nfs, int fd) {
    if (fd < 0 || fd >= NFS_MAX_FD) return;
    nfs->fds[fd].valid = 0;
    nfs_sync(nfs); // flush on close
}

// ─── READ ───────────────────────────────────────────────

int nfs_read(NfsDev *nfs, int fd, void *buf, unsigned int len) {
    if (fd < 0 || fd >= NFS_MAX_FD || !nfs->fds[fd].valid) return -1;

    NfsFile  *f = &nfs->fds[fd];
    NfsInode *nd = &nfs->inodes[f->inode_idx];

    unsigned int remaining = len;
    unsigned char *out = (unsigned char*)buf;
    unsigned int pos = f->pos;

    if (pos >= nd->size) return 0; // EOF
    if (pos + remaining > nd->size) remaining = nd->size - pos;

    unsigned int read_total = 0;
    while (remaining > 0) {
        // Tentukan blok mana dan offset di dalam blok
        unsigned int blk_idx  = pos / NFS_SECTOR_SIZE;
        unsigned int blk_off  = pos % NFS_SECTOR_SIZE;
        unsigned int can_read = NFS_SECTOR_SIZE - blk_off;
        if (can_read > remaining) can_read = remaining;

        // Dapatkan LBA
        unsigned int lba = NFS_INVALID_BLK;
        if (blk_idx < NFS_BLOCKS_PER_INODE) {
            lba = nd->direct[blk_idx];
        }
        if (lba == NFS_INVALID_BLK) break;

        if (nfs_read_sector(nfs, lba, g_sector_buf) < 0) break;
        nfs_memcpy(out, g_sector_buf + blk_off, can_read);

        out        += can_read;
        pos        += can_read;
        remaining  -= can_read;
        read_total += can_read;
    }

    f->pos = pos;
    return (int)read_total;
}

// ─── WRITE ──────────────────────────────────────────────

int nfs_write(NfsDev *nfs, int fd, const void *buf, unsigned int len) {
    if (fd < 0 || fd >= NFS_MAX_FD || !nfs->fds[fd].valid) return -1;
    if (!nfs->fds[fd].writable) return -1;

    NfsFile  *f = &nfs->fds[fd];
    NfsInode *nd = &nfs->inodes[f->inode_idx];

    const unsigned char *in = (const unsigned char*)buf;
    unsigned int pos = f->pos;
    unsigned int written = 0;

    while (len > 0) {
        unsigned int blk_idx = pos / NFS_SECTOR_SIZE;
        unsigned int blk_off = pos % NFS_SECTOR_SIZE;
        unsigned int can_write = NFS_SECTOR_SIZE - blk_off;
        if (can_write > len) can_write = len;

        // Dapatkan atau alokasi blok
        unsigned int lba = NFS_INVALID_BLK;
        if (blk_idx < NFS_BLOCKS_PER_INODE) {
            if (nd->direct[blk_idx] == NFS_INVALID_BLK) {
                // Alokasi blok baru
                lba = nfs_alloc_block(nfs);
                if (!lba) { uart_println("[nfs] ERR: disk penuh"); break; }
                nd->direct[blk_idx] = lba;
            } else {
                lba = nd->direct[blk_idx];
            }
        } else {
            uart_println("[nfs] ERR: file terlalu besar (max 4KB direct)");
            break;
        }

        // Read-modify-write (partial write ke dalam sector)
        if (blk_off != 0 || can_write < NFS_SECTOR_SIZE) {
            nfs_read_sector(nfs, lba, g_sector_buf);
        } else {
            nfs_memset(g_sector_buf, 0, 512);
        }

        nfs_memcpy(g_sector_buf + blk_off, in, can_write);
        if (nfs_write_sector(nfs, lba, g_sector_buf) < 0) break;

        in      += can_write;
        pos     += can_write;
        len     -= can_write;
        written += can_write;

        if (pos > nd->size) nd->size = pos;
    }

    f->pos = pos;
    nfs_flush_inodes(nfs);
    return (int)written;
}

// ─── LS ─────────────────────────────────────────────────

void nfs_ls(NfsDev *nfs, const char *path,
            void (*cb)(const char *name, int type, unsigned int size)) {
    int dir_idx = nfs_find(nfs, path);
    if (dir_idx < 0) { uart_println("[nfs] ERR: dir tidak ditemukan"); return; }
    if (nfs->inodes[dir_idx].type != NFS_TYPE_DIR) {
        uart_println("[nfs] ERR: bukan direktori"); return;
    }

    for (int i = 0; i < MAX_INODES; i++) {
        if (nfs->inodes[i].type == NFS_TYPE_FREE) continue;
        if ((int)nfs->inodes[i].parent_inode != dir_idx) continue;
        if (i == 0 && dir_idx == 0) continue; // skip root self
        cb(nfs->inodes[i].name, nfs->inodes[i].type, nfs->inodes[i].size);
    }
}
