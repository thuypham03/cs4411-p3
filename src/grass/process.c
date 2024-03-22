#pragma GCC diagnostic ignored "-Wunknown-pragmas"

#include <inttypes.h>
#include <stdio.h>
#ifndef NO_UCONTEXT
#include <ucontext.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <earth/earth.h>
#include <earth/intf.h>
#include <egos/malloc.h>
#include <egos/queue.h>
#include <egos/syscall.h>
#include <egos/block.h>
#ifdef EMA
#include <egos/ema.h>
#endif
#include <egos/context.h>
#include "process.h"

/* The Earth layer is a bit ununusal in that we can specify the physical memory
 * and TLB size in software.
 */
#define TLB_SIZE		16			// #entries in TLB
#define PHYS_FRAMES		1024		// #physical frames

#define MAX_PROCS		100			// maximum #processes

/* The process that is currently running.  This variable is external
 * and can be used by other modules.
 */
struct process *proc_current;


/* Run (aka ready) queue.
 */
static struct queue proc_runnable;


/* A frame is a physical page.
 */
struct frame {
	char contents[PAGESIZE];
};
static struct frame *proc_frames;			// array of frames
static struct queue proc_freeframes;		// list of free frames

/* Information about frames.
 */

/* Other global variables.
 */
static unsigned int proc_nprocs;			// #processes
static unsigned int proc_nrunnable;			// #runnable processes
static struct queue proc_free;				// free processes
static struct process *proc_next;			// next process to run after ctx switch
static struct process proc_set[MAX_PROCS];	// set of all processes
static bool proc_shutting_down;			// cleaning up
static unsigned long proc_curfew;			// when to shut down

/* We keep various statistics in this structure.
 */
static struct stats {
	unsigned int npage_in, npage_out;
} stats;


static void proc_cleanup(){
	printf("final clean up\n\r");


	/* Release the free frames list.
	 */
	unsigned int frame;
	while (queue_get_uint(&proc_freeframes, &frame))
		;

	/* Release the free process list.
	 */
	while (queue_get(&proc_free) != 0)
		;

	/* Release the run queue.
	 */
	while (queue_get(&proc_runnable) != 0)
	 	;
	queue_release(&proc_runnable);

	// my_dump(false);		// print info about allocated memory
}

/* Initialize a message queue.
 */
static void mq_init(struct msg_queue *mq){
	mq->waiting = false;
	queue_init(&mq->messages);
}

/* Allocate a process structure.
 */
struct process *proc_alloc(gpid_t owner, char *descr, unsigned int uid){
	static gpid_t pid_gen = 1;				// to generate new process ids
	struct process *p = queue_get(&proc_free);

	if (p == 0) {
		printf("proc_alloc: no more slots\n");
		return 0;
	}
	memset(p, 0, sizeof(*p));
	p->pid = pid_gen++;
	p->uid = uid;
	p->owner = owner;
	snprintf(p->descr, sizeof(p->descr), "K %s", descr);
	p->state = PROC_RUNNABLE;
	proc_nprocs++;
	proc_nrunnable++;

	/* The kernel stack pointer must be aligned to 16 bytes.
	 */
	p->kernel_sp = (address_t) &p->kernelstack[KERNEL_STACK_SIZE] & ~0xF;

	unsigned int i;
	for (i = 0; i < MSG_NTYPES; i++) {
		mq_init(&p->mboxes[i]);
	}

	return p;
}

/* Release a process structure, the final step in the death of a process.
 */
static void proc_release(struct process *proc){
	assert(proc->state == PROC_ZOMBIE);

	/* Release any messages that might still be on the queues.
	 */
	unsigned int i;
	for (i = 0; i < MSG_NTYPES; i++) {
		struct msg_queue *mq = &proc->mboxes[i];
		struct message *msg;

		while ((msg = queue_get(&mq->messages)) != 0) {
			m_free(msg->contents);
			m_free(msg);
		}
		queue_release(&mq->messages);
	}

	/* Release the message buffer.
	 */
	if (proc->msgbuf != 0) {
		m_free(proc->msgbuf);
	}

	/* Invoke the cleanup function if any.
	 */
	if (proc->finish != 0) {
		(*proc->finish)(proc->arg);
	}

	proc_nprocs--;
	proc->state = PROC_FREE;
	queue_add(&proc_free, proc);
}

