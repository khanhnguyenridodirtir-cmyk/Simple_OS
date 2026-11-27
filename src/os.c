/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * System OS module
 */
#include "cpu.h"
#include "timer.h"
#include "sched.h"
#include "loader.h"
#include "mm.h"
#ifdef MM64
#include "mm64.h"
#endif

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "sysbus.h"
#include "dpi_rtl.h"
#include "dma.h"
#include "libmem.h"

static int time_slot;
static int num_cpus;
static int done = 0;
static struct krnl_t os;

/* So CPU con dang chay. co_processor dung no de biet khi nao duoc nghi:
 * neu no giu time slot mai ma khong detach thi timer_routine se cho
 * vinh vien va ca he thong treo luc stop_timer(). */
static volatile int cpu_alive = 0;

/* RAM ma bus DMA noi vao. main() gan TRUOC khi tao thread co_processor
 * nen thread doc duoc an toan (pthread_create la diem dong bo). */
static struct memphy_struct *dma_bus_ram = NULL;

/* ===================================================================
 * Ngat DMA (interrupt) - Section 2.3.1 cua de:
 *   "When the I/O controller completes the operation ... it generates an
 *    interrupt. The CPU temporarily suspends the current task, executes
 *    the interrupt handler to process the I/O result."
 *
 * Thiet bi (thread co_processor) bao xong -> dma_irq_raise() bat co ngat.
 * Moi CPU kiem tra co ngat o RANH GIOI LENH (dau moi vong cpu_routine),
 * giong CPU that kiem tra chan IRQ sau moi lenh. CPU nao gap truoc thi
 * nhan ngat va chay dma_irq_handler(); moi ngat chi mot CPU xu ly.
 *
 * Khong co cho nao CPU dung cho DMA: neu chua co ngat thi chay tiep.
 * Chan IRQ cua mach (irq = ch_done & intr_en) khong dung duoc vi RTL
 * duoc cap gan cung intr_en = 0, nen ta bat ngat khi tick() thay ch_done.
 * =================================================================== */
#define DMA_IRQ_MAX 32
static int             irq_status[DMA_IRQ_MAX];   /* DMA_JOB_DONE / ERROR */
static unsigned        irq_bytes[DMA_IRQ_MAX];
static int             irq_head = 0, irq_count = 0;
static pthread_mutex_t irq_lock = PTHREAD_MUTEX_INITIALIZER;

/* phia thiet bi: bat co ngat, tra ve ngay */
static void dma_irq_raise(int status, unsigned bytes) {
	pthread_mutex_lock(&irq_lock);
	if (irq_count < DMA_IRQ_MAX) {
		int t = (irq_head + irq_count) % DMA_IRQ_MAX;
		irq_status[t] = status;
		irq_bytes[t] = bytes;
		irq_count++;
	}
	pthread_mutex_unlock(&irq_lock);
}

/* Hang cho DMA: process co lenh ke tiep can ket qua DMA nam o day,
 * KHONG o hang doi MLQ, nen khong ton slot CPU nao. Handler danh thuc.
 * Process cho van nam trong running_list; put_proc() luc danh thuc se go
 * no khoi running_list va dua vao dung hang doi MLQ theo prio. */
#define DMA_WAIT_MAX 64
static struct pcb_t   *dma_wait[DMA_WAIT_MAX];
static int             dma_wait_count = 0;
static pthread_mutex_t dma_wait_lock = PTHREAD_MUTEX_INITIALIZER;

/* CPU goi khi libmem_dma_blocked(proc) == 1.
 * Kiem tra LAI trong khoa: neu handler vua chay xong giua lan kiem tra
 * truoc va luc nay thi khong duoc ngu (mat tin hieu danh thuc).
 * tra 1 = da cho ngu, 0 = khong con phai cho (chay tiep duoc) */
