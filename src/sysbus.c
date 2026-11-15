/*
 * System bus bridge between the Verilated DMA RTL and host RAM.
 * Bank 0 may either own an allocation (standalone testbench) or borrow the
 * OS MEMPHY storage (integrated OS simulation).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sysbus.h"

typedef struct {
    uint8_t *data;
    size_t size;
    int initialized;
    int owns_data;
} memory_bank_t_struct;

static memory_bank_t_struct g_banks[MAX_MEMORY_BANKS] = {{0}};
static int g_bank_count = 0;

memory_bank_t memory_bank_init(size_t size_bytes)
{
    if (size_bytes == 0) return MEMORY_BANK_INVALID;

    for (int i = 0; i < MAX_MEMORY_BANKS; ++i) {
        if (!g_banks[i].initialized) {
            uint8_t *data = (uint8_t *)calloc(size_bytes, 1);
            if (!data) return MEMORY_BANK_INVALID;
            g_banks[i].data = data;
            g_banks[i].size = size_bytes;
            g_banks[i].initialized = 1;
            g_banks[i].owns_data = 1;
            ++g_bank_count;
            printf("Memory bank [%d] initialized: %zu bytes\n", i, size_bytes);
            return i;
        }
    }
    return MEMORY_BANK_INVALID;
}

int memory_bank_bind(void *buffer, size_t size_bytes)
{
    if (!buffer || size_bytes == 0) return -1;

    /* RTL currently addresses one flat RAM bank, mapped to bank 0. */
    if (g_banks[0].initialized) {
        if (g_banks[0].data == (uint8_t *)buffer &&
            g_banks[0].size == size_bytes) return 0;
        if (g_banks[0].owns_data) free(g_banks[0].data);
        else return -1; /* Do not silently replace a live borrowed bank. */
        g_banks[0] = (memory_bank_t_struct){0};
        --g_bank_count;
    }

    g_banks[0].data = (uint8_t *)buffer;
    g_banks[0].size = size_bytes;
    g_banks[0].initialized = 1;
    g_banks[0].owns_data = 0;
    ++g_bank_count;
    return 0;
}

void memory_bank_cleanup(void)
{
    for (int i = 0; i < MAX_MEMORY_BANKS; ++i) {
        if (!g_banks[i].initialized) continue;
        if (g_banks[i].owns_data) free(g_banks[i].data);
        g_banks[i] = (memory_bank_t_struct){0};
    }
    g_bank_count = 0;
}

int memory_bank_is_available(void)
{
    return g_banks[0].initialized && g_banks[0].data != NULL;
}

int memory_bank_check_range(uint64_t offset, size_t length)
{
    if (!memory_bank_is_available()) return 0;
    if (offset > (uint64_t)g_banks[0].size) return 0;
    if (length > g_banks[0].size - (size_t)offset) return 0;
    return 1;
}

void *memory_bank_get_ptr(uint64_t offset)
{
    if (!memory_bank_check_range(offset, 1)) return NULL;
    return g_banks[0].data + (size_t)offset;
}

size_t memory_bank_get_size(void)
{
    return memory_bank_is_available() ? g_banks[0].size : 0;
}

/* DPI callbacks called by the generated Verilator model. */
int dpi_bank_read32_legacy(long long offset)
{
    uint32_t value = 0;
    if (offset < 0 || !memory_bank_check_range((uint64_t)offset, sizeof(value))) {
        fprintf(stderr, "DMA bus read out of range: 0x%llx\n", offset);
        return 0;
    }
    memcpy(&value, g_banks[0].data + (size_t)offset, sizeof(value));
    return (int)value;
}

int dpi_bank_write32_legacy(long long offset, int value)
{
    if (offset < 0 || !memory_bank_check_range((uint64_t)offset, sizeof(uint32_t))) {
        fprintf(stderr, "DMA bus write out of range: 0x%llx\n", offset);
        return -1;
    }
    uint32_t word = (uint32_t)value;
    memcpy(g_banks[0].data + (size_t)offset, &word, sizeof(word));
    return 0;
}

void dma_done_callback(void)
{
    printf("DMA RTL callback: transfer completed\n");
    fflush(stdout);
}
