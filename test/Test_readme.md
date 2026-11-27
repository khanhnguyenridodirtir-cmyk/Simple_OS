# Hệ thống Kiểm thử Phân hệ Quản lý Bộ nhớ & Tích hợp (`test/`)

Thư mục này chứa bộ kiểm thử đơn vị (Unit Test) và kiểm thử tích hợp (Integration Test) cho toàn bộ phân hệ quản lý bộ nhớ 64-bit (`MM64`), bộ nhớ vật lý (`MEMPHY`), thư viện bộ nhớ ảo (`libmem`), lời gọi hệ thống `syscall 17` (`sys_memmap`), và cơ chế đồng bộ với `DMA`.

---

## 1. Mục đích và Phạm vi Kiểm thử

| Tệp mã nguồn | Target biên dịch | Số lượng Test | Nội dung & Mục tiêu kiểm thử |
| :--- | :--- | :---: | :--- |
| **`test_memphy.c`** | `test_memphy` | **18** (`MP01`–`MP18`) | Kiểm thử quản lý frame vật lý (`RAM`/`SWAP`): khởi tạo dung lượng, cấp phát/thu hồi frame, chống *double-free* và tràn biên, chế độ truy cập ngẫu nhiên/tuần tự, kiểm tra bất biến danh sách `free_fp_list`/`used_fp_list` (`verify_memphy_invariants`) và truy cập đa luồng (`pthread`). |
| **`test_mm64.c`** | `test_mm64` | **33** (`MM01`–`MM33`) | Kiểm thử phân trang 5 cấp 64-bit (`PGD -> P4D -> PUD -> PMD -> PT`, không gian 57-bit): giải mã địa chỉ ảo, thao tác `PTE` (`PRESENT`, `SWAPPED`), ánh xạ nhiều trang (`vmap_page_range`, `vm_map_ram`, `vm_map_kernel`), mở rộng `sbrk` qua `inc_vma_limit`, *Global Page Replacement* giữa nhiều tiến trình, và giải phóng cây bảng trang. |
| **`test_libmem.c`** | `test_libmem` | **24** (`LM01`–`LM24`) | Kiểm thử API bộ nhớ mức người dùng & kernel (`liballoc`, `libfree`, `libread`, `libwrite`, `libkmem_malloc`, `copy_from_user`, `copy_to_user`): phân tuyến 3 vùng `VMA_SMALL`/`MEDIUM`/`LARGE`, tái sử dụng vùng trống (*first-fit*), đọc/ghi xuyên ranh giới trang 4KB, *swap-out/swap-in* bảo toàn dữ liệu, và chặn xung đột `RAW`/`WAR` với `libmem_dma_memcpy`. |
| **`test_syscall17.c`** | `test_syscall17` | **15** (`SC17-01`–`SC17-15`) | Kiểm thử luồng xử lý lời gọi hệ thống `syscall 17` (`__sys_memmap` qua `libsyscall`): kiểm tra đủ 5 phép toán (`SYSMEM_MAP_OP`, `SYSMEM_INC_OP`, `SYSMEM_SWP_OP`, `SYSMEM_IO_READ`, `SYSMEM_IO_WRITE`), cô lập theo `PID` trong `running_list`, từ chối tham số/địa chỉ ngoài biên và gọi đồng thời từ nhiều tiến trình. |
| **`test_integration_mem.c`** | `test_integration_mem` | **19** (`IT01`–`IT19`) | Kiểm thử tích hợp toàn tuyến (*End-to-End*) kết hợp `User Space -> Syscall 17 -> VMA -> MM64 -> MEMPHY`: cô lập không gian địa chỉ ảo giữa các tiến trình, thay thế trang toàn cục đa tiến trình, bảo vệ vùng nhớ Kernel khỏi truy cập trái phép từ User, copy dữ liệu xuyên trang, thu hồi tài nguyên khi tiến trình kết thúc, và tích hợp truyền `DMA` đa trang. |
| **`test_helpers.h` / `test_helpers.c`** | *(Dùng chung)* | — | Cung cấp framework chạy test (`TEST_ASSERT`, `TEST_ASSERT_EQ`, `TEST_ASSERT_NEQ`, bảng tổng kết `test_summary`), hàm kiểm tra tính toàn vẹn danh sách frame vật lý và hàm stub `do_dma_start()` phục vụ kiểm thử độc lập không cần phần cứng RTL. |

---

## 2. Yêu cầu trước khi chạy Test

> **Bắt buộc biên dịch hệ điều hành (`make os`) trước khi chạy bất kỳ bài test nào.**
> 
> Lý do: Lệnh `make os` sẽ tự động cấp quyền thực thi và chạy script `src/syscalltbl.sh` để sinh tệp bảng lời gọi hệ thống `src/syscalltbl.lst` (được `src/syscall.c` nhúng trực tiếp bằng chỉ thị `#include`). Nếu chưa có tệp này, quá trình biên dịch các bài test (như `test_mm64`) sẽ báo lỗi thiếu tệp.

Đứng tại **thư mục gốc của dự án** (`Simple_OS/`), chạy:

```bash
make clean
make os
```

---

## 3. Hướng dẫn Biên dịch và Chạy Test

Tất cả các lệnh bên dưới đều được thực thi tại **thư mục gốc của dự án** (nơi đặt `Makefile`).

### Cách 1: Biên dịch và chạy toàn bộ bộ test (Khuyến nghị)

Sử dụng target `test_all` để tự động biên dịch và chạy tuần tự cả 5 bộ test (`test_memphy`, `test_mm64`, `test_libmem`, `test_syscall17`, `test_integration_mem`):

```bash
make test_all
```

*(Lưu ý: Lệnh sử dụng toán tử `&&` nên nếu một bộ test gặp lỗi và trả về mã thoát khác 0, chuỗi kiểm thử sẽ dừng ngay tại bộ test đó để tiện khoanh vùng lỗi).*

### Cách 2: Biên dịch và chạy riêng từng bộ test

Nếu chỉ muốn kiểm tra một phân hệ cụ thể, hãy gọi đúng tên target tương ứng trong `Makefile` rồi chạy tệp thực thi vừa sinh ra:

1. **Kiểm thử bộ nhớ vật lý (`MEMPHY`):**
   ```bash
   make test_memphy
   ./test_memphy
   ```

2. **Kiểm thử phân trang 5 cấp 64-bit & VMA (`MM64` & `mm-vm`):**
   ```bash
   make test_mm64
   ./test_mm64
   ```

3. **Kiểm thử thư viện cấp phát & đọc/ghi bộ nhớ (`libmem`):**
   ```bash
   make test_libmem
   ./test_libmem
   ```

4. **Kiểm thử lời gọi hệ thống bộ nhớ (`syscall 17`):**
   ```bash
   make test_syscall17
   ./test_syscall17
   ```

5. **Kiểm thử tích hợp toàn phân hệ bộ nhớ (`Integration Test`):**
   ```bash
   make test_integration_mem
   ./test_integration_mem
   ```

---

## 4. Dọn dẹp tệp biên dịch

Để xóa toàn bộ các tệp nhị phân `test_*`, tệp đối tượng `obj/*.o` và tệp sinh tự động `src/syscalltbl.lst`:

```bash
make clean
```