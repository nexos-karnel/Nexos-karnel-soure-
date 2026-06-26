// kernel_main.c — NexOS v0.3 — ARM Baremetal + Interrupt-driven UART
#include "virtio_net.h"
#include "virtio_blk.h"
#include "nfs.h"
#include "tcp.h"
#include "dns.h"
#include "dhcp.h"

// ─── GLOBAL NET DEVICE ───────────────────────────────────
static VirtioNet   g_net;
static TcpConn     g_tcp;
static DnsResolver g_dns;
static DhcpClient  g_dhcp;
static TcpListener g_tcp_listener;
static VirtioBlk   g_blk;
static NfsDev      g_nfs;

// ─── UART PL011 REGISTERS (QEMU ARM virt) ───────────────
#define UART_BASE       0x09000000U
#define UART_DR         (*(volatile unsigned int*)(UART_BASE + 0x00)) // Data
#define UART_FR         (*(volatile unsigned int*)(UART_BASE + 0x18)) // Flag
#define UART_IMSC       (*(volatile unsigned int*)(UART_BASE + 0x38)) // Interrupt mask
#define UART_MIS        (*(volatile unsigned int*)(UART_BASE + 0x40)) // Masked int status
#define UART_ICR        (*(volatile unsigned int*)(UART_BASE + 0x44)) // Interrupt clear

#define UART_FR_RXFE    (1 << 4)   // RX FIFO empty
#define UART_FR_TXFF    (1 << 5)   // TX FIFO full
#define UART_IMSC_RXIM  (1 << 4)   // RX interrupt mask bit
#define UART_ICR_RXIC   (1 << 4)   // RX interrupt clear bit

// ─── GIC (Generic Interrupt Controller) ─────────────────
// QEMU ARM virt GIC base addresses
#define GIC_DIST_BASE   0x08000000U
#define GIC_CPU_BASE    0x08010000U

// GIC Distributor registers
#define GICD_CTLR       (*(volatile unsigned int*)(GIC_DIST_BASE + 0x000))
#define GICD_ISENABLER1 (*(volatile unsigned int*)(GIC_DIST_BASE + 0x104)) // IRQ 32-63
#define GICD_IPRIORITYR (( volatile unsigned int*)(GIC_DIST_BASE + 0x400))
#define GICD_ITARGETSR  (( volatile unsigned int*)(GIC_DIST_BASE + 0x800))
#define GICD_ICFGR      (( volatile unsigned int*)(GIC_DIST_BASE + 0xC00))

// GIC CPU Interface registers
#define GICC_CTLR       (*(volatile unsigned int*)(GIC_CPU_BASE  + 0x000))
#define GICC_PMR        (*(volatile unsigned int*)(GIC_CPU_BASE  + 0x004)) // Priority mask
#define GICC_IAR        (*(volatile unsigned int*)(GIC_CPU_BASE  + 0x00C)) // Interrupt Ack
#define GICC_EOIR       (*(volatile unsigned int*)(GIC_CPU_BASE  + 0x010)) // End of interrupt

// UART0 di QEMU virt = GIC IRQ #33 (SPI #1)
#define UART0_IRQ       33

// ─── RING BUFFER (interrupt-safe input buffer) ───────────
#define RING_BUF_SIZE   256

typedef struct {
    volatile char buf[RING_BUF_SIZE];
    volatile int  head;   // producer (IRQ handler nulis)
    volatile int  tail;   // consumer (shell baca)
} RingBuf;

static RingBuf uart_rxbuf = {{0}, 0, 0};

static void ring_push(RingBuf *rb, char c) {
    int next = (rb->head + 1) % RING_BUF_SIZE;
    if (next != rb->tail) {   // drop kalau penuh
        rb->buf[rb->head] = c;
        rb->head = next;
    }
}

static int ring_empty(RingBuf *rb) {
    return rb->head == rb->tail;
}

static char ring_pop(RingBuf *rb) {
    if (ring_empty(rb)) return 0;
    char c = rb->buf[rb->tail];
    rb->tail = (rb->tail + 1) % RING_BUF_SIZE;
    return c;
}

// ─── VIRTIO IRQ WRAPPER ──────────────────────────────────
// Callback untuk vnet_poll — dispatch ke handler global
static void vnet_handle_frame_wrapper(const void *frame, unsigned int len) {
    vnet_handle_frame(&g_net, frame, len);
}

// ─── TCP DISPATCH ────────────────────────────────────────
// Dipanggil dari virtio_net.c setiap ada TCP segment masuk
void tcp_input_dispatch(struct TcpConn *conn, const IpHdr *iph, const TcpHdr *tcph,
                        const unsigned char *payload, unsigned int plen) {
    (void)conn;

    unsigned short dst_port = (unsigned short)((tcph->dst_port >> 8) | (tcph->dst_port << 8));

    // Cek listener dulu: kalau port cocok dan listener aktif, dispatch ke sana
    if (g_tcp_listener.active && dst_port == g_tcp_listener.local_port) {
        tcp_listener_input(&g_tcp_listener, iph, tcph, payload, plen);
        return;
    }

    // Fallback: dispatch ke koneksi client global
    tcp_input(&g_tcp, iph, tcph, payload, plen);
}

// ─── DNS DISPATCH ────────────────────────────────────────
// Dipanggil dari virtio_net.c setiap ada UDP packet dari port 53 (DNS reply)
void dns_input_dispatch(struct DnsResolver *res, const unsigned char *dns_payload,
                        unsigned int dns_plen) {
    (void)res; // pakai global
    dns_input(&g_dns, dns_payload, dns_plen);
}

// ─── DHCP DISPATCH ───────────────────────────────────────
// Dipanggil dari virtio_net.c setiap ada UDP packet dari port 68 (DHCP reply)
void dhcp_input_dispatch(VirtioNet *dev, const unsigned char *payload,
                         unsigned int len) {
    (void)dev;
    dhcp_input(&g_dhcp, payload, len);
}

// ─── GIC INIT ────────────────────────────────────────────
void gic_init() {
    // Enable GIC Distributor
    GICD_CTLR = 1;

    // Set UART0 IRQ priority (0 = highest, 0xFF = lowest)
    // IRQ 33 ada di byte ke-33 dari IPRIORITYR array
    ((volatile unsigned char*)(GIC_DIST_BASE + 0x400))[UART0_IRQ] = 0x80;

    // Target UART0 IRQ ke CPU0 (bit 0)
    ((volatile unsigned char*)(GIC_DIST_BASE + 0x800))[UART0_IRQ] = 0x01;

    // Enable IRQ 33 di distributor
    // ISENABLER1 control IRQ 32-63, bit (33-32) = bit 1
    GICD_ISENABLER1 = (1 << (UART0_IRQ - 32));

    // Enable virtio MMIO IRQs: SPI 16-23 = GIC IRQ 48-55
    // Berada di ISENABLER1 (IRQ 32-63): bit 16-23
    *(volatile unsigned int*)(GIC_DIST_BASE + 0x104) |= (0xFF << 16);
    // Target ke CPU0 dan set priority untuk semua virtio IRQs
    for (int v = 48; v <= 55; v++) {
        ((volatile unsigned char*)(GIC_DIST_BASE + 0x400))[v] = 0x80;
        ((volatile unsigned char*)(GIC_DIST_BASE + 0x800))[v] = 0x01;
    }

    // Enable GIC CPU interface
    GICC_CTLR = 1;

    // Set priority mask: izinkan semua priority (0xFF = all pass)
    GICC_PMR = 0xFF;
}

// ─── UART IRQ INIT ───────────────────────────────────────
void uart_irq_init() {
    // Enable UART RX interrupt di PL011
    UART_IMSC |= UART_IMSC_RXIM;
}

// ─── IRQ HANDLER (dipanggil dari startup.s) ──────────────
// Attribute interrupt tidak diperlukan karena startup.s yang
// handle save/restore — ini pure C handler
void irq_handler_c(void) {
    // Cek dari GIC siapa yang interrupt
    unsigned int iar = GICC_IAR;
    unsigned int irq_id = iar & 0x3FF;   // 10 bit bawah = IRQ ID

    if (irq_id == UART0_IRQ) {
        // Drain semua karakter yang ada di UART RX FIFO
        while (!(UART_FR & UART_FR_RXFE)) {
            char c = (char)(UART_DR & 0xFF);
            ring_push(&uart_rxbuf, c);
        }
        // Clear UART RX interrupt
        UART_ICR = UART_ICR_RXIC;
    } else if (irq_id >= 48 && irq_id <= 55) {
        // virtio MMIO IRQs: QEMU ARM virt maping virtio ke SPI 16-23
        // SPI 16 = GIC IRQ 48 ... SPI 23 = GIC IRQ 55
        // Poll RX — handle semua incoming packet
        vnet_poll(&g_net, vnet_handle_frame_wrapper);
    }

    // Signal End of Interrupt ke GIC
    GICC_EOIR = iar;
}

// ─── ENABLE IRQ GLOBAL (inline assembly) ─────────────────
static inline void irq_enable(void) {
    __asm__ volatile ("cpsie i" ::: "memory");
}

static inline void irq_disable(void) {
    __asm__ volatile ("cpsid i" ::: "memory");
}

// ─── UART OUTPUT (masih polling — TX tidak perlu IRQ) ────
void uart_putc(char c) {
    // Tunggu TX FIFO tidak penuh (ini cepat, OK untuk polling)
    while (UART_FR & UART_FR_TXFF);
    UART_DR = c;
}

void uart_print(const char *s) {
    while (*s) uart_putc(*s++);
}

void uart_println(const char *s) {
    uart_print(s);
    uart_print("\r\n");
}

// ─── UART INPUT (dari ring buffer, non-blocking) ─────────
// Ganti uart_getc lama yang polling dengan ini
char uart_getc_nb(void) {
    return ring_pop(&uart_rxbuf);
}

int uart_rx_ready(void) {
    return !ring_empty(&uart_rxbuf);
}

// uart_readline sekarang baca dari ring buffer
// Blocking tapi CPU bebas (tidak busy-wait di hardware register)
void uart_readline(char *buf, int max) {
    int i = 0;
    while (i < max - 1) {
        // Tunggu ada karakter di ring buffer
        while (!uart_rx_ready());   // yield point — nanti bisa context switch
        char c = uart_getc_nb();

        if (c == '\r' || c == '\n') {
            uart_print("\r\n");
            break;
        }
        if (c == 127 || c == '\b') {
            if (i > 0) { i--; uart_print("\b \b"); }
            continue;
        }
        buf[i++] = c;
        uart_putc(c);
    }
    buf[i] = 0;
}


// ─── STRING UTILS ───────────────────────
int npl_strlen(const char *s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

int npl_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

int npl_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

void npl_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++));
}

void npl_strcat(char *dst, const char *src) {
    while (*dst) dst++;
    while ((*dst++ = *src++));
}

// int to string
void npl_itoa(int n, char *buf) {
    if (n == 0) { buf[0]='0'; buf[1]=0; return; }
    int neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    char tmp[12]; int i=0;
    while (n > 0) { tmp[i++] = '0' + (n%10); n /= 10; }
    int k = 0;
    if (neg) buf[k++] = '-';
    for (int j=0; j<i; j++) buf[k+j] = tmp[i-1-j];
    buf[k+i] = 0;
}

// string to int
int npl_atoi(const char *s) {
    int n = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') n = n*10 + (*s++ - '0');
    return neg ? -n : n;
}

// check apakah char adalah digit
int is_digit(char c) { return c >= '0' && c <= '9'; }

// ─── MEMORY MANAGER ─────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// NexOS — Free-List Memory Allocator (drop-in pengganti bump allocator)
// ─────────────────────────────────────────────────────────────────────────────
//
// ARSITEKTUR:
//   Header setiap blok disimpan IN-PLACE di heap (bukan di tabel eksternal).
//   Layout di memori:
//
//     [ FreeBlock header | ... data ... ] [ FreeBlock header | ... data ... ]
//      ^                                   ^
//      ptr yang balik dari kmalloc dimulai SETELAH header (user_ptr)
//
//   FreeBlock header (16 bytes, 8-byte aligned):
//     - magic   : 0xA110CA7E  → deteksi double-free / korupsi
//     - size    : ukuran DATA (tidak termasuk header)
//     - used    : 1 = aktif, 0 = bebas
//     - next    : pointer ke FreeBlock berikutnya di free list
//
// STRATEGI:
//   - Alloc  : First-fit search di free list → split kalau sisa ≥ MIN_SPLIT
//   - Free   : Tandai used=0, masuk ke free list, coalesce blok adjacent
//   - Coalesce: Merge blok bebas yang bersebelahan → mencegah fragmentasi
//
// STATS (kompatibel dengan cmd_np_mem yang sudah ada):
//   mem_total()      → total heap size
//   mem_used()       → bytes di blok yang sedang dipakai (header tidak dihitung)
//   mem_free_bytes() → bytes yang benar-benar bisa dipakai
//   mem_freed()      → alias mem_free_bytes() (kompatibilitas)
//   mem_allocated()  → total heap - free (berbeda dari bump allocator)
// ─────────────────────────────────────────────────────────────────────────────

// ──────────────── KONFIGURASI ───────────────────────────────────────────────

#define MEM_ALIGN       8           // Semua alokasi 8-byte aligned
#define BLOCK_MAGIC     0xA110CA7EU // Magic number untuk validasi header
#define MIN_SPLIT       (sizeof(FreeBlock) + 8)  // Minimum sisa untuk di-split

// ──────────────── TIPE DATA ─────────────────────────────────────────────────

typedef struct FreeBlock {
    unsigned int        magic;  // BLOCK_MAGIC — deteksi korupsi
    unsigned int        size;   // ukuran DATA saja (bytes, sudah aligned)
    int                 used;   // 1 = aktif, 0 = bebas
    struct FreeBlock   *next;   // linked list (semua blok, urut by address)
} FreeBlock;

// ──────────────── STATE INTERNAL ────────────────────────────────────────────

extern char __heap_start;
extern char __heap_end;

static FreeBlock *heap_head      = (FreeBlock*)0; // head linked list
static unsigned int g_heap_start = 0;
static unsigned int g_heap_end   = 0;

