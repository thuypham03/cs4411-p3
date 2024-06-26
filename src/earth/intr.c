#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <poll.h>
#include <assert.h>
#include <earth/earth.h>
#include <earth/intr.h>
#include <egos/queue.h>

/* State about "device"
 */
struct device {
	struct device *next;			// for linked list
	int fd;							// file descriptor
	void (*read_avail)(void *);		// upcall for reading
	void *arg;						// argument passed to upcalls
};

/* Definition of an abstract event.
 */
struct event {
	void (*handler)(void *arg);
	void *arg;
};

/* Global and private data.
 */
struct intr {
	intr_handler_t handler;			// interrupt vector
	struct device *devs;			// linked list of devices
	unsigned int ndevs;				// # devices
	stack_t sigstk;					// signal stack
	unsigned int sig_depth;			// for nested interrupts
	struct queue events;			// queue of events scheduled
};
static struct intr intr;

/* For debugging only.
 */
#ifdef notdef
static void intr_print(void){
	sigset_t mask_null, mask_cur;
	int i;

	sigemptyset(&mask_null);
	sigprocmask(SIG_BLOCK, &mask_null, &mask_cur);
	printf("curmask: ");
	for (i = 0; i < 64; i++) {
		if (sigismember(&mask_cur, i)) {
			switch (i) {
			case SIGIO:
				printf(" IO");
				break;
			case SIGALRM:
				printf(" ALRM");
				break;
			case SIGVTALRM:
				printf(" VTALRM");
				break;
			default:
				printf(" %d", i);
			}
		}
	}
	printf("\n");
}
#endif // notdef

#ifdef OBSOLETE
/* Copy the interrupt stack to stack_copy.
 */
static void intr_save(void *stack_copy, void *sp){
	if (1 || (char *) sp < (char *) intr.sigstk.ss_sp ||
			(char *) sp >= (char *) intr.sigstk.ss_sp + intr.sigstk.ss_size) {
		fprintf(stderr, "intr_save: bad stack pointer %"PRIaddr" %"PRIaddr" %"PRIaddr"\n", (address_t) intr.sigstk.ss_sp, (address_t) sp, (address_t) intr.sigstk.ss_sp + intr.sigstk.ss_size);
		exit(1);
	}
	int offset = (char *) sp - (char *) intr.sigstk.ss_sp;
	int size = intr.sigstk.ss_size - offset;
	// printf("saving %d %d bytes\n", size, SIGSTKSZ);
	memcpy((char *) stack_copy + offset, (char *) intr.sigstk.ss_sp + offset, size);
}

/* Copy the stack copy back to the interrupt stack.
 */
static void intr_restore(void *p, void *sp){
	if (1 || (char *) sp < (char *) intr.sigstk.ss_sp ||
			(char *) sp >= (char *) intr.sigstk.ss_sp + intr.sigstk.ss_size) {
		fprintf(stderr, "intr_restore: bad stack pointer\n");
		exit(1);
	}
	int offset = (char *) sp - (char *) intr.sigstk.ss_sp;
	int size = intr.sigstk.ss_size - offset;
	// printf("restoring %d bytes\n", size);
	memcpy((char *) intr.sigstk.ss_sp + offset, (char *) p + offset, size);
}
#endif // OBSOLETE

/* Signals come in here, running on the alternate stack.
 */