/* Put the current process on the run queue.
 */
static void proc_to_runqueue(struct process *p){
	assert(p->state == PROC_RUNNABLE);
	queue_add(&proc_runnable, p);
}

/* Find a process by process id.
 */
struct process *proc_find(gpid_t pid){
	struct process *p;

	for (p = proc_set; p < &proc_set[MAX_PROCS]; p++) {
		if (p->state != PROC_FREE && p->pid == pid) {
			return p;
		}
	}
	return 0;
}

/* Current process wants to wait for a message on a particular queue
 * that it owns.
 */
bool proc_recv(enum msg_type mtype, unsigned int max_time, void *contents,
			unsigned int *psize, gpid_t *psrc, unsigned int *puid){
	assert(proc_current->state == PROC_RUNNABLE);
	struct msg_queue *mq = &proc_current->mboxes[mtype];
	assert(!mq->waiting);

	// printf("proc_recv %u: %p\n\r", proc_current->pid, contents);

	/* If there are no messages, wait.
	 */
	if (queue_empty(&mq->messages)) {
		mq->waiting = true;
		proc_current->state = PROC_WAITING;
		proc_nrunnable--;
		if (max_time != 0) {
			proc_current->exptime = sys_gettime() + max_time;
			proc_current->alarm_set = true;
		}
		proc_yield();
		assert(!mq->waiting);
	}
	else {
		/* There shouldn't be a reply yet if this is part of an RPC.
		 */
		assert(mtype != MSG_REPLY);
	}

	/* Get the message, if any.
	 */
	assert(proc_current->state == PROC_RUNNABLE);
	struct message *msg = queue_get(&mq->messages);
	if (msg == 0) {
		return false;
	}

	/* Copy the message to the recipient.
	 */
	if (msg->size < *psize) {
		*psize = msg->size;
	}
	memcpy(contents, msg->contents, *psize);
	if (psrc != 0) {
		*psrc = msg->src;
	}
	if (puid != 0) {
		*puid = msg->uid;
	}
	m_free(msg->contents);
	m_free(msg);
	return true;
}

/* Send a message of the given type to the given process.  This routine
 * may be called from an interrupt handler, and so proc_current is not
 * necessarily the source of the message.
 */
bool proc_send(gpid_t src_pid, unsigned int src_uid, gpid_t dst_pid,
			enum msg_type mtype, const void *contents, unsigned int size){
	/* See who the destination process is.
	 */
	struct process *dst = proc_find(dst_pid);
	if (dst == 0) {
		printf("proc_send %u: unknown destination %u\n\r", src_pid, dst_pid);
		return false;
	}
	if (dst->state == PROC_ZOMBIE) {
		return false;
	}

	struct msg_queue *mq = &dst->mboxes[mtype];

	/* If it's a response, the process should be waiting for one.
	 */
	if (mtype == MSG_REPLY) {
		if (dst->state != PROC_WAITING || !mq->waiting
											|| dst->server != src_pid) {
			printf("%u: dst %u (%u) not waiting for reply (%u %u %u)\n",
									src_pid, dst_pid, dst->pid,
									dst->state, mq->waiting, dst->server);
			return false;
		}
	}

	/* Copy the message.
	 */
	struct message *msg = new_alloc(struct message);
	msg->src = src_pid;
	msg->uid = src_uid;
	msg->contents = m_alloc(size);
	memcpy(msg->contents, contents, size);
	msg->size = size;

	/* Add the message to the message queue.
	 */
	queue_add(&mq->messages, msg);

	/* Wake up the process if it's waiting.
	 */
	if (mq->waiting) {
		assert(dst->state == PROC_WAITING);
		dst->state = PROC_RUNNABLE;
		proc_nrunnable++;
		dst->alarm_set = false;

		/* dst == proc_current is possible if the process is waiting for
		 * input.  In that case it shouldn't be put on the runnable queue
		 * because it will automatically resume from earth.intr.suspend().
		 */
		if (dst != proc_current) {

			proc_to_runqueue(dst);
		}
		mq->waiting = false;
	}

	return true;
}

