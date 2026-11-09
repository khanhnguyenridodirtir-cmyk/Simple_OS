#include <stdio.h>
#include <string.h>
#include "queue.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

static struct pcb_t P[60];

static void mk(int i, uint32_t pid, uint32_t pr) {
	memset(&P[i], 0, sizeof(P[i]));
	P[i].pid = pid;
#ifdef MLQ_SCHED
	P[i].prio = pr;
#else
	P[i].priority = pr;
#endif
}

static void order(struct queue_t *q, const uint32_t *exp, int n, const char *name) {
	int k;
	for (k = 0; k < n; k++) {
		struct pcb_t *p = dequeue(q);
		if (p == NULL || p->pid != exp[k]) {
			printf("  FAIL %s at %d: got %d expect %u\n", name, k, p ? (int)p->pid : -1, exp[k]);
			fails++;
			return;
		}
	}
	CHECK(empty(q), name);
	printf("  ok: %s\n", name);
}

int main(void) {
	struct queue_t q;
	int i;

	/* 1. empty */
	q.size = 0;
	CHECK(dequeue(&q) == NULL, "dequeue on empty -> NULL");
	CHECK(dequeue(NULL) == NULL, "dequeue(NULL) -> NULL");
	enqueue(NULL, &P[0]);
	enqueue(&q, NULL);
	CHECK(q.size == 0, "NULL args ignored");
	printf("  ok: empty/NULL cases\n");

	/* 2. same priority -> FIFO (MLQ case) */
	q.size = 0;
	mk(0, 1, 5); mk(1, 2, 5); mk(2, 3, 5);
	enqueue(&q, &P[0]); enqueue(&q, &P[1]); enqueue(&q, &P[2]);
	{ uint32_t e[] = {1, 2, 3}; order(&q, e, 3, "same prio -> FIFO"); }

	/* 3. mixed priorities, smaller value first, ties FIFO */
	q.size = 0;
	mk(0, 1, 10); mk(1, 2, 3); mk(2, 3, 7); mk(3, 4, 3);
	for (i = 0; i < 4; i++) enqueue(&q, &P[i]);
	{ uint32_t e[] = {2, 4, 3, 1}; order(&q, e, 4, "mixed prio, tie keeps arrival order"); }

	/* 4. overflow: only MAX_QUEUE_SIZE kept, no write past array */
	q.size = 0;
	for (i = 0; i < 55; i++) { mk(i, i + 1, 0); enqueue(&q, &P[i]); }
	CHECK(q.size == MAX_QUEUE_SIZE, "size capped at MAX_QUEUE_SIZE");
	{ uint32_t e[MAX_QUEUE_SIZE]; for (i = 0; i < MAX_QUEUE_SIZE; i++) e[i] = i + 1;
	  order(&q, e, MAX_QUEUE_SIZE, "overflow keeps first 50 in order"); }

	/* 5. round-robin inside one MLQ level: dequeue -> run -> enqueue back */
	q.size = 0;
	mk(0, 1, 1); mk(1, 2, 1); mk(2, 3, 1);
	for (i = 0; i < 3; i++) enqueue(&q, &P[i]);
	{
		uint32_t exp[] = {1, 2, 3, 1, 2, 3, 1, 2, 3};
		int okrr = 1;
		for (i = 0; i < 9; i++) {
			struct pcb_t *p = dequeue(&q);
			if (!p || p->pid != exp[i]) okrr = 0;
			enqueue(&q, p); /* put_proc puts it back to the tail */
		}
		CHECK(okrr && q.size == 3, "round-robin A B C A B C ...");
		if (okrr) printf("  ok: round-robin rotation\n");
	}

	/* 6. remove from middle keeps relative order of the rest */
	q.size = 0;
	mk(0, 1, 9); mk(1, 2, 9); mk(2, 3, 0); mk(3, 4, 9); mk(4, 5, 9);
	for (i = 0; i < 5; i++) enqueue(&q, &P[i]);
	CHECK(dequeue(&q)->pid == 3, "middle highest-prio taken first");
	CHECK(q.proc[q.size] == NULL, "vacated tail slot cleared");
	{ uint32_t e[] = {1, 2, 4, 5}; order(&q, e, 4, "order preserved after middle removal"); }

	/* 7. stress: 10000 random enqueue/dequeue, every process must come out
	 *    exactly once (no loss, no duplicate) and always the highest prio */
	{
		static struct pcb_t S[10000];
		static int out_cnt[10000];
		int next = 0, outs = 0, bad = 0, k;
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
				/* expected = smallest prio, earliest arrival among ties */
				uint32_t minp = 0xFFFFFFFFu;
				for (k = 0; k < q.size; k++) {
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
		for (k = 0; k < 10000; k++)
			if (out_cnt[k] != 1) bad++;
		CHECK(bad == 0 && empty(&q) && dequeue(&q) == NULL,
		      "stress: no loss, no duplicate, always highest prio");
		if (bad == 0) printf("  ok: stress 10000 ops (no loss / no duplicate)\n");
	}

	printf(fails ? "RESULT: %d FAIL\n" : "RESULT: ALL PASS\n", fails);
	return fails != 0;
}