static int dma_wait_park(int cpu, struct pcb_t *proc) {
	int parked = 0;
	pthread_mutex_lock(&dma_wait_lock);
	if (libmem_dma_inflight(proc->pid) > 0 && libmem_dma_blocked(proc)
	    && dma_wait_count < DMA_WAIT_MAX) {
		dma_wait[dma_wait_count++] = proc;
		parked = 1;
		printf("\tCPU %d: PID %d cho DMA (%s) -> WAITING, CPU chay process khac\n",
		       cpu, proc->pid,
		       proc->pc >= proc->code->size ? "het lenh, chua duoc tra frame"
		                                    : "lenh ke tiep dung vao region DMA");
	}
	pthread_mutex_unlock(&dma_wait_lock);
	return parked;
}

/* handler goi sau khi go mot job: process nao het phai cho thi ve READY */
static void dma_wait_wakeup(int cpu) {
	pthread_mutex_lock(&dma_wait_lock);
	for (int i = 0; i < dma_wait_count; ) {
		struct pcb_t *p = dma_wait[i];
		int still = libmem_dma_inflight(p->pid) > 0 && libmem_dma_blocked(p);
		if (still) { i++; continue; }
		put_proc(p);                         /* WAITING -> READY */
		printf("\tCPU %d: PID %d WAITING -> READY\n", cpu, p->pid);
		dma_wait[i] = dma_wait[--dma_wait_count];
	}
	pthread_mutex_unlock(&dma_wait_lock);
}

static int dma_irq_pending(void) {
	pthread_mutex_lock(&irq_lock);
	int n = irq_count;
	pthread_mutex_unlock(&irq_lock);
	return n;
}

static int dma_wait_any(void) {
	pthread_mutex_lock(&dma_wait_lock);
	int n = dma_wait_count;
	pthread_mutex_unlock(&dma_wait_lock);
	return n;
}

/* phia CPU: interrupt handler, xu ly ket qua cua mot lan DMA */
static void dma_irq_handler(int cpu, int status, unsigned bytes) {
	uint32_t pid, src, dst, len;
	if (libmem_dma_complete(&pid, &src, &dst, &len) != 0)
		return;                         /* ngat gia: khong co job nao */
	if (status == DMA_JOB_DONE)
		printf("\tCPU %d: NGAT DMA -> handler: PID %u xong rg%u -> rg%u, "
		       "%u/%u byte, region duoc mo khoa\n",
		       cpu, pid, src, dst, bytes, len);
	else
		printf("\tCPU %d: NGAT DMA -> handler: PID %u rg%u -> rg%u LOI, "
		       "bo job, region duoc mo khoa\n", cpu, pid, src, dst);
	dma_wait_wakeup(cpu);
}

/* goi o ranh gioi lenh: co ngat thi nhan mot ngat va chay handler */
static void dma_irq_check(int cpu) {
	int status = 0, have = 0;
	unsigned bytes = 0;
	pthread_mutex_lock(&irq_lock);
	if (irq_count > 0) {
		status = irq_status[irq_head];
		bytes = irq_bytes[irq_head];
		irq_head = (irq_head + 1) % DMA_IRQ_MAX;
		irq_count--;
		have = 1;
	}
	pthread_mutex_unlock(&irq_lock);
	if (have)
		dma_irq_handler(cpu, status, bytes);
}

#ifdef MM_PAGING
static unsigned long memramsz;
static unsigned long memswpsz[PAGING_MAX_MMSWP];

struct mmpaging_ld_args {
	/* A dispatched argument struct to compact many-fields passing to loader */
	int vmemsz;
	struct memphy_struct *mram;
	struct memphy_struct **mswp;
	struct memphy_struct *active_mswp;
	int active_mswp_id;
	struct timer_id_t  *timer_id;
};
#endif

static struct ld_args{
	char ** path;
	unsigned long * start_time;
#ifdef MLQ_SCHED
	unsigned long * prio;
#endif
} ld_processes;
int num_processes;

struct cpu_args {
	struct timer_id_t * timer_id;
	int id;
};

