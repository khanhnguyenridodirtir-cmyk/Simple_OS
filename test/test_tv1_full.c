#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include "common.h"
#include "sched.h"
#include "queue.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s (dong %d)\n", msg, __LINE__); \
        fails++; \
    } else { \
        printf("  [PASS] %s\n", msg); \
    } \
} while (0)

#define CHECK_QUIET(cond, msg) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s (dong %d)\n", msg, __LINE__); \
        fails++; \
    } \
} while (0)

static int fails = 0;
static struct krnl_t global_krnl;

/* =========================================================================
 * HELPER FUNCTIONS CHO QUEUE TEST (từ test_queue_2.c)
 * ========================================================================= */
static struct pcb_t q_pool[60];

static void q_mk(int i, uint32_t pid, uint32_t pr) {
    memset(&q_pool[i], 0, sizeof(q_pool[i]));
    q_pool[i].pid = pid;
#ifdef MLQ_SCHED
    q_pool[i].prio = pr;
#else
    q_pool[i].priority = pr;
#endif
}

static void q_verify_order(struct queue_t *q, const uint32_t *exp, int n, const char *name) {
    for (int k = 0; k < n; k++) {
        struct pcb_t *p = dequeue(q);
        if (p == NULL || p->pid != exp[k]) {
            printf("  [FAIL] %s tai vi tri %d: nhan PID %d, ky vong %u\n",
                   name, k, p ? (int)p->pid : -1, exp[k]);
            fails++;
            return;
        }
    }
    CHECK(empty(q), name);
}

