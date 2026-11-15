#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "queue.h"
#include "sched.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s (line %d)\n", msg, __LINE__); fails++; } \
    else { printf("  [PASS] %s\n", msg); } \
} while (0)

static int fails = 0;

static struct pcb_t *create_dummy_proc(uint32_t pid, uint32_t prio) {
    struct pcb_t *p = (struct pcb_t *)calloc(1, sizeof(struct pcb_t));
    p->pid = pid;
    p->prio = prio;
    return p;
}

int main(void) {
    printf("=== BAT DAU TEST EDGE CASES CHO MLQ SCHEDULER ===\n");
    init_scheduler();

    /* Test 1: Lay proc khi he thong hoan toan rong */
    CHECK(queue_empty() == 1, "Ban dau queue phai rong");
    CHECK(get_proc() == NULL, "get_proc() khi rong phai tra ve NULL");

    /* Test 2: Thu tu uu tien MLQ (prio nho hon chay truoc) */
    struct pcb_t *p_low  = create_dummy_proc(101, 10);
    struct pcb_t *p_high = create_dummy_proc(102, 2);
    struct pcb_t *p_mid  = create_dummy_proc(103, 5);

    add_proc(p_low);
    add_proc(p_high);
    add_proc(p_mid);

    struct pcb_t *r1 = get_proc();
    CHECK(r1 != NULL && r1->pid == 102, "Uu tien cao nhat (prio=2) duoc lay ra dau tien");
    
    /* Tra lai p_high vao hang doi (het time slice) */
    put_proc(r1);
    struct pcb_t *r2 = get_proc();
    CHECK(r2 != NULL && r2->pid == 102, "p_high van con slot nen tiep tuc duoc chon");
    finish_proc(r2);

    struct pcb_t *r3 = get_proc();
    CHECK(r3 != NULL && r3->pid == 103, "Tiep theo la prio=5");
    finish_proc(r3);

    struct pcb_t *r4 = get_proc();
    CHECK(r4 != NULL && r4->pid == 101, "Cuoi cung la prio=10");
    finish_proc(r4);

    CHECK(get_proc() == NULL, "Sau khi finish het, get_proc() phai tra ve NULL");

    /* Test 3: Kiem tra co che reset slot khi dung can slot */
    struct pcb_t *p_starve = create_dummy_proc(201, MAX_PRIO - 1); /* Chi co 1 slot */
    add_proc(p_starve);

    struct pcb_t *s1 = get_proc();
    CHECK(s1 != NULL && s1->pid == 201, "Lay lan 1 (tieu hao 1 slot duy nhat)");
    put_proc(s1); /* Het slot o muc prio nay */

    struct pcb_t *s2 = get_proc();
    CHECK(s2 != NULL && s2->pid == 201, "Reset slot thanh cong khi tat ca queue khac rong");
    finish_proc(s2);

    free(p_low); free(p_high); free(p_mid); free(p_starve);

    printf("=== KET QUA: %d LOI ===\n", fails);
    return fails != 0;
}