/* Wake up the given process that is waiting for a message.
 */
static void proc_wakeup(struct process *p){
	assert(p->state == PROC_WAITING);
	p->state = PROC_RUNNABLE;
	proc_nrunnable++;
	p->alarm_set = false;
	if (p != proc_current) {
		proc_to_runqueue(p);
	}
	unsigned int i;
	for (i = 0; i < MSG_NTYPES; i++) {
		p->mboxes[i].waiting = false;
	}
}

/* All processes come here when they die.  The process may still executing on
 * its kernel stack, but we can clean up most everything else, and also notify
 * the owner of the process.
 */
void proc_term(struct process *proc, int status){
	assert(proc_nprocs > 0);

	if (proc->state == PROC_RUNNABLE) {
		proc_nrunnable--;
	}

	/* See if the owner is still around.
	 */
	if (proc->state != PROC_ZOMBIE) {
		proc->state = PROC_ZOMBIE;

		/* Notify owner, if any.
		 */
		struct process *owner = proc_find(proc->owner);
		if (owner != 0) {
			struct msg_event mev;
			memset(&mev, 0, sizeof(mev));
			mev.type = MEV_PROCDIED;
			mev.pid = proc->pid;
			mev.status = status;
			(void) proc_send(proc->pid, proc->uid, owner->pid, MSG_EVENT, &mev, sizeof(mev));
		}

		/* Also, if this is a server that clients are waiting for, fail
		 * their rpcs.
		 */
		struct process *p;
		for (p = proc_set; p < &proc_set[MAX_PROCS]; p++) {
			if (p->state == PROC_WAITING &&
					p->mboxes[MSG_REPLY].waiting && p->server == proc->pid) {
				printf("Process %u waiting for reply from %u\n\r", p->pid, proc->pid);
				proc_wakeup(p);
			}
		}
	}

	/* Release any allocated frames.
	 */
	for (unsigned int i = 0; i < VIRT_PAGES; i++) {
		switch (proc->pages[i].status) {
		case PI_UNINIT:
			break;
		case PI_VALID:
			earth.log.p("proc_term: pid=%u: release frame=%u (page=%u)", proc->pid, proc->pages[i].u.frame, i);
			earth.tlb.unmap(VIRT_BASE / PAGESIZE + i);
			queue_add_uint(&proc_freeframes, proc->pages[i].u.frame);
			break;
		default:
			assert(0);
		}
	}
}

/* What exactly needs to happen to a process to kill it depends on its state.
 */
void proc_zap(struct process *killer, struct process *p, int status){
	/* See if it's allowed.
	 */
	if (killer != 0 && killer->uid != 0 && killer->uid != p->uid) {
		printf("proc_zap: only allowed to kill own processes %u %u\n\r", killer->uid, p->uid);
		return;
	}
	assert(proc_nprocs > 0);

	switch (p->state) {
	case PROC_RUNNABLE:
		proc_term(p, status);
		break;
	case PROC_WAITING:
		proc_term(p, status);
		if (p != proc_current) {
			proc_release(p);
		}
		break;
	case PROC_ZOMBIE:
		break;
	default:
		assert(0);
	}
}

/* Kill process pid, or all interruptable processes if pid == 0.
 */
void proc_kill(gpid_t killer, gpid_t pid, int status){
	struct process *p, *k;

	if ((k = proc_find(killer)) == 0) {
		printf("proc_kill: unknown killer\n\r");
		return;
	}

	/* If pid == 0, kill all interruptable processes.
	 */
	if (pid == 0) {
		for (p = proc_set; p < &proc_set[MAX_PROCS]; p++) {
			if (p->state != PROC_FREE && p->interruptable) {
				proc_zap(k, p, status);
			}
		}
		if (proc_current->state == PROC_ZOMBIE) {
			proc_yield();
		}
		return;
	}

	if ((p = proc_find(pid)) == 0) {
		printf("proc_kill: no process %u\n\r", pid);
	}
	else {
		proc_zap(k, p, status);

		/* If terminated self, don't return but give up the CPU.
		 */
		if (p->state == PROC_ZOMBIE) {
			proc_yield();
		}
	}
}

/* Entry point of new processes.  It is invoked from ctx_start().
 */