static void * cpu_routine(void * args) {
	struct timer_id_t * timer_id = ((struct cpu_args*)args)->timer_id;
	int id = ((struct cpu_args*)args)->id;
	/* Check for new process in ready queue */
	int time_left = 0;
	struct pcb_t * proc = NULL;
	while (1) {
		/* Ranh gioi lenh: co ngat DMA thi chay handler truoc */
		dma_irq_check(id);

		/* Check the status of current process */
		if (proc == NULL) {
			/* No process is running, the we load new process from
		 	* ready queue */
			proc = get_proc();
			if (proc == NULL) {
				next_slot(timer_id);
				//continue; /* First load failed. skip dummy load */
			}
		}else if (proc->pc == proc->code->size
			  && libmem_dma_inflight(proc->pid) == 0) {
			// Nghia la con tro pc nay la tro xong toi lenh cuoi
			  /* The porcess has finish it job */
#ifdef SEE_PROGRAM_COUNTER
    			printf("\tCPU %d: Processed %2d has finished\n", id ,proc->pid);
#endif
    			finish_proc(proc);

#ifdef MM_PAGING
    #ifdef MM64
        	
   		  /* B1: Kiem tra va thu hoi cac physical frame RAM/SWAP */
        if (proc->mm == NULL) {
            fprintf(stderr,
                    "ERROR: Process %d has NULL mm_struct\n",
                    proc->pid);
            exit(EXIT_FAILURE);
        }

        if (free_pcb_memph(proc) != 0) {
            fprintf(stderr,
                    "ERROR: Cannot release memory of PID %d\n",
                    proc->pid);
            exit(EXIT_FAILURE);
        }

        /* B2: Giai phong cay page table 5 cap */
        mm_free_page_tables(proc->mm);

        /* B3: Giai phong cac VMA va free region */
        struct vm_area_struct *vma = proc->mm->mmap;

        while (vma != NULL) {
            struct vm_area_struct *next_vma = vma->vm_next;

            struct vm_rg_struct *rg = vma->vm_freerg_list;

            while (rg != NULL) {
                struct vm_rg_struct *next_rg = rg->rg_next;
                free(rg);
                rg = next_rg;
            }

            free(vma);
            vma = next_vma;
        }

        /* B4: Giai phong kernel cache pool (neu co) */
        free(proc->mm->kcpooltbl);

        /* B5: Giai phong mm_struct */
        free(proc->mm);
        proc->mm = NULL;
    #else
			free_pcb_memph(proc);
			if (proc->mm != NULL) {
				free(proc->mm);
				proc->mm = NULL;
			}
    #endif
#endif

			free(proc);
			proc = get_proc();
			time_left = 0;
		}else if (time_left == 0) {
			/* The process has done its job in current time slot */
			printf("\tCPU %d: Put process %2d to run queue\n",
				id, proc->pid);
			put_proc(proc);
			proc = get_proc();
		}
		
		/* Recheck process status after loading new process */
		if (proc == NULL && done && dma_wait_any() == 0
		    && dma_irq_pending() == 0) {
			/* No process to run, exit */
			printf("\tCPU %d stopped\n", id);
			break;
		}else if (proc == NULL) {
			/* There may be new processes to run in
			 * next time slots, just skip current slot */
			next_slot(timer_id);
			continue;
		}else if (time_left == 0) {
			printf("\tCPU %d: Dispatched process %2d\n",
				id, proc->pid);
			time_left = time_slot;
		}
		
		/* Lenh ke tiep can ket qua DMA (hoac process da het lenh nhung
		 * DMA van dang ghi vao frame cua no): chi process nay phai doi.
		 * Tra no ve hang doi, CPU nhuong slot cho process khac. */
		if (libmem_dma_blocked(proc)) {
			if (!dma_wait_park(id, proc))
				continue;     /* DMA vua xong: quay lai dau vong */
			proc = NULL;          /* RUNNING -> WAITING */
			time_left = 0;
			next_slot(timer_id);
			continue;
		}

		/* Run current process */
		int old_pc = proc->pc;
		run(proc);
#ifdef SEE_PROGRAM_COUNTER
		printf("[EXEC] CPU=%d PID=%d PC=%d -> %d\n",
       id, proc->pid, old_pc, proc->pc);
#endif
		time_left--;
		next_slot(timer_id);
	}
	__sync_fetch_and_sub(&cpu_alive, 1);
	detach_event(timer_id);
	pthread_exit(NULL);
}