static void run_queue_tests(void) {
    printf("\n[PHAN A] KIEM THU CAU TRUC HANG DOI (QUEUE UNIT & STRESS TEST):\n");
    struct queue_t q;

    /* A1. Empty & NULL */
    q.size = 0;
    CHECK_QUIET(dequeue(&q) == NULL, "dequeue() tren hang doi rong -> NULL");
    CHECK_QUIET(dequeue(NULL) == NULL, "dequeue(NULL) -> NULL");
    enqueue(NULL, &q_pool[0]);
    enqueue(&q, NULL);
    CHECK(q.size == 0, "Xu ly an toan hang doi rong va tham so NULL");

    /* A2. Same priority -> FIFO */
    q.size = 0;
    q_mk(0, 1, 5); q_mk(1, 2, 5); q_mk(2, 3, 5);
    enqueue(&q, &q_pool[0]); enqueue(&q, &q_pool[1]); enqueue(&q, &q_pool[2]);
    { uint32_t e[] = {1, 2, 3}; q_verify_order(&q, e, 3, "Cung do uu tien -> giu thu tu FIFO"); }

    /* A3. Mixed priorities & tie-breaking */
    q.size = 0;
    q_mk(0, 1, 10); q_mk(1, 2, 3); q_mk(2, 3, 7); q_mk(3, 4, 3);
    for (int i = 0; i < 4; i++) enqueue(&q, &q_pool[i]);
    { uint32_t e[] = {2, 4, 3, 1}; q_verify_order(&q, e, 4, "Uu tien nho chay truoc, bang nhau giu FIFO"); }

    /* A4. Overflow protection */
    q.size = 0;
    for (int i = 0; i < 55; i++) { q_mk(i, i + 1, 0); enqueue(&q, &q_pool[i]); }
    CHECK_QUIET(q.size == MAX_QUEUE_SIZE, "Gioi han kich thuoc toi da MAX_QUEUE_SIZE");
    {
        uint32_t e[MAX_QUEUE_SIZE];
        for (int i = 0; i < MAX_QUEUE_SIZE; i++) e[i] = i + 1;
        q_verify_order(&q, e, MAX_QUEUE_SIZE, "Chong tran mang khi enqueue > MAX_QUEUE_SIZE");
    }

    /* A5. Round-robin rotation */
    q.size = 0;
    q_mk(0, 1, 1); q_mk(1, 2, 1); q_mk(2, 3, 1);
    for (int i = 0; i < 3; i++) enqueue(&q, &q_pool[i]);
    {
        uint32_t exp[] = {1, 2, 3, 1, 2, 3, 1, 2, 3};
        int okrr = 1;
        for (int i = 0; i < 9; i++) {
            struct pcb_t *p = dequeue(&q);
            if (!p || p->pid != exp[i]) okrr = 0;
            enqueue(&q, p);
        }
        CHECK(okrr && q.size == 3, "Xoay vong Round-Robin A -> B -> C");
    }

    /* A6. Remove from middle */
    q.size = 0;
    q_mk(0, 1, 9); q_mk(1, 2, 9); q_mk(2, 3, 0); q_mk(3, 4, 9); q_mk(4, 5, 9);
    for (int i = 0; i < 5; i++) enqueue(&q, &q_pool[i]);
    CHECK_QUIET(dequeue(&q)->pid == 3, "Lay phan tu uu tien cao nhat o giua hang doi");
    CHECK_QUIET(q.proc[q.size] == NULL, "Xoa con tro du thua o cuoi mang");
    { uint32_t e[] = {1, 2, 4, 5}; q_verify_order(&q, e, 4, "Bao toan thu tu sau khi lay phan tu giua"); }

    /* A7. Stress test 10,000 random ops */
    {
        static struct pcb_t S[10000];
        static int out_cnt[10000];
        memset(out_cnt, 0, sizeof(out_cnt));
        int next = 0, outs = 0, bad = 0;
        unsigned seed = 12345;
        q.size = 0;

        while (outs < 10000) {
            seed = seed * 1103515245u + 12345u;
            int do_enq = (next < 10000) && (q.size < MAX_QUEUE_SIZE) &&
                         (q.size == 0 || ((seed >> 16) & 1));
            if (do_enq) {
                memset(&S[next], 0, sizeof(S[next]));
                S[next].pid = next;
#ifdef MLQ_SCHED
                S[next].prio = (seed >> 8) % 5;
#else
                S[next].priority = (seed >> 8) % 5;
#endif
                enqueue(&q, &S[next]);
                next++;
            } else {
                uint32_t minp = 0xFFFFFFFFu;
                for (int k = 0; k < q.size; k++) {
#ifdef MLQ_SCHED
                    uint32_t pk = q.proc[k]->prio;
#else
                    uint32_t pk = q.proc[k]->priority;
#endif
                    if (pk < minp) minp = pk;
                }
                struct pcb_t *p = dequeue(&q);
                if (p == NULL) { bad++; break; }
#ifdef MLQ_SCHED
                if (p->prio != minp) bad++;
#else
                if (p->priority != minp) bad++;
#endif
                out_cnt[p->pid]++;
                outs++;
            }
        }
        for (int k = 0; k < 10000; k++) {
            if (out_cnt[k] != 1) bad++;
        }
        CHECK(bad == 0 && empty(&q) && dequeue(&q) == NULL,
              "Stress test 10,000 thao tac ngau nhien (khong mat, khong trung, dung prio)");
    }
}

/* =========================================================================
 * HELPER FUNCTIONS CHO MLQ SCHEDULER BASIC TEST
 * ========================================================================= */
static struct pcb_t *make_proc(uint32_t pid, uint32_t prio) {
    struct pcb_t *p = (struct pcb_t *)calloc(1, sizeof(struct pcb_t));
    p->pid = pid;
    p->prio = prio;
    p->krnl = &global_krnl;
    return p;
}

