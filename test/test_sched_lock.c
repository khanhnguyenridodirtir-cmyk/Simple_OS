#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include "queue.h"
#include "sched.h"

#define NCPU	4
#define NPROC	40
#define NROUND	300	
#define SLICE	2
#define TIMEOUT	3	

static struct krnl_t os;	
static struct pcb_t * pcb[NPROC + 1];
static int need[NPROC + 1];	
static int on_cpu[NPROC + 1];	
static int done_cnt[NPROC + 1];	
static int finished;
static int loader_done;
static int double_run;		
static int bad_ptr;		

static pthread_mutex_t cnt_lock = PTHREAD_MUTEX_INITIALIZER;

static int is_my_pcb(struct pcb_t * p) {
	int i;
	for (i = 1; i <= NPROC; i++)
		if (pcb[i] == p)
			return 1;
	return 0;
}

static void * loader(void * arg) {
	int i;
	for (i = 1; i <= NPROC; i++) {
		add_proc(pcb[i]);
		if (i % 4 == 0)
			usleep(20);
	}
	pthread_mutex_lock(&cnt_lock);
	loader_done = 1;
	pthread_mutex_unlock(&cnt_lock);
	return NULL;
}

static void * cpu(void * arg) {
	time_t start = time(NULL);
	int i, stop;
	struct pcb_t * p;

	while (1) {
		if (time(NULL) - start > TIMEOUT)
			break;	
		p = get_proc();
		if (p == NULL) {
			pthread_mutex_lock(&cnt_lock);
			stop = (loader_done && finished >= NPROC);
			pthread_mutex_unlock(&cnt_lock);
			if (stop)
				break;
			continue;
		}
		if (!is_my_pcb(p)) {	
			pthread_mutex_lock(&cnt_lock);
			bad_ptr++;
			pthread_mutex_unlock(&cnt_lock);
			break;
		}

		pthread_mutex_lock(&cnt_lock);
		if (on_cpu[p->pid])
			double_run++;
		on_cpu[p->pid] = 1;
		pthread_mutex_unlock(&cnt_lock);

		for (i = 0; i < SLICE && p->pc < need[p->pid]; i++)
			p->pc++;

		pthread_mutex_lock(&cnt_lock);
		on_cpu[p->pid] = 0;
		pthread_mutex_unlock(&cnt_lock);

		if (p->pc >= need[p->pid]) {
			finish_proc(p);
			pthread_mutex_lock(&cnt_lock);
			done_cnt[p->pid]++;
			finished++;
			pthread_mutex_unlock(&cnt_lock);
		} else {
			put_proc(p);
		}
	}
	return NULL;
}

int main(int argc, char * argv[]) {
	pthread_t ld, c[NCPU];
	int r, i, lost, dup, left, err = 0;
	int nround = NROUND;
	long total = 0;

	if (argc > 1)
		nround = atoi(argv[1]);

	setvbuf(stdout, NULL, _IONBF, 0);	
	init_scheduler();
	srand(2026);
	printf("[H04] test_sched_lock: %d CPU + 1 loader, %d proc/round, %d round\n",
		NCPU, NPROC, nround);

	for (r = 1; r <= nround; r++) {
		finished = 0;
		loader_done = 0;
		for (i = 1; i <= NPROC; i++) {
			pcb[i] = (struct pcb_t *)calloc(1, sizeof(struct pcb_t));
			pcb[i]->pid = i;
			pcb[i]->prio = rand() % 4;	
			pcb[i]->krnl = &os;
			need[i] = 1 + rand() % 10;
			on_cpu[i] = 0;
			done_cnt[i] = 0;
		}

		pthread_create(&ld, NULL, loader, NULL);
		for (i = 0; i < NCPU; i++)
			pthread_create(&c[i], NULL, cpu, NULL);
		pthread_join(ld, NULL);
		for (i = 0; i < NCPU; i++)
			pthread_join(c[i], NULL);

		lost = 0;
		dup = 0;
		for (i = 1; i <= NPROC; i++) {
			if (done_cnt[i] == 0) lost++;
			if (done_cnt[i] > 1) dup++;
		}
		left = os.running_list->size;	
		total += finished;

		if (lost || dup || double_run || bad_ptr || left != 0 || !queue_empty()) {
			printf("[H04] round %d LOI: xong %d/%d, mat %d, xong 2 lan %d, "
				"2 CPU chay chung %d, con tro la %d, running_list con %d, "
				"ready queue %s\n", r, finished, NPROC, lost, dup,
				double_run, bad_ptr, left,
				queue_empty() ? "rong" : "CON PROC");
			err = 1;
			break;
		}
		for (i = 1; i <= NPROC; i++)
			free(pcb[i]);
	}

	if (!err)
		printf("[H04] %d round OK: %ld/%ld proc xong dung 1 lan, "
			"2 CPU chay chung = 0, running_list cuoi = 0\n",
			nround, total, (long)nround * NPROC);
	printf("[H04] KET QUA: %s\n", err ? "FAIL" : "PASS");
	return err;
}