/*
 * co_processor - thread rieng cho DMA, chay SONG SONG voi CPU
 *
 * Moi time slot no lam dung ba viec: noi bus neu chua noi, cap mot
 * nhum chu ky clock cho job dang chay, roi tra slot lai cho timer.
 * Nho the CPU khong phai dung cho DMA - dung yeu cau cua de:
 * "Implement the asynchronous interraction mechanism between CPU
 *  and co-processor."
 */
static void * co_processor(void *arg) {
    struct timer_id_t * timer_id = (struct timer_id_t *) arg;

    /* Tat ghi VCD: ham write() cua cpu.c trung ten voi POSIX write()
     * ma Verilator dung de ghi file -> segfault luc dong file. */
    rtl_dma_set_trace(0);

    if (rtl_dma_init_simulation() != 0) {
        printf("Cannot initialize RTL simulation\n");
        if (timer_id) detach_event(timer_id);
        return (void*)1;
    }

    /* Noi bus vao RAM cua OS ngay tu dau (main da gan dma_bus_ram). */
    if (dma_bus_ram != NULL && dma_bus_ram->storage != NULL)
        memory_bank_attach(dma_bus_ram->storage, (size_t) dma_bus_ram->maxsz);

    /* Chay toi khi het CPU VA het viec DMA. Process phat lenh DMA roi
     * chay tiep (bat dong bo) nen co the KET THUC truoc khi DMA cua no
     * xong. Neu dung som theo cpu_alive thi job dang chay va cac yeu
     * cau trong hang doi bi bo do giua chung. */
    while (__atomic_load_n(&cpu_alive, __ATOMIC_SEQ_CST) > 0
           || dpi_rtl_dma_state() == DMA_JOB_RUNNING
           || do_dma_pending() > 0) {
        if (dpi_rtl_dma_state() == DMA_JOB_RUNNING) {
            /* Cycle stealing: trong nhum DMA_CLK_PER_SLOT chu ky nay DMA
             * chiem bus RAM, nen giu khoa cua RAM. CPU nao dung MEMPHY luc
             * nay chi doi het nhum (vai micro giay), KHONG doi ca job. */
            int fin;
            if (dma_bus_ram) pthread_mutex_lock(&dma_bus_ram->lock);
            fin = dpi_rtl_dma_tick(DMA_CLK_PER_SLOT);
            if (dma_bus_ram) pthread_mutex_unlock(&dma_bus_ram->lock);
            if (fin) {
                unsigned bytes = dpi_rtl_dma_bytes();

                printf("\tDMA: chuyen xong %u byte sau %llu chu ky clock\n",
                       bytes, (unsigned long long) dpi_rtl_dma_cycles());
                /* Bao CPU bang ngat. Thiet bi khong cho CPU xu ly ngat. */
                dma_irq_raise(dpi_rtl_dma_state(), bytes);
                /* BAT BUOC: state DONE khong tu ve IDLE, phai ha
                 * ch_enable roi dap them nhip thi mach moi nha kenh. */
                dpi_rtl_dma_reap();
            }
        }

        /* Kenh ranh ma con yeu cau xep hang thi khoi dong cai ke tiep.
         * Lam ngay trong slot nay nen job sau khong phai doi them. */
        if (dpi_rtl_dma_state() == DMA_JOB_IDLE) {
            int pid = do_dma_kick();
            if (pid >= 0)
                printf("\tDMA: lay yeu cau cua PID %d khoi hang doi "
                       "(con %d cho)\n", pid, do_dma_pending());
        }

        if (timer_id) next_slot(timer_id);
        else break;
    }

    if (timer_id) detach_event(timer_id);

    /* Dong tracer de rtl_trace.vcd thuc su co du lieu.
     * Ban goc khong goi ham nay o dau ca nen file luon 0 byte. */
    rtl_dma_cleanup_simulation();
    return NULL;
}

