/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * DMA C Testbench 
 */

/**
 * Build modes:
 *   DMA_MAIN=1 - Build as standalone executable with main()
 *   DMA_MAIN=0 - Build as library module with do_dma()
 */
   
#ifndef DMA_MAIN
#define DMA_MAIN 0
#endif

#include "sysbus.h"
#include "dpi_rtl.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cstdbool>
#include <thread>
#include <atomic>

// Generated DPI functions from RTL
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vdma_top.h"
#include "Vdma_top__Dpi.h"  

/* Can khong dieu kien: nhanh else ben duoi phai doc
 * caller->krnl->mram->storage, tuc RAM that cua OS. */
extern "C" {
#include "common.h"
#include "os-mm.h"
}

// RTL Direct Test - Drive Verilator RTL directly
extern "C" int dpi_rtl_dma_copy(uint64_t src_offset, uint64_t dst_offset, uint64_t length);
extern "C" int dpi_rtl_dma_is_available(void);

/*
 * dpi_dma_copy - perform DMA copy via DPI
 * src_offset: source memory offset
 * dst_offset: destination memory offset
 * length: number of bytes to copy
 */
int dpi_dma_copy(struct pcb_t *caller, uint64_t src_offset, uint64_t dst_offset, uint64_t length) {
    void *src = NULL;
    void *dst = NULL;
    //struct krnl_t *krnl = caller->krnl;

    if (memory_bank_is_available()) {
        src = memory_bank_get_ptr(src_offset);
        dst = memory_bank_get_ptr(dst_offset);
    } else if (caller && caller->krnl && caller->krnl->mram
                      && caller->krnl->mram->storage) {
        /* Chay duoi ./os: chua ai goi memory_bank_init() nen bank rong.
         * Dang ky thang RAM that cua OS lam bank - tu day moi offset
         * RTL nhin thay la offset trong os.mram->storage. */
        struct krnl_t *krnl = caller->krnl;
        memory_bank_attach(krnl->mram->storage, (size_t) krnl->mram->maxsz);
        src = memory_bank_get_ptr(src_offset);
        dst = memory_bank_get_ptr(dst_offset);
    }

    if (!src || !dst) {
        /* memory_bank_get_ptr() tra NULL khi offset >= kich thuoc bank.
         * Ban goc khong kiem tra nen ghi thang vao dia chi 0 -> segfault. */
        printf("dpi_dma_copy: offset ngoai mien (src=%llu dst=%llu bank=%zu)\n",
               (unsigned long long) src_offset,
               (unsigned long long) dst_offset,
               memory_bank_get_size());
        return -1;
    }

    printf("dpi_dma_copy: src=%llu, dst=%llu, len=%llu\n", 
		    (unsigned long long) src_offset, 
		    (unsigned long long) dst_offset,
		    (unsigned long long) length);

#if DMA_MAIN == 1
    /* Chi sinh du lieu mau khi chay ./dma_testbench doc lap.
     * Duoi ./os thi vung nguon la du lieu THAT cua process - ghi de
     * len no la pha hong bo nho cua chuong trinh dang chay. */
    uint8_t* psrc = (uint8_t*)src;
    uint64_t pattern_length = (length <= 255) ? length : 255;

    for (uint64_t i = 0; i < length; i++) {
        // Pattern cycles through 0x01 to min(length, 255)
        psrc[i] = (uint8_t)((i % pattern_length) + 1);
    }
#endif

    printf("Before copy:\n");
    printf("  src[0] = 0x%02x, src[1] = 0x%02x\n"
           "  src[2] = 0x%02x, src[3] = 0x%02x\n"
           "  src[4] = 0x%02x, src[5] = 0x%02x\n"
           "  src[6] = 0x%02x, src[7] = 0x%02x\n",
           ((uint8_t*)src)[0], ((uint8_t*)src)[1],
           ((uint8_t*)src)[2], ((uint8_t*)src)[3],
           ((uint8_t*)src)[4], ((uint8_t*)src)[5],
           ((uint8_t*)src)[6], ((uint8_t*)src)[7]);
    printf("  dst[0] = 0x%02x, dst[1] = 0x%02x\n"
           "  dst[2] = 0x%02x, dst[3] = 0x%02x\n"
           "  dst[4] = 0x%02x, dst[5] = 0x%02x\n"
           "  dst[6] = 0x%02x, dst[7] = 0x%02x\n",
           ((uint8_t*)dst)[0], ((uint8_t*)dst)[1],
           ((uint8_t*)dst)[2], ((uint8_t*)dst)[3],
           ((uint8_t*)dst)[4], ((uint8_t*)dst)[5],
           ((uint8_t*)dst)[6], ((uint8_t*)dst)[7]);

    printf("\nCalling RTL DMA...\n");
    
    // Call the RTL DMA function directly with provided parameters
    int result = dpi_rtl_dma_copy(src_offset, dst_offset, length);
    
    printf("\nAfter copy:\n");
    printf("  src[0] = 0x%02x, src[1] = 0x%02x\n"
           "  src[2] = 0x%02x, src[3] = 0x%02x\n"
           "  src[4] = 0x%02x, src[5] = 0x%02x\n"
           "  src[6] = 0x%02x, src[7] = 0x%02x\n",
           ((uint8_t*)src)[0], ((uint8_t*)src)[1],
           ((uint8_t*)src)[2], ((uint8_t*)src)[3],
           ((uint8_t*)src)[4], ((uint8_t*)src)[5],
           ((uint8_t*)src)[6], ((uint8_t*)src)[7]);
    printf("  dst[0] = 0x%02x, dst[1] = 0x%02x\n"
           "  dst[2] = 0x%02x, dst[3] = 0x%02x\n"
           "  dst[4] = 0x%02x, dst[5] = 0x%02x\n"
           "  dst[6] = 0x%02x, dst[7] = 0x%02x\n",
           ((uint8_t*)dst)[0], ((uint8_t*)dst)[1],
           ((uint8_t*)dst)[2], ((uint8_t*)dst)[3],
           ((uint8_t*)dst)[4], ((uint8_t*)dst)[5],
           ((uint8_t*)dst)[6], ((uint8_t*)dst)[7]);
    
    return result;
}

