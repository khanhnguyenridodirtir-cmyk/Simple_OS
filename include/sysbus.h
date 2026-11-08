/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * system bus protoype header file
 */

#ifndef SYSTEM_BUS_H
#define SYSTEM_BUS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of memory banks supported
#define MAX_MEMORY_BANKS 8

// Memory bank handle type
typedef int memory_bank_t;

// Invalid memory bank handle
#define MEMORY_BANK_INVALID (-1)

// Initialize a memory bank with given size in bytes
memory_bank_t memory_bank_init(size_t size_bytes);

/* memory_bank_attach - dang ky mot vung nho CO SAN lam bank
 *
 * Khac memory_bank_init o hai diem:
 *   - KHONG malloc, dung thang vung storage nguoi goi dua vao
 *   - KHONG free khi cleanup (co owns_data = 0)
 *
 * Dung de tro bus cua DMA vao RAM that cua OS (os.mram->storage)
 * thay vi mot mang malloc rieng. Sau khi goi ham nay, hai ham DPI
 * dpi_bank_read32_legacy / write32_legacy chay dung ma RTL khong
 * phai sua mot dong nao.
 */
memory_bank_t memory_bank_attach(void *storage, size_t size_bytes);

/* So lan RTL goi dma_done_callback() ke tu lan reset dem gan nhat.
 * Day la duong bao hoan thanh di tu phan cung len phan mem. */
unsigned long dma_done_count(void);
void          dma_done_reset(void);

// Cleanup all memory banks
void memory_bank_cleanup(void);

// Get pointer to locate data at offset
void* memory_bank_get_ptr(uint64_t offset);

// Get bank size in bytes
size_t memory_bank_get_size(void);

// Check if memory bank is initialized
int memory_bank_is_available(void);

// DPI Callback: Called from RTL when DMA read access
int dpi_bank_read32_legacy(long long offset);

// DPI Callback: Called from RTL when DMA write access
int dpi_bank_write32_legacy(long long offset, int value);

// DPI Callback: Called from RTL when DMA transfer completes
void dma_done_callback(void);
#ifdef __cplusplus
}
#endif

#endif // SYSTEM_BUS_H