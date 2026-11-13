/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * DPI RTL C prototype to interface with DMA RTL
 */

#ifndef DPI_RTL_WRAPPER_H
#define DPI_RTL_WRAPPER_H

#include <stdint.h>
#include "sysbus.h"

#ifdef __cplusplus
extern "C" {
#endif

// Verilator RTL simulation functions
int rtl_dma_init_simulation(void);
void rtl_dma_cleanup_simulation(void);
int dpi_rtl_dma_copy(uint64_t src_offset, uint64_t dst_offset, uint64_t length);
int dpi_rtl_dma_is_available(void);
void dma_done_callback(void);

/* Bat/tat ghi rtl_trace.vcd. ./os PHAI goi rtl_dma_set_trace(0) truoc
 * rtl_dma_init_simulation(): ham write() cua src/cpu.c trung ten voi
 * POSIX write() ma Verilator dung de ghi VCD. */
void rtl_dma_set_trace(int on);

/* ===================================================================
 * Giao dien DMA bat dong bo  (TV4 - T03)
 * -------------------------------------------------------------------
 * dpi_rtl_dma_copy() o tren la ban CHAN: no dap clock trong mot vong
 * while cho den khi mach bao xong, nen thread goi no bi giam lai.
 * Yeu cau cua de la "CPU khoi dong DMA roi tiep tuc xu ly", nen ta
 * tach lam ba buoc: nap lenh - cap nhip - hoi trang thai.
 * =================================================================== */

typedef enum {
    DMA_JOB_IDLE    = 0,   /* khong co job nao */
    DMA_JOB_RUNNING = 1,   /* da nhan descriptor, mach dang chay */
    DMA_JOB_DONE    = 2,   /* ch_done = 1, da copy xong */
    DMA_JOB_ERROR   = 3    /* ch_error = 1 */
} dma_job_state_t;

/* So chu ky clock cap cho DMA trong mot time slot cua OS.
 * Khong anh huong tinh dung, chi anh huong job chay het may slot. */
#define DMA_CLK_PER_SLOT   64

/* Tran an toan: 50000 chu ky, bang max_cycles cu (100000 nua chu ky). */
#define DMA_MAX_CYCLES     50000

/* Mach chuyen theo nhip 4 byte (DepSet_hbdce0907:223-239). Nhip cuoi
 * VAN ghi du 4 byte ke ca khi chi con 1-3 byte, nen length khong chia
 * het 4 se ghi tran sang vung ben canh. Tang OS lam tron truoc khi gui. */
#define DMA_BEAT_BYTES     4
#define DMA_LEN_ROUNDUP(n) (((n) + (DMA_BEAT_BYTES - 1)) & ~((uint32_t)DMA_BEAT_BYTES - 1))

/* Nap descriptor va bat kenh. TRA VE NGAY, khong dap mot nhip nao.
 * tra 0  = mach da nhan
 *     -1 = chua init mo phong, hoac dang co job khac chay */
int dpi_rtl_dma_start(uint64_t src_offset, uint64_t dst_offset, uint64_t length);

/* Cap nclk chu ky clock cho job dang chay roi tra ve.
 * tra 1 = job da ket thuc (xong hoac loi), 0 = con chay */
int dpi_rtl_dma_tick(int nclk);

/* Trang thai job hien tai. Doc ch_done, KHONG doc cong state:
 * state = current_state & 3 ma FSM rong 3 bit nen DONE(4) trung IDLE(0). */
dma_job_state_t dpi_rtl_dma_state(void);

/* So byte mach bao da chuyen xong (cong ch_bytes_transferred). */
uint32_t dpi_rtl_dma_bytes(void);

/* So chu ky clock job vua roi da tieu ton - dung de bao cao. */
uint64_t dpi_rtl_dma_cycles(void);

/* Ha ch_enable de NHA KENH va tra channel ve IDLE.
 * BAT BUOC goi sau moi job: state DONE khong tu ve IDLE,
 * quen goi thi job thu hai khong bao gio khoi dong duoc. */
int dpi_rtl_dma_reap(void);

/* Tam dung / chay tiep giua chung. ch_pause dong bang ca FSM. */
void dpi_rtl_dma_pause(int on);

#ifdef __cplusplus
}
#endif

#endif
