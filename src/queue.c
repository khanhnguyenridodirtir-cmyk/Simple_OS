#include <stdio.h>
#include <stdlib.h>
#include "queue.h"

static uint32_t queue_prio_of(struct pcb_t *proc)
{
#ifdef MLQ_SCHED
        return proc->prio;
#else
        return proc->priority;
#endif
}

int empty(struct queue_t *q)
{
        if (q == NULL)
                return 1;
        return (q->size == 0);
}

void enqueue(struct queue_t *q, struct pcb_t *proc)
{
        /* TODO: put a new process to queue [q] */
        if (q == NULL || proc == NULL)
                return;
 
        if (q->size >= MAX_QUEUE_SIZE)
                return;
 
        q->proc[q->size] = proc;
        q->size++;
}

struct pcb_t *dequeue(struct queue_t *q)
{
        /* TODO: return a pcb whose prioprity is the highest
         * in the queue [q] and remember to remove it from q
         * */
        struct pcb_t *proc;
        int x = 0;
        int i;
 
        if (empty(q)) return NULL;

        for (i = 1; i < q->size; i++) {
                if (queue_prio_of(q->proc[i]) < queue_prio_of(q->proc[x]))
                        x = i;
        }

          proc = q->proc[x];

        for (i = x; i < q->size - 1; i++)  q->proc[i] = q->proc[i + 1];
 
        q->size--;
        q->proc[q->size] = NULL; 
 
        return proc;
}

struct pcb_t *purgequeue(struct queue_t *q, struct pcb_t *proc)
{
        /* TODO: remove a specific item from queue
         * */
        return NULL;
}