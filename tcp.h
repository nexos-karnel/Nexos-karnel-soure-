// tcp.h — NexOS minimal TCP stack
// Scope: satu koneksi, retransmit dengan exponential backoff, no window scaling
// Cukup buat HTTP GET satu request

#ifndef TCP_H
#define TCP_H

#include "virtio_net.h"
#include "nexos_time.h"

// ─── TCP HEADER ──────────────────────────────────────────
// NOTE: TcpHdr sudah didefinisikan di virtio_net.h (di-include di atas),
// supaya vnet_handle_frame() bisa dispatch TCP segment tanpa circular
// include. Definisi di sini dihapus biar gak ada duplikasi struct.

// TCP flags
#define TCP_FIN  (1 << 0)
#define TCP_SYN  (1 << 1)
#define TCP_RST  (1 << 2)
#define TCP_PSH  (1 << 3)
#define TCP_ACK  (1 << 4)
#define TCP_URG  (1 << 5)

// ─── TCP STATE MACHINE ───────────────────────────────────
typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,         // server: nunggu SYN masuk (bind sudah)
    TCP_SYN_RCVD,       // server: terima SYN, sudah balas SYN-ACK, nunggu ACK
    TCP_SYN_SENT,       // client: kita kirim SYN, nunggu SYN-ACK
    TCP_ESTABLISHED,    // koneksi aktif (client & server)
    TCP_FIN_WAIT,       // kita kirim FIN, nunggu ACK
    TCP_CLOSE_WAIT,     // terima FIN dari remote, belum kirim FIN
    TCP_TIME_WAIT,      // nunggu sebelum benar-benar closed
} TcpState;

// ─── BUFFER SIZES ────────────────────────────────────────
#define TCP_RX_BUF_SIZE  4096   // buffer data yang diterima
#define TCP_TX_BUF_SIZE  4096   // buffer data yang belum di-ACK (retransmit)

// ─── RETRANSMIT CONFIG ───────────────────────────────────
// RTO awal dalam ticks (nexos_ticks, biasanya ~62.5MHz atau 1GHz di QEMU).
// 15000000 ≈ ~240ms di 62.5MHz, ~15ms di 1GHz — cukup buat QEMU usernet.
// Backoff: RTO double tiap retry (exponential backoff).
#define TCP_RTO_INIT     15000000U   // RTO awal (ticks)
#define TCP_RTO_MAX      480000000U  // RTO maksimum setelah backoff (~8x init)
#define TCP_RETRANSMIT_MAX  4        // max retry sebelum RST koneksi

// ─── TCP CONNECTION ──────────────────────────────────────
typedef struct {
    TcpState        state;

    // Identitas koneksi
    unsigned char   remote_ip[4];
    unsigned short  remote_port;
    unsigned short  local_port;

    // Sequence numbers (sisi kita)
    unsigned int    isn;        // initial sequence number kita
    unsigned int    snd_una;    // oldest unACKed seq (= seq sebelum data pending)
    unsigned int    snd_nxt;    // next seq yang akan dikirim

    // Sequence number sisi remote
    unsigned int    ack;        // next seq yang kita harapkan dari remote (= ACK kita)

    // Remote window
    unsigned short  remote_win;

    // ── TX buffer (retransmit store) ──────────────────────
    // Simpan data yang sudah dikirim tapi belum di-ACK oleh remote.
    // Ukuran = snd_nxt - snd_una bytes (max TCP_TX_BUF_SIZE).
    // Layout: linear, bukan ring — disederhanakan karena kita hanya punya
    // satu "flight" data kecil (HTTP request, biasanya <512 byte).
    unsigned char   tx_buf[TCP_TX_BUF_SIZE];
    unsigned int    tx_len;         // bytes di tx_buf yang belum di-ACK

    // ── Retransmit state ──────────────────────────────────
    unsigned int    rto_ticks;      // RTO saat ini (makin besar tiap retry)
    unsigned int    last_send_tick; // kapan terakhir kirim segment data
    int             retransmit_count; // sudah retry berapa kali
    int             rto_armed;      // 1 = ada unACKed data, timer jalan

    // ── RX buffer ─────────────────────────────────────────
    unsigned char   rx_buf[TCP_RX_BUF_SIZE];
    unsigned int    rx_head;    // write pointer
    unsigned int    rx_tail;    // read pointer
    unsigned int    rx_len;     // bytes available

    // Flag: remote kirim FIN
    int             remote_fin;

    // Pointer ke network device
    VirtioNet       *dev;

} TcpConn;