void ctx_entry(void){
	/* Invoke the new process.
	 */
	proc_current = proc_next;
	(*proc_current->start)(proc_current->arg);

	printf("process %u terminated!!\n\r", proc_current->pid);
	sys_exit(STAT_ILLMEM);
}

/* Right after a context switch we need to do some administration.
 *	- clean up the previous process if it's a zombie
 *	- update proc_current
 *	- flush the TLB
 */
static void proc_after_switch(){
	assert(proc_next != proc_current);
	assert(proc_next->state == PROC_RUNNABLE);

	/* Clean up the old process if it's a zombie.
	 */
	if (proc_current->state == PROC_ZOMBIE) {
		proc_release(proc_current);
	}

	/* Flush the TLB
	 */
	earth.tlb.flush();

	/* Update the proc_current pointer.
	 */
	proc_current = proc_next;
	earth.log.p("proc_after_switch: pid=%u", proc_current->pid);
}

/* Create a process.  Initially only contains a stack segment.  Boolean
 * 'user' is true is the process should run as a user process with a
 * paged stack and interrupts enabled.
 */
gpid_t proc_create_uid(gpid_t owner, char *descr, void (*start)(void *), void *arg, unsigned int uid){
	/* Don't allow new processes to be created.
	 */
	if (proc_shutting_down) {
		return 0;
	}

	/* See who the owner is.
	 */
	struct process *parent;
	if ((parent = proc_find(owner)) == 0) {
		printf("proc_create_uid: no parent\n");
		return 0;
	}

	/* If not the superuser, new process has same uid as parent.
	 */
	if (parent->uid != 0) {
		uid = parent->uid;
	}

	/* Allocate a process control block.
	 */
	struct process *proc;
	if ((proc = proc_alloc(owner, descr, uid)) == 0) {
		printf("proc_create_uid: out of memory\n");
		return 0;
	}

	proc->start = start;
	proc->arg = arg;

	/* Initialize the signal stack for first use.
	 */
	sigstk_init(proc);

	/* Save the process id for the result value.
	 */
	gpid_t pid = proc->pid;

	/* Put the current process on the run queue.
	 */
	proc_to_runqueue(proc_current);

	/* Start the new process, which commences at ctx_entry();
	 */
	proc_next = proc;

#ifdef NO_UCONTEXT
	ctx_start(&proc_current->kernel_sp, proc->kernel_sp);
#else
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

	getcontext(&proc->ctx);
	proc->ctx.uc_stack.ss_sp = proc->kernelstack;
    proc->ctx.uc_stack.ss_size = KERNEL_STACK_SIZE;
    proc->ctx.uc_link = 0;
	makecontext(&proc->ctx, ctx_entry, 0);
	swapcontext(&proc_current->ctx, &proc->ctx);

#pragma clang diagnostic push
#endif

	proc_after_switch();
	return pid;
}

/* Like proc_create_uid, but with uid = 0.
 */
gpid_t proc_create(gpid_t owner, char *descr, void (*start)(void *), void *arg){
	return proc_create_uid(owner, descr, start, arg, 0);
}

/* Yield to another process.  This is basically the main scheduler.
 */
