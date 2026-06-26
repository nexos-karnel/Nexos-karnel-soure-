// nexos_time.h — ARM Generic Timer helper (CNTPCT)
// QEMU "virt" machine + cortex-a15 punya ARM Generic Timer yang jalan
// terus dari boot, gak perlu setup interrupt apapun untuk baca nilainya.
// Dipakai sebagai dasar timeout/backoff untuk DNS dan TCP retransmit.
//
// CNTPCT = Physical Counter, 64-bit, diakses via coprocessor read.
// Frekuensinya tergantung CNTFRQ (biasanya 62.5MHz atau 1GHz di QEMU virt,
// tapi kita gak butuh akurasi detik — cukup nilai monoton naik buat
// bandingin "sudah berapa lama" relatif.

#ifndef NEXOS_TIME_H
#define NEXOS_TIME_H

// Baca CNTPCT (physical counter), 64-bit, return lower 32-bit aja —
// cukup karena dipakai cuma buat selisih waktu pendek (timeout/backoff),
// wrap-around 32-bit pada frekuensi MHz tetap aman dipakai dengan
// (unsigned int) subtraction (wrap-safe selama selisihnya gak sampai
// melebihi ~beberapa menit, yang gak akan terjadi untuk timeout kita).
static inline unsigned int nexos_ticks(void) {
    unsigned int lo, hi;
    __asm__ volatile ("mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
    (void)hi;
    return lo;
}

// Cek apakah sudah lewat `timeout_ticks` sejak `start_tick`.
// Pakai subtraction unsigned supaya wrap-safe.
static inline int nexos_elapsed(unsigned int start_tick, unsigned int timeout_ticks) {
    return (nexos_ticks() - start_tick) >= timeout_ticks;
}

#endif // NEXOS_TIME_H
