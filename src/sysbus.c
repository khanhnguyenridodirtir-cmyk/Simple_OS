/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * system bus - Only for dma_testbech simulation access
 */
/*
 * System-bus bridge between the Verilated DMA RTL and the OS RAM.
 * The RTL uses a single flat bank (bank 0). The bank may own an allocation
 * for standalone tests, or borrow the OS MEMPHY storage for OS integration.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include "sysbus.h"

typedef struct {
    uint8_t *data;
    size_t size;
    int initialized;
    int owns_data;
} memory_bank_t_struct;

static memory_bank_t_struct g_banks[MAX_MEMORY_BANKS] = {{0}};
static pthread_mutex_t g_bank_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_callback_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_dma_done_count = 0;

memory_bank_t memory_bank_init(size_t size_bytes)
{
    uint8_t *data;
    if (size_bytes == 0) return MEMORY_BANK_INVALID;

    pthread_mutex_lock(&g_bank_lock);
    /* The current RTL maps all DMA addresses to one flat bank: bank 0. */
    if (g_banks[0].initialized) {
        pthread_mutex_unlock(&g_bank_lock);
        return MEMORY_BANK_INVALID;
    }

    data = (uint8_t *)calloc(size_bytes, 1);
    if (!data) {
        pthread_mutex_unlock(&g_bank_lock);
        return MEMORY_BANK_INVALID;
    }

    g_banks[0].data = data;
    g_banks[0].size = size_bytes;
    g_banks[0].initialized = 1;
    g_banks[0].owns_data = 1;
    pthread_mutex_unlock(&g_bank_lock);

    printf("Memory bank [0] initialized: %zu bytes\n", size_bytes);
    return 0;
}

int memory_bank_bind(void *buffer, size_t size_bytes)
{
    if (!buffer || size_bytes == 0) return -1;

    pthread_mutex_lock(&g_bank_lock);
    if (g_banks[0].initialized) {
        if (g_banks[0].data == (uint8_t *)buffer) {
            int same_size = (g_banks[0].size == size_bytes);
            pthread_mutex_unlock(&g_bank_lock);
            return same_size ? 0 : -1;
        }
        /* It is safe to replace an owned test allocation, but never a
         * borrowed buffer that belongs to the live OS memory subsystem. */
        if (!g_banks[0].owns_data) {
            pthread_mutex_unlock(&g_bank_lock);
            return -1;
        }
        free(g_banks[0].data);
    }

    g_banks[0].data = (uint8_t *)buffer;
    g_banks[0].size = size_bytes;
    g_banks[0].initialized = 1;
    g_banks[0].owns_data = 0;
    pthread_mutex_unlock(&g_bank_lock);
    return 0;
}

memory_bank_t memory_bank_attach(void *storage, size_t size_bytes)
{
    return memory_bank_bind(storage, size_bytes) == 0 ? 0 : MEMORY_BANK_INVALID;
}

int memory_bank_check_range(uint64_t offset, size_t length)
{
    int valid = 0;
    pthread_mutex_lock(&g_bank_lock);
    if (g_banks[0].initialized && g_banks[0].data != NULL &&
        offset <= (uint64_t)g_banks[0].size &&
        length <= g_banks[0].size - (size_t)offset) {
        valid = 1;
    }
    pthread_mutex_unlock(&g_bank_lock);
    return valid;
}

void *memory_bank_get_ptr(uint64_t offset)
{
    void *ptr = NULL;
    pthread_mutex_lock(&g_bank_lock);
    if (g_banks[0].initialized && g_banks[0].data != NULL &&
        offset < (uint64_t)g_banks[0].size) {
        ptr = g_banks[0].data + (size_t)offset;
    }
    pthread_mutex_unlock(&g_bank_lock);
    return ptr;
}

size_t memory_bank_get_size(void)
{
    size_t size;
    pthread_mutex_lock(&g_bank_lock);
    size = (g_banks[0].initialized && g_banks[0].data != NULL)
         ? g_banks[0].size : 0;
    pthread_mutex_unlock(&g_bank_lock);
    return size;
}

int memory_bank_is_available(void)
{
    int available;
    pthread_mutex_lock(&g_bank_lock);
    available = g_banks[0].initialized && g_banks[0].data != NULL;
    pthread_mutex_unlock(&g_bank_lock);
    return available;
}

void memory_bank_cleanup(void)
{
    int i;
    pthread_mutex_lock(&g_bank_lock);
    for (i = 0; i < MAX_MEMORY_BANKS; ++i) {
        if (!g_banks[i].initialized) continue;
        if (g_banks[i].owns_data) free(g_banks[i].data);
        memset(&g_banks[i], 0, sizeof(g_banks[i]));
    }
    pthread_mutex_unlock(&g_bank_lock);
}

int dpi_bank_read32_legacy(long long offset)
{
    uint32_t value = 0;
    void *src;
    if (offset < 0 || !memory_bank_check_range((uint64_t)offset, sizeof(value))) {
        fprintf(stderr, "DMA bus read out of range: 0x%llx\n", offset);
        return 0;
    }
    src = memory_bank_get_ptr((uint64_t)offset);
    if (!src) return 0;
    memcpy(&value, src, sizeof(value));
    return (int)value;
}

int dpi_bank_write32_legacy(long long offset, int value)
{
    uint32_t word = (uint32_t)value;
    void *dst;
    if (offset < 0 || !memory_bank_check_range((uint64_t)offset, sizeof(word))) {
        fprintf(stderr, "DMA bus write out of range: 0x%llx\n", offset);
        return -1;
    }
    dst = memory_bank_get_ptr((uint64_t)offset);
    if (!dst) return -1;
    memcpy(dst, &word, sizeof(word));
    return 0;
}

void dma_done_callback(void)
{
    pthread_mutex_lock(&g_callback_lock);
    ++g_dma_done_count;
    pthread_mutex_unlock(&g_callback_lock);
    printf("DMA completed\n");
    fflush(stdout);
}

unsigned long dma_done_count(void)
{
    unsigned long count;
    pthread_mutex_lock(&g_callback_lock);
    count = g_dma_done_count;
    pthread_mutex_unlock(&g_callback_lock);
    return count;
}

void dma_done_reset(void)
{
    pthread_mutex_lock(&g_callback_lock);
    g_dma_done_count = 0;
    pthread_mutex_unlock(&g_callback_lock);
}