static void signal_handler(int sig, siginfo_t *si, void *arg){
	/* user context when the interrupt happens
	 * program counter, stack pointer and syscall arguments
     */

	ucontext_t *uc = arg;
#ifdef MACOSX
#ifdef x86_32
	void (**ip)(void) = (void (**)(void)) &uc->uc_mcontext->__ss.__eip;
	void **sp = (void **) &uc->uc_mcontext->__ss.__esp;
	void *syscall_arg = (void *) uc->uc_mcontext->__ss.__eax;
#endif
#ifdef x86_64
	void (**ip)(void) = (void (**)(void)) &uc->uc_mcontext->__ss.__rip;
	void **sp = (void **) &uc->uc_mcontext->__ss.__rsp;
	void *syscall_arg = (void *) uc->uc_mcontext->__ss.__rdi;
#endif
#ifdef arm64
	void (**ip)(void) = (void (**)(void)) &uc->uc_mcontext->__ss.__pc;
	void **sp = (void **) &uc->uc_mcontext->__ss.__sp;
	void *syscall_arg = (void *) uc->uc_mcontext->__ss.__x[0];
#endif
#endif /* MACOSX */
#ifdef LINUX
#ifdef x86_32
	void (**ip)(void) = (void (**)(void)) &uc->uc_mcontext.gregs[REG_EIP /* 14 */];
	void **sp = (void **) &uc->uc_mcontext.gregs[REG_ESP /*  7 */];
	void *syscall_arg = (void *) uc->uc_mcontext.gregs[REG_EAX];
#endif
#ifdef x86_64
	void (**ip)(void) = (void (**)(void)) &uc->uc_mcontext.gregs[REG_RIP];
	void **sp = (void **) &uc->uc_mcontext.gregs[REG_RSP];
	void *syscall_arg = (void *) uc->uc_mcontext.gregs[REG_RDI];
#endif
#endif /* LINUX */

	if (intr.sig_depth != 0) {
		fprintf(stderr, "earth: signal_handler depth=%u sig=%d: addr=%"PRIaddr" ip=%"PRIaddr" sp=%"PRIaddr"\n\r",
				intr.sig_depth, sig, (address_t) si->si_addr,
				(address_t) *ip, (address_t) *sp);
		fprintf(stderr, "signal_handler: too deep\n");
		kill(getpid(), SIGQUIT);		// to help with gdb
		exit(1);
	}

    // WARNING: SIGBUS is usually an indication of bad memory alignment
    //          si->si_addr may not contain the address accessed but
    //          the address of the instruction where it happened
	if ((sig == SIGSEGV || sig == SIGBUS) && ((address_t) si->si_addr < VIRT_BASE || (address_t) si->si_addr >= VIRT_TOP)) {
		fprintf(stderr, "earth: signal_handler depth=%u sig=%d: addr=%"PRIaddr" ip=%"PRIaddr" sp=%"PRIaddr"\n\r",
				intr.sig_depth, sig, (address_t) si->si_addr,
				(address_t) *ip, (address_t) *sp);
	}

	if (0) {
		fprintf(stderr, "earth: INTR depth=%u sig=%d: addr=%"PRIaddr" arg=%"PRIaddr" ip=%"PRIaddr" sp=%"PRIaddr"\n\r",
				intr.sig_depth, sig, (address_t) si->si_addr,
                (address_t) syscall_arg, (address_t) *ip, (address_t) *sp);
	}

	struct intr_context ic;

#ifdef MACOSX
    ic.context = uc->uc_mcontext;
    ic.ctxsize = sizeof(*uc->uc_mcontext);
#endif
#ifdef LINUX
    ic.context = &uc->uc_mcontext;
    ic.ctxsize = sizeof(uc->uc_mcontext);
#endif
    assert(ic.ctxsize <= SIGNAL_CONTEXT_SIZE);
	ic.ip = *ip;
	ic.sp = *sp;

	/* Turn on access to physical memory.
	 */
	// mem_protect(P_READ | P_WRITE);

	intr.sig_depth++;

	switch (sig) {
	case SIGBUS:
	case SIGSEGV:
		ic.type = INTR_PAGE_FAULT;
		if (si->si_addr == (void *) 1) {		// address used for init --- clear regs
#ifdef MACOSX
			memset(&uc->uc_mcontext->__ss, 0, sizeof(uc->uc_mcontext->__ss));
#endif
#ifdef LINUX
			// memset(&uc->uc_mcontext.gregs, 0, sizeof(uc->uc_mcontext.gregs));
#endif
		}
		(*intr.handler)(&ic, (void *) si->si_addr);
		break;
	case SIGALRM:
	case SIGVTALRM:
		ic.type = INTR_CLOCK;
		(*intr.handler)(&ic, 0);
		break;
	case SIGIO:
		ic.type = INTR_IO;
		(*intr.handler)(&ic, 0);
		break;
	case SIGILL:
		ic.type = INTR_SYSCALL;
		ic.ip += 4;		// skip over illegal instruction
        *ip = ic.ip;
        // printf("SYSCALL %p\n\r", syscall_arg);
		(*intr.handler)(&ic, syscall_arg);
		break;
	default:
		fprintf(stderr, "signal_handler: unknown signal\n");
		exit(1);
	}

	if (--intr.sig_depth != 0) {
		fprintf(stderr, "signal_handler: too shallow?\n");
		exit(1);
	}
	// mem_protect(0);

	/* Update ip (pc) and sp.
	 * This is the difference between "interrupt" and "trap".
	 * "interrupt" should return to the exact program counter
	 * "trap" should return to the program counter of next instruction
	 */
    // printf("RTI BEFORE %d %p %p\n\r", sig, *ip, *sp);
	*ip = ic.ip;
	*sp = ic.sp;

    // printf("RTI AFTER %d %p %p\n\r", sig, *ip, *sp);
}

/* Set up everything.
 */
