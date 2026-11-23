/*
 * C++ driver for the generated Verilator DMA model.
 * The model is driven by one dedicated DMA service thread; callers must not
 * call this function concurrently from CPU threads.
 */
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vdma_top.h"
#include "sysbus.h"
#include "dpi_rtl.h"
#include <cstdio>
#include <cstdint>
#include <atomic>

static Vdma_top *top = nullptr;
static VerilatedContext *context = nullptr;
static VerilatedVcdC *tracer = nullptr;
static bool is_tracing = false;
static std::atomic<bool> pause_requested(false);

static void dump_trace(void)
{
    if (is_tracing && tracer && context) tracer->dump(context->time());
}

/* One full clock cycle, ending with clk low. */
static void clock_cycle(void)
{
    top->ch_pause = pause_requested.load() ? 1 : 0;
    top->clk = 0;
    top->eval();
    dump_trace();
    if (context) context->timeInc(5);

    top->ch_pause = pause_requested.load() ? 1 : 0;
    top->clk = 1;
    top->eval();
    dump_trace();
    if (context) context->timeInc(5);

    top->ch_pause = pause_requested.load() ? 1 : 0;
    top->clk = 0;
    top->eval();
}

extern "C" void rtl_dma_set_pause(int paused)
{
    pause_requested.store(paused != 0);
}

extern "C" int rtl_dma_init_simulation(void)
{
    if (top) return 0;

    context = new VerilatedContext;
    top = new Vdma_top(context, "dma_top");

    top->clk = 0;
    top->rst_n = 0;
    top->ch_enable = 0;
    pause_requested.store(false);
    top->ch_pause = 0;
    top->desc_valid = 0;
    top->desc_data_0 = 0;
    top->desc_data_1 = 0;
    top->desc_data_2 = 0;
    top->desc_data_3 = 0;
    top->eval();

    /* Synchronous active-low reset. */
    clock_cycle();
    clock_cycle();
    top->rst_n = 1;
    clock_cycle();

    context->traceEverOn(true);
    tracer = new VerilatedVcdC;
    top->trace(tracer, 99);
    tracer->open("rtl_trace.vcd");
    is_tracing = true;

    printf("Initialized DMA RTL (Verilator)\n");
    return 0;
}

extern "C" void rtl_dma_cleanup_simulation(void)
{
    if (tracer) {
        tracer->close();
        delete tracer;
        tracer = nullptr;
    }
    if (top) {
        top->final();
        delete top;
        top = nullptr;
    }
    delete context;
    context = nullptr;
    is_tracing = false;
}

extern "C" int dpi_rtl_dma_copy(uint64_t source, uint64_t destination,
                                  uint64_t length)
{
    if (!top || !context) return -1;
    if (length == 0 || (length & 3U) != 0 || length > UINT32_MAX ||
        (source & 3U) != 0 || (destination & 3U) != 0) {
        fprintf(stderr, "DMA offsets must be 4-byte aligned and length must be a positive multiple of 4\n");
        return -1;
    }
    if (!memory_bank_check_range(source, (size_t)length) ||
        !memory_bank_check_range(destination, (size_t)length)) {
        fprintf(stderr, "DMA source/destination range is outside RAM\n");
        return -1;
    }

    /* A completed job returns to IDLE after ch_enable is lowered. */
    top->desc_valid = 0;
    top->ch_enable = 0;
    clock_cycle();

    printf("[DMA START] SRC=0x%llx DST=0x%llx LEN=%llu bytes\n",
       (unsigned long long)source,
       (unsigned long long)destination,
       (unsigned long long)length);

    top->desc_data_0 = source;       /* byte offset from RAM base */
    top->desc_data_1 = destination;  /* byte offset from RAM base */
    top->desc_data_2 = length;       /* byte count */
    top->desc_data_3 = 0;            /* reserved */
    top->ch_enable = 1;
    top->desc_valid = 1;

    /* Wait for the descriptor handshake. desc_ready is a registered output. */
    unsigned int cycles = 0;
    const unsigned int handshake_limit = 16;
    while (!top->desc_ready && cycles++ < handshake_limit) {
        clock_cycle();
    }
    top->desc_valid = 0;
    if (!top->desc_ready) {
        fprintf(stderr, "DMA descriptor was not accepted\n");
        top->ch_enable = 0;
        clock_cycle();
        return -1;
    }

    /* DMA runs independently of the submitting CPU thread. */
    const uint64_t max_cycles64 = 1000ULL + length * 16ULL;
    const unsigned int max_cycles =
        max_cycles64 > 10000000ULL ? 10000000U : (unsigned int)max_cycles64;
    cycles = 0;
    unsigned int last_state = top->state;

    while (!top->ch_done && !top->ch_error && cycles < max_cycles) {
        clock_cycle();
        ++cycles;

        if (top->state != last_state) {
            printf("[DMA STATE] %u -> %u | cycle=%u | transferred=%u bytes\n",
                last_state,
                top->state,
                cycles,
                top->ch_bytes_transferred);

            last_state = top->state;
        }
    }

    const bool completed = top->ch_done;
    const bool failed = top->ch_error;
    const uint32_t transferred = top->ch_bytes_transferred;

    /* Release the channel. DONE does not return to IDLE while enabled. */
    top->ch_enable = 0;
    clock_cycle();

    if (!completed || failed || transferred != (uint32_t)length) {
        fprintf(stderr,
                "DMA failed: done=%u error=%u bytes=%u expected=%llu cycles=%u\n",
                completed ? 1U : 0U, failed ? 1U : 0U, transferred,
                (unsigned long long)length, cycles);
        return -1;
    }
    return 0;
}

extern "C" int dpi_rtl_dma_is_available(void)
{
    return top != nullptr;
}
