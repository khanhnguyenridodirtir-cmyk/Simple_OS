# demacOprx – OS Simulation with DMA (Verilator RTL)

Operating System Simulation for HCMC University of Technology Course CO2018.

## Làm việc nhóm

1. Mỗi thành viên làm trên một branch riêng và push code lên branch đó.
2. Chỉ merge sau khi đã kiểm tra và test phần việc của branch.
3. Ghi chú các điểm quan trọng trong code; cập nhật README khi thêm chức năng để các thành viên khác dễ review.

## Môi trường

Dự án sử dụng Ubuntu/Linux, C/C++ và Verilator để mô phỏng DMA RTL.
Tham khảo [Course CO2018 Documentation](https://docs.google.com/document/d/1vCxl0sBotFjONqRJBAlnkTVYtlL1XZQrTyVl_dO0zxg/edit?usp=sharing).

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
