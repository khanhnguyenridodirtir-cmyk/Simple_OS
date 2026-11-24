#if defined(_PTHREAD_H) && !defined(__SCHED_H_FORWARDED) // tranh viec khai bao pthread vong lam ko hien / ko dung dc
#define __SCHED_H_FORWARDED
#include_next <sched.h>
#else
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MLQ_SCHED
#define MLQ_SCHED
#endif

#define MAX_PRIO 140

int queue_empty(void);

void init_scheduler(void);
void finish_scheduler(void);

/* Get the next process from ready queue */
struct pcb_t * get_proc(void);

/* Put a process back to run queue */
void put_proc(struct pcb_t * proc);

/* Add a new process to ready queue */
void add_proc(struct pcb_t * proc);

void finish_proc(struct pcb_t * proc);

//HELPER TIM KIEM
struct queue_t;
typedef int (*sched_running_match_t)(struct pcb_t *, void *);
struct pcb_t *sched_find_running_proc_in(struct queue_t *list,
                                        sched_running_match_t match,
                                        void *arg);
struct pcb_t *sched_find_running_pid_in(struct queue_t *list, uint32_t pid);
struct pcb_t *sched_find_running_proc(sched_running_match_t match, void *arg);
struct pcb_t *sched_find_running_pid(uint32_t pid);



#ifdef __cplusplus
}
#endif

#endif