// ─── FUNGSI PUBLIK ───────────────────────────────────────

// Init koneksi baru (set state ke CLOSED, ikat ke device)
void tcp_init(TcpConn *conn, VirtioNet *dev, unsigned short local_port);

// Connect ke remote IP:port (kirim SYN)
// Return 0 kalau SYN terkirim, -1 kalau gagal
int  tcp_connect(TcpConn *conn, const unsigned char ip[4], unsigned short port);

// Send data (hanya kalau ESTABLISHED)
// Return bytes yang diantri, -1 kalau gagal
int  tcp_send(TcpConn *conn, const void *data, unsigned int len);

// Close koneksi (kirim FIN)
void tcp_close(TcpConn *conn);

// Retransmit poll — panggil tiap loop iterasi saat nunggu response.
// Cek apakah ada unACKed data yang sudah timeout, lalu retransmit.
// Return 0 = normal, -1 = max retry tercapai (koneksi di-RST/CLOSED).
int  tcp_poll(TcpConn *conn);

// Handle incoming TCP segment — dipanggil dari vnet_handle_frame
void tcp_input(TcpConn *conn, const IpHdr *iph, const TcpHdr *tcph,
               const unsigned char *payload, unsigned int plen);

// Baca data dari RX buffer
// Return jumlah bytes yang dibaca (0 kalau kosong)
unsigned int tcp_read(TcpConn *conn, void *buf, unsigned int maxlen);

// Cek apakah ada data di RX buffer
unsigned int tcp_rx_available(TcpConn *conn);

// Print state koneksi ke UART
void tcp_print_state(TcpConn *conn);

// ─── SERVER MODE: LISTENER ───────────────────────────────

// Backlog: max koneksi pending yang bisa di-queue sebelum di-accept
#define TCP_BACKLOG_SIZE    4

// Satu entry di backlog — koneksi yang sudah terima SYN, sudah balas SYN-ACK,
// nunggu ACK final (SYN_RCVD) atau sudah ESTABLISHED tapi belum di-accept
typedef struct {
    TcpConn conn;       // koneksi penuh (termasuk buffer, seq numbers, dll)
    int     used;       // 1 = slot ini aktif
} TcpBacklogEntry;

// Listener — ikat ke satu port, tampung koneksi masuk di backlog queue
typedef struct {
    VirtioNet          *dev;
    unsigned short      local_port;
    int                 active;         // 1 = sedang listen

    TcpBacklogEntry     backlog[TCP_BACKLOG_SIZE];
    int                 backlog_count;  // jumlah entry terisi
} TcpListener;

// Init listener, ikat ke port. Return 0 sukses, -1 gagal
int  tcp_listen(TcpListener *lst, VirtioNet *dev, unsigned short port);

// Terima koneksi dari backlog (non-blocking)
// Ada koneksi ESTABLISHED: copy ke *out_conn, return 1. Kosong: return 0.
int  tcp_accept(TcpListener *lst, TcpConn *out_conn);

// Dispatch paket masuk ke listener — dipanggil dari tcp_input_dispatch
void tcp_listener_input(TcpListener *lst, const IpHdr *iph, const TcpHdr *tcph,
                        const unsigned char *payload, unsigned int plen);

// Poll semua koneksi di backlog (retransmit SYN-ACK kalau timeout)
void tcp_listener_poll(TcpListener *lst);

// Print status listener ke UART
void tcp_listener_print(TcpListener *lst);

#endif // TCP_H