static void run_mlq_logic_tests(void) {
    printf("\n[PHAN B] KIEM THU LOGIC LAP LICH MLQ & CO CHE SLOT:\n");

    /* B1. Empty state */
    CHECK(queue_empty() == 1, "Hang doi scheduler ban dau phai rong");
    CHECK(get_proc() == NULL, "get_proc() khi rong phai tra ve NULL");

    /* B2. Priority ordering */
    struct pcb_t *p10 = make_proc(101, 10);
    struct pcb_t *p2  = make_proc(102, 2);
    struct pcb_t *p5  = make_proc(103, 5);

    add_proc(p10);
    add_proc(p2);
    add_proc(p5);
    CHECK(queue_empty() == 0, "queue_empty() == 0 sau khi them 3 tien trinh");

    struct pcb_t *r1 = get_proc();
    CHECK(r1 != NULL && r1->pid == 102, "Lay ra PID 102 (prio=2) dau tien");

    put_proc(r1);
    struct pcb_t *r2 = get_proc();
    CHECK(r2 != NULL && r2->pid == 102, "Put ve van lay lai PID 102 do con slot");
    finish_proc(r2);

    struct pcb_t *r3 = get_proc();
    CHECK(r3 != NULL && r3->pid == 103, "Tiep theo lay ra PID 103 (prio=5)");
    finish_proc(r3);

    struct pcb_t *r4 = get_proc();
    CHECK(r4 != NULL && r4->pid == 101, "Cuoi cung lay ra PID 101 (prio=10)");
    finish_proc(r4);

    CHECK(queue_empty() == 1, "Tat ca hang doi da rong sau khi finish het");
    free(p10); free(p2); free(p5);

    /* B3. Slot exhaustion & reset */
    struct pcb_t *p_low_slot = make_proc(201, MAX_PRIO - 1);
    add_proc(p_low_slot);

    struct pcb_t *s1 = get_proc();
    CHECK(s1 != NULL && s1->pid == 201, "Lan 1 lay duoc PID 201 (tieu hao het 1 slot)");
    put_proc(s1);

    struct pcb_t *s2 = get_proc();
    CHECK(s2 != NULL && s2->pid == 201, "Lan 2 tu dong nap lai slot va lay duoc PID 201");
    finish_proc(s2);
    free(p_low_slot);
}

/* =========================================================================
 * HELPER FUNCTIONS CHO CONCURRENCY STRESS TEST (từ test_sched_lock_2.c)
 * ========================================================================= */
#define STRESS_NCPU     4
#define STRESS_NPROC    40
#define STRESS_SLICE    2
#define STRESS_TIMEOUT  3

static struct pcb_t *st_pcb[STRESS_NPROC + 1];
static int st_need[STRESS_NPROC + 1];
static int st_on_cpu[STRESS_NPROC + 1];
static int st_done_cnt[STRESS_NPROC + 1];
static int st_finished;
static int st_loader_done;
static int st_double_run;
static int st_bad_ptr;
static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;

static int st_is_my_pcb(struct pcb_t *p) {
    for (int i = 1; i <= STRESS_NPROC; i++) {
        if (st_pcb[i] == p) return 1;
    }
    return 0;
}

static void *st_loader_worker(void *arg) {
    (void)arg;
    for (int i = 1; i <= STRESS_NPROC; i++) {
        add_proc(st_pcb[i]);
        if (i % 4 == 0) usleep(20);
    }
    pthread_mutex_lock(&st_lock);
    st_loader_done = 1;
    pthread_mutex_unlock(&st_lock);
    return NULL;
}

static void *st_cpu_worker(void *arg) {
    (void)arg;
    time_t start = time(NULL);
    int stop;

    while (1) {
        if (time(NULL) - start > STRESS_TIMEOUT) break;

        struct pcb_t *p = get_proc();
        if (p == NULL) {
            pthread_mutex_lock(&st_lock);
            stop = (st_loader_done && st_finished >= STRESS_NPROC);
            pthread_mutex_unlock(&st_lock);
            if (stop) break;
            continue;
        }

        if (!st_is_my_pcb(p)) {
            pthread_mutex_lock(&st_lock);
            st_bad_ptr++;
            pthread_mutex_unlock(&st_lock);
            break;
        }

        pthread_mutex_lock(&st_lock);
        if (st_on_cpu[p->pid]) st_double_run++;
        st_on_cpu[p->pid] = 1;
        pthread_mutex_unlock(&st_lock);

        for (int i = 0; i < STRESS_SLICE && p->pc < st_need[p->pid]; i++) {
            p->pc++;
        }

        pthread_mutex_lock(&st_lock);
        st_on_cpu[p->pid] = 0;
        pthread_mutex_unlock(&st_lock);

        if (p->pc >= st_need[p->pid]) {
            finish_proc(p);
            pthread_mutex_lock(&st_lock);
            st_done_cnt[p->pid]++;
            st_finished++;
            pthread_mutex_unlock(&st_lock);
        } else {
            put_proc(p);
        }
    }
    return NULL;
}