static void intr_initialize(intr_handler_t handler){
	static char *sigstk;
	if (sigstk == NULL) sigstk = malloc(3*SIGSTKSZ);
	struct sigaction sa;
	sigset_t mask_disable;			// to disable interrupts

	intr.handler = handler;
	queue_init(&intr.events);

	/* Initialize interrupt masks.  Currently, the only interrupt source
	 * to disable is timer and I/O interrupts.  Other sources such as
	 * page faults and system calls continue to go through.
	 */
	sigemptyset(&mask_disable);
	sigaddset(&mask_disable, SIGALRM);
	sigaddset(&mask_disable, SIGVTALRM);
	sigaddset(&mask_disable, SIGIO);

	/* Set up the interrupt stack.
	 */
	intr.sigstk.ss_sp = sigstk;
	intr.sigstk.ss_size = 3*SIGSTKSZ;
	intr.sigstk.ss_flags = 0;
	if (sigaltstack(&intr.sigstk, 0) != 0) {
		perror("sigaltstack");
		exit(1);
	}

	/* Set up the page fault handler.  Should run on the interrupt stack
	 * as user stack may not even be allocated yet.
	 */
	sa.sa_sigaction = signal_handler;
	sa.sa_flags = SA_ONSTACK | SA_RESTART | SA_SIGINFO | SA_NODEFER;
	sa.sa_mask = mask_disable;
	if (sigaction(SIGBUS, &sa, 0) != 0) {
		perror("sigaction SIGBUS");
		exit(1);
	}
	if (sigaction(SIGSEGV, &sa, 0) != 0) {
		perror("sigaction SIGBUS");
		exit(1);
	}

	/* Set up the system call handler.  We set the NODEFER flag so that
	 * system calls remain enabled.  While there are typically no nested
	 * system call (system calls invoked from within system calls), the
	 * way we switch from stack to stack it may be that we switch to
	 * a "pagefault" interrupt stack and should have system calls enabled.
	 */
	sa.sa_sigaction = signal_handler;
	sa.sa_mask = mask_disable;
	sa.sa_flags = SA_ONSTACK | SA_RESTART | SA_SIGINFO | SA_NODEFER;
	if (sigaction(SIGILL, &sa, 0) != 0) {
		perror("sigaction SIGILL");
		exit(1);
	}

	/* Set up the timer handler.
	 */
	sa.sa_sigaction = signal_handler;
	sa.sa_mask = mask_disable;
	sa.sa_flags = SA_ONSTACK | SA_RESTART | SA_SIGINFO;
	if (sigaction(SIGALRM, &sa, 0) != 0) {
		perror("sigaction SIGALRM");
		exit(1);
	}

	/* Set up the handler for SIGIO that allows us to preempt
	 * processes when I/O is possible.
	 */
	sa.sa_sigaction = signal_handler;
	sa.sa_mask = mask_disable;
	sa.sa_flags = SA_ONSTACK | SA_RESTART | SA_SIGINFO;
	if (sigaction(SIGIO, &sa, 0) != 0) {
		perror("sigaction SIGIO");
		exit(1);
	}
}

/* Wait for I/O to be pending or until maxtime (in milliseconds) has expired.
 * Interrupts are actually disabled at this point.
 */
static void intr_suspend(unsigned int maxtime){
	/* First check for events.
	 */
	for (;;) {
		struct event *ev = queue_get(&intr.events);
		if (ev == 0) {
			break;
		}
		(*ev->handler)(ev->arg);
		free(ev);
		maxtime = 0;		// don't wait for any time for other things
	}
	
	/* If there's nothing to check and no waiting involved, return.
	 */
	if (intr.ndevs == 0 && maxtime == 0) {
		return;
	}

	struct pollfd *fds = (struct pollfd *) calloc(intr.ndevs, sizeof(*fds));
	struct device *dev;
	int i;

	for (dev = intr.devs, i = 0; dev != 0; dev = dev->next, i++) {
		fds[i].fd = dev->fd;
		fds[i].events = POLLIN;
	}
	int n = poll(fds, intr.ndevs, maxtime);
	if (n < 0) {
		perror("poll");
	}
	else {
		for (dev = intr.devs, i = 0; n > 0 && dev != 0; dev = dev->next, i++) {
			if (fds[i].revents & POLLIN) {
				(*dev->read_avail)(dev->arg);
				n--;
			}
		}
		if (n > 0) {
			fprintf(stderr, "intr_suspend: unhandled input %d\n", n);
			exit(1);
		}
	}
	free(fds);
}

/* Register a "device", represented by a file descriptor.  Currently only
 * for reading.
 */
static void intr_register_dev(int fd, void (*read_avail)(void *), void *arg){
	struct device *dev = calloc(1, sizeof(struct device));
	
	dev->fd = fd;
	dev->read_avail = read_avail;
	dev->arg = arg;
	dev->next = intr.devs;
	intr.devs = dev;
	intr.ndevs++;
}

/* Schedule an event to be invoked at the next intr_suspend().
 */
static void intr_sched_event(void (*handler)(void *arg), void *arg){
	struct event *ev = calloc(1, sizeof(struct event));
	ev->handler = handler;
	ev->arg = arg;
	queue_add(&intr.events, ev);
}

void intr_setup(struct intr_intf *ii){
	ii->initialize = intr_initialize;
	ii->suspend = intr_suspend;
	ii->register_dev = intr_register_dev;
#ifdef OBSOLETE
	ii->save = intr_save;
	ii->restore = intr_restore;
#endif
	ii->sched_event = intr_sched_event;
}