static void * ld_routine(void * args) {
#ifdef MM_PAGING
	struct memphy_struct* mram = ((struct mmpaging_ld_args *)args)->mram;
	struct memphy_struct** mswp = ((struct mmpaging_ld_args *)args)->mswp;
	struct memphy_struct* active_mswp = ((struct mmpaging_ld_args *)args)->active_mswp;
	struct timer_id_t * timer_id = ((struct mmpaging_ld_args *)args)->timer_id;
#else
	struct timer_id_t * timer_id = (struct timer_id_t*)args;
#endif
	int i = 0;
  /* TODO init kernel page table directory */
#ifdef MM64
	os.krnl_pgd = NULL;
	os.krnl_p4d = NULL;
	os.krnl_pud = NULL;
	os.krnl_pmd = NULL;
	os.krnl_pt = NULL;
#else
	os.krnl_pgd = (uint32_t*)malloc(PAGING_MAX_PGN * sizeof(uint32_t));
#endif
	i=0;
	printf("ld_routine\n");
	os.mram = mram;
	os.mswp = mswp;
	os.active_mswp = active_mswp;
	os.mm = (struct mm_struct*)malloc(sizeof(struct mm_struct));
	// Kiem tra cho chac
	if (os.mm == NULL) {
	#ifdef MAINDEBUGCONFIG
    	fprintf(stderr, "ERROR: Cannot allocate kernel mm_struct\n");
	#endif
		exit(EXIT_FAILURE);
	}
	// Khoi tao mm cho os va kiem tra luon
	if (init_mm(os.mm, NULL) != 0) {
	#ifdef MAINDEBUGCONFIG
		fprintf(stderr, "ERROR: Cannot initialize kernel mm_struct\n");
	#endif
		free(os.mm);
		os.mm = NULL;
		exit(EXIT_FAILURE);
	}
	while (i < num_processes) {
		struct pcb_t * proc = load(ld_processes.path[i]);
		// Tuong tu tren
	if (proc == NULL) {
	#ifdef MAINDEBUGCONFIG
		fprintf(stderr, "ERROR: Cannot load process from %s\n",
			ld_processes.path[i]);
	#endif
			exit(EXIT_FAILURE);
	}
		proc->krnl = &os;

#ifdef MLQ_SCHED
	proc->prio = ld_processes.prio[i];
#endif
	while (current_time() < ld_processes.start_time[i]) {
		next_slot(timer_id);
	}
#ifdef MM_PAGING
	proc->mm = (struct mm_struct*)malloc(sizeof(struct mm_struct));
	// Tuong tu tren
	if (proc->mm == NULL) {

	#ifdef MAINDEBUGCONFIG
    	fprintf(stderr,
			"ERROR: Cannot allocate mm_struct for PID %d\n",
			proc->pid);
	#endif
		exit(EXIT_FAILURE);
	}
	// Tuong tu khi init cua kernel memmory thi kiem tra luon trang thai tra ve
	if (init_mm(proc->mm, proc) != 0) {

	#ifdef MAINDEBUGCONFIG
		fprintf(stderr, "ERROR: Cannot initialize mm_struct for PID %d\n",
		proc->pid);
	#endif
		free(proc->mm);
		proc->mm = NULL;
		exit(EXIT_FAILURE);
	}
#endif

		printf("\tLoaded a process at %s, PID: %d PRIO: %ld\n",
		ld_processes.path[i], proc->pid, ld_processes.prio[i]);
		add_proc(proc);
		free(ld_processes.path[i]);
		i++;
		next_slot(timer_id);
	}
	free(ld_processes.path);
	free(ld_processes.start_time);
#ifdef MLQ_SCHED
	free(ld_processes.prio);
	ld_processes.prio=NULL;
#endif
	done = 1;
	
	detach_event(timer_id);
	pthread_exit(NULL);
}

