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

#define MAX_MEMORY_BANKS 8

typedef int memory_bank_t;
#define MEMORY_BANK_INVALID (-1)

/* Allocate an owned RAM bank for the standalone DMA testbench. */
memory_bank_t memory_bank_init(size_t size_bytes);

/* Bind/attach an existing RAM buffer as bank 0. The caller keeps ownership. */
int memory_bank_bind(void *buffer, size_t size_bytes);
memory_bank_t memory_bank_attach(void *storage, size_t size_bytes);

/* Validate [offset, offset + length) against bank 0. */
int memory_bank_check_range(uint64_t offset, size_t length);

void memory_bank_cleanup(void);
void *memory_bank_get_ptr(uint64_t offset);
size_t memory_bank_get_size(void);
int memory_bank_is_available(void);

/* DPI callbacks invoked by the generated RTL model. */
int dpi_bank_read32_legacy(long long offset);
int dpi_bank_write32_legacy(long long offset, int value);
void dma_done_callback(void);

/* Number of completion callbacks observed since the last reset. */
unsigned long dma_done_count(void);
void dma_done_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* SYSTEM_BUS_H */
