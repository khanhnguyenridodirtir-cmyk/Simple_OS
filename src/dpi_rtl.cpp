/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * DPI RTL C++ interface to  DMA RTL
 */

#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vdma_top.h"
#include "sysbus.h"
#include "dpi_rtl.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <pthread.h>

// DMA RTL simulation structures
static Vdma_top* top = nullptr;
static VerilatedContext* context = nullptr;
static VerilatedVcdC* tracer = nullptr;

static bool is_tracing = false;
static uint64_t sim_time = 0;

/* Co ghi waveform hay khong. Mac dinh BAT cho ./dma_testbench.
 * co_processor() trong ./os phai TAT truoc khi init - ly do o
 * rtl_dma_set_trace() ben duoi. */
static bool g_trace_wanted = true;

/* ===================================================================
 * Khoa cho model Verilator
 * -------------------------------------------------------------------
 * Duoi ./os co HAI loai thread cung cham vao mot doi tuong `top`:
 *   - thread CPU  goi dpi_rtl_dma_start()  (qua libmem_dma_memcpy)
 *   - thread DMA  goi dpi_rtl_dma_tick() / _reap()  (co_processor)
 * Ca hai deu goi top->eval(). Model Verilator KHONG thread-safe: hai
 * eval() chong nhau se lam hong trang thai ben trong mach. Moi ham
 * public duoi day giu khoa nay trong suot than ham.
 * =================================================================== */
static pthread_mutex_t g_dma_lock = PTHREAD_MUTEX_INITIALIZER;
struct DmaLock {
    DmaLock()  { pthread_mutex_lock(&g_dma_lock); }
    ~DmaLock() { pthread_mutex_unlock(&g_dma_lock); }
};

/**
 * rtl_dma_set_trace - bat/tat ghi file rtl_trace.vcd
 *
 * PHAI tat khi chay trong ./os. Thu vien Verilator ghi VCD bang ham
 * POSIX ::write(fd, buf, len). Nhung src/cpu.c:60 cua OS dinh nghia mot
 * ham C toan cuc cung ten `write(struct pcb_t*, ...)`. Linker noi loi
 * goi cua Verilator vao ham cua cpu.c -> segfault luc flush/close VCD.
 * ./dma_testbench khong link cpu.c nen khong bi.
 * Goi TRUOC rtl_dma_init_simulation().
 */
extern "C" void rtl_dma_set_trace(int on) {
    g_trace_wanted = (on != 0);
}

/**
 * rtl_dma_init_simulation - Initialize the RTL simulation for DMA
 */
extern "C" int rtl_dma_init_simulation(void) {
    if (top) return 0;
    if (context) return 0;
    
    context = new VerilatedContext;
    const char* args[] = {};
    context->commandArgs(0, args);
    top = new Vdma_top(context, "dma_top");
    
    sim_time = 0;
    if (g_trace_wanted) {
        tracer = new VerilatedVcdC;
        context->traceEverOn(true);
        top->trace(tracer, 99);
        tracer->open("rtl_trace.vcd");
        is_tracing = true;
    } else {
        is_tracing = false;
    }
    
    printf("Initialized dma_rtl simulation\n");
    return 0;
}

/**
 * rtl_dma_cleanup_simulation - Cleanup the RTL simulation for DMA
 */
extern "C" void rtl_dma_cleanup_simulation(void) {
    if (tracer) {
        tracer->close();
        delete tracer;
        tracer = nullptr;
    }
    if (top) {
        delete top;
        top = nullptr;
    }
    if (context) {
        delete context;
        context = nullptr;
    }
    is_tracing = false;
    sim_time = 0;
    printf("Clean up dma_rtl simulation\n");
}

/* ===================================================================
 * Trang thai job DMA - chi mot kenh nen mot bo bien toan cuc la du.
 * RTL set last_desc = 1 cung (DepSet_hbdce0907:180) => khong co hang
 * doi phan cung, moi luc chi chay duoc mot descriptor.
 * =================================================================== */
static dma_job_state_t g_state  = DMA_JOB_IDLE;
static uint64_t        g_cycles = 0;   /* so chu ky job hien tai da chay */

/* Dap tron mot chu ky clock: xuong 0 roi len 1.
 * Mach chi nhuc nhich khi co eval(), khong tu chay nhu FPGA that. */
static inline void tick_one_cycle(void) {
    top->clk = 0;
    top->eval();
    top->clk = 1;
    top->eval();
    if (is_tracing) {
        tracer->dump(sim_time);
        sim_time += 10;
    }
    g_cycles++;
}

/**
 * dpi_rtl_dma_start - nap descriptor va bat kenh, TRA VE NGAY
 *
 * Dieu kien khoi dong lay nguyen tu RTL (DepSet_hbdce0907:369-372):
 *     start_cond = (current_state == IDLE)
 *                & ch_enable & ~ch_pause & desc_valid
 * Bon dieu kien phai dung dong thoi, thieu mot cai thi mach im lang.
 */