static void read_config(const char * path) {
	// B1 mo file doc
	FILE * file;
	if ((file = fopen(path, "r")) == NULL) {
		printf("Cannot find configure file at %s\n", path);
		exit(1);
	}
	//B2 Doc time slot , so cpu, so luong
	fscanf(file, "%d %d %d\n", &time_slot, &num_cpus, &num_processes);
	ld_processes.path = (char**)malloc(sizeof(char*) * num_processes);
	ld_processes.start_time = (unsigned long*)
		malloc(sizeof(unsigned long) * num_processes);
#ifdef MM_PAGING
	int sit;
#ifdef MM_FIXED_MEMSZ
	/* We provide here a back compatible with legacy OS simulatiom config file
         * In which, it have no addition config line for Mema, keep only one line
	 * for legacy info 
         *  [time slice] [N = Number of CPU] [M = Number of Processes to be run]
         */
        memramsz  =  0x100000000;
        memswpsz[0] = 0x1000000;
	for(sit = 1; sit < PAGING_MAX_MMSWP; sit++)
		memswpsz[sit] = 0;
#else
	/* Read input config of memory size: MEMRAM and upto 4 MEMSWP (mem swap)
	 * Format: (size=0 result non-used memswap, must have RAM and at least 1 SWAP)
	 *        MEM_RAM_SZ MEM_SWP0_SZ MEM_SWP1_SZ MEM_SWP2_SZ MEM_SWP3_SZ
	*/
	// B3 : Doc memRam , memSwap, 
	fscanf(file, FORMAT_ARG "\n", &memramsz);
	for(sit = 0; sit < PAGING_MAX_MMSWP; sit++)
		fscanf(file, FORMAT_ARG, &(memswpsz[sit])); 

       fscanf(file, "\n"); /* Final character */
#endif
#endif

#ifdef MLQ_SCHED
	ld_processes.prio = (unsigned long*)
		malloc(sizeof(unsigned long) * num_processes);
#endif
	//B4 : Doc va luu tru Process
	int i;
	for (i = 0; i < num_processes; i++) {
		ld_processes.path[i] = (char*)malloc(sizeof(char) * 100);
		ld_processes.path[i][0] = '\0';
		strcat(ld_processes.path[i], "input/proc/");
		char proc[100]={0};
#ifdef MLQ_SCHED
		fscanf(file, "%lu %s %lu\n", &ld_processes.start_time[i], proc, &ld_processes.prio[i]);
#else
		fscanf(file, "%lu %s\n", &ld_processes.start_time[i], proc);
#endif
		strcat(ld_processes.path[i], proc);
	}
	// Doc xong phai dong 
	fclose(file);
}