void proc_yield(void){
	/* See if there's any I/O to be done.
	 */
	earth.intr.suspend(0);

	assert(proc_nprocs > 0);

	/* Try to find a process to run.
	 */
	for (;;) {
		/* First check if there are any processes waiting for a timeout
		 * that are now runnable.  Also keep track of how long until
		 * the next timeout, if any.
		 */
		struct process *p;
		unsigned long now = sys_gettime(), next = now + 1000;
		if (proc_curfew != 0 && now > proc_curfew) {
			printf("cleanup: timed out\n");
			proc_cleanup();
			earth.dev_gate.exit(0);
		}
		for (p = proc_set; p < &proc_set[MAX_PROCS]; p++) {
			if (proc_shutting_down && p->state != PROC_FREE) {
				proc_zap(0, p, STAT_SHUTDOWN);
			}
			else if (p->state == PROC_WAITING && p->alarm_set) {
				if (p->exptime <= now) {
					proc_wakeup(p);
				}
				if (p->exptime < next) {
					next = p->exptime;
				}
			}
		}

		/* See if there are other processes to run.  If so, we're done.
		 */
		while ((proc_next = queue_get(&proc_runnable)) != 0) {
			if (proc_next->state == PROC_RUNNABLE) {
				break;
			}
			assert(proc_next->state == PROC_ZOMBIE);
			proc_release(proc_next);
		}

		/* There should always be at least one process.
		 */
		assert(proc_nprocs > 0);

		/* See if we found a suitable process to schedule.
		 */
		if (proc_next != 0) {
			break;
		}

		/* If I'm runnable, I can simply return now without a context switch,
		 * TLB flush and other nasty overheads.  Note: the process may not be
		 * runnable any more because processes can be killed.
		 */
		if (proc_current->state == PROC_RUNNABLE) {
			return;
		}

		/* If this is the last process remaining when shutting down,
		 * actually clean things up.
		 */
		if (proc_nprocs == 1 && proc_shutting_down) {
			proc_cleanup();
			earth.dev_gate.exit(0);
		}

		/* No luck.  We'll wait for a while.
		 */
		earth.intr.suspend(next - now);

	}


	assert(proc_next != proc_current);
	if (proc_next->pid == proc_current->pid) {		// for debugging
		printf("==> %u\n\r", proc_current->pid);
		proc_dump();
	}
	assert(proc_next->pid != proc_current->pid);
	assert(proc_next->state == PROC_RUNNABLE);

	/* Make sure the current process is schedulable.
	 */
	if (proc_current->state == PROC_RUNNABLE) {
		proc_to_runqueue(proc_current);
	}

	if (proc_shutting_down) {
		printf("proc_yield: switch from %u to %u next\n", proc_current->pid, proc_next->pid);
	}


#ifdef NO_UCONTEXT
	ctx_switch(&proc_current->kernel_sp, proc_next->kernel_sp);
#else
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	swapcontext(&proc_current->ctx, &proc_next->ctx);
#pragma clang diagnostic push
#endif

	proc_after_switch();
}


/* Allocate a (pinned) frame for the given page.
 */
static void proc_frame_alloc(unsigned int page){
	/* First check to see if there's a frame on the free list.
	 */
	unsigned int frame_no;
	bool success = queue_get_uint(&proc_freeframes, &frame_no);
	if (success) {
		earth.log.p("proc_frame_alloc: pid=%u: page=%u assign frame=%x", proc_current->pid, page, frame_no);
		proc_current->pages[page].status = PI_VALID;
		proc_current->pages[page].u.frame = frame_no;
		return;
	}


	earth.log.panic("proc_frame_alloc: out of frames");
}

/* Initialize a newly allocated frame.
 */
static void frame_init(struct frame *frame, unsigned int abs_page){
	struct process *p = proc_current;

    /* See if the page is in the executable.
     */
	struct exec_header *eh = &p->hdr.eh;
    unsigned int offset = eh->eh_offset;
    for (unsigned int i = 0; i < eh->eh_nsegments; i++) {
        struct exec_segment *es = &p->hdr.es[i];
        if (es->es_first <= abs_page && abs_page < es->es_first + es->es_npages) {
            if (abs_page < es->es_first + es->es_nblocks) {
                earth.log.p("start read");
                /* Read the page.
                 */
                unsigned int size = PAGESIZE;
                bool success = file_read(p->executable.server,
                    p->executable.file_no,
                    (offset + (abs_page - es->es_first)) * PAGESIZE,
                    frame, &size);
                if (!success) {
                    printf("--> %u %u\n", p->executable.server, p->executable.file_no);
                }
                assert(success);
                assert(size <= PAGESIZE);
                if (size < PAGESIZE) {
                    memset((char *) frame + size, 0, PAGESIZE - size);
                }
                earth.log.p("finish read");
                return;
            }
            else {
                break;
            }
        }
        offset += es->es_nblocks;
    }

	/* Otherwise zero-init it.
	 */
    earth.log.p("zero init");
    memset(frame, 0, PAGESIZE);
}

/* Got a page fault at the given virtual address.  Map the page.
 */