/*
 * do_dma - perform DMA function
 * source: source memory
 * destination: destination memory
 * length: number of bytes to copy
 */
extern "C" int do_dma(struct pcb_t *caller,uint64_t source, uint64_t destination, uint64_t length) {
    
    int test_errors = 0;

    /* Perform DMA copy using DPI with provided caller and parameters */
    test_errors += dpi_dma_copy(caller, source, destination, length);

    /* dma_done_callback() KHONG can goi tu day: chinh mach goi no qua
     * DPI tai suon len cua ch_done. Dem lai bang dma_done_count(). */

    return test_errors;
}

/*
 * do_dma_start - khoi dong DMA roi TRA VE NGAY (duong bat dong bo)
 * Dung cho luong: CPU nap lenh -> di lam viec khac -> thinh thoang hoi.
 */
extern "C" int do_dma_start(struct pcb_t *caller, uint64_t source,
                            uint64_t destination, uint64_t length) {
    void *src = NULL, *dst = NULL;

    if (!memory_bank_is_available()
        && caller && caller->krnl && caller->krnl->mram
        && caller->krnl->mram->storage) {
        memory_bank_attach(caller->krnl->mram->storage,
                           (size_t) caller->krnl->mram->maxsz);
    }
    src = memory_bank_get_ptr(source);
    dst = memory_bank_get_ptr(destination);
    if (!src || !dst) {
        printf("do_dma_start: offset ngoai mien\n");
        return -1;
    }

    return dpi_rtl_dma_start(source, destination, length);
}

/*
 * do_dma_poll - cap nclk chu ky clock cho job dang chay
 * tra 1 = xong, 0 = con chay
 */