int main(int argc, char * argv[]) {
	/* Read config */
	if (argc != 2) {
		printf("Usage: os [path to configure file]\n");
		return 1;
	}
	char path[100];
	path[0] = '\0';
	strcat(path, "input/");
	strcat(path, argv[1]);
	read_config(path);

	pthread_t * cpu = (pthread_t*)malloc(num_cpus * sizeof(pthread_t));
	pthread_t * dma = (pthread_t*)malloc(sizeof(pthread_t));
	struct cpu_args * args =
		(struct cpu_args*)malloc(sizeof(struct cpu_args) * num_cpus);
	pthread_t ld;
	
	/* Init timer */
	int i;
	for (i = 0; i < num_cpus; i++) {
		args[i].timer_id = attach_event();
		args[i].id = i;
	}
	struct timer_id_t * ld_event = attach_event();

	/* PHAI dang ky truoc start_timer(): attach_event() tra NULL khi
	 * timer da chay (timer.c:96). Ban goc tao thread co_processor o
	 * duoi cung main, luc do dang ky da muon. */
	struct timer_id_t * dma_event = attach_event();

	start_timer();

#ifdef MM_PAGING
	/* Init all MEMPHY include 1 MEMRAM and n of MEMSWP */
	int rdmflag = 1; /* By default memphy is RANDOM ACCESS MEMORY */

	struct memphy_struct mram;
	struct memphy_struct mswp[PAGING_MAX_MMSWP];

	/* Create MEM RAM */
	if (init_memphy(&mram, memramsz, rdmflag) != 0) {
		#ifdef MAINDEBUGCONFIG
			fprintf(stderr, "ERROR: Cannot initialize MEMRAM\n");
		#endif
    	exit(EXIT_FAILURE);
	}
	int sit=0;
	for (sit = 0; sit < PAGING_MAX_MMSWP; sit++) {
		// Co may swap ko su dung thi mepswpsz do se =0 nen rang truoc
		if (memswpsz[sit] == 0) {
			#ifdef MAINDEBUGCONFIG
				printf("MEMSWP %d: Not used, skipping\n", sit);
			#endif
        	memset(&mswp[sit], 0, sizeof(struct memphy_struct));
        	continue;
    	}
		if (init_memphy(&mswp[sit], memswpsz[sit], rdmflag) != 0) {
			#ifdef MAINDEBUGCONFIG
				fprintf(stderr, "ERROR: Cannot initialize MEMSWP %d\n", sit);
			#endif
			exit(EXIT_FAILURE);
		}
	}
	/* In Paging mode, it needs passing the system mem to each PCB through loader*/
	struct mmpaging_ld_args *mm_ld_args = (struct mmpaging_ld_args*)malloc(sizeof(struct mmpaging_ld_args));
	if (mm_ld_args == NULL) {
        fprintf(stderr, "ERROR: Cannot allocate mmpaging_ld_args\n");
        exit(EXIT_FAILURE);
    }
	mm_ld_args->timer_id = ld_event;
	mm_ld_args->mram = (struct memphy_struct *) &mram;
	mm_ld_args->mswp = (struct memphy_struct**) &mswp;
	
	int active_swap_id = -1;

	for (int sit = 0; sit < PAGING_MAX_MMSWP; sit++) {
		if (memswpsz[sit] > 0) {
			active_swap_id = sit;
			break;
		}
	}

	if (active_swap_id == -1) {
		#ifdef MAINDEBUGCINFIG
			fprintf(stderr, "ERROR: No valid SWAP device\n");
		#endif
		exit(EXIT_FAILURE);
	}

	mm_ld_args->active_mswp = &mswp[active_swap_id];
	mm_ld_args->active_mswp_id = active_swap_id;


#endif

	/* Init scheduler */
	init_scheduler();

	/* Run CPU and loader */
#ifdef MM_PAGING
	pthread_create(&ld, NULL, ld_routine, (void*)mm_ld_args);
#else
	pthread_create(&ld, NULL, ld_routine, (void*)ld_event);
#endif
	cpu_alive = num_cpus;
	for (i = 0; i < num_cpus; i++) {
		pthread_create(&cpu[i], NULL,
			cpu_routine, (void*)&args[i]);
	}

	/* Run co-processor - co time slot rieng, chay song song voi CPU */
#ifdef MM_PAGING
	dma_bus_ram = &mram;
#endif
	pthread_create(dma, NULL, co_processor, (void*)dma_event);

	/* Wait for CPU and loader finishing */
	for (i = 0; i < num_cpus; i++) {
		pthread_join(cpu[i], NULL);
	}
	pthread_join(ld, NULL); 
	pthread_join(*dma, NULL);
	
	
	/* Stop timer */
	stop_timer();

	#ifdef MM_PAGING
	/* Giai phong kernel mm_struct */
	if (os.mm != NULL) {

		/* Giai phong cay page table 5 cap */
		mm_free_page_tables(os.mm);

		/*  Giai phong cac VMA va free region */
		struct vm_area_struct *vma = os.mm->mmap;

		while (vma != NULL) {
			struct vm_area_struct *next_vma = vma->vm_next;

			struct vm_rg_struct *rg = vma->vm_freerg_list;

			while (rg != NULL) {
				struct vm_rg_struct *next_rg = rg->rg_next;
				free(rg);
				rg = next_rg;
			}

			free(vma);
			vma = next_vma;
		}

		/*  Giai phong kernel cache pool neu co */
		free(os.mm->kcpooltbl);

		/* B4: Giai phong mm_struct */
		free(os.mm);
		os.mm = NULL;
	}

	/* Giai phong tat ca MEM SWAP */
	for (int sit = 0; sit < PAGING_MAX_MMSWP; sit++) {
    if (memswpsz[sit] > 0) {
        MEMPHY_destroy(&mswp[sit]);
    }
}

	/* Giai phong MEM RAM */
	MEMPHY_destroy(&mram);

	/* Giai phong tham so truyen cho loader */
	free(mm_ld_args);
#endif

	/* Giai phong cac mang thread va argument */
	free(cpu);
	free(dma);
	free(args);

	return 0;

}