// ──────────────── HELPER ────────────────────────────────────────────────────

static unsigned int mem_align_up(unsigned int n) {
    return (n + MEM_ALIGN - 1) & ~(MEM_ALIGN - 1);
}

// Pointer ke byte pertama setelah blok b (header + data)
static FreeBlock* block_next_addr(FreeBlock *b) {
    return (FreeBlock*)((unsigned char*)b + sizeof(FreeBlock) + b->size);
}

// ──────────────── INIT ──────────────────────────────────────────────────────

void mem_init() {
    g_heap_start = (unsigned int)&__heap_start;
    g_heap_end   = (unsigned int)&__heap_end;

    // Seluruh heap jadi satu blok bebas raksasa
    heap_head        = (FreeBlock*)g_heap_start;
    heap_head->magic = BLOCK_MAGIC;
    heap_head->size  = (g_heap_end - g_heap_start) - sizeof(FreeBlock);
    heap_head->used  = 0;
    heap_head->next  = (FreeBlock*)0;
}

// ──────────────── COALESCE ──────────────────────────────────────────────────
//
// Dipanggil setelah kfree(). Scan linked list dari head, merge setiap
// pasangan blok bebas yang alamatnya bersebelahan.
// Contoh:
//   [FREE 64B] → [FREE 128B] → [USED 32B]
//   setelah coalesce:
//   [FREE 192B + sizeof(FreeBlock)] → [USED 32B]
//
static void mem_coalesce() {
    FreeBlock *cur = heap_head;
    while (cur && cur->next) {
        FreeBlock *nxt = cur->next;
        // Keduanya bebas DAN bersebelahan di memori?
        if (!cur->used && !nxt->used && block_next_addr(cur) == nxt) {
            // Merge: cur menyerap header + data nxt
            cur->size += sizeof(FreeBlock) + nxt->size;
            cur->next  = nxt->next;
            // Invalidate header nxt biar tidak ada yang pakai lagi
            nxt->magic = 0xDEADBEEFU;
            // Jangan advance cur — cek lagi pasangan baru (cur, cur->next)
        } else {
            cur = cur->next;
        }
    }
}

// ──────────────── KMALLOC ───────────────────────────────────────────────────

void* kmalloc(unsigned int size) {
    if (size == 0 || !heap_head) return (void*)0;

    unsigned int asize = mem_align_up(size);

    // First-fit: cari blok bebas pertama yang cukup besar
    FreeBlock *cur = heap_head;
    while (cur) {
        if (cur->magic != BLOCK_MAGIC) return (void*)0; // heap corrupt
        if (!cur->used && cur->size >= asize) break;
        cur = cur->next;
    }
    if (!cur) return (void*)0; // OOM

    // Split: kalau sisa setelah alokasi cukup besar, bikin blok baru
    unsigned int leftover = cur->size - asize;
    if (leftover >= MIN_SPLIT) {
        // Blok baru tepat setelah data kita
        FreeBlock *split    = (FreeBlock*)((unsigned char*)cur + sizeof(FreeBlock) + asize);
        split->magic        = BLOCK_MAGIC;
        split->size         = leftover - sizeof(FreeBlock);
        split->used         = 0;
        split->next         = cur->next;

        cur->size           = asize;
        cur->next           = split;
    }

    cur->used = 1;

    // Kembalikan pointer ke DATA (setelah header)
    return (void*)((unsigned char*)cur + sizeof(FreeBlock));
}

// ──────────────── KFREE ─────────────────────────────────────────────────────

void kfree(void *ptr) {
    if (!ptr) return;

    // Header ada SEBELUM pointer yang dikembalikan ke user
    FreeBlock *b = (FreeBlock*)((unsigned char*)ptr - sizeof(FreeBlock));

    // Validasi
    if (b->magic != BLOCK_MAGIC) return; // bukan blok valid / sudah corrupt
    if (!b->used) return;                // double-free protection

    b->used = 0;

    // Gabungkan blok bebas yang bersebelahan
    mem_coalesce();
}

// ──────────────── STATS ─────────────────────────────────────────────────────

unsigned int mem_total() {
    return g_heap_end - g_heap_start;
}

unsigned int mem_used() {
    unsigned int total = 0;
    FreeBlock *cur = heap_head;
    while (cur) {
        if (cur->used) total += cur->size;
        cur = cur->next;
    }
    return total;
}

unsigned int mem_free_bytes() {
    // Jumlahkan semua blok bebas
    unsigned int total = 0;
    FreeBlock *cur = heap_head;
    while (cur) {
        if (!cur->used) total += cur->size;
        cur = cur->next;
    }
    return total;
}

// Kompatibilitas dengan cmd_np_mem lama
unsigned int mem_freed() {
    return mem_free_bytes();
}

unsigned int mem_allocated() {
    return mem_total() - mem_free_bytes();
}

// Hitung jumlah blok (untuk debugging)
int mem_block_count() {
    int n = 0;
    FreeBlock *cur = heap_head;
    while (cur) { n++; cur = cur->next; }
    return n;
}

int mem_free_block_count() {
    int n = 0;
    FreeBlock *cur = heap_head;
    while (cur) { if (!cur->used) n++; cur = cur->next; }
    return n;
}

// ──────────────── CMD: np mem (versi baru) ───────────────────────────────────
//
// Ganti blok cmd_np_mem() yang lama dengan ini.
// Menampilkan info tambahan: jumlah blok, fragmentasi, dll.
//
void cmd_np_mem() {
    unsigned int total  = mem_total();
    unsigned int used   = mem_used();
    unsigned int free_b = mem_free_bytes();
    int n_blocks        = mem_block_count();
    int n_free          = mem_free_block_count();
    char buf[16];

    uart_println("\r\n ┌──────────────────────────────────────┐");
    uart_println  ("   NexOS Memory Detail (Free-List)       ");
    uart_println  (" ├──────────────────────────────────────┤");

    uart_print("   RAM Total   : "); print_size(total);  uart_print("\r\n");
    uart_print("   RAM Used    : "); print_size(used);   uart_print("\r\n");
    uart_print("   RAM Free    : "); print_size(free_b); uart_print("\r\n");

    uart_println(" ├──────────────────────────────────────┤");

    // Progress bar
    int bar = total > 0 ? (int)((long)used * 20 / total) : 0;
    uart_print("   Usage  [");
    for (int i = 0; i < 20; i++) uart_putc(i < bar ? '#' : '.');
    uart_print("] ");
    npl_itoa(total > 0 ? (int)((long)used * 100 / total) : 0, buf);
    uart_print(buf);
    uart_println("%");

    uart_println(" ├──────────────────────────────────────┤");

    uart_print("   Blocks total: "); npl_itoa(n_blocks, buf); uart_println(buf);
    uart_print("   Blocks free : "); npl_itoa(n_free,   buf); uart_println(buf);
    uart_print("   Allocator   : Free-list + coalesce\r\n");
    uart_print("   Align       : 8-byte\r\n");
    uart_print("   Header size : "); npl_itoa(sizeof(FreeBlock), buf);
    uart_print(buf); uart_println(" bytes/block");

    uart_println(" └──────────────────────────────────────┘\r\n");
}


// ─── PACKAGE DATABASE ────────────────────
#define MAX_PKGS      16
#define PKG_NAME_LEN  32
#define PKG_VER_LEN   16
#define PKG_DESC_LEN  64

typedef enum {
    PKG_STATUS_AVAILABLE = 0,
    PKG_STATUS_INSTALLED,
} PkgStatus;

typedef struct {
    char      name[PKG_NAME_LEN];
    char      version[PKG_VER_LEN];
    char      desc[PKG_DESC_LEN];
    PkgStatus status;
    int       size_kb;
    int       is_local;
} Package;

static Package pkg_db[MAX_PKGS] = {
    {"python",   "3.11.0", "Python interpreter port",        PKG_STATUS_AVAILABLE, 8192, 0},
    {"lua",      "5.4.6",  "Lua scripting language",         PKG_STATUS_AVAILABLE,  256, 0},
    {"busybox",  "1.36.0", "Unix tools collection",          PKG_STATUS_AVAILABLE,  512, 0},
    {"vim",      "9.0",    "Text editor",                    PKG_STATUS_AVAILABLE, 1024, 0},
    {"curl",     "8.0.0",  "HTTP client tool",               PKG_STATUS_AVAILABLE,  384, 0},
    {"nsh",      "0.1.0",  "NexOS shell (built-in)",         PKG_STATUS_INSTALLED,   16, 1},
    {"nexfetch", "1.0.0",  "System info tool",               PKG_STATUS_AVAILABLE,    8, 1},
    {"calc",     "1.0.0",  "Simple calculator",              PKG_STATUS_AVAILABLE,    4, 1},
    {"cat",      "1.0.0",  "Print file contents",            PKG_STATUS_AVAILABLE,    2, 1},
    {"ls",       "1.0.0",  "List directory (stub)",          PKG_STATUS_AVAILABLE,    2, 1},
    {"",         "",       "",                               PKG_STATUS_AVAILABLE,    0, 0},
};
static int pkg_count = 10;

// ─── PACKAGE MANAGER (npl) ───────────────

Package* npl_find(const char *name) {
    for (int i = 0; i < pkg_count; i++) {
        if (npl_strcmp(pkg_db[i].name, name) == 0)
            return &pkg_db[i];
    }
    return (Package*)0;
}

void npl_cmd_list() {
    uart_println("\r\n Available packages:\r\n");
    uart_println(" NAME         VER       STATUS      SIZE     DESC");
    uart_println(" ─────────────────────────────────────────────────────");
    for (int i = 0; i < pkg_count; i++) {
        if (pkg_db[i].name[0] == 0) continue;
        uart_print(" ");
        uart_print(pkg_db[i].name);
        int pad = 13 - npl_strlen(pkg_db[i].name);
        for (int p=0; p<pad; p++) uart_putc(' ');
        uart_print(pkg_db[i].version);
        pad = 10 - npl_strlen(pkg_db[i].version);
        for (int p=0; p<pad; p++) uart_putc(' ');
        if (pkg_db[i].status == PKG_STATUS_INSTALLED)
            uart_print("[installed] ");
        else
            uart_print("[available] ");
        char szbuf[16];
        npl_itoa(pkg_db[i].size_kb, szbuf);
        uart_print(szbuf);
        uart_print("KB   ");
        uart_println(pkg_db[i].desc);
    }
    uart_print("\r\n");
}

void npl_cmd_install(const char *name) {
    uart_print("\r\n[npl] Looking for: ");
    uart_println(name);
    Package *pkg = npl_find(name);
    if (!pkg) {
        uart_print("[npl] ERR: Package '");
        uart_print(name);
        uart_println("' not found.");
        uart_println("[npl] Tip: coba 'npl search <nama>' atau 'npl list'");
        return;
    }
    if (pkg->status == PKG_STATUS_INSTALLED) {
        uart_print("[npl] '");
        uart_print(name);
        uart_println("' sudah terinstall.");
        return;
    }
    uart_print("[npl] Installing ");
    uart_print(pkg->name);
    uart_print(" v");
    uart_print(pkg->version);
    uart_print(" (");
    char szbuf[16];
    npl_itoa(pkg->size_kb, szbuf);
    uart_print(szbuf);
    uart_println("KB)...");
    if (pkg->is_local) {
        uart_println("[npl] Source: local");
    } else {
        uart_println("[npl] Source: remote (via QEMU semihosting)");
        uart_println("[npl] Fetching... [##########] 100%");
    }
    uart_print("[npl] Extracting  [");
    for (int i = 0; i < 20; i++) {
        uart_putc('#');
        for (volatile int d = 0; d < 2000000; d++);
    }
    uart_println("] Done!");
    pkg->status = PKG_STATUS_INSTALLED;
    uart_print("[npl] Successfully installed: ");
    uart_println(pkg->name);
    uart_print("\r\n");
}

void npl_cmd_remove(const char *name) {
    Package *pkg = npl_find(name);
    if (!pkg) {
        uart_print("[npl] ERR: '");
        uart_print(name);
        uart_println("' not found.");
        return;
    }
    if (pkg->status != PKG_STATUS_INSTALLED) {
        uart_print("[npl] '");
        uart_print(name);
        uart_println("' tidak terinstall.");
        return;
    }
    if (npl_strcmp(name, "nsh") == 0) {
        uart_println("[npl] ERR: Tidak bisa hapus nsh (core package).");
        return;
    }
    pkg->status = PKG_STATUS_AVAILABLE;
    uart_print("[npl] Removed: ");
    uart_println(name);
}

// Forward declare HTTP helpers (didefinisikan di bawah, dipakai npl_cmd_update)
static int http_tcp_connect(unsigned char ip[4], unsigned short port,
                             unsigned short local_port, const char *tag);

// ─── NPL UPDATE STATE ────────────────────
// npl update: one-command blocking (ARP + TCP + GET + parse)
// State enum dipertahankan untuk kompatibilitas internal
typedef enum {
    NPL_UPDATE_IDLE = 0,
    NPL_UPDATE_ARP,         // nunggu ARP resolve
    NPL_UPDATE_CONNECTING,  // SYN terkirim, nunggu SYN-ACK
    NPL_UPDATE_WAITING,     // GET terkirim, nunggu response
    NPL_UPDATE_DONE,
    NPL_UPDATE_ERROR,
} NplUpdateState;

static NplUpdateState npl_update_state = NPL_UPDATE_IDLE;
static unsigned char  npl_server_ip[4] = {10, 0, 2, 2};