void proc_pagefault(address_t virt, enum access access){
	static unsigned int tlb_index;			// to allocate TLB entries
	struct process *p = proc_current;

	assert(p->state == PROC_RUNNABLE);

	earth.log.p("proc_pagefault: pid=%u: addr=%p", p->pid, virt);
	// printf("GOT PAGEFAULT: pid=%u addr=%p access=%d\n\r", p->pid, virt, access);

	/* First see if it's a legal address.
	 */
	if (virt < VIRT_BASE || virt >= VIRT_TOP) {
		printf("proc_pagefault pid=%u: address %p out of range\n\r",
								p->pid, virt);
		proc_term(p, STAT_ILLMEM);
		proc_yield();
	}

	/* Figure out which page it is.
	 */
	unsigned int rel_page = (virt - VIRT_BASE) / PAGESIZE;
	unsigned int abs_page = virt / PAGESIZE;

    /* Search for it in the segment table of the executable.
     * If it's not there, then it's hopefully heap or stack.
     */
    struct exec_segment *es = NULL;
    for (unsigned int i = 0; i < p->hdr.eh.eh_nsegments; i++) {
        struct exec_segment *ces = &p->hdr.es[i];
        if (ces->es_first <= abs_page &&
                            abs_page < ces->es_first + ces->es_npages) {
            es = ces;
            break;
        }
    }

	/* See if the entry is already mapped.
	 */
	int index = earth.tlb.get_entry(abs_page);
	earth.log.p("proc_pagefault: fault in page=%x, index=%d", rel_page, index);

    /* If we do not know if the access is a read or a write, we try
     * to figure it out here.  There are a variety of cases depending
     * on whether the entry is already mapped and, if so, how it is
     * mapped, and what the restrictions on the segment are.  One trick
     * we use is to first map a segment as read-only.  If we get a page
     * fault, then we map the page as writable and mark the page as dirty.
     */
    if (access == ACCESS_UNKNOWN) {
        /* First check if the entry is already mapped.
         */
        if (index >= 0) {
            /* If the segment is not writable and the entry is already mapped,
             * then it must be some illegal access like a write or an exec.
             */
            if (es != NULL && !(es->es_prot & P_WRITE)) {
                printf("proc_pagefault pid=%u: illegal access at %p\n\r",
                                        p->pid, virt);
                proc_term(p, STAT_ILLMEM);
                proc_yield();
                assert(false);
            }
            
            /* The segment is writable.  See how the page is currently mapped.
             */
            unsigned int prot;
            earth.tlb.get(index, NULL, NULL, &prot);

            /* If the segment is writable and the current protection bits
             * allow writes, then it's probably an execute access.  We deny
             * the access.
             */
            if (prot & P_WRITE) {
                printf("proc_pagefault pid=%u: trying to execute at %p\n\r",
                                        p->pid, virt);
                proc_term(p, STAT_ILLMEM);
                proc_yield();
                assert(false);
            }

            /* If the segment is writable but the current protection bits do
             * not allow writes, then guess the access is a write.  It
             * may be an execute access---we'll catch that the next
             * page fault.
             */
            access = ACCESS_WRITE;
        }

        /* If the entry is not already mapped, assume it's a read access.
         * If we're wrong and get another page fault, we'll reconsider.
         */
        else {
            access = ACCESS_READ;
        }
    }


	switch (p->pages[rel_page].status) {
	case PI_UNINIT:
		assert(index < 0);
		proc_frame_alloc(rel_page);
		frame_init(&proc_frames[p->pages[rel_page].u.frame], abs_page);
		break;
	case PI_VALID:
		break;
	default:
		assert(0);
	}

	/* Sanity checks.
	 */
	assert(p->pages[rel_page].status == PI_VALID);

	/* See if we need to allocate a new index.
	 */
	if (index < 0) {
		index = tlb_index;
		tlb_index = (tlb_index + 1) % TLB_SIZE;
	}

	/* Map the page to the frame.
	 */
	struct frame *frame = &proc_frames[p->pages[rel_page].u.frame];
	if (access == ACCESS_READ) {
        if (es != NULL && (es->es_prot & P_EXEC)) {
// printf("map executable\n");
            earth.tlb.map(index, abs_page, frame, P_READ | P_EXEC);
        }
        else {
// printf("map readable\n");
            earth.tlb.map(index, abs_page, frame, P_READ);
        }
    }
    else {
// printf("map writable\n");
		earth.tlb.map(index, abs_page, frame, P_READ | P_WRITE);
	}
}