static void run_concurrency_stress_tests(int nround) {
    printf("\n[PHAN C] STRESS TEST DA LUONG (%d CPU + 1 Loader x %d proc x %d round):\n",
           STRESS_NCPU, STRESS_NPROC, nround);

    pthread_t ld, c[STRESS_NCPU];
    int stress_err = 0;
    long total_done = 0;
    st_double_run = 0;
    st_bad_ptr = 0;

    srand(2026);
    for (int r = 1; r <= nround; r++) {
        st_finished = 0;
        st_loader_done = 0;
        for (int i = 1; i <= STRESS_NPROC; i++) {
            st_pcb[i] = (struct pcb_t *)calloc(1, sizeof(struct pcb_t));
            st_pcb[i]->pid = i;
            st_pcb[i]->prio = rand() % 4;
            st_pcb[i]->krnl = &global_krnl;
            st_need[i] = 1 + rand() % 10;
            st_on_cpu[i] = 0;
            st_done_cnt[i] = 0;
        }

        pthread_create(&ld, NULL, st_loader_worker, NULL);
        for (int i = 0; i < STRESS_NCPU; i++) {
            pthread_create(&c[i], NULL, st_cpu_worker, NULL);
        }
        pthread_join(ld, NULL);
        for (int i = 0; i < STRESS_NCPU; i++) {
            pthread_join(c[i], NULL);
        }

        int lost = 0, dup = 0;
        for (int i = 1; i <= STRESS_NPROC; i++) {
            if (st_done_cnt[i] == 0) lost++;
            if (st_done_cnt[i] > 1) dup++;
        }
        int left = (global_krnl.running_list != NULL) ? global_krnl.running_list->size : 0;
        total_done += st_finished;

        for (int i = 1; i <= STRESS_NPROC; i++) {
            free(st_pcb[i]);
        }

        if (lost || dup || st_double_run || st_bad_ptr || left != 0 || !queue_empty()) {
            printf("  [FAIL] Loi tai round %d: xong %d/%d, mat %d, trung %d, chay chung %d, con tro la %d, running_list con %d\n",
                   r, st_finished, STRESS_NPROC, lost, dup, st_double_run, st_bad_ptr, left);
            stress_err = 1;
            break;
        }
    }

    char msg[160];
    snprintf(msg, sizeof(msg),
             "Hoan tat %d round (%ld/%ld proc dung 1 lan, 0 race condition, running_list = 0)",
             nround, total_done, (long)nround * STRESS_NPROC);
    CHECK(stress_err == 0, msg);
}

/* =========================================================================
 * MAIN
 * ========================================================================= */
int main(int argc, char *argv[]) {
    int nround = 100; /* Mac dinh 100 round (~0.1s), co the truyen 300 qua argv[1] */
    if (argc > 1) {
        int arg_r = atoi(argv[1]);
        if (arg_r > 0) nround = arg_r;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("============================================================\n");
    printf("   KIEM THU TOAN DIEN SCHEDULER & QUEUE (TV1 CONSOLIDATED)\n");
    printf("============================================================\n");

    init_scheduler();

    run_queue_tests();
    run_mlq_logic_tests();
    run_concurrency_stress_tests(nround);

    printf("\n============================================================\n");
    printf("   KET QUA TONG KET: %d LOI (%s)\n", fails, fails == 0 ? "ALL PASS" : "FAIL");
    printf("============================================================\n");
    return fails != 0;
}