// Parse satu baris package index dari server
// Format: "name|version|desc|size_kb\n"
// Return: 0=up-to-date, 1=updated, 2=new, -1=skip
static int npl_parse_pkg_line(const char *line) {
    char name[PKG_NAME_LEN]    = {0};
    char ver[PKG_VER_LEN]      = {0};
    char desc[PKG_DESC_LEN]    = {0};
    char size_s[8]             = {0};

    int i = 0, j = 0;
    while (line[i] && line[i] != '|' && j < PKG_NAME_LEN-1) name[j++] = line[i++];
    if (line[i] == '|') i++; j = 0;
    while (line[i] && line[i] != '|' && j < PKG_VER_LEN-1)  ver[j++]  = line[i++];
    if (line[i] == '|') i++; j = 0;
    while (line[i] && line[i] != '|' && j < PKG_DESC_LEN-1) desc[j++] = line[i++];
    if (line[i] == '|') i++; j = 0;
    while (line[i] && line[i] != '\n' && line[i] != '\r' && j < 7) size_s[j++] = line[i++];

    if (name[0] == 0) return -1;

    Package *pkg = npl_find(name);
    if (pkg) {
        int updated = 0;
        if (npl_strcmp(pkg->version, ver) != 0) {
            npl_strcpy(pkg->version, ver);
            updated = 1;
        }
        if (desc[0] && npl_strcmp(pkg->desc, desc) != 0) {
            npl_strcpy(pkg->desc, desc);
        }
        if (size_s[0]) pkg->size_kb = npl_atoi(size_s);
        return updated ? 1 : 0;
    } else {
        if (pkg_count < MAX_PKGS - 1) {
            Package *np = &pkg_db[pkg_count++];
            npl_strcpy(np->name,    name);
            npl_strcpy(np->version, ver);
            npl_strcpy(np->desc,    desc[0] ? desc : "Remote package");
            np->size_kb = size_s[0] ? npl_atoi(size_s) : 0;
            np->status  = PKG_STATUS_AVAILABLE;
            np->is_local = 0;
            return 2;
        }
        return -1;
    }
}

// Parse seluruh HTTP response — skip header, proses body line by line
// Print output bergaya apt: per-package status + summary di akhir
static void npl_parse_response(const unsigned char *data, unsigned int len) {
    // Cari "\r\n\r\n"
    unsigned int body_start = 0;
    for (unsigned int i = 0; i + 3 < len; i++) {
        if (data[i]=='\r' && data[i+1]=='\n' &&
            data[i+2]=='\r' && data[i+3]=='\n') {
            body_start = i + 4;
            break;
        }
    }
    if (body_start == 0) {
        uart_println("[npl] ERR: Tidak bisa parse HTTP response.");
        return;
    }

    int n_uptodate = 0, n_updated = 0, n_new = 0;
    char line[128];
    unsigned int li = 0;

    // Helper inline buat print satu entry
    // Kita buffer nama+ver buat print setelah parse
    char e_name[PKG_NAME_LEN], e_ver[PKG_VER_LEN], e_oldver[PKG_VER_LEN];

    for (unsigned int i = body_start; i <= len; i++) {
        char c = (i < len) ? (char)data[i] : '\n';
        if (c == '\n' || c == '\r') {
            if (li > 0) {
                line[li] = 0;
                if (line[0] == '#' || line[0] == 0) { li = 0; continue; }

                // Snapshot nama+ver sebelum parse buat print
                int fi = 0, fj = 0;
                e_name[0] = e_ver[0] = e_oldver[0] = 0;
                while (line[fi] && line[fi] != '|' && fj < PKG_NAME_LEN-1)
                    e_name[fj++] = line[fi++];
                e_name[fj] = 0;
                if (line[fi] == '|') fi++; fj = 0;
                while (line[fi] && line[fi] != '|' && fj < PKG_VER_LEN-1)
                    e_ver[fj++] = line[fi++];
                e_ver[fj] = 0;

                // Snapshot versi lama kalau ada
                Package *existing = npl_find(e_name);
                if (existing) npl_strcpy(e_oldver, existing->version);

                int r = npl_parse_pkg_line(line);
                if (r == 0) {
                    uart_print("  Hit: "); uart_print(e_name);
                    uart_print(" "); uart_println(e_ver);
                    n_uptodate++;
                } else if (r == 1) {
                    uart_print("  Get: "); uart_print(e_name);
                    uart_print(" "); uart_print(e_oldver);
                    uart_print(" -> "); uart_println(e_ver);
                    n_updated++;
                } else if (r == 2) {
                    uart_print("  New: "); uart_print(e_name);
                    uart_print(" "); uart_println(e_ver);
                    n_new++;
                }
                li = 0;
            }
        } else if (li < 127) {
            line[li++] = c;
        }
    }

    // Summary
    char buf[8];
    uart_println("");
    uart_print("  "); npl_itoa(n_uptodate + n_updated + n_new, buf); uart_print(buf);
    uart_println(" packages checked.");
    if (n_updated > 0) {
        uart_print("  "); npl_itoa(n_updated, buf); uart_print(buf);
        uart_println(" packages can be upgraded.");
    }
    if (n_new > 0) {
        uart_print("  "); npl_itoa(n_new, buf); uart_print(buf);
        uart_println(" new packages available.");
    }
    if (n_updated == 0 && n_new == 0) {
        uart_println("  All packages are up to date.");
    }
}

void npl_cmd_update() {
    if (!g_net.ready) {
        uart_println("[npl] ERR: Network belum ready.");
        uart_println("[npl] Jalankan QEMU dengan -netdev user,id=net0 -device virtio-net-device,netdev=net0");
        return;
    }

    // ── Mirror list (QEMU usernet: satu server, tapi tetap tampil kayak apt) ──
    const char *mirrors[] = {
        "https://packages.npl.nexos.dev/nexos-main",
        "https://mirror.nexos.id/apt/nexos-main",
        "https://mirror.nexos.dev/apt/nexos-main",
        "http://10.0.2.2/packages",   // QEMU host — ini yang beneran dipake
    };
    const int mirror_count = 4;

    uart_println("");
    uart_println("Hit:1 http://10.0.2.2 nexos InRelease");
    uart_println("Testing the available mirrors:");

    // Simulasi mirror check (3 dari 4 berhasil, sesuai QEMU env)
    for (int mi = 0; mi < mirror_count; mi++) {
        uart_print("[*] (");
        char nb[4]; npl_itoa(mi == 0 ? 10 : 1, nb);
        uart_print(nb); uart_print(") ");
        uart_print(mirrors[mi]);
        // Mirror terakhir (QEMU host) selalu ok, sisanya variatif
        if (mi == 1) uart_println(": ok");
        else if (mi == 2) uart_println(": ok");
        else uart_println(": ok");
    }
    uart_println("");

    // ── Fetch package index ──
    uart_println("Fetching package index...");
    uart_print("Get:1 http://10.0.2.2 nexos/main npl-index ");

    // Progress dots selama connect
    if (http_tcp_connect(npl_server_ip, 80, 1236, "[npl]") < 0) return;

    const char *req = "GET /packages HTTP/1.0\r\nHost: 10.0.2.2\r\nUser-Agent: npl/1.0 NexOS/0.6\r\nConnection: close\r\n\r\n";
    if (tcp_send(&g_tcp, req, npl_strlen(req)) < 0) {
        uart_println("\r\n[npl] ERR: gagal kirim request.");
        return;
    }

    // Baca response + print progress dots
    unsigned int rg = 0;
    unsigned int dot_counter = 0;
    while (rg++ < 50000000U) {
        vnet_poll(&g_net, vnet_handle_frame_wrapper);
        if (tcp_poll(&g_tcp) < 0) {
            uart_println("\r\n[npl] ERR: retransmit gagal.");
            return;
        }
        // Print dot tiap ~500k iterasi
        if (++dot_counter >= 500000U) {
            uart_putc('.');
            dot_counter = 0;
        }
        if (g_tcp.state == TCP_CLOSED && g_tcp.rx_len == 0) break;
        if (g_tcp.rx_len > 0) rg = 0;
    }

    if (g_tcp.rx_len == 0) {
        uart_println("\r\n[npl] ERR: Tidak ada response dari server.");
        return;
    }

    char b[8]; npl_itoa((int)g_tcp.rx_len, b);
    uart_print(" ["); uart_print(b); uart_println(" B]");
    uart_println("");

    // ── Parse + print per-package status ──
    npl_parse_response(g_tcp.rx_buf, g_tcp.rx_len);
    g_tcp.rx_len = g_tcp.rx_head = g_tcp.rx_tail = 0;
    npl_update_state = NPL_UPDATE_DONE;
    uart_println("");
    uart_println("Reading package lists... Done");
    tcp_close(&g_tcp);
}


void npl_cmd_search(const char *query) {
    uart_print("\r\n[npl] Search results for '");
    uart_print(query);
    uart_println("':\r\n");
    int found = 0;
    for (int i = 0; i < pkg_count; i++) {
        if (pkg_db[i].name[0] == 0) continue;
        int match = 0;
        int qlen = npl_strlen(query);
        int nlen = npl_strlen(pkg_db[i].name);
        for (int j = 0; j <= nlen - qlen; j++) {
            if (npl_strncmp(pkg_db[i].name + j, query, qlen) == 0) {
                match = 1; break;
            }
        }
        if (match) {
            uart_print("  ");
            uart_print(pkg_db[i].name);
            uart_print(" v");
            uart_print(pkg_db[i].version);
            uart_print(" — ");
            uart_println(pkg_db[i].desc);
            found++;
        }
    }
    if (!found) {
        uart_print("  Tidak ada hasil untuk '");
        uart_print(query);
        uart_println("'");
    }
    uart_print("\r\n");
}

void npl_cmd_info(const char *name) {
    Package *pkg = npl_find(name);
    if (!pkg) {
        uart_print("[npl] Package '");
        uart_print(name);
        uart_println("' tidak ditemukan.");
        return;
    }
    uart_println("\r\n Package Info:");
    uart_print("  Name    : "); uart_println(pkg->name);
    uart_print("  Version : "); uart_println(pkg->version);
    uart_print("  Desc    : "); uart_println(pkg->desc);
    uart_print("  Status  : ");
    uart_println(pkg->status == PKG_STATUS_INSTALLED ? "Installed" : "Available");
    char szbuf[16]; npl_itoa(pkg->size_kb, szbuf);
    uart_print("  Size    : "); uart_print(szbuf); uart_println(" KB");
    uart_print("  Source  : ");
    uart_println(pkg->is_local ? "local" : "remote");
    uart_print("\r\n");
}

void npl_cmd_help() {
    uart_println("\r\n npl — NexOS Package Manager\r\n");
    uart_println("  npl list              — tampil semua package");
    uart_println("  npl install <name>    — install package");
    uart_println("  npl remove <name>     — hapus package");
    uart_println("  npl update            — update index");
    uart_println("  npl search <query>    — cari package");
    uart_println("  npl info <name>       — detail package");
    uart_println("  npl help              — bantuan ini");
    uart_print("\r\n");
}

// ─── VIRTUAL FILESYSTEM ──────────────────
// Mendukung direktori + file dengan path absolut

#define MAX_FILES    32
#define MAX_FILENAME 64
#define MAX_FILESIZE 256

#define VNODE_FILE 0
#define VNODE_DIR  1

typedef struct {
    char name[MAX_FILENAME];   // nama file/dir (relatif, e.g. "kernel.c")
    char path[MAX_FILENAME];   // path absolut, e.g. "/projects/kernel.c"
    char content[MAX_FILESIZE];
    int  size;
    int  used;
    int  type;                 // VNODE_FILE atau VNODE_DIR
} VFile;

static VFile vfs[MAX_FILES];
static int   vfs_count = 0;

// current working directory
static char cwd[MAX_FILENAME] = "/";

// ── path helpers ──────────────────────────

// Gabung parent + name jadi path, hasil ke out
void path_join(const char *parent, const char *name, char *out) {
    if (npl_strcmp(parent, "/") == 0) {
        out[0] = '/';
        npl_strcpy(out + 1, name);
    } else {
        npl_strcpy(out, parent);
        int l = npl_strlen(out);
        out[l] = '/';
        npl_strcpy(out + l + 1, name);
    }
}

// Resolve path: kalau name mulai '/' langsung, kalau tidak join dengan cwd
void path_resolve(const char *name, char *out) {
    if (!name || name[0] == 0) {
        npl_strcpy(out, cwd);
        return;
    }
    if (name[0] == '/') {
        npl_strcpy(out, name);
    } else {
        path_join(cwd, name, out);
    }
}

// Ambil basename dari path (bagian setelah '/' terakhir)
void path_basename(const char *p, char *out) {
    int last = 0, i = 0;
    while (p[i]) { if (p[i] == '/') last = i + 1; i++; }
    npl_strcpy(out, p + last);
}

// Ambil dirname dari path (bagian sebelum '/' terakhir)
void path_dirname(const char *p, char *out) {
    int last = 0, i = 0;
    while (p[i]) { if (p[i] == '/') last = i; i++; }
    if (last == 0) { out[0] = '/'; out[1] = 0; return; }
    for (int j = 0; j < last; j++) out[j] = p[j];
    out[last] = 0;
}

// ── VFS lookup ───────────────────────────

VFile* vfs_find_path(const char *abspath) {
    for (int i = 0; i < vfs_count; i++) {
        if (vfs[i].used && npl_strcmp(vfs[i].path, abspath) == 0)
            return &vfs[i];
    }
    return (VFile*)0;
}

// Compat: cari file berdasarkan nama relatif terhadap cwd
VFile* vfs_find(const char *name) {
    char abspath[MAX_FILENAME];
    path_resolve(name, abspath);
    return vfs_find_path(abspath);
}

// Buat node baru, return pointer atau NULL kalau penuh
VFile* vfs_alloc() {
    if (vfs_count >= MAX_FILES) return (VFile*)0;
    VFile *f = &vfs[vfs_count++];
    f->content[0] = 0;
    f->size = 0;
    f->used = 1;
    return f;
}

// ── Init VFS ─────────────────────────────
void vfs_init() {
    // root dir
    VFile *r = vfs_alloc();
    npl_strcpy(r->path, "/");
    npl_strcpy(r->name, "/");
    r->type = VNODE_DIR;

    // file bawaan di root
    VFile *f;

    f = vfs_alloc();
    npl_strcpy(f->path, "/readme.txt");
    npl_strcpy(f->name, "readme.txt");
    npl_strcpy(f->content, "Welcome to NexOS v0.2!\nBuilt by Risa.\n");
    f->size = 38; f->type = VNODE_FILE;

    f = vfs_alloc();
    npl_strcpy(f->path, "/motd.txt");
    npl_strcpy(f->name, "motd.txt");
    npl_strcpy(f->content, "Have a great day hacking NexOS!\n");
    f->size = 32; f->type = VNODE_FILE;

    f = vfs_alloc();
    npl_strcpy(f->path, "/version.txt");
    npl_strcpy(f->name, "version.txt");
    npl_strcpy(f->content, "NexOS 0.2 ARM Baremetal\n");
    f->size = 24; f->type = VNODE_FILE;
}

// ─── CALCULATOR ──────────────────────────
// Support: +, -, *, / dengan satu operator
// Contoh: "10+5", "100/4", "3*7"

typedef struct {
    int  a;
    char op;
    int  b;
    int  valid;
} CalcExpr;

CalcExpr calc_parse(const char *expr) {
    CalcExpr e = {0, 0, 0, 0};
    int i = 0;
    int neg_a = 0;

    // skip spasi
    while (expr[i] == ' ') i++;

    // tanda minus di depan angka pertama
    if (expr[i] == '-') { neg_a = 1; i++; }

    // parse angka pertama
    if (!is_digit(expr[i])) return e;
    while (is_digit(expr[i])) e.a = e.a*10 + (expr[i++]-'0');
    if (neg_a) e.a = -e.a;

    // skip spasi
    while (expr[i] == ' ') i++;

    // operator
    if (expr[i] != '+' && expr[i] != '-' &&
        expr[i] != '*' && expr[i] != '/') return e;
    e.op = expr[i++];

    // skip spasi
    while (expr[i] == ' ') i++;

    // tanda minus di depan angka kedua
    int neg_b = 0;
    if (expr[i] == '-') { neg_b = 1; i++; }

    // parse angka kedua
    if (!is_digit(expr[i])) return e;
    while (is_digit(expr[i])) e.b = e.b*10 + (expr[i++]-'0');
    if (neg_b) e.b = -e.b;

    e.valid = 1;
    return e;
}

void cmd_calc(const char *expr) {
    if (!expr || expr[0] == 0) {
        uart_println("[calc] Usage: calc <expr>  contoh: calc 10+5");
        return;
    }

    CalcExpr e = calc_parse(expr);
    if (!e.valid) {
        uart_println("[calc] ERR: Ekspresi tidak valid.");
        uart_println("[calc] Format: <angka> <op> <angka>  (+, -, *, /)");
        return;
    }

    int result = 0;
    int err = 0;

    switch (e.op) {
        case '+': result = e.a + e.b; break;
        case '-': result = e.a - e.b; break;
        case '*': result = e.a * e.b; break;
        case '/':
            if (e.b == 0) {
                uart_println("[calc] ERR: Division by zero!");
                err = 1;
            } else {
                result = e.a / e.b;
            }
            break;
    }

    if (!err) {
        // print: a op b = result
        char buf[16];
        uart_print("  ");
        npl_itoa(e.a, buf); uart_print(buf);
        uart_putc(' '); uart_putc(e.op); uart_putc(' ');
        npl_itoa(e.b, buf); uart_print(buf);
        uart_print(" = ");
        npl_itoa(result, buf); uart_println(buf);

        // kalau ada sisa pembagian
        if (e.op == '/' && (e.a % e.b) != 0) {
            uart_print("  (sisa: ");
            npl_itoa(e.a % e.b, buf);
            uart_print(buf);
            uart_println(")");
        }
    }
}

// ─── COMMAND: cat ────────────────────────
void cmd_cat(const char *filename) {
    if (!filename || filename[0] == 0) {
        uart_println("[cat] Usage: cat <filename>");
        return;
    }
    char abspath[MAX_FILENAME];
    path_resolve(filename, abspath);
    VFile *f = vfs_find_path(abspath);
    if (!f || f->type != VNODE_FILE) {
        uart_print("[cat] ERR: File '");
        uart_print(abspath);
        uart_println("' tidak ditemukan.");
        return;
    }
    uart_print("\r\n");
    uart_print(f->content);
    uart_print("\r\n");
}

// ─── COMMAND: ls ─────────────────────────
void cmd_ls(const char *arg) {
    char target[MAX_FILENAME];
    if (!arg || arg[0] == 0) npl_strcpy(target, cwd);
    else path_resolve(arg, target);

    // pastikan target adalah dir
    VFile *td = vfs_find_path(target);
    if (!td || td->type != VNODE_DIR) {
        uart_print("[ls] ERR: '");
        uart_print(target);
        uart_println("': bukan direktori.");
        return;
    }

    uart_print("\r\n ");
    uart_print(target);
    uart_println(":\r\n");
    uart_println("  TYPE    SIZE    NAME");
    uart_println("  ───────────────────────────────");

    int count = 0;
    for (int i = 0; i < vfs_count; i++) {
        if (!vfs[i].used) continue;
        if (npl_strcmp(vfs[i].path, target) == 0) continue; // skip dir itu sendiri

        // cek apakah direct child dari target
        char par[MAX_FILENAME];
        path_dirname(vfs[i].path, par);
        if (npl_strcmp(par, target) != 0) continue;

        if (vfs[i].type == VNODE_DIR) {
            uart_print("  [dir]   -       ");
            uart_println(vfs[i].name);
        } else {
            char szbuf[16];
            uart_print("  [file]  ");
            npl_itoa(vfs[i].size, szbuf);
            uart_print(szbuf);
            int pad = 8 - npl_strlen(szbuf);
            for (int p = 0; p < pad; p++) uart_putc(' ');
            uart_println(vfs[i].name);
        }
        count++;
    }

    if (count == 0) uart_println("  (kosong)");
    uart_print("\r\n  ");
    char nbuf[8]; npl_itoa(count, nbuf);
    uart_print(nbuf);
    uart_println(" item(s).\r\n");
}

// ─── COMMAND: pwd ────────────────────────
void cmd_pwd() {
    uart_print("\r\n  ");
    uart_println(cwd);
    uart_print("\r\n");
}

// ─── COMMAND: cd ─────────────────────────
void cmd_cd(const char *path) {
    char target[MAX_FILENAME];

    if (!path || path[0] == 0) {
        npl_strcpy(cwd, "/");
        uart_print("  cwd: "); uart_println(cwd);
        return;
    }
    if (npl_strcmp(path, "..") == 0) {
        path_dirname(cwd, target);
        npl_strcpy(cwd, target);
        uart_print("  cwd: "); uart_println(cwd);
        return;
    }
    if (npl_strcmp(path, ".") == 0) {
        uart_print("  cwd: "); uart_println(cwd);
        return;
    }
    path_resolve(path, target);
    VFile *d = vfs_find_path(target);
    if (!d || !d->used) {
        uart_print("[cd] ERR: '"); uart_print(target); uart_println("': No such directory.");
        return;
    }
    if (d->type != VNODE_DIR) {
        uart_print("[cd] ERR: '"); uart_print(target); uart_println("': bukan direktori.");
        return;
    }
    npl_strcpy(cwd, target);
    uart_print("  cwd: "); uart_println(cwd);
}

// ─── COMMAND: mkdir ──────────────────────
void cmd_mkdir(const char *name) {
    if (!name || name[0] == 0) {
        uart_println("[mkdir] Usage: mkdir <dirname>");
        return;
    }
    char abspath[MAX_FILENAME];
    path_resolve(name, abspath);

    if (vfs_find_path(abspath)) {
        uart_print("[mkdir] ERR: '"); uart_print(abspath); uart_println("' sudah ada.");
        return;
    }
    // cek parent ada dan merupakan dir
    char par[MAX_FILENAME];
    path_dirname(abspath, par);
    VFile *pd = vfs_find_path(par);
    if (!pd || pd->type != VNODE_DIR) {
        uart_print("[mkdir] ERR: Parent dir '"); uart_print(par); uart_println("' tidak ada.");
        return;
    }
    VFile *d = vfs_alloc();
    if (!d) { uart_println("[mkdir] ERR: VFS penuh."); return; }
    npl_strcpy(d->path, abspath);
    path_basename(abspath, d->name);
    d->type = VNODE_DIR;
    uart_print("[mkdir] Dibuat: "); uart_println(abspath);
}

// ─── COMMAND: touch ──────────────────────
void cmd_touch(const char *name) {
    if (!name || name[0] == 0) {
        uart_println("[touch] Usage: touch <filename>");
        return;
    }
    char abspath[MAX_FILENAME];
    path_resolve(name, abspath);

    if (vfs_find_path(abspath)) {
        uart_print("[touch] '"); uart_print(abspath); uart_println("' sudah ada.");
        return;
    }
    char par[MAX_FILENAME];
    path_dirname(abspath, par);
    VFile *pd = vfs_find_path(par);
    if (!pd || pd->type != VNODE_DIR) {
        uart_print("[touch] ERR: Direktori '"); uart_print(par); uart_println("' tidak ada.");
        return;
    }
    VFile *f = vfs_alloc();
    if (!f) { uart_println("[touch] ERR: VFS penuh."); return; }
    npl_strcpy(f->path, abspath);
    path_basename(abspath, f->name);
    f->type = VNODE_FILE;
    uart_print("[touch] Dibuat: "); uart_println(abspath);
}

// ─── COMMAND: write ──────────────────────
void cmd_write(const char *name, const char *content) {
    if (!name || name[0] == 0) {
        uart_println("[write] Usage: write <filename> <content>");
        return;
    }
    char abspath[MAX_FILENAME];
    path_resolve(name, abspath);
    VFile *f = vfs_find_path(abspath);
    if (!f) {
        char par[MAX_FILENAME];
        path_dirname(abspath, par);
        VFile *pd = vfs_find_path(par);
        if (!pd || pd->type != VNODE_DIR) {
            uart_print("[write] ERR: Dir '"); uart_print(par); uart_println("' tidak ada.");
            return;
        }
        f = vfs_alloc();
        if (!f) { uart_println("[write] ERR: VFS penuh."); return; }
        npl_strcpy(f->path, abspath);
        path_basename(abspath, f->name);
        f->type = VNODE_FILE;
    }
    int len = npl_strlen(content);
    if (len >= MAX_FILESIZE - 1) len = MAX_FILESIZE - 2;
    for (int i = 0; i < len; i++) f->content[i] = content[i];
    f->content[len] = '\n'; f->content[len+1] = 0;
    f->size = len + 1;
    uart_print("[write] Ditulis ke '"); uart_print(abspath); uart_println("'.");
}

// ─── COMMAND: rm ─────────────────────────
void cmd_rm(const char *name) {
    if (!name || name[0] == 0) {
        uart_println("[rm] Usage: rm <filename>");
        return;
    }
    char abspath[MAX_FILENAME];
    path_resolve(name, abspath);
    VFile *f = vfs_find_path(abspath);
    if (!f) {
        uart_print("[rm] ERR: '"); uart_print(abspath); uart_println("' tidak ditemukan.");
        return;
    }
    if (f->type == VNODE_DIR) {
        // cek apakah kosong
        for (int i = 0; i < vfs_count; i++) {
            if (!vfs[i].used || npl_strcmp(vfs[i].path, abspath) == 0) continue;
            char par[MAX_FILENAME];
            path_dirname(vfs[i].path, par);
            if (npl_strcmp(par, abspath) == 0) {
                uart_println("[rm] ERR: Direktori tidak kosong. Hapus isinya dulu.");
                return;
            }
        }
    }
    f->used = 0;
    f->name[0] = 0; f->path[0] = 0;
    uart_print("[rm] Dihapus: "); uart_println(abspath);
}

// ─── COMMAND: cp ─────────────────────────
void cmd_cp(const char *src, const char *dst) {
    if (!src || src[0] == 0 || !dst || dst[0] == 0) {
        uart_println("[cp] Usage: cp <src> <dst>");
        return;
    }
    char asrc[MAX_FILENAME], adst[MAX_FILENAME];
    path_resolve(src, asrc);
    path_resolve(dst, adst);

    VFile *sf = vfs_find_path(asrc);
    if (!sf || sf->type != VNODE_FILE) {
        uart_print("[cp] ERR: '"); uart_print(asrc); uart_println("': file tidak ditemukan.");
        return;
    }
    // kalau dst adalah dir, copy ke dalam dir itu
    VFile *dd = vfs_find_path(adst);
    if (dd && dd->type == VNODE_DIR) {
        char bname[MAX_FILENAME];
        path_basename(asrc, bname);
        path_join(adst, bname, adst);
    }
    if (vfs_find_path(adst)) {
        uart_print("[cp] ERR: '"); uart_print(adst); uart_println("' sudah ada.");
        return;
    }
    char par[MAX_FILENAME];
    path_dirname(adst, par);
    VFile *pd = vfs_find_path(par);
    if (!pd || pd->type != VNODE_DIR) {
        uart_print("[cp] ERR: Dir '"); uart_print(par); uart_println("' tidak ada.");
        return;
    }
    VFile *nf = vfs_alloc();
    if (!nf) { uart_println("[cp] ERR: VFS penuh."); return; }
    npl_strcpy(nf->path, adst);
    path_basename(adst, nf->name);
    npl_strcpy(nf->content, sf->content);
    nf->size = sf->size;
    nf->type = VNODE_FILE;
    uart_print("[cp] "); uart_print(asrc); uart_print(" -> "); uart_println(adst);
}

// ─── COMMAND: mv ─────────────────────────
void cmd_mv(const char *src, const char *dst) {
    if (!src || src[0] == 0 || !dst || dst[0] == 0) {
        uart_println("[mv] Usage: mv <src> <dst>");
        return;
    }
    char asrc[MAX_FILENAME], adst[MAX_FILENAME];
    path_resolve(src, asrc);
    path_resolve(dst, adst);

    VFile *sf = vfs_find_path(asrc);
    if (!sf) {
        uart_print("[mv] ERR: '"); uart_print(asrc); uart_println("': tidak ditemukan.");
        return;
    }
    // kalau dst adalah dir, pindah ke dalam dir itu
    VFile *dd = vfs_find_path(adst);
    if (dd && dd->type == VNODE_DIR) {
        char bname[MAX_FILENAME];
        path_basename(asrc, bname);
        path_join(adst, bname, adst);
    }
    if (vfs_find_path(adst)) {
        uart_print("[mv] ERR: '"); uart_print(adst); uart_println("' sudah ada.");
        return;
    }
    char par[MAX_FILENAME];
    path_dirname(adst, par);
    VFile *pd = vfs_find_path(par);
    if (!pd || pd->type != VNODE_DIR) {
        uart_print("[mv] ERR: Dir '"); uart_print(par); uart_println("' tidak ada.");
        return;
    }
    uart_print("[mv] "); uart_print(asrc); uart_print(" -> "); uart_println(adst);
    npl_strcpy(sf->path, adst);
    path_basename(adst, sf->name);
}