extern "C" int dpi_rtl_dma_start(uint64_t source, uint64_t destination,
                                 uint64_t length) {
    DmaLock lk;
    if (!top) {
        printf("Cannot start DMA: RTL simulation not initialized\n");
        return -1;
    }
    if (g_state == DMA_JOB_RUNNING) {
        /* Mot kenh, mot job. Nguoi goi phai doi hoac xep hang. */
        return -1;
    }

    /* Reset ve IDLE cho chac truoc moi job */
    top->clk = 0;  top->rst_n = 0;  top->eval();
    top->clk = 1;                   top->eval();
    top->rst_n = 1;
    top->clk = 0;                   top->eval();
    top->clk = 1;                   top->eval();

    /* Descriptor: chi 3 word co nghia.
     * desc_data_3 khong duoc RTL doc o bat ky dau - da grep ca
     * src/Vdma_top*.cpp, no chi bi gan 0 trong ham init. */
    top->desc_data_0 = source;        /* offset nguon, KHONG phai con tro */
    top->desc_data_1 = destination;   /* offset dich */
    top->desc_data_2 = length;        /* so byte; RTL cat con 32 bit */
    top->desc_data_3 = 0;

    top->desc_valid = 1;
    top->ch_enable  = 1;
    top->ch_pause   = 0;

    g_state  = DMA_JOB_RUNNING;
    g_cycles = 0;

    /* Mot chu ky de mach chuyen IDLE -> LOAD, roi tra ve ngay.
     * Tu day CPU ranh di lam viec khac. */
    tick_one_cycle();
    return 0;
}

/**
 * dpi_rtl_dma_tick - cap nclk chu ky clock cho job dang chay
 * tra 1 neu job da ket thuc, 0 neu con chay
 */
extern "C" int dpi_rtl_dma_tick(int nclk) {
    DmaLock lk;
    if (!top || g_state != DMA_JOB_RUNNING) {
        return (g_state == DMA_JOB_DONE || g_state == DMA_JOB_ERROR);
    }

    for (int i = 0; i < nclk; i++) {
        if (top->ch_done) break;
        if (g_cycles >= DMA_MAX_CYCLES) {
            printf("DMA timeout sau %llu chu ky\n",
                   (unsigned long long) g_cycles);
            g_state = DMA_JOB_ERROR;
            return 1;
        }
        tick_one_cycle();
    }

    if (top->ch_done) {
        /* Dap them 2 nua chu ky voi ch_enable VAN giu, de mach kip goi
         * dma_done_callback() tai suon len cua ch_done. */
        top->clk ^= 1; top->eval();
        top->clk ^= 1; top->eval();
        g_state = top->ch_error ? DMA_JOB_ERROR : DMA_JOB_DONE;
        return 1;
    }
    return 0;   /* con chay, slot sau tick tiep */
}

extern "C" dma_job_state_t dpi_rtl_dma_state(void) { DmaLock lk; return g_state; }

extern "C" uint32_t dpi_rtl_dma_bytes(void) {
    DmaLock lk;
    return top ? (uint32_t) top->ch_bytes_transferred : 0u;
}

extern "C" uint64_t dpi_rtl_dma_cycles(void) { DmaLock lk; return g_cycles; }

/**
 * dpi_rtl_dma_reap - nha kenh, tra channel ve IDLE
 *
 * BAT BUOC. May trang thai: 0 IDLE -> 1 -> 2 COPY -> 3 -> 4 DONE,
 * va state 4 CHI ve 0 khi ch_enable = 0 (DepSet_hbdce0907:34-36).
 * Ha ch_enable chua du - con phai dap clock thi mach moi chuyen,
 * vi current_state <= next_state nam trong khoi tuan tu theo clk.
 */
extern "C" int dpi_rtl_dma_reap(void) {
    DmaLock lk;
    if (!top) return -1;

    top->desc_valid = 0;
    top->ch_enable  = 0;
    top->ch_pause   = 0;

    /* Hai chu ky de FSM thuc su di tu DONE ve IDLE */
    tick_one_cycle();
    tick_one_cycle();

    int rc = (g_state == DMA_JOB_DONE) ? 0 : -1;
    g_state = DMA_JOB_IDLE;
    return rc;
}

/**
 * dpi_rtl_dma_pause - dong bang / chay tiep giua chung
 * RTL dong 358: current_state <= next_state CHI khi ch_pause == 0,
 * nen ch_pause = 1 dong bang ca may trang thai, ha xuong thi chay
 * tiep dung cho dang do.
 */
extern "C" void dpi_rtl_dma_pause(int on) {
    DmaLock lk;
    if (top) top->ch_pause = on ? 1 : 0;
}

/**
 * dpi_rtl_dma_copy - ban CHAN cu, giu lai cho ./dma_testbench
 *
 * Gio no chi la mot vong lap mong tren ba ham bat dong bo o tren.
 * Giu lai de khong pha API cu va de chung minh hai duong cho ra
 * cung mot ket qua.
 */
extern "C" int dpi_rtl_dma_copy(uint64_t source, uint64_t destination,
                                uint64_t length) {
    if (dpi_rtl_dma_start(source, destination, length) != 0) return -1;
    while (!dpi_rtl_dma_tick(DMA_CLK_PER_SLOT)) { /* cho */ }
    return dpi_rtl_dma_reap();
}


/**
 * dpi_rtl_dma_is_available - Check if RTL DMA is available
 */
extern "C" int dpi_rtl_dma_is_available(void) {
    return top != nullptr;
}



