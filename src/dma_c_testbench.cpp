/*
 * DMA service and C entry point used by the OS CPU instruction dispatcher.
 * CPU threads submit requests; a dedicated co-processor thread owns and clocks
 * the Verilated RTL model. Each request waits for its own completion event.
 */
#ifndef DMA_MAIN
#define DMA_MAIN 0
#endif

#include "common.h"
#include "sysbus.h"
#include "dpi_rtl.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>

extern "C" int dpi_rtl_dma_copy(uint64_t, uint64_t, uint64_t);

struct DmaRequest {
    uint64_t source;
    uint64_t destination;
    uint64_t length;
    std::mutex mutex;
    std::condition_variable completed_cv;
    bool completed = false;
    int result = -1;
};

static std::mutex service_mutex;
static std::condition_variable service_cv;
static std::deque<std::shared_ptr<DmaRequest> > request_queue;
static std::thread service_thread;
static bool service_started = false;
static bool service_ready = false;
static bool service_failed = false;
static bool service_stopping = false;

static void dma_worker(void)
{
    const int init_rc = rtl_dma_init_simulation();
    {
        std::lock_guard<std::mutex> lock(service_mutex);
        service_failed = (init_rc != 0);
        service_ready = true;
    }
    service_cv.notify_all();
    if (init_rc != 0) return;

    for (;;) {
        std::shared_ptr<DmaRequest> request;
        {
            std::unique_lock<std::mutex> lock(service_mutex);
            service_cv.wait(lock, [] {
                return service_stopping || !request_queue.empty();
            });
            if (service_stopping && request_queue.empty()) break;
            request = request_queue.front();
            request_queue.pop_front();
        }

        const int rc = dpi_rtl_dma_copy(request->source,
                                        request->destination,
                                        request->length);
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            request->result = rc;
            request->completed = true;
        }
        request->completed_cv.notify_one();
    }
    rtl_dma_cleanup_simulation();
}

extern "C" void dma_service_pause(int paused)
{
    rtl_dma_set_pause(paused);
}

extern "C" int dma_service_start(void)
{
    std::unique_lock<std::mutex> lock(service_mutex);
    if (service_started) return service_failed ? -1 : 0;

    service_ready = false;
    service_failed = false;
    service_stopping = false;
    service_started = true;
    service_thread = std::thread(dma_worker);
    service_cv.wait(lock, [] { return service_ready; });
    const int rc = service_failed ? -1 : 0;
    lock.unlock();

    if (rc != 0 && service_thread.joinable()) {
        service_thread.join();
        std::lock_guard<std::mutex> reset_lock(service_mutex);
        service_started = false;
    }
    return rc;
}

extern "C" void dma_service_stop(void)
{
    {
        std::lock_guard<std::mutex> lock(service_mutex);
        if (!service_started) return;
        service_stopping = true;
    }
    service_cv.notify_all();
    if (service_thread.joinable()) service_thread.join();

    std::lock_guard<std::mutex> lock(service_mutex);
    service_started = false;
    service_ready = false;
    service_failed = false;
    service_stopping = false;
}

extern "C" int do_dma(struct pcb_t *caller, uint64_t source,
                       uint64_t destination, uint64_t length)
{
    if (length == 0 || (length & 3U) != 0 || length > UINT32_MAX ||
        (source & 3U) != 0 || (destination & 3U) != 0) {
        fprintf(stderr, "do_dma: offsets must be 4-byte aligned and length a positive multiple of 4\n");
        return -1;
    }

    /* In OS mode, expose the actual MEMPHY RAM buffer to the RTL bus. */
    if (!memory_bank_is_available() && caller && caller->krnl &&
        caller->krnl->mram && caller->krnl->mram->storage) {
        if (memory_bank_bind(caller->krnl->mram->storage,
                             (size_t)caller->krnl->mram->maxsz) != 0) {
            fprintf(stderr, "do_dma: cannot bind OS RAM to DMA bus\n");
            return -1;
        }
    }
    if (!memory_bank_check_range(source, (size_t)length) ||
        !memory_bank_check_range(destination, (size_t)length)) {
        fprintf(stderr, "do_dma: source/destination outside mapped RAM\n");
        return -1;
    }

    std::shared_ptr<DmaRequest> request(new DmaRequest);
    request->source = source;
    request->destination = destination;
    request->length = length;

    {
        std::lock_guard<std::mutex> lock(service_mutex);
        if (!service_started || !service_ready || service_failed ||
            service_stopping) {
            fprintf(stderr, "do_dma: DMA service is not running\n");
            return -1;
        }
        request_queue.push_back(request);
    }
    service_cv.notify_one();

    std::unique_lock<std::mutex> request_lock(request->mutex);
    request->completed_cv.wait(request_lock, [&] { return request->completed; });
    return request->result;
}

#if DMA_MAIN == 1
int main(int argc, char **argv)
{
    uint64_t source = 0x1000;
    uint64_t destination = 0x2000;
    uint64_t length = 512;

    if (argc >= 4) {
        source = strtoull(argv[1], NULL, 0);
        destination = strtoull(argv[2], NULL, 0);
        length = strtoull(argv[3], NULL, 0);
    }

    if (length == 0 || (length & 3U) != 0) {
        fprintf(stderr, "Length must be a positive multiple of 4 bytes\n");
        return 2;
    }

    const size_t ram_size = 1024U * 1024U;
    if (memory_bank_init(ram_size) == MEMORY_BANK_INVALID ||
        !memory_bank_check_range(source, (size_t)length) ||
        !memory_bank_check_range(destination, (size_t)length)) {
        fprintf(stderr, "Invalid RAM allocation or DMA range\n");
        return 2;
    }

    uint8_t *src = (uint8_t *)memory_bank_get_ptr(source);
    uint8_t *dst = (uint8_t *)memory_bank_get_ptr(destination);
    for (uint64_t i = 0; i < length; ++i) {
        src[i] = (uint8_t)((i % 251U) + 1U);
        dst[i] = 0;
    }

    if (dma_service_start() != 0) {
        fprintf(stderr, "Cannot start DMA service\n");
        memory_bank_cleanup();
        return 1;
    }

    const int rc = do_dma(NULL, source, destination, length);
    int mismatch = 0;
    for (uint64_t i = 0; i < length; ++i) {
        if (src[i] != dst[i]) {
            fprintf(stderr, "Mismatch at byte %llu: src=%02x dst=%02x\n",
                    (unsigned long long)i, src[i], dst[i]);
            mismatch = 1;
            break;
        }
    }

    printf("DMA test: src=0x%llx dst=0x%llx len=%llu result=%s compare=%s\n",
           (unsigned long long)source, (unsigned long long)destination,
           (unsigned long long)length, rc == 0 ? "OK" : "FAIL",
           mismatch ? "FAIL" : "PASS");

    dma_service_stop();
    memory_bank_cleanup();
    return (rc == 0 && !mismatch) ? 0 : 1;
}
#endif