extern "C" int do_dma_poll(int nclk) {
    return dpi_rtl_dma_tick(nclk);
}

#if DMA_MAIN == 1
/* main function for independent standalone dma_testbench build
 * [Usage]: ./dma_testbench [src_offset] [dst_offset] [length] [async]
 * source: source memory offset
 * destination: destination memory offset
 * length: number of bytes to copy
 */
int main(int argc, char** argv) {
    // Default test parameters
    uint64_t source = 0x1000;
    uint64_t destination = 0x2000;
    uint64_t length = 512;

    /*
     *  Only accept command line arguments if provided
     *  [Usage]: ./dma_testbench [source_offset] [destination_offset] [length]
     */
    if (argc >= 4) {
        source = strtoull(argv[1], NULL, 0);
        destination = strtoull(argv[2], NULL, 0);
        length = strtoull(argv[3], NULL, 0);
    }
    
    printf("Running dma_testbench source=0x%llx, destination=0x%llx, length=%llu\n",
           (unsigned long long)source, (unsigned long long)destination, (unsigned long long)length);

    /*
     * Initialize the system memory bank
     * This is necessary to independent standalone dma_testbench build .
     */
    memory_bank_init(1024 * 1024);

    /* Initialize the internal Verilator RTL simulation
     * This is necessary to ensure that the RTL simulation is ready for DPI calls.
     */
    if (rtl_dma_init_simulation() != 0) {
        printf("Cannot initialize RTL simulation\n");
        return 1;
    }

    /*
    * Create process control block for the caller
    * Properly initialize all required fields
    */
    struct pcb_t *caller = (struct pcb_t *)calloc(1, sizeof(struct pcb_t));
    caller->krnl = (struct krnl_t *)calloc(1, sizeof(struct krnl_t));
    caller->krnl->mm = (struct mm_struct *)calloc(1, sizeof(struct mm_struct));
    caller->krnl->mram = (struct memphy_struct *)calloc(1, sizeof(struct memphy_struct));
    caller->krnl->active_mswp = (struct memphy_struct *)calloc(1, sizeof(struct memphy_struct));
    caller->mm = caller->krnl->mm;
    caller->pid = 0;  // Set a default PID

    int result;

    if (argc >= 5 && strcmp(argv[4], "async") == 0) {
        /* --- Che do bat dong bo: mo phong vong lap cua co_processor ---
         * Moi "time slot" chi cap DMA_CLK_PER_SLOT chu ky clock roi
         * nha CPU ra. In ra de thay job KEO DAI qua nhieu slot, tuc
         * CPU khong he bi giam trong luc DMA chay. */
        printf("\n=== Che do BAT DONG BO (%d chu ky/slot) ===\n",
               DMA_CLK_PER_SLOT);

        if (do_dma_start(caller, source, destination, length) != 0) {
            printf("Khong khoi dong duoc DMA\n");
            return 1;
        }
        printf("slot  0: da nap lenh, CPU duoc tra ve ngay "
               "(trang thai = RUNNING)\n");

        int slot = 0;
        while (do_dma_poll(DMA_CLK_PER_SLOT) == 0) {
            slot++;
            printf("slot %2d: DMA con chay (%u byte), CPU van lam viec khac\n",
                   slot, dpi_rtl_dma_bytes());
        }
        slot++;
        printf("slot %2d: DMA xong - %u byte sau %llu chu ky, "
               "dma_done_callback() duoc goi %lu lan\n",
               slot, dpi_rtl_dma_bytes(),
               (unsigned long long) dpi_rtl_dma_cycles(),
               dma_done_count());

        result = dpi_rtl_dma_reap();
        printf("da nha kenh (ch_enable = 0), trang thai ve IDLE\n\n");
    } else {
        result = do_dma(caller, source, destination, length);
    }

    // Cleanup
    free(caller->krnl->active_mswp);
    free(caller->krnl->mram);
    free(caller->krnl->mm);
    free(caller->krnl);
    free(caller);

    return result;
}
#endif
