# Merge notes: TV4 + TV5

## Quyết định hợp nhất

- **Lõi DMA/OS:** giữ luồng bất đồng bộ của TV4 trong `src/dma_c_testbench.cpp`, `src/dpi_rtl.cpp`, `src/libmem.c`, `src/os.c` và các header tương ứng. Luồng này có `do_dma_start()`, hàng đợi FIFO, `do_dma_kick()` và thread `co_processor()` chạy song song với CPU.
- **System bus:** kết hợp các API bind/range-check của TV5 (`memory_bank_bind()`, `memory_bank_check_range()`) với API attach và bộ đếm `dma_done_callback()` của TV4. Bank 0 được dùng làm vùng RAM phẳng mà RTL truy cập.
- **Tương thích:** giữ `rtl_dma_set_trace()` của TV4 và thêm alias `rtl_dma_set_pause()` từ TV5, ánh xạ sang `dpi_rtl_dma_pause()`.
- **Tài nguyên riêng TV4:** giữ `docs/CHOT-INTERFACE-DMA.md`, `input/os_dma_queue` và `input/proc/dma_big`.
- **Các file giống nhau:** giữ nguyên, gồm Makefile, các file RTL/Verilator generated, hầu hết mã OS, input và output test có sẵn.

## Kiểm tra

`make os` và `make dma` cần môi trường Verilator tương thích với generated model trong `src/` và `include/`. Nếu build tại máy khác, kiểm tra phiên bản Verilator theo tài liệu `docs/CHOT-INTERFACE-DMA.md`.
