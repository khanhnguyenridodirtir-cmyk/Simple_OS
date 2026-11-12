#include <stdio.h>
#include "mm.h"

int main() {
    struct memphy_struct dummy_ram;
    addr_t fpn1, fpn2;

    printf("=== TEST MEMPHY MODULE ===\n");

    /* Khởi tạo RAM mô phỏng 4096 bytes (4 frames x 1024B) */
    init_memphy(&dummy_ram, 4096, 1);
    
    MEMPHY_dump(&dummy_ram);

    if (MEMPHY_get_freefp(&dummy_ram, &fpn1) == 0)
        printf("[OK] Lay duoc Frame 1: FPN = %lu\n", fpn1);
    if (MEMPHY_get_freefp(&dummy_ram, &fpn2) == 0)
        printf("[OK] Lay duoc Frame 2: FPN = %lu\n", fpn2);

    BYTE data_in = 0xAB;
    BYTE data_out = 0;
    addr_t test_addr = (fpn1 * 1024) + 10;

    MEMPHY_write(&dummy_ram, test_addr, data_in);
    MEMPHY_read(&dummy_ram, test_addr, &data_out);
    printf("[OK] Ghi 0x%02x, Doc duoc: 0x%02x\n", data_in, data_out);

    MEMPHY_dump(&dummy_ram);

    MEMPHY_put_freefp(&dummy_ram, fpn1);
    printf("[OK] Da tra Frame %lu ve free_fp_list\n", fpn1);

    return 0;
}