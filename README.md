# demacOprx – OS Simulation with DMA (Verilator RTL)

Operating System Simulation for HCMC University of Technology Course CO2018.

## Làm việc nhóm

1. Mỗi thành viên làm trên một branch riêng và push code lên branch đó.
2. Chỉ merge sau khi đã kiểm tra và test phần việc của branch.
3. Ghi chú các điểm quan trọng trong code; cập nhật README khi thêm chức năng để các thành viên khác dễ review.

## Môi trường & Cài đặt

Dự án sử dụng Ubuntu/Linux, C/C++ và **Verilator** để biên dịch và mô phỏng mạch phần cứng DMA RTL.
Tham khảo [Course CO2018 Documentation](https://docs.google.com/document/d/1vCxl0sBotFjONqRJBAlnkTVYtlL1XZQrTyVl_dO0zxg/edit?usp=sharing).

Cài đặt trình biên dịch C/C++, `make` và `verilator` trên Ubuntu/Debian (hoặc WSL):

```bash
sudo apt update
sudo apt install -y build-essential g++ make verilator
```

Kiểm tra phiên bản sau khi cài đặt:

```bash
verilator --version
```

## Cấu hình hệ thống (`include/os-cfg.h`)

Trước khi biên dịch, có thể bật/tắt các tính năng và chế độ in nhật ký gỡ lỗi (debug log) thông qua các cờ tiền xử lý (`#define`) trong tệp `include/os-cfg.h` (và `src/libmem.c`):

| Cờ cấu hình | Trạng thái khuyến nghị | Công dụng & Lưu ý khi bật |
| :--- | :--- | :--- |
| **`MLQ_SCHED`** | **Bắt buộc bật** | Kích hoạt bộ lập lịch hàng đợi đa mức (Multi-Level Queue, 140 mức ưu tiên) và đọc cột `prio` từ file cấu hình. *(Nhánh lập lịch đơn khi tắt cờ này chưa hoàn thiện)*. |
| **`MM_PAGING`** | **Bắt buộc bật** | Kích hoạt phân hệ quản lý bộ nhớ ảo phân trang (RAM, SWAP, không gian kernel và nối bus DMA với `mram`). |
| **`MM64`** | **Bắt buộc bật** | Kích hoạt mô hình địa chỉ 64-bit (57-bit địa chỉ ảo) với phân trang 5 cấp (`PGD -> P4D -> PUD -> PMD -> PT`). Dùng kèm với `MM_PAGING`. |
| **`MM_FIXED_MEMSZ`** | *Tùy chọn (Tắt)* | Dùng kích thước RAM/SWAP mặc định cố định (`4 GiB` RAM, `16 MiB` SWAP0) thay vì đọc từ file cấu hình. Chỉ bật khi chạy các file cấu hình cũ chỉ có 1 dòng thông số CPU/tiến trình. |
| **`IODUMP`** | *Bật khi cần kiểm tra* | In trạng thái bộ nhớ vật lý (`MEMPHY_dump`), thông tin vùng nhớ được cấp phát (`[ALLOC]`) và dữ liệu đọc/ghi thực tế (`[VERIFY READ]`, `[VERIFY Write]`). |
| **`PAGETBL_DUMP`** | *Bật khi cần debug* | In toàn bộ bảng trang của tiến trình sau mỗi thao tác `alloc`, `read`, `write`. *(Lưu ý: tạo rất nhiều log nếu tiến trình thực thi nhiều lệnh)*. |
| **`SEE_PROGRAM_COUNTER`** | *Bật khi cần debug* | In chi tiết từng bước thực thi lệnh của CPU (`[EXEC] CPU=... PID=... PC=... -> ...`) và thông báo khi tiến trình hoàn tất toàn bộ lệnh. |
| **`SCHED_DBG`** | *Bật khi cần debug* | In nhật ký hoạt động của bộ lập lịch MLQ kèm mốc thời gian `t`: nạp tiến trình, lấy tiến trình, số `slot` còn lại, nạp lại vòng mới và kích thước `running_list`. |
| **`MAINDEBUGCONFIG`** | *Tùy chọn* | In thông báo lỗi chi tiết ra `stderr` nếu gặp sự cố khi đọc file cấu hình hoặc khởi tạo `MEMRAM`/`MEMSWP`. |
| **`DMADEBUG`** *(trong `src/libmem.c`)* | *Bật khi cần debug* | In chi tiết quá trình cắt trang, căn lề 4-byte (`dau`, `DMA`, `duoi`), trạng thái xếp hàng DMA hoặc lý do từ chối tham số không hợp lệ. |

> **Lưu ý:** Sau khi thay đổi bất kỳ cờ nào trong `include/os-cfg.h`, cần chạy `make clean` trước khi biên dịch lại để đảm bảo toàn bộ các tệp đối tượng (`.o`) được cập nhật đồng bộ.
Hien tai dang bat cac co  
## Build toàn bộ OS

```bash
make clean
make os
```

Chạy mô phỏng:

```bash
./os [configuration_file]
```

Ví dụ:

```bash
./os os_dma_queue
```

## Build DMA testbench độc lập

```bash
make dma
```

Chạy testbench:

```bash
./dma_testbench
./dma_testbench 0x1000 0x2000 1024
./dma_testbench 0x1000 0x2000 1024 async
```

## Kiến trúc DMA sau khi hợp nhất

- CPU gọi `libmem_dma_memcpy()` và nạp yêu cầu DMA bất đồng bộ; CPU không phải chờ DMA copy xong.
- `co_processor()` chạy song song với CPU, cấp clock cho RTL theo từng time slot.
- Một hàng đợi FIFO phần mềm xử lý các yêu cầu khi kênh DMA đơn đang bận.
- System bus dùng bank 0. Khi chạy OS, bank 0 trỏ đến RAM thật `os.mram->storage`; khi chạy testbench độc lập, bank 0 được cấp phát riêng.
- Các vùng đọc/ghi được kiểm tra biên trước khi RTL truy cập RAM.

Chi tiết handshake, trạng thái FSM, giới hạn độ dài và quy tắc nhả kênh nằm trong [docs/CHOT-INTERFACE-DMA.md](docs/CHOT-INTERFACE-DMA.md).

## License

See [LICENSE](LICENSE) — educational use only for CO2018.