// ─── COMMAND: find ───────────────────────
void cmd_find(const char *query) {
    if (!query || query[0] == 0) {
        uart_println("[find] Usage: find <nama>");
        return;
    }
    uart_print("\r\n[find] Mencari '");
    uart_print(query);
    uart_println("':\r\n");
    int found = 0;
    int qlen = npl_strlen(query);
    for (int i = 0; i < vfs_count; i++) {
        if (!vfs[i].used || vfs[i].type == VNODE_DIR) continue;
        // substring search di nama file
        int nlen = npl_strlen(vfs[i].name);
        for (int j = 0; j <= nlen - qlen; j++) {
            if (npl_strncmp(vfs[i].name + j, query, qlen) == 0) {
                uart_print("  "); uart_println(vfs[i].path);
                found++;
                break;
            }
        }
    }
    if (!found) {
        uart_print("  Tidak ditemukan: '"); uart_print(query); uart_println("'");
    } else {
        char buf[8]; npl_itoa(found, buf);
        uart_print("\r\n  "); uart_print(buf); uart_println(" file ditemukan.");
    }
    uart_print("\r\n");
}

// ─── COMMAND: tree ───────────────────────
// Rekursif print tree dari suatu path
void tree_print(const char *dirpath, const char *prefix, int *total_files, int *total_dirs) {
    // kumpulkan children
    char children[MAX_FILES][MAX_FILENAME];
    int  child_type[MAX_FILES]; // 0=file, 1=dir
    int  nchild = 0;

    for (int i = 0; i < vfs_count; i++) {
        if (!vfs[i].used) continue;
        if (npl_strcmp(vfs[i].path, dirpath) == 0) continue;
        char par[MAX_FILENAME];
        path_dirname(vfs[i].path, par);
        if (npl_strcmp(par, dirpath) != 0) continue;
        npl_strcpy(children[nchild], vfs[i].name);
        child_type[nchild] = (vfs[i].type == VNODE_DIR) ? 1 : 0;
        nchild++;
    }

    for (int i = 0; i < nchild; i++) {
        int last = (i == nchild - 1);
        uart_print(prefix);
        uart_print(last ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ");
        if (child_type[i] == VNODE_DIR) {
            uart_print("\033[34m"); // biru untuk dir
            uart_print(children[i]);
            uart_println("/\033[0m");
            (*total_dirs)++;
            // rekursi
            char newpath[MAX_FILENAME];
            path_join(dirpath, children[i], newpath);
            char newprefix[MAX_FILENAME];
            npl_strcpy(newprefix, prefix);
            npl_strcat(newprefix, last ? "    " : "\xe2\x94\x82   ");
            tree_print(newpath, newprefix, total_files, total_dirs);
        } else {
            uart_println(children[i]);
            (*total_files)++;
        }
    }
}

void cmd_tree(const char *arg) {
    char target[MAX_FILENAME];
    if (!arg || arg[0] == 0) npl_strcpy(target, cwd);
    else path_resolve(arg, target);

    VFile *d = vfs_find_path(target);
    if (!d || d->type != VNODE_DIR) {
        uart_print("[tree] ERR: '"); uart_print(target); uart_println("': bukan direktori.");
        return;
    }

    uart_print("\r\n\033[34m"); uart_print(target); uart_println("/\033[0m");
    int tf = 0, td = 0;
    tree_print(target, "", &tf, &td);

    uart_print("\r\n  ");
    char buf[8];
    npl_itoa(td, buf); uart_print(buf); uart_print(" dir, ");
    npl_itoa(tf, buf); uart_print(buf); uart_println(" file.\r\n");
}

// ─── COMMAND: stat ───────────────────────
void cmd_stat(const char *name) {
    if (!name || name[0] == 0) {
        uart_println("[stat] Usage: stat <file|dir>");
        return;
    }
    char abspath[MAX_FILENAME];
    path_resolve(name, abspath);
    VFile *f = vfs_find_path(abspath);
    if (!f || !f->used) {
        uart_print("[stat] ERR: '");
        uart_print(abspath);
        uart_println("': tidak ditemukan.");
        return;
    }

    // hitung jumlah lines kalau file
    int lines = 0;
    if (f->type == VNODE_FILE && f->size > 0) {
        for (int i = 0; i < f->size; i++)
            if (f->content[i] == '\n') lines++;
    }

    // hitung children kalau dir
    int children = 0;
    if (f->type == VNODE_DIR) {
        for (int i = 0; i < vfs_count; i++) {
            if (!vfs[i].used) continue;
            if (npl_strcmp(vfs[i].path, abspath) == 0) continue;
            char par[MAX_FILENAME];
            path_dirname(vfs[i].path, par);
            if (npl_strcmp(par, abspath) == 0) children++;
        }
    }

    char buf[16];
    uart_println("\r\n ┌─────────────────────────────────┐");
    uart_print  ("   Name     : "); uart_println(f->name);
    uart_print  ("   Type     : ");
    uart_println(f->type == VNODE_DIR ? "Directory" : "File");
    uart_print  ("   Path     : "); uart_println(f->path);

    if (f->type == VNODE_FILE) {
        uart_print("   Size     : ");
        npl_itoa(f->size, buf); uart_print(buf); uart_println(" bytes");
        uart_print("   Lines    : ");
        npl_itoa(lines, buf); uart_print(buf); uart_println(" line(s)");
    } else {
        uart_print("   Items    : ");
        npl_itoa(children, buf); uart_print(buf); uart_println(" item(s)");
    }

    // parent dir
    char par[MAX_FILENAME];
    path_dirname(abspath, par);
    uart_print  ("   Parent   : "); uart_println(par);
    uart_println(" └─────────────────────────────────┘\r\n");
}

// ─── COMMAND: whoami ─────────────────────
void cmd_whoami() {
    uart_println("\r\n  User  : root");
    uart_println("  Host  : nexos");
    uart_println("  Arch  : ARM AArch32");
    uart_print("\r\n");
}

// ─── COMMAND: uptime ─────────────────────
// Pakai tick counter sederhana (busy-loop based)
static unsigned int boot_ticks = 0;

void cmd_uptime() {
    // setiap tick ~= 1 unit, bukan detik beneran di baremetal
    // tapi kita bisa tampil sebagai ilustrasi
    char buf[16];
    uart_print("\r\n  Boot ticks: ");
    npl_itoa((int)boot_ticks, buf);
    uart_print(buf);
    uart_println("  (bare-metal tick, bukan detik real)");
    uart_print("\r\n");
}

// ─── COMMAND: history ────────────────────
#define HISTORY_MAX  10
#define HISTORY_LEN  128

static char cmd_history[HISTORY_MAX][HISTORY_LEN];
static int  history_count = 0;

void history_add(const char *line) {
    if (!line || line[0] == 0) return;
    // geser kalau sudah penuh
    if (history_count >= HISTORY_MAX) {
        for (int i = 0; i < HISTORY_MAX - 1; i++)
            npl_strcpy(cmd_history[i], cmd_history[i+1]);
        history_count = HISTORY_MAX - 1;
    }
    npl_strcpy(cmd_history[history_count++], line);
}

void cmd_history_print() {
    uart_println("\r\n Command History:\r\n");
    if (history_count == 0) {
        uart_println("  (kosong)");
    }
    for (int i = 0; i < history_count; i++) {
        char nbuf[8];
        uart_print("  ");
        npl_itoa(i+1, nbuf);
        uart_print(nbuf);
        uart_print("  ");
        uart_println(cmd_history[i]);
    }
    uart_print("\r\n");
}

// ─── COMMAND: env ────────────────────────
// Environment variables sederhana (key=value)
#define MAX_ENVS    8
#define ENV_KEY_LEN 32
#define ENV_VAL_LEN 64

typedef struct {
    char key[ENV_KEY_LEN];
    char val[ENV_VAL_LEN];
    int  used;
} EnvVar;

static EnvVar env_table[MAX_ENVS] = {
    {"PATH",    "/bin:/usr/bin",   1},
    {"HOME",    "/",               1},
    {"USER",    "root",            1},
    {"SHELL",   "nsh",             1},
    {"OS",      "NexOS",           1},
    {"VERSION", "0.2",             1},
    {"",        "",                0},
};
static int env_count = 6;

EnvVar* env_find(const char *key) {
    for (int i = 0; i < env_count; i++)
        if (env_table[i].used && npl_strcmp(env_table[i].key, key) == 0)
            return &env_table[i];
    return (EnvVar*)0;
}

// export KEY=VALUE
void cmd_export(const char *arg) {
    if (!arg || arg[0] == 0) {
        uart_println("[export] Usage: export KEY=VALUE");
        return;
    }
    // cari tanda '='
    char key[ENV_KEY_LEN] = {0};
    char val[ENV_VAL_LEN] = {0};
    int i = 0;
    while (arg[i] && arg[i] != '=') { key[i] = arg[i]; i++; }
    key[i] = 0;
    if (arg[i] == '=') {
        int j = 0;
        i++;
        while (arg[i]) val[j++] = arg[i++];
        val[j] = 0;
    }
    if (key[0] == 0) {
        uart_println("[export] ERR: Key tidak boleh kosong.");
        return;
    }
    EnvVar *e = env_find(key);
    if (e) {
        npl_strcpy(e->val, val);
    } else {
        if (env_count >= MAX_ENVS) {
            uart_println("[export] ERR: Tabel env penuh.");
            return;
        }
        npl_strcpy(env_table[env_count].key, key);
        npl_strcpy(env_table[env_count].val, val);
        env_table[env_count].used = 1;
        env_count++;
    }
    uart_print("[export] ");
    uart_print(key);
    uart_print("=");
    uart_println(val);
}

void cmd_env() {
    uart_println("\r\n Environment Variables:\r\n");
    for (int i = 0; i < env_count; i++) {
        if (!env_table[i].used) continue;
        uart_print("  ");
        uart_print(env_table[i].key);
        uart_print("=");
        uart_println(env_table[i].val);
    }
    uart_print("\r\n");
}

// ─── NEXFETCH ────────────────────────────
void cmd_nexfetch() {
    uart_println("\r\n  ███╗   ██╗███████╗██╗  ██╗ ██████╗ ███████╗");
    uart_println("  ████╗  ██║██╔════╝╚██╗██╔╝██╔═══██╗██╔════╝");
    uart_println("  ██╔██╗ ██║█████╗   ╚███╔╝ ██║   ██║███████╗");
    uart_println("  ██║╚██╗██║██╔══╝   ██╔██╗ ██║   ██║╚════██║");
    uart_println("  ██║ ╚████║███████╗██╔╝ ██╗╚██████╔╝███████║");
    uart_println("  ╚═╝  ╚═══╝╚══════╝╚═╝  ╚═╝ ╚═════╝ ╚══════╝\r\n");
    uart_println("  OS      : NexOS v0.2");
    uart_println("  Kernel  : NexOS ARM Baremetal");
    uart_println("  CPU     : ARM Cortex-A15 (AArch32)");
    uart_println("  UART    : PL011 @ 0x09000000");
    uart_println("  Machine : QEMU ARM virt");
    uart_println("  Author  : Risa");
    uart_println("  Shell   : nsh v0.2");
    char fbuf[8];
    uart_print("  Files   : ");
    npl_itoa(vfs_count, fbuf);
    uart_print(fbuf);
    uart_println(" file(s) in VFS");
    uart_print("\r\n");
}

// ─── COMMAND: npinfo ─────────────────────
void cmd_npinfo() {
    char buf[16];
    uart_println("\r\n ┌──────────────────────────────────────┐");
    uart_println  ("   NexOS System Info                     ");
    uart_println  (" ├──────────────────────────────────────┤");
    uart_println  ("   OS       : NexOS v0.3");
    uart_println  ("   Kernel   : ARM Baremetal");
    uart_println  ("   CPU      : ARM Cortex-A15 (AArch32)");
    uart_println  ("   Machine  : QEMU ARM virt");
    uart_println  ("   UART     : PL011 @ 0x09000000");
    uart_println  ("   Author   : Risa");
    uart_print    ("   Shell    : nsh v0.3\r\n");
    uart_println  (" ├──────────────────────────────────────┤");
    uart_print    ("   RAM      : "); print_size(mem_total()); uart_print(" total, ");
    print_size(mem_used()); uart_print(" used, ");
    print_size(mem_free_bytes()); uart_print(" free\r\n");
    uart_print    ("   VFS      : ");
    npl_itoa(vfs_count, buf); uart_print(buf); uart_println(" node(s)");
    uart_print    ("   Packages : ");
    int inst = 0;
    for (int i = 0; i < pkg_count; i++)
        if (pkg_db[i].status == PKG_STATUS_INSTALLED) inst++;
    npl_itoa(inst, buf); uart_print(buf); uart_print(" installed / ");
    npl_itoa(pkg_count, buf); uart_print(buf); uart_println(" total");
    uart_println  (" └──────────────────────────────────────┘\r\n");
}

// ─── COMMAND: np stat ────────────────────
void cmd_np_stat() {
    char buf[16];
    uart_println("\r\n ╔══════════════════════════════════════╗");
    uart_println  ("   NexOS Full System Status              ");
    uart_println  (" ╠══════════════════════════════════════╣");

    // -- Memory --
    uart_println  ("   [MEMORY]");
    uart_print    ("   Total    : "); print_size(mem_total());       uart_print("\r\n");
    uart_print    ("   Used     : "); print_size(mem_used());       uart_print("\r\n");
    uart_print    ("   Free     : "); print_size(mem_free_bytes()); uart_print("\r\n");
    uart_print    ("   Blocks   : ");
    npl_itoa(mem_block_count(), buf); uart_print(buf);
    uart_print(" total, "); npl_itoa(mem_free_block_count(), buf); uart_print(buf); uart_println(" free");

    uart_println  (" ╠══════════════════════════════════════╣");

    // -- VFS --
    uart_println  ("   [VFS]");
    int files = 0, dirs = 0;
    for (int i = 0; i < vfs_count; i++) {
        if (!vfs[i].used) continue;
        if (vfs[i].type == VNODE_FILE) files++;
        else dirs++;
    }
    uart_print("   Files    : "); npl_itoa(files, buf); uart_println(buf);
    uart_print("   Dirs     : "); npl_itoa(dirs,  buf); uart_println(buf);
    uart_print("   CWD      : "); uart_println(cwd);

    uart_println  (" ╠══════════════════════════════════════╣");

    // -- Packages --
    uart_println  ("   [PACKAGES]");
    int inst = 0, avail = 0;
    for (int i = 0; i < pkg_count; i++) {
        if (pkg_db[i].status == PKG_STATUS_INSTALLED) inst++;
        else avail++;
    }
    uart_print("   Installed : "); npl_itoa(inst,  buf); uart_println(buf);
    uart_print("   Available : "); npl_itoa(avail, buf); uart_println(buf);

    uart_println  (" ╠══════════════════════════════════════╣");

    // -- Env --
    uart_println  ("   [ENVIRONMENT]");
    uart_print("   Vars     : "); npl_itoa(env_count, buf); uart_println(buf);

    uart_println  (" ╠══════════════════════════════════════╣");

    // -- Shell --
    uart_println  ("   [SHELL]");
    uart_print("   History  : "); npl_itoa(history_count, buf); uart_print(buf); uart_println(" cmd(s)");
    uart_print("   Uptime   : "); npl_itoa((int)boot_ticks, buf); uart_print(buf); uart_println(" tick(s)");

    uart_println  (" ╚══════════════════════════════════════╝\r\n");
}

// ─── COMMAND: ifconfig ───────────────────
void cmd_ifconfig() {
    if (!g_net.ready) {
        uart_println("[net] ERR: virtio-net belum ready.");
        uart_println("[net] Pastikan QEMU dijalankan dengan flag:");
        uart_println("[net]   -netdev user,id=net0 -device virtio-net-device,netdev=net0");
        return;
    }
    uart_println("\r\n eth0:");
    uart_print  ("   MAC  : "); vnet_print_mac(g_net.mac); uart_print("\r\n");
    uart_print  ("   IP   : "); vnet_print_ip(g_net.ip);   uart_print("\r\n");
    uart_print  ("   GW   : "); vnet_print_ip(g_net.gateway); uart_print("\r\n");
    uart_print  ("   Mask : "); vnet_print_ip(g_net.netmask); uart_print("\r\n");
    uart_print  ("   TX   : "); char buf[16]; npl_itoa((int)g_net.tx_count,  buf); uart_print(buf); uart_println(" packets");
    uart_print  ("   RX   : ");                              npl_itoa((int)g_net.rx_count,  buf); uart_print(buf); uart_println(" packets");
    uart_print  ("   Drop : ");                              npl_itoa((int)g_net.rx_dropped, buf); uart_print(buf); uart_println(" packets");
    uart_print("\r\n");
}

// ─── COMMAND: arp ────────────────────────
void cmd_arp() {
    if (!g_net.ready) {
        uart_println("[arp] Network belum ready.");
        return;
    }
    arp_print_table(&g_net.arp);
}

// ─── COMMAND: ping ───────────────────────
void cmd_ping(const char *ipstr) {
    if (!g_net.ready) {
        uart_println("[ping] ERR: Network belum ready.");
        return;
    }
    if (!ipstr || ipstr[0] == 0) {
        uart_println("[ping] Usage: ping <ip>  contoh: ping 10.0.2.2");
        return;
    }

    // Parse IP string "a.b.c.d"
    unsigned char target[4] = {0,0,0,0};
    int i = 0, oct = 0;
    while (ipstr[i] && oct < 4) {
        unsigned int n = 0;
        while (ipstr[i] >= '0' && ipstr[i] <= '9') n = n*10 + (ipstr[i++]-'0');
        target[oct++] = (unsigned char)n;
        if (ipstr[i] == '.') i++;
    }

    uart_print("[ping] PING "); vnet_print_ip(target); uart_print("\r\n");

    // Resolve MAC via ARP table
    ArpEntry *ae = arp_resolve(&g_net, target);
    if (!ae) {
        uart_println("[ping] ERR: ARP table penuh.");
        return;
    }
    if (ae->state == ARP_PENDING) {
        uart_println("[ping] ARP request dikirim, tunggu reply...");
        uart_println("[ping] Coba ping lagi setelah beberapa saat.");
        uart_println("[ping] Atau ketik 'arp' untuk lihat status table.");
        return;
    }

    // MAC sudah resolved — kirim ICMP echo request
    // Build frame langsung di sini
    unsigned char frame[sizeof(EthHdr) + sizeof(IpHdr) + sizeof(IcmpHdr) + 8];
    net_memset(frame, 0, sizeof(frame));

    EthHdr *eth = (EthHdr*)frame;
    net_memcpy(eth->dst, ae->mac,   ETH_ALEN);
    net_memcpy(eth->src, g_net.mac, ETH_ALEN);
    eth->ethertype = htons(0x0800);

    IpHdr *ip = (IpHdr*)(frame + sizeof(EthHdr));
    ip->ihl_ver   = 0x45;
    ip->ttl       = 64;
    ip->proto     = 1; // ICMP
    ip->total_len = htons(sizeof(IpHdr) + sizeof(IcmpHdr) + 8);
    net_memcpy(ip->src, g_net.ip, 4);
    net_memcpy(ip->dst, target,   4);
    ip->checksum  = ip_checksum(ip, sizeof(IpHdr));

    IcmpHdr *icmp = (IcmpHdr*)(frame + sizeof(EthHdr) + sizeof(IpHdr));
    icmp->type     = 8; // echo request
    icmp->code     = 0;
    icmp->id       = htons(0x1337);
    icmp->seq      = htons(1);
    // payload "NexOS!!!"
    unsigned char *payload = (unsigned char*)icmp + sizeof(IcmpHdr);
    const char *msg = "NexOS!!!";
    for (int j = 0; j < 8; j++) payload[j] = msg[j];
    icmp->checksum = ip_checksum(icmp, sizeof(IcmpHdr) + 8);

    vnet_send(&g_net, frame, sizeof(frame));
    uart_print("[ping] Echo request → "); vnet_print_ip(target);
    uart_print(" (MAC: "); vnet_print_mac(ae->mac); uart_print(")\r\n");
    uart_println("[ping] Ketik 'ping' lagi atau poll untuk lihat reply.");
}

// ─── COMMAND: net ────────────────────────
void cmd_net(const char *sub, const char *arg) {
    if (npl_strcmp(sub, "init") == 0) {
        // Parse IP dari arg atau pakai default QEMU user-net
        unsigned char ip[4] = {10, 0, 2, 15}; // QEMU user-net default guest IP
        if (arg && arg[0]) {
            int i = 0, oct = 0;
            while (arg[i] && oct < 4) {
                unsigned int n = 0;
                while (arg[i] >= '0' && arg[i] <= '9') n = n*10 + (arg[i++]-'0');
                ip[oct++] = (unsigned char)n;
                if (arg[i] == '.') i++;
            }
        }
        // Gateway & netmask default QEMU "-netdev user" (10.0.2.x/24, gw=10.0.2.2).
        // Kalau IP di-override manual ke subnet lain, gateway dianggap .1 di
        // subnet yang sama — asumsi paling umum buat jaringan rumahan/LAN.
        unsigned char gateway[4];
        unsigned char netmask[4] = {255, 255, 255, 0};
        if (ip[0] == 10 && ip[1] == 0 && ip[2] == 2) {
            gateway[0] = 10; gateway[1] = 0; gateway[2] = 2; gateway[3] = 2;
        } else {
            gateway[0] = ip[0]; gateway[1] = ip[1]; gateway[2] = ip[2]; gateway[3] = 1;
        }
        int r = vnet_init(&g_net, ip, gateway, netmask);
        if (r < 0) uart_println("[net] Init gagal.");
        else        uart_println("[net] Init berhasil!");

        // Init DNS resolver — QEMU "-netdev user" nyediain DNS forwarder
        // di 10.0.2.3 (beda dari gateway 10.0.2.2). Kalau subnet di-override
        // manual, tebak DNS server = .3 di subnet yang sama (konvensi QEMU).
        unsigned char dns_server[4];
        if (ip[0] == 10 && ip[1] == 0 && ip[2] == 2) {
            dns_server[0] = 10; dns_server[1] = 0; dns_server[2] = 2; dns_server[3] = 3;
        } else {
            dns_server[0] = ip[0]; dns_server[1] = ip[1]; dns_server[2] = ip[2]; dns_server[3] = 3;
        }
        dns_init(&g_dns, &g_net, dns_server);

    } else if (npl_strcmp(sub, "dhcp") == 0) {
        if (!g_net.ready) { uart_println("[net] Belum init. Jalankan: net init"); return; }
        uart_println("\n[dhcp] Memulai DORA...");
        dhcp_init(&g_dhcp, &g_net);
        if (dhcp_start(&g_dhcp) < 0) {
            uart_println("[dhcp] ERR: Gagal kirim DISCOVER.");
            return;
        }
        // Blocking poll loop
        unsigned int guard = 0;
        while (guard++ < 100000000U) {
            vnet_poll(&g_net, vnet_handle_frame_wrapper);
            DhcpState st = dhcp_poll(&g_dhcp);
            if (st == DHCP_STATE_BOUND) {
                // Apply DNS juga
                dns_init(&g_dns, &g_net, g_dhcp.dns);
                uart_println("[dhcp] DNS resolver updated.");
                break;
            }
            if (st == DHCP_STATE_FAILED) {
                uart_println("[dhcp] DHCP gagal. Coba 'net init' untuk static IP.");
                break;
            }
        }

    } else if (npl_strcmp(sub, "poll") == 0) {
        if (!g_net.ready) { uart_println("[net] Belum init."); return; }
        uart_println("[net] Manual poll RX...");
        vnet_poll(&g_net, vnet_handle_frame_wrapper);
        uart_println("[net] Poll selesai.");

    } else if (npl_strcmp(sub, "stat") == 0) {
        cmd_ifconfig();

    } else {
        uart_println("\r\n net — NexOS Network Commands\r\n");
        uart_println("  net init [ip]    — init virtio-net (default IP: 10.0.2.15)");
        uart_println("  net dhcp         — auto-configure IP via DHCP (DORA)");
        uart_println("  net stat         — tampil status interface");
        uart_println("  net poll         — manual poll RX packet");
        uart_println("\r\n Contoh QEMU flags:");
        uart_println("  -netdev user,id=net0 -device virtio-net-device,netdev=net0\r\n");
    }
}

// ─── COMMAND: dns ────────────────────────
// Format: dns <hostname>  — resolve hostname jadi IP via UDP/53
void cmd_dns(const char *hostname) {
    if (!g_net.ready) {
        uart_println("[dns] ERR: Network belum ready (jalankan: net init)");
        return;
    }
    if (!hostname || hostname[0] == 0) {
        uart_println("[dns] Usage: dns <hostname>");
        uart_println("[dns] Contoh: dns example.com");
        return;
    }

    if (dns_is_ip_literal(hostname)) {
        uart_println("[dns] Sudah berupa IP literal, gak perlu resolve.");
        return;
    }

    if (dns_resolve_start(&g_dns, hostname) < 0) return;

    // Loop blocking: poll RX + dns_poll sampai RESOLVED/FAILED.
    // Dibatasi iterasi maksimum supaya gak hang selamanya kalau ada bug
    // yang gak diantisipasi (pengaman tambahan di atas retry/timeout
    // internal dns_poll sendiri).
    unsigned int guard = 0;
    const unsigned int GUARD_MAX = 20000000U;

    while (guard++ < GUARD_MAX) {
        vnet_poll(&g_net, vnet_handle_frame_wrapper);
        DnsState st = dns_poll(&g_dns);
        if (st == DNS_RESOLVED || st == DNS_FAILED) break;
    }

    if (guard >= GUARD_MAX) {
        uart_println("[dns] ERR: guard limit tercapai, dibatalkan.");
        return;
    }

    dns_print_state(&g_dns);
}

// ─── SHARED: parse URL jadi host + path ──────────────────────────────────────
// url: "http://hostname/path" atau "http://1.2.3.4/path"
// out_host: buffer 128 byte untuk hostname/IP string
// out_path: buffer 256 byte untuk path (include leading '/')
// return 0 ok, -1 bad url
static int http_parse_url(const char *url, char *out_host, char *out_path) {
    const char *p = url;
    if (p[0]=='h'&&p[1]=='t'&&p[2]=='t'&&p[3]=='p'&&p[4]==':'&&p[5]=='/'&&p[6]=='/') p += 7;

    int hi = 0;
    while (*p && *p != '/' && hi < 127) out_host[hi++] = *p++;
    out_host[hi] = 0;
    if (hi == 0) return -1;

    int pi = 0;
    if (*p == '/') {
        while (*p && pi < 255) out_path[pi++] = *p++;
    }
    if (pi == 0) { out_path[0] = '/'; pi = 1; }
    out_path[pi] = 0;
    return 0;
}

// ─── SHARED: resolve host ke IP (DNS atau IP literal) ─────────────────────
// return 0 ok (ip terisi), -1 gagal
static int http_resolve_host(const char *host, unsigned char ip[4], const char *tag) {
    if (dns_is_ip_literal(host)) {
        return dns_parse_ip_literal(host, ip);
    }

    uart_print(tag); uart_print(" Resolving "); uart_print(host); uart_println("...");
    if (dns_resolve_start(&g_dns, host) < 0) {
        uart_print(tag); uart_println(" ERR: dns_resolve_start gagal.");
        return -1;
    }

    unsigned int guard = 0;
    const unsigned int GUARD_MAX = 20000000U;
    while (guard++ < GUARD_MAX) {
        vnet_poll(&g_net, vnet_handle_frame_wrapper);
        DnsState st = dns_poll(&g_dns);
        if (st == DNS_RESOLVED || st == DNS_FAILED) break;
    }

    if (g_dns.state != DNS_RESOLVED) {
        uart_print(tag); uart_println(" ERR: DNS gagal resolve host.");
        return -1;
    }

    ip[0] = g_dns.resolved_ip[0]; ip[1] = g_dns.resolved_ip[1];
    ip[2] = g_dns.resolved_ip[2]; ip[3] = g_dns.resolved_ip[3];
    return 0;
}

// ─── SHARED: TCP connect blocking ─────────────────────────────────────────
// return 0 ok, -1 gagal
static int http_tcp_connect(unsigned char ip[4], unsigned short port,
                             unsigned short local_port, const char *tag) {
    if (g_tcp.state != TCP_CLOSED) {
        uart_print(tag); uart_println(" ERR: TCP connection masih aktif (net poll?).");
        return -1;
    }
    tcp_init(&g_tcp, &g_net, local_port);

    // ARP resolve (blocking, max ~5 juta iter)
    unsigned int ag = 0;
    ArpEntry *ae = arp_resolve(&g_net, ip);
    while ((!ae || ae->state != ARP_RESOLVED) && ag++ < 5000000U) {
        vnet_poll(&g_net, vnet_handle_frame_wrapper);
        ae = arp_resolve(&g_net, ip);
    }
    if (!ae || ae->state != ARP_RESOLVED) {
        uart_print(tag); uart_println(" ERR: ARP timeout.");
        return -1;
    }

    if (tcp_connect(&g_tcp, ip, port) < 0) {
        uart_print(tag); uart_println(" ERR: gagal kirim SYN.");
        return -1;
    }

    // Tunggu ESTABLISHED — tcp_poll() handle SYN retransmit kalau RTO timeout
    unsigned int cg = 0;
    while (g_tcp.state == TCP_SYN_SENT && cg++ < 10000000U) {
        vnet_poll(&g_net, vnet_handle_frame_wrapper);
        if (tcp_poll(&g_tcp) < 0) break;  // max retry tercapai
    }
    if (g_tcp.state != TCP_ESTABLISHED) {
        uart_print(tag); uart_println(" ERR: TCP connect gagal (timeout/refused).");
        return -1;
    }
    return 0;
}

// ─── SHARED: kirim HTTP request + baca response ───────────────────────────
// method: "GET" atau "HEAD"
// show_headers: 1 = print headers, 0 = skip (body only)
// show_body:    1 = print body, 0 = skip (HEAD)
// return total bytes body (atau -1 error)
static int http_do_request(const char *method, const char *host,
                            const char *path, int show_headers, int show_body,
                            const char *tag) {
    // Build request
    char req[512];
    int i = 0;
    // METHOD SP path SP HTTP/1.0 CRLF
    const char *m = method; while (*m) req[i++] = *m++;
    req[i++] = ' ';
    const char *pp = path; while (*pp) req[i++] = *pp++;
    const char *ver = " HTTP/1.0\r\nHost: "; while (*ver) req[i++] = *ver++;
    const char *hh = host; while (*hh) req[i++] = *hh++;
    const char *ua = "\r\nUser-Agent: NexOS/0.6\r\nConnection: close\r\n\r\n";
    while (*ua) req[i++] = *ua++;
    req[i] = 0;

    if (tcp_send(&g_tcp, req, (unsigned int)i) < 0) {
        uart_print(tag); uart_println(" ERR: gagal kirim request.");
        return -1;
    }

    // Baca response (blocking sampai TCP closed atau buffer penuh)
    unsigned char buf[256];
    unsigned int total_body = 0;
    int in_headers = 1;

    // Print separator
    uart_println("");
    uart_println("────────────────────────────────────────");

    unsigned int rg = 0;
    while (rg++ < 50000000U) {
        vnet_poll(&g_net, vnet_handle_frame_wrapper);
        if (tcp_poll(&g_tcp) < 0) {
            uart_print(tag); uart_println(" ERR: retransmit gagal, koneksi direset.");
            break;
        }

        unsigned int n = tcp_read(&g_tcp, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;

            if (in_headers) {
                // Scan untuk end-of-headers (\r\n\r\n)
                for (unsigned int bi = 0; bi < n; bi++) {
                    if (in_headers) {
                        // Cek 4-byte sequence
                        if (bi + 3 < n &&
                            buf[bi]=='\r' && buf[bi+1]=='\n' &&
                            buf[bi+2]=='\r' && buf[bi+3]=='\n') {
                            // Print sisa header sebelum pemisah
                            if (show_headers) {
                                buf[bi+4] = 0;
                                uart_print((char*)buf);
                            }
                            in_headers = 0;
                            // Print body setelah \r\n\r\n
                            if (show_body && bi + 4 < n) {
                                unsigned char *body_start = buf + bi + 4;
                                unsigned int blen = n - (bi + 4);
                                body_start[blen] = 0;
                                uart_print((char*)body_start);
                                total_body += blen;
                            }
                            break;
                        }
                    }
                }
                if (in_headers && show_headers) {
                    uart_print((char*)buf);
                }
            } else {
                if (show_body) {
                    uart_print((char*)buf);
                    total_body += n;
                }
            }
            rg = 0; // reset guard tiap kali dapat data
        }

        if (g_tcp.state == TCP_CLOSED && n == 0) break;
    }

    uart_println("\r\n────────────────────────────────────────");
    return (int)total_body;
}

// ─── COMMAND: wget ───────────────────────────────────────────────────────────
// Usage: wget <url>
// One-command: DNS resolve (kalau hostname) → TCP connect → GET → print body
void cmd_wget(const char *url) {
    if (!g_net.ready) {
        uart_println("[wget] ERR: Network belum ready (jalankan: net init).");
        return;
    }
    if (!url || url[0] == 0) {
        uart_println("[wget] Usage: wget <url>");
        uart_println("[wget] Contoh: wget http://example.com/index.html");
        uart_println("[wget]         wget http://10.0.2.2/file.txt");
        return;
    }

    char host[128], path[256];
    if (http_parse_url(url, host, path) < 0) {
        uart_println("[wget] ERR: URL tidak valid. Format: http://host/path");
        return;
    }

    uart_print("[wget] URL  : "); uart_println(url);
    uart_print("[wget] Host : "); uart_println(host);
    uart_print("[wget] Path : "); uart_println(path);

    unsigned char ip[4];
    if (http_resolve_host(host, ip, "[wget]") < 0) return;
    uart_print("[wget] IP   : "); vnet_print_ip(ip); uart_println("");

    if (http_tcp_connect(ip, 80, 1234, "[wget]") < 0) return;
    uart_println("[wget] Connected. Sending GET...");

    int body_bytes = http_do_request("GET", host, path, 0, 1, "[wget]");
    if (body_bytes < 0) return;

    uart_print("[wget] Done. Body: ");
    char nb[12]; int n = body_bytes, k = 0;
    if (!n) { nb[k++] = '0'; } else { char t[12]; int j=0; while(n){t[j++]='0'+n%10;n/=10;} while(j--) nb[k++]=t[j+1]; }
    nb[k] = 0;
    uart_print(nb); uart_println(" bytes.");
}

// ─── COMMAND: curl ───────────────────────────────────────────────────────────
// Usage: curl <url>          — GET, print header + body
//        curl -I <url>       — HEAD only, print headers
//        curl -s <url>       — GET, body only (silent/no headers)
void cmd_curl(const char *arg1, const char *arg2) {
    if (!g_net.ready) {
        uart_println("[curl] ERR: Network belum ready (jalankan: net init).");
        return;
    }

    // Parse flag dan URL
    const char *url = NULL;
    int flag_head    = 0; // -I: HEAD request
    int flag_silent  = 0; // -s: body only

    if (!arg1 || arg1[0] == 0) {
        uart_println("[curl] Usage: curl [-I|-s] <url>");
        uart_println("[curl] Contoh: curl http://example.com");
        uart_println("[curl]         curl -I http://example.com");
        uart_println("[curl]         curl -s http://10.0.2.2/api");
        return;
    }

    if (arg1[0] == '-') {
        if (arg1[1] == 'I') flag_head   = 1;
        else if (arg1[1] == 's') flag_silent = 1;
        url = (arg2 && arg2[0]) ? arg2 : NULL;
    } else {
        url = arg1;
    }

    if (!url || url[0] == 0) {
        uart_println("[curl] ERR: URL tidak ditemukan.");
        return;
    }

    char host[128], path[256];
    if (http_parse_url(url, host, path) < 0) {
        uart_println("[curl] ERR: URL tidak valid.");
        return;
    }

    if (!flag_silent) {
        uart_print("[curl] URL  : "); uart_println(url);
        uart_print("[curl] Host : "); uart_println(host);
        uart_print("[curl] Path : "); uart_println(path);
        if (flag_head) uart_println("[curl] Mode : HEAD");
    }

    unsigned char ip[4];
    if (http_resolve_host(host, ip, "[curl]") < 0) return;
    if (!flag_silent) {
        uart_print("[curl] IP   : "); vnet_print_ip(ip); uart_println("");
    }

    if (http_tcp_connect(ip, 80, 1235, "[curl]") < 0) return;
    if (!flag_silent) uart_println("[curl] Connected.");

    // curl default: show headers + body. -I: header only. -s: body only.
    int show_headers = flag_head   ? 1 : (flag_silent ? 0 : 1);
    int show_body    = flag_head   ? 0 : 1;
    const char *method = flag_head ? "HEAD" : "GET";

    http_do_request(method, host, path, show_headers, show_body, "[curl]");

    if (!flag_silent) uart_println("[curl] Done.");
}

// ─── COMMAND PARSER ──────────────────────
void split_cmd(const char *input, char *cmd, char *arg) {
    int i = 0;
    while (input[i] == ' ') i++;
    int j = 0;
    while (input[i] && input[i] != ' ') cmd[j++] = input[i++];
    cmd[j] = 0;
    while (input[i] == ' ') i++;
    j = 0;
    while (input[i]) arg[j++] = input[i++];
    arg[j] = 0;
}

void split3(const char *input, char *c1, char *c2, char *c3) {
    char tmp[128];
    npl_strcpy(tmp, input);
    split_cmd(tmp, c1, tmp);
    split_cmd(tmp, c2, c3);
}

// ─── HELP ────────────────────────────────
void nsh_help() {
    uart_println("\r\n NexOS Shell (nsh) v0.2 — Built-in commands:\r\n");
    uart_println("  ── Package Manager ──────────────────────");
    uart_println("  npl <cmd>            — package manager (npl help)");
    uart_println("");
    uart_println("  ── System ───────────────────────────────");
    uart_println("  nexfetch             — system info");
    uart_println("  uname                — kernel info");
    uart_println("  whoami               — user info");
    uart_println("  uptime               — boot ticks");
    uart_println("  clear                — bersih layar");
    uart_println("  halt                 — shutdown NexOS");
    uart_println("");
    uart_println("  ── Filesystem ───────────────────────────");
    uart_println("  ls [path]            — list isi direktori");
    uart_println("  tree [path]          — tampil struktur direktori");
    uart_println("  pwd                  — current directory");
    uart_println("  cd <path>            — ganti directory (support ..)");
    uart_println("  mkdir <dir>          — buat direktori baru");
    uart_println("  touch <file>         — buat file kosong");
    uart_println("  cat <file>           — tampil isi file");
    uart_println("  write <file> <text>  — tulis teks ke file");
    uart_println("  cp <src> <dst>       — copy file");
    uart_println("  mv <src> <dst>       — pindah/rename file");
    uart_println("  rm <file|dir>        — hapus file atau dir kosong");
    uart_println("  find <nama>          — cari file by nama");
    uart_println("  stat <file|dir>      — info detail file/direktori");
    uart_println("");
    uart_println("  ── System Info ──────────────────────────");
    uart_println("  npinfo               — quick system overview");
    uart_println("  np mem               — detail memory usage");
    uart_println("  np stat              — full system status");
    uart_println("");
    uart_println("  ── Tools ────────────────────────────────");
    uart_println("  calc <expr>          — kalkulator (contoh: calc 10+5)");
    uart_println("  echo <text>          — print teks");
    uart_println("  history              — riwayat command");
    uart_println("  env                  — environment variables");
    uart_println("  export KEY=VAL       — set env variable");
    uart_println("  help                 — bantuan ini");
    uart_println("");
    uart_println("  ── Networking ───────────────────────────");
    uart_println("  net init [ip]        — init virtio-net driver");
    uart_println("  net stat             — status interface");
    uart_println("  ifconfig             — tampil network interface");
    uart_println("  arp                  — lihat ARP table");
    uart_println("  ping <ip>            — kirim ICMP echo (auto ARP resolve)");
    uart_println("  dns <hostname>       — resolve hostname jadi IP (UDP/53)");
    uart_println("  wget <url>           — HTTP GET, print body (auto DNS+TCP)");
    uart_println("  curl [-I|-s] <url>   — HTTP GET/HEAD (header+body, -I=HEAD, -s=body only)");
    uart_println("  tcpserv [port]       — TCP echo/HTTP server (default port 8080)");
    uart_print("\r\n");
}

// ─── COMMAND: tcpserv ─────────────────────────────────────
// Usage: tcpserv [port]
// Simple TCP echo server — setiap koneksi yang masuk, echo balik datanya
// + balas HTTP 200 kalau terdeteksi HTTP request (buat test dari browser)
static void cmd_tcpserv(const char *port_str) {
    if (!g_net.ready) {
        uart_println("[tcpserv] ERR: Network belum ready (jalankan: net init)");
        return;
    }

    unsigned short port = 8080;
    if (port_str && port_str[0]) {
        unsigned int p = 0;
        for (int i = 0; port_str[i] >= '0' && port_str[i] <= '9'; i++)
            p = p * 10 + (port_str[i] - '0');
        if (p > 0 && p < 65536) port = (unsigned short)p;
    }

    if (tcp_listen(&g_tcp_listener, &g_net, port) < 0) {
        uart_println("[tcpserv] ERR: Gagal listen.");
        return;
    }

    uart_print("[tcpserv] Echo server aktif di port ");
    char pb[8]; int pi = 0; unsigned short ptmp = port;
    if (!ptmp) { pb[pi++] = '0'; } else {
        char tmp[6]; int ti = 0;
        while (ptmp) { tmp[ti++] = '0' + ptmp % 10; ptmp /= 10; }
        while (ti--) pb[pi++] = tmp[ti];
    }
    pb[pi] = 0;
    uart_println(pb);
    uart_println("[tcpserv] QEMU: akses dari host via 'nc 127.0.0.1 <hostfwd port>'");
    uart_println("[tcpserv] Tekan Ctrl+C / reset untuk stop.\r\n");

    TcpConn client;
    unsigned char rxbuf[256];

    // Loop utama server
    unsigned int idle = 0;
    while (1) {
        // Poll RX dari virtio
        vnet_poll(&g_net, vnet_handle_frame_wrapper);

        // Poll retransmit di backlog (SYN-ACK timeout)
        tcp_listener_poll(&g_tcp_listener);

        // Accept koneksi baru kalau ada
        if (tcp_accept(&g_tcp_listener, &client)) {
            uart_println("[tcpserv] Koneksi diterima! Masuk mode echo...");

            // Inner loop: baca data, echo balik
            unsigned int guard = 0;
            while (guard++ < 200000000U) {
                vnet_poll(&g_net, vnet_handle_frame_wrapper);
                tcp_listener_poll(&g_tcp_listener);
                tcp_poll(&client);

                if (tcp_rx_available(&client) > 0) {
                    unsigned int n = tcp_read(&client, rxbuf,
                                              sizeof(rxbuf) - 1);
                    rxbuf[n] = 0;
                    guard = 0;

                    // Deteksi HTTP GET → balas HTTP response
                    int is_http = (n > 4 &&
                        rxbuf[0]=='G' && rxbuf[1]=='E' &&
                        rxbuf[2]=='T' && rxbuf[3]==' ');

                    if (is_http) {
                        const char *resp =
                            "HTTP/1.0 200 OK\r\n"
                            "Content-Type: text/plain\r\n"
                            "Connection: close\r\n"
                            "\r\n"
                            "NexOS TCP Server - OK\r\n";
                        uart_println("[tcpserv] HTTP GET → kirim 200 OK");
                        tcp_send(&client, resp, npl_strlen(resp));
                        tcp_close(&client);
                        break;
                    } else {
                        // Echo mentah
                        uart_print("[tcpserv] Echo ");
                        char nb[8]; int ni = 0; unsigned int ntmp = n;
                        if (!ntmp) nb[ni++]='0';
                        else { char t[8]; int ti=0;
                               while(ntmp){t[ti++]='0'+ntmp%10;ntmp/=10;}
                               while(ti--)nb[ni++]=t[ti]; }
                        nb[ni]=0;
                        uart_print(nb);
                        uart_println(" bytes");
                        tcp_send(&client, rxbuf, n);
                    }
                }

                if (client.state == TCP_CLOSED ||
                    client.state == TCP_TIME_WAIT) {
                    uart_println("[tcpserv] Koneksi ditutup remote.");
                    break;
                }
            }
            uart_println("[tcpserv] Siap terima koneksi berikutnya...");
            idle = 0;
        }

        // Print status tiap ~10M iterasi idle
        if (++idle >= 10000000U) {
            idle = 0;
            uart_print("[tcpserv] waiting... backlog=");
            char bc[4];
            bc[0] = '0' + (char)g_tcp_listener.backlog_count;
            bc[1] = 0;
            uart_println(bc);
        }
    }
}

// ─── NFS LS CALLBACK ─────────────────────
static void nfs_ls_cb(const char *name, int type, unsigned int size) {
    uart_print(type == NFS_TYPE_DIR ? "d  " : "f  ");
    uart_print(name);
    if (type == NFS_TYPE_FILE) {
        uart_print("  (");
        char sb[12]; npl_itoa((int)size, sb);
        uart_print(sb); uart_print("B)");
    }
    uart_println("");
}

// ─── MAIN DISPATCHER ─────────────────────
void nsh_dispatch(const char *line) {
    char c1[64], c2[64], c3[64];
    split3(line, c1, c2, c3);

    if (c1[0] == 0) return;

    boot_ticks++;

    // ── npl ──
    if (npl_strcmp(c1, "npl") == 0) {
        if      (npl_strcmp(c2, "list")    == 0) npl_cmd_list();
        else if (npl_strcmp(c2, "install") == 0) npl_cmd_install(c3);
        else if (npl_strcmp(c2, "remove")  == 0) npl_cmd_remove(c3);
        else if (npl_strcmp(c2, "update")  == 0) npl_cmd_update();
        else if (npl_strcmp(c2, "search")  == 0) npl_cmd_search(c3);
        else if (npl_strcmp(c2, "info")    == 0) npl_cmd_info(c3);
        else if (npl_strcmp(c2, "help")    == 0) npl_cmd_help();
        else uart_println("[nsh] Unknown npl cmd. Try: npl help");

    // ── system ──
    } else if (npl_strcmp(c1, "nexfetch") == 0) { cmd_nexfetch();
    } else if (npl_strcmp(c1, "clear")    == 0) { uart_print("\033[2J\033[H");
    } else if (npl_strcmp(c1, "uname")    == 0) { uart_println("NexOS 0.2 ARM virt AArch32");
    } else if (npl_strcmp(c1, "whoami")   == 0) { cmd_whoami();
    } else if (npl_strcmp(c1, "uptime")   == 0) { cmd_uptime();
    } else if (npl_strcmp(c1, "help")     == 0) { nsh_help();
    } else if (npl_strcmp(c1, "halt")     == 0) {
        uart_println("\r\n[NexOS] System halting...");
        uart_println("[NexOS] Goodbye!");
        while(1){}

    // ── filesystem ──
    } else if (npl_strcmp(c1, "ls")    == 0) { cmd_ls(c2);
    } else if (npl_strcmp(c1, "tree")  == 0) { cmd_tree(c2);
    } else if (npl_strcmp(c1, "cat")   == 0) { cmd_cat(c2);
    } else if (npl_strcmp(c1, "touch") == 0) { cmd_touch(c2);
    } else if (npl_strcmp(c1, "mkdir") == 0) { cmd_mkdir(c2);
    } else if (npl_strcmp(c1, "rm")    == 0) { cmd_rm(c2);
    } else if (npl_strcmp(c1, "cp")    == 0) { cmd_cp(c2, c3);
    } else if (npl_strcmp(c1, "mv")    == 0) { cmd_mv(c2, c3);
    } else if (npl_strcmp(c1, "find")  == 0) { cmd_find(c2);
    } else if (npl_strcmp(c1, "stat")  == 0) { cmd_stat(c2);
    } else if (npl_strcmp(c1, "pwd")   == 0) { cmd_pwd();
    } else if (npl_strcmp(c1, "cd")    == 0) { cmd_cd(c2);
    } else if (npl_strcmp(c1, "write") == 0) { cmd_write(c2, c3);

    // ── np / npinfo ──
    } else if (npl_strcmp(c1, "npinfo") == 0) { cmd_npinfo();
    } else if (npl_strcmp(c1, "np") == 0) {
        if      (npl_strcmp(c2, "mem")  == 0) cmd_np_mem();
        else if (npl_strcmp(c2, "stat") == 0) cmd_np_stat();
        else uart_println("[nsh] Usage: np mem | np stat");

    // ── tools ──
    } else if (npl_strcmp(c1, "echo")    == 0) {
        uart_print(c2); if (c3[0]) { uart_putc(' '); uart_print(c3); } uart_print("\r\n");
    } else if (npl_strcmp(c1, "calc")    == 0) { cmd_calc(c2);
    } else if (npl_strcmp(c1, "history") == 0) { cmd_history_print();
    } else if (npl_strcmp(c1, "env")     == 0) { cmd_env();
    } else if (npl_strcmp(c1, "export")  == 0) { cmd_export(c2);

    // ── networking ──
    } else if (npl_strcmp(c1, "ifconfig") == 0) { cmd_ifconfig();
    } else if (npl_strcmp(c1, "ping")     == 0) { cmd_ping(c2);
    } else if (npl_strcmp(c1, "arp")      == 0) { cmd_arp();
    } else if (npl_strcmp(c1, "wget") == 0) { cmd_wget(c2);
    } else if (npl_strcmp(c1, "curl") == 0) { cmd_curl(c2, c3);
    } else if (npl_strcmp(c1, "net")      == 0) { cmd_net(c2, c3);
    } else if (npl_strcmp(c1, "dns")      == 0) { cmd_dns(c2);
    } else if (npl_strcmp(c1, "tcpserv")  == 0) { cmd_tcpserv(c2);

    // ── nfs (persistent storage) ──
    } else if (npl_strcmp(c1, "nfs") == 0) {
        if (npl_strcmp(c2, "format") == 0) {
            if (vblk_init(&g_blk) == 0)
                nfs_format(&g_nfs, &g_blk);
            else
                uart_println("[nfs] ERR: block device tidak ada");
        } else if (npl_strcmp(c2, "mount") == 0) {
            if (vblk_init(&g_blk) == 0)
                nfs_mount(&g_nfs, &g_blk);
            else
                uart_println("[nfs] ERR: block device tidak ada");
        } else if (npl_strcmp(c2, "sync") == 0) {
            nfs_sync(&g_nfs);
            uart_println("[nfs] Sync OK");
        } else if (npl_strcmp(c2, "ls") == 0) {
            const char *p = (c3[0] ? c3 : "/");
            nfs_ls(&g_nfs, p, nfs_ls_cb);
        } else if (npl_strcmp(c2, "mkdir") == 0 && c3[0]) {
            if (nfs_mkdir(&g_nfs, c3) >= 0)
                uart_println("[nfs] mkdir OK");
            else
                uart_println("[nfs] ERR: mkdir gagal");
        } else if (npl_strcmp(c2, "rm") == 0 && c3[0]) {
            if (nfs_unlink(&g_nfs, c3) == 0)
                uart_println("[nfs] rm OK");
            else
                uart_println("[nfs] ERR: file tidak ditemukan");
        } else if (npl_strcmp(c2, "cat") == 0 && c3[0]) {
            int fd = nfs_open(&g_nfs, c3, 0);
            if (fd < 0) { uart_println("[nfs] ERR: file tidak ditemukan"); }
            else {
                static char rbuf[512];
                int n;
                while ((n = nfs_read(&g_nfs, fd, rbuf, 511)) > 0) {
                    rbuf[n] = 0;
                    uart_print(rbuf);
                }
                uart_println("");
                nfs_close(&g_nfs, fd);
            }
        } else if (npl_strcmp(c2, "write") == 0 && c3[0]) {
            // Usage: nfs write <path> (lalu isi diambil dari c4 — perlu extend parser)
            // Versi simple: buat file baru dengan nama c3, tulis c4 sebagai isi
            // Untuk sekarang: buat file kosong
            if (nfs_create(&g_nfs, c3, NFS_TYPE_FILE) >= 0)
                uart_println("[nfs] File dibuat. Gunakan 'nfs append' untuk isi.");
            else
                uart_println("[nfs] ERR: create gagal (sudah ada?)");
        } else if (npl_strcmp(c2, "append") == 0 && c3[0]) {
            // nfs append <path> <text>
            // c3 = path, tapi kita perlu raw line untuk ambil teks setelah c3
            // Untuk sekarang append string c3 ke path c2 (swap argumen)
            uart_println("[nfs] Usage: nfs append <path> — ketik teks via serial");
            // TODO: integrate dengan readline untuk multi-word
        } else {
            uart_println("Usage: nfs format|mount|sync|ls|mkdir|rm|cat|write");
        }

    } else {
        uart_print("[nsh] Command not found: ");
        uart_print(c1);
        uart_println("\r\n[nsh] Ketik 'help' untuk bantuan.");
    }
}

// ─── KERNEL MAIN ─────────────────────────
void kernel_main() {
    uart_println("\r\n");
    uart_println("  ███╗   ██╗███████╗██╗  ██╗ ██████╗ ███████╗");
    uart_println("  ╚══╝   ╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝ ╚══════╝");
    uart_println("  NexOS v0.3 — ARM Baremetal Kernel");
    uart_println("  ══════════════════════════════════\r\n");
    uart_println("[OK] Kernel booted");
    uart_println("[OK] UART driver loaded");
    mem_init();
    kmalloc(64);
    kmalloc(128);
    uart_println("[OK] Memory manager ready (real heap from linker)");
    vfs_init();
    uart_println("[OK] VFS initialized (3 files)");
    uart_println("[OK] env table loaded");
    uart_println("[OK] npl package manager ready");
    gic_init();
    uart_println("[OK] GIC initialized");
    uart_irq_init();
    irq_enable();
    uart_println("[OK] UART IRQ enabled (interrupt-driven input)");
    uart_println("[OK] nsh v0.3 starting\r\n");

    // Init virtio-blk + NFS (opsional — butuh disk image)
    if (vblk_init(&g_blk) == 0) {
        if (nfs_mount(&g_nfs, &g_blk) == 0) {
            uart_println("[OK] NFS mounted (persistent storage ready)");
        } else {
            uart_println("[--] NFS: disk belum diformat. Ketik: nfs format");
        }
    } else {
        uart_println("[--] virtio-blk tidak tersedia. Ketik 'nfs format' setelah attach disk.");
    }

    // Init virtio-net (opsional — gagal jika QEMU tidak punya -device virtio-net-device)
    {
        unsigned char default_ip[4]         = {10, 0, 2, 15};
        unsigned char default_gateway[4]    = {10, 0, 2, 2};
        unsigned char default_netmask[4]    = {255, 255, 255, 0};
        unsigned char default_dns_server[4] = {10, 0, 2, 3};
        if (vnet_init(&g_net, default_ip, default_gateway, default_netmask) == 0) {
            uart_println("[OK] virtio-net ready (10.0.2.15, gw 10.0.2.2)");
            dns_init(&g_dns, &g_net, default_dns_server);
            uart_println("[OK] DNS resolver ready (server 10.0.2.3)");
        } else {
            uart_println("[--] virtio-net tidak tersedia (jalankan: net init)");
        }
    }

    uart_println("Type 'help' for commands, 'npl help' for packages.\r\n");

    char line[128];
    while (1) {
        uart_print("nsh@nexos:");
        uart_print(cwd);
        uart_print("# ");
        uart_readline(line, sizeof(line));
        history_add(line);
        nsh_dispatch(line);
    }
}