/* Entry point for all interrupts.
 */
void proc_got_interrupt(){
    earth.log.p("got_interrupt: pid=%u type=%u", proc_current->pid, proc_current->intr_type);
	switch (proc_current->intr_type) {
	case INTR_PAGE_FAULT:
		proc_pagefault((address_t) proc_current->intr_arg, ACCESS_UNKNOWN);
		break;
	case INTR_SYSCALL:
		proc_syscall();
		break;
	case INTR_CLOCK:
		proc_yield();
		break;
	case INTR_IO:
		/* There might be a process that is now runnable.
		 */
		proc_yield();
		break;
	default:
		assert(0);
	}
}

/* Dump the state of all processes.
 */
void proc_dump(void){
	struct process *p;

	printf("%u processes (current = %u, nrunnable = %u",
		proc_nprocs, proc_current->pid, proc_nrunnable);
	printf("):\n\r");

	printf("PID   DESCRIPTION  UID STATUS      RES SWP OWNER ALARM   EXEC\n\r");
	for (p = proc_set; p < &proc_set[MAX_PROCS]; p++) {
		if (p->state == PROC_FREE) {
			continue;
		}
		printf("%4u: %-12.12s %3u ", p->pid, p->descr, p->uid);
		switch (p->state) {
		case PROC_RUNNABLE:
			if (p == proc_current) {
				printf("%.12p", p->intr_ip);
			}
			else {
				printf("RUNNABLE   ");
			}
			break;
		case PROC_WAITING:
			if (p->mboxes[MSG_REQUEST].waiting) {
				printf("AWAIT REQST");
			}
			if (p->mboxes[MSG_REPLY].waiting) {
				printf("AWAIT %5u", p->server);
			}
			if (p->mboxes[MSG_EVENT].waiting) {
				printf("AWAIT EVENT");
			}
			break;
		case PROC_ZOMBIE:
			printf("ZOMBIE     ");
			break;
		default:
			assert(0);
		}

		unsigned in_mem = 0, on_disk = 0;
		for (unsigned i = 0; i < VIRT_PAGES; i++) {
			switch (p->pages[i].status) {
				case PI_UNINIT:	break;
				case PI_VALID:	in_mem++; break;
				default: assert(0);
			}
		}
		printf(" %3u %3u", in_mem, on_disk);

		printf(" %5u ", p->owner);
		if (p->state == PROC_WAITING && p->alarm_set) {
			printf("%5ld", p->exptime - sys_gettime());
		}
		else {
			printf("     ");
		}
		if (p->executable.server != 0) {
			printf("   %u:%u", p->executable.server, p->executable.file_no);
		}
		printf("\n\r");
	}
}

/* Initialize this module.
 */
void proc_initialize(void){
	/* Sanity check sme constants.
	 */
	assert(PAGESIZE >= BLOCK_SIZE);
	assert(PAGESIZE % BLOCK_SIZE == 0);


	unsigned int i;

	/* Initialize the TLB.
	 */
	earth.tlb.initialize(TLB_SIZE);

	/* Allocate the physical memory.
	 */
	// proc_frames = earth.mem.initialize(PHYS_FRAMES, P_READ | P_WRITE);
	proc_frames = m_alloc(PHYS_FRAMES * PAGESIZE);

	/* Create a free list of physical frames.
	 */
	queue_init(&proc_freeframes);
	for (i = 0; i < PHYS_FRAMES; i++) {
		queue_add_uint(&proc_freeframes, i);
	}


	/* Initialize the free list of processes.
	 */
	queue_init(&proc_free);
	for (i = 0; i < MAX_PROCS; i++) {
		proc_set[i].pid = i;
		queue_add(&proc_free, &proc_set[i]);
	}

	/* Initialize the run queue (aka ready queue).
	 */
	queue_init(&proc_runnable);

	/* Allocate a process record for the current process.
	 */
	proc_current = proc_alloc(1, "main", 0);
}

/* Invoked when shutting down the kernel.
 */
void proc_shutdown(){
	proc_shutting_down = true;
	proc_curfew = earth.clock.now() + 3000;
}
