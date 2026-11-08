# Chốt interface DMA

*Đã xác minh trên RTL đã verilate (`src/Vdma_top*.cpp`) và chạy thử `./dma_testbench`.*

## Cấu hình

| Chân | Ý nghĩa |
|---|---|
| `desc_data_0` | **offset** nguồn, tính từ đầu RAM — không phải con trỏ |
| `desc_data_1` | **offset** đích, cùng quy ước |
| `desc_data_2` | số **byte**, hiệu dụng 32 bit, **phải là bội số của 4** |
| `desc_data_3` | không dùng — RTL không đọc. Luôn ghi 0 |

**Khởi động:** `desc_valid=1` **và** `ch_enable=1` **và** `ch_pause=0` **và** FSM đang IDLE.
Thiếu một điều kiện thì mạch im lặng không nhận.

## Năm ràng buộc

1. **`length` phải là bội số của 4.** Mạch chạy theo nhịp 4 byte; nhịp cuối vẫn
   ghi đủ 4 byte kể cả khi chỉ còn 1–3 → **ghi tràn sang vùng bên cạnh**.
   Tầng OS làm tròn lên trước khi gửi.

2. **Hạ `ch_enable = 0` sau mỗi job.** State DONE không tự về IDLE. Quên →
   **job thứ hai treo vĩnh viễn, không báo lỗi.** `os_2_mlq_paging` chạy `m2s`
   hai lần nên chắc chắn dính.

3. **Đọc `ch_done`, không đọc cổng `state`.** `state = current_state & 3` mà FSM
   rộng 3 bit → DONE(4) hiện ra `0`, trùng y hệt IDLE(0).

4. **Không dùng `irq`.** `irq = ch_done & intr_en`, mà `intr_en` hardcode 0 →
   vĩnh viễn 0. Thay bằng poll `ch_done` mỗi time slot.

5. **Verilator phải là 5.03x.** Bản 5.04+ đổi API → `Vdma_top*.cpp` không compile.
   Hai máy phải cùng bản.

## Máy trạng thái

```
0 IDLE → 1 LOAD → 2 COPY → 3 FINISH → 4 DONE → (ch_enable=0) → 0
```

`ch_done = 1` chỉ ở state 4. `dma_done_callback()` được gọi đúng một lần,
tại sườn lên của `ch_done`. `ch_pause = 1` đóng băng cả FSM, hạ xuống thì
chạy tiếp đúng chỗ đang dở.
