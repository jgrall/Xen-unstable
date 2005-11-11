/*
 *  linux/arch/x86-64/traps.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *  Copyright (C) 2000, 2001, 2002 Andi Kleen, SuSE Labs
 *
 *  Pentium III FXSR, SSE support
 *	Gareth Hughes <gareth@valinux.com>, May 2000
 *
 *  $Id: traps.c,v 1.36 2002/03/24 11:09:10 ak Exp $
 */

/*
 * 'Traps.c' handles hardware traps and faults after we have saved some
 * state in 'entry.S'.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/ptrace.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/nmi.h>

#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/atomic.h>
#include <asm/debugreg.h>
#include <asm/desc.h>
#include <asm/i387.h>
#include <asm/kdebug.h>
#include <asm/processor.h>

#include <asm/smp.h>
#include <asm/pgalloc.h>
#include <asm/pda.h>
#include <asm/proto.h>
#include <asm/nmi.h>

#include <linux/irq.h>


extern struct gate_struct idt_table[256]; 

asmlinkage void divide_error(void);
asmlinkage void debug(void);
asmlinkage void nmi(void);
asmlinkage void int3(void);
asmlinkage void overflow(void);
asmlinkage void bounds(void);
asmlinkage void invalid_op(void);
asmlinkage void device_not_available(void);
asmlinkage void double_fault(void);
asmlinkage void coprocessor_segment_overrun(void);
asmlinkage void invalid_TSS(void);
asmlinkage void segment_not_present(void);
asmlinkage void stack_segment(void);
asmlinkage void general_protection(void);
asmlinkage void page_fault(void);
asmlinkage void coprocessor_error(void);
asmlinkage void simd_coprocessor_error(void);
asmlinkage void reserved(void);
asmlinkage void alignment_check(void);
asmlinkage void machine_check(void);
asmlinkage void spurious_interrupt_bug(void);
asmlinkage void call_debug(void);

struct notifier_block *die_chain;
static DEFINE_SPINLOCK(die_notifier_lock);

int register_die_notifier(struct notifier_block *nb)
{
	int err = 0;
	unsigned long flags;
	spin_lock_irqsave(&die_notifier_lock, flags);
	err = notifier_chain_register(&die_chain, nb);
	spin_unlock_irqrestore(&die_notifier_lock, flags);
	return err;
}

static inline void conditional_sti(struct pt_regs *regs)
{
	if (regs->eflags & X86_EFLAGS_IF)
		local_irq_enable();
}

static int kstack_depth_to_print = 10;

#ifdef CONFIG_KALLSYMS
#include <linux/kallsyms.h> 
int printk_address(unsigned long address)
{ 
	unsigned long offset = 0, symsize;
	const char *symname;
	char *modname;
	char *delim = ":"; 
	char namebuf[128];

	symname = kallsyms_lookup(address, &symsize, &offset, &modname, namebuf); 
	if (!symname) 
		return printk("[<%016lx>]", address);
	if (!modname) 
		modname = delim = ""; 		
        return printk("<%016lx>{%s%s%s%s%+ld}",
		      address,delim,modname,delim,symname,offset); 
} 
#else
int printk_address(unsigned long address)
{ 
	return printk("[<%016lx>]", address);
} 
#endif

static unsigned long *in_exception_stack(unsigned cpu, unsigned long stack,
					unsigned *usedp, const char **idp)
{
	static const char ids[N_EXCEPTION_STACKS][8] = {
		[DEBUG_STACK - 1] = "#DB",
		[NMI_STACK - 1] = "NMI",
		[DOUBLEFAULT_STACK - 1] = "#DF",
		[STACKFAULT_STACK - 1] = "#SS",
		[MCE_STACK - 1] = "#MC",
	};
	unsigned k;

	for (k = 0; k < N_EXCEPTION_STACKS; k++) {
		unsigned long end;

		end = per_cpu(init_tss, cpu).ist[k];
		if (stack >= end)
			continue;
		if (stack >= end - EXCEPTION_STKSZ) {
			if (*usedp & (1U << k))
				break;
			*usedp |= 1U << k;
			*idp = ids[k];
			return (unsigned long *)end;
		}
	}
	return NULL;
}

/*
 * x86-64 can have upto three kernel stacks: 
 * process stack
 * interrupt stack
 * severe exception (double fault, nmi, stack fault, debug, mce) hardware stack
 */

void show_trace(unsigned long *stack)
{
	unsigned long addr;
	const unsigned cpu = safe_smp_processor_id();
	unsigned long *irqstack_end = (unsigned long *)cpu_pda[cpu].irqstackptr;
	int i;
	unsigned used = 0;

	printk("\nCall Trace:");

#define HANDLE_STACK(cond) \
	do while (cond) { \
		addr = *stack++; \
		if (kernel_text_address(addr)) { \
			/* \
			 * If the address is either in the text segment of the \
			 * kernel, or in the region which contains vmalloc'ed \
			 * memory, it *may* be the address of a calling \
			 * routine; if so, print it so that someone tracing \
			 * down the cause of the crash will be able to figure \
			 * out the call path that was taken. \
			 */ \
			i += printk_address(addr); \
			if (i > 50) { \
				printk("\n       "); \
				i = 0; \
			} \
			else \
				i += printk(" "); \
		} \
	} while (0)

	for(i = 0; ; ) {
		const char *id;
		unsigned long *estack_end;
		estack_end = in_exception_stack(cpu, (unsigned long)stack,
						&used, &id);

		if (estack_end) {
			i += printk(" <%s> ", id);
			HANDLE_STACK (stack < estack_end);
			i += printk(" <EOE> ");
			stack = (unsigned long *) estack_end[-2];
			continue;
		}
		if (irqstack_end) {
			unsigned long *irqstack;
			irqstack = irqstack_end -
				(IRQSTACKSIZE - 64) / sizeof(*irqstack);

			if (stack >= irqstack && stack < irqstack_end) {
				i += printk(" <IRQ> ");
				HANDLE_STACK (stack < irqstack_end);
				stack = (unsigned long *) (irqstack_end[-1]);
				irqstack_end = NULL;
				i += printk(" <EOI> ");
				continue;
			}
		}
		break;
	}

	HANDLE_STACK (((long) stack & (THREAD_SIZE-1)) != 0);
#undef HANDLE_STACK
	printk("\n");
}

void show_stack(struct task_struct *tsk, unsigned long * rsp)
{
	unsigned long *stack;
	int i;
	const int cpu = safe_smp_processor_id();
	unsigned long *irqstack_end = (unsigned long *) (cpu_pda[cpu].irqstackptr);
	unsigned long *irqstack = (unsigned long *) (cpu_pda[cpu].irqstackptr - IRQSTACKSIZE);    

	// debugging aid: "show_stack(NULL, NULL);" prints the
	// back trace for this cpu.

	if (rsp == NULL) {
		if (tsk)
			rsp = (unsigned long *)tsk->thread.rsp;
		else
			rsp = (unsigned long *)&rsp;
	}

	stack = rsp;
	for(i=0; i < kstack_depth_to_print; i++) {
		if (stack >= irqstack && stack <= irqstack_end) {
			if (stack == irqstack_end) {
				stack = (unsigned long *) (irqstack_end[-1]);
				printk(" <EOI> ");
			}
		} else {
		if (((long) stack & (THREAD_SIZE-1)) == 0)
			break;
		}
		if (i && ((i % 4) == 0))
			printk("\n       ");
		printk("%016lx ", *stack++);
		touch_nmi_watchdog();
	}
	show_trace((unsigned long *)rsp);
}

/*
 * The architecture-independent dump_stack generator
 */
void dump_stack(void)
{
	unsigned long dummy;
	show_trace(&dummy);
}

EXPORT_SYMBOL(dump_stack);

void show_registers(struct pt_regs *regs)
{
	int i;
	int in_kernel = (regs->cs & 3) == 0;
	unsigned long rsp;
	const int cpu = safe_smp_processor_id(); 
	struct task_struct *cur = cpu_pda[cpu].pcurrent; 

		rsp = regs->rsp;

	printk("CPU %d ", cpu);
	__show_regs(regs);
	printk("Process %s (pid: %d, threadinfo %p, task %p)\n",
		cur->comm, cur->pid, cur->thread_info, cur);

	/*
	 * When in-kernel, we also print out the stack and code at the
	 * time of the fault..
	 */
	if (in_kernel) {

		printk("Stack: ");
		show_stack(NULL, (unsigned long*)rsp);

		printk("\nCode: ");
		if(regs->rip < PAGE_OFFSET)
			goto bad;

		for(i=0;i<20;i++)
		{
			unsigned char c;
			if(__get_user(c, &((unsigned char*)regs->rip)[i])) {
bad:
				printk(" Bad RIP value.");
				break;
			}
			printk("%02x ", c);
		}
	}
	printk("\n");
}	

void handle_BUG(struct pt_regs *regs)
{ 
	struct bug_frame f;
	char tmp;

	if (regs->cs & 3)
		return; 
	if (__copy_from_user(&f, (struct bug_frame *) regs->rip, 
			     sizeof(struct bug_frame)))
		return; 
	if ((unsigned long)f.filename < __PAGE_OFFSET || 
	    f.ud2[0] != 0x0f || f.ud2[1] != 0x0b) 
		return;
	if (__get_user(tmp, f.filename))
		f.filename = "unmapped filename"; 
	printk("----------- [cut here ] --------- [please bite here ] ---------\n");
	printk(KERN_ALERT "Kernel BUG at %.50s:%d\n", f.filename, f.line);
} 

#ifdef CONFIG_BUG
void out_of_line_bug(void)
{ 
	BUG(); 
} 
#endif

static DEFINE_SPINLOCK(die_lock);
static int die_owner = -1;

void oops_begin(void)
{
	int cpu = safe_smp_processor_id(); 
	/* racy, but better than risking deadlock. */ 
	local_irq_disable();
	if (!spin_trylock(&die_lock)) { 
		if (cpu == die_owner) 
			/* nested oops. should stop eventually */;
		else
			spin_lock(&die_lock); 
	}
	die_owner = cpu; 
	console_verbose();
	bust_spinlocks(1); 
}

void oops_end(void)
{ 
	die_owner = -1;
	bust_spinlocks(0); 
	spin_unlock(&die_lock); 
	if (panic_on_oops)
		panic("Oops"); 
} 

void __die(const char * str, struct pt_regs * regs, long err)
{
	static int die_counter;
	printk(KERN_EMERG "%s: %04lx [%u] ", str, err & 0xffff,++die_counter);
#ifdef CONFIG_PREEMPT
	printk("PREEMPT ");
#endif
#ifdef CONFIG_SMP
	printk("SMP ");
#endif
#ifdef CONFIG_DEBUG_PAGEALLOC
	printk("DEBUG_PAGEALLOC");
#endif
	printk("\n");
	notify_die(DIE_OOPS, (char *)str, regs, err, 255, SIGSEGV);
	show_registers(regs);
	/* Executive summary in case the oops scrolled away */
	printk(KERN_ALERT "RIP ");
	printk_address(regs->rip); 
	printk(" RSP <%016lx>\n", regs->rsp); 
}

void die(const char * str, struct pt_regs * regs, long err)
{
	oops_begin();
	handle_BUG(regs);
	__die(str, regs, err);
	oops_end();
	do_exit(SIGSEGV); 
}
static inline void die_if_kernel(const char * str, struct pt_regs * regs, long err)
{
	if (!(regs->eflags & VM_MASK) && (regs->cs == __KERNEL_CS))
		die(str, regs, err);
}

#ifdef CONFIG_X86_LOCAL_APIC
void die_nmi(char *str, struct pt_regs *regs)
{
	oops_begin();
	/*
	 * We are in trouble anyway, lets at least try
	 * to get a message out.
	 */
	printk(str, safe_smp_processor_id());
	show_registers(regs);
	if (panic_on_timeout || panic_on_oops)
		panic("nmi watchdog");
	printk("console shuts up ...\n");
	oops_end();
	do_exit(SIGSEGV);
}
#endif

static void do_trap(int trapnr, int signr, char *str, 
			   struct pt_regs * regs, long error_code, siginfo_t *info)
{
	conditional_sti(regs);

#ifdef CONFIG_CHECKING
       { 
               unsigned long gs; 
               struct x8664_pda *pda = cpu_pda + safe_smp_processor_id(); 
               rdmsrl(MSR_GS_BASE, gs); 
               if (gs != (unsigned long)pda) { 
                       wrmsrl(MSR_GS_BASE, pda); 
                       printk("%s: wrong gs %lx expected %p rip %lx\n", str, gs, pda,
			      regs->rip);
               }
       }
#endif

	if ((regs->cs & 3)  != 0) { 
		struct task_struct *tsk = current;

		if (exception_trace && unhandled_signal(tsk, signr))
			printk(KERN_INFO
			       "%s[%d] trap %s rip:%lx rsp:%lx error:%lx\n",
			       tsk->comm, tsk->pid, str,
			       regs->rip,regs->rsp,error_code); 

		tsk->thread.error_code = error_code;
		tsk->thread.trap_no = trapnr;
		if (info)
			force_sig_info(signr, info, tsk);
		else
			force_sig(signr, tsk);
		return;
	}


	/* kernel trap */ 
	{	     
		const struct exception_table_entry *fixup;
		fixup = search_exception_tables(regs->rip);
		if (fixup) {
			regs->rip = fixup->fixup;
		} else	
			die(str, regs, error_code);
		return;
	}
}

#define DO_ERROR(trapnr, signr, str, name) \
asmlinkage void do_##name(struct pt_regs * regs, long error_code) \
{ \
	if (notify_die(DIE_TRAP, str, regs, error_code, trapnr, signr) \
							== NOTIFY_STOP) \
		return; \
	do_trap(trapnr, signr, str, regs, error_code, NULL); \
}

#define DO_ERROR_INFO(trapnr, signr, str, name, sicode, siaddr) \
asmlinkage void do_##name(struct pt_regs * regs, long error_code) \
{ \
	siginfo_t info; \
	info.si_signo = signr; \
	info.si_errno = 0; \
	info.si_code = sicode; \
	info.si_addr = (void __user *)siaddr; \
	if (notify_die(DIE_TRAP, str, regs, error_code, trapnr, signr) \
							== NOTIFY_STOP) \
		return; \
	do_trap(trapnr, signr, str, regs, error_code, &info); \
}

DO_ERROR_INFO( 0, SIGFPE,  "divide error", divide_error, FPE_INTDIV, regs->rip)
DO_ERROR( 4, SIGSEGV, "overflow", overflow)
DO_ERROR( 5, SIGSEGV, "bounds", bounds)
DO_ERROR_INFO( 6, SIGILL,  "invalid operand", invalid_op, ILL_ILLOPN, regs->rip)
DO_ERROR( 7, SIGSEGV, "device not available", device_not_available)
DO_ERROR( 9, SIGFPE,  "coprocessor segment overrun", coprocessor_segment_overrun)
DO_ERROR(10, SIGSEGV, "invalid TSS", invalid_TSS)
DO_ERROR(11, SIGBUS,  "segment not present", segment_not_present)
DO_ERROR_INFO(17, SIGBUS, "alignment check", alignment_check, BUS_ADRALN, 0)
DO_ERROR(18, SIGSEGV, "reserved", reserved)
DO_ERROR(12, SIGBUS,  "stack segment", stack_segment)
DO_ERROR( 8, SIGSEGV, "double fault", double_fault)

asmlinkage void do_general_protection(struct pt_regs * regs, long error_code)
{
	conditional_sti(regs);

#ifdef CONFIG_CHECKING
       { 
               unsigned long gs; 
               struct x8664_pda *pda = cpu_pda + safe_smp_processor_id(); 
               rdmsrl(MSR_GS_BASE, gs); 
               if (gs != (unsigned long)pda) { 
                       wrmsrl(MSR_GS_BASE, pda); 
		       oops_in_progress++;
                       printk("general protection handler: wrong gs %lx expected %p\n", gs, pda);
		       oops_in_progress--;
               }
       }
#endif

	if ((regs->cs & 3)!=0) { 
		struct task_struct *tsk = current;

		if (exception_trace && unhandled_signal(tsk, SIGSEGV))
			printk(KERN_INFO
		       "%s[%d] general protection rip:%lx rsp:%lx error:%lx\n",
			       tsk->comm, tsk->pid,
			       regs->rip,regs->rsp,error_code); 

		tsk->thread.error_code = error_code;
		tsk->thread.trap_no = 13;
		force_sig(SIGSEGV, tsk);
		return;
	} 

	/* kernel gp */
	{
		const struct exception_table_entry *fixup;
		fixup = search_exception_tables(regs->rip);
		if (fixup) {
			regs->rip = fixup->fixup;
			return;
		}
		if (notify_die(DIE_GPF, "general protection fault", regs,
					error_code, 13, SIGSEGV) == NOTIFY_STOP)
			return;
		die("general protection fault", regs, error_code);
	}
}

static void mem_parity_error(unsigned char reason, struct pt_regs * regs)
{
	printk("Uhhuh. NMI received. Dazed and confused, but trying to continue\n");
	printk("You probably have a hardware problem with your RAM chips\n");

	/* Clear and disable the memory parity error line. */
	reason = (reason & 0xf) | 4;
	outb(reason, 0x61);
}

static void io_check_error(unsigned char reason, struct pt_regs * regs)
{
	printk("NMI: IOCK error (debug interrupt?)\n");
	show_registers(regs);

	/* Re-enable the IOCK line, wait for a few seconds */
	reason = (reason & 0xf) | 8;
	outb(reason, 0x61);
	mdelay(2000);
	reason &= ~8;
	outb(reason, 0x61);
}

static void unknown_nmi_error(unsigned char reason, struct pt_regs * regs)
{	printk("Uhhuh. NMI received for unknown reason %02x.\n", reason);
	printk("Dazed and confused, but trying to continue\n");
	printk("Do you have a strange power saving mode enabled?\n");
}

/* Runs on IST stack. This code must keep interrupts off all the time.
   Nested NMIs are prevented by the CPU. */
asmlinkage void default_do_nmi(struct pt_regs *regs)
{
	unsigned char reason = 0;

	/* Only the BSP gets external NMIs from the system.  */
	if (!smp_processor_id())
		reason = get_nmi_reason();

	if (!(reason & 0xc0)) {
		if (notify_die(DIE_NMI_IPI, "nmi_ipi", regs, reason, 0, SIGINT)
								== NOTIFY_STOP)
			return;
#ifdef CONFIG_X86_LOCAL_APIC
		/*
		 * Ok, so this is none of the documented NMI sources,
		 * so it must be the NMI watchdog.
		 */
		if (nmi_watchdog > 0) {
			nmi_watchdog_tick(regs,reason);
			return;
		}
#endif
		unknown_nmi_error(reason, regs);
		return;
	}
	if (notify_die(DIE_NMI, "nmi", regs, reason, 0, SIGINT) == NOTIFY_STOP)
		return; 

	/* AK: following checks seem to be broken on modern chipsets. FIXME */

	if (reason & 0x80)
		mem_parity_error(reason, regs);
	if (reason & 0x40)
		io_check_error(reason, regs);
}

asmlinkage void do_int3(struct pt_regs * regs, long error_code)
{
	if (notify_die(DIE_INT3, "int3", regs, error_code, 3, SIGTRAP) == NOTIFY_STOP) {
		return;
	}
	do_trap(3, SIGTRAP, "int3", regs, error_code, NULL);
	return;
}

/* Help handler running on IST stack to switch back to user stack
   for scheduling or signal handling. The actual stack switch is done in
   entry.S */
asmlinkage struct pt_regs *sync_regs(struct pt_regs *eregs)
{
	struct pt_regs *regs = eregs;
	/* Did already sync */
	if (eregs == (struct pt_regs *)eregs->rsp)
		;
	/* Exception from user space */
	else if (eregs->cs & 3)
		regs = ((struct pt_regs *)current->thread.rsp0) - 1;
	/* Exception from kernel and interrupts are enabled. Move to
 	   kernel process stack. */
	else if (eregs->eflags & X86_EFLAGS_IF)
		regs = (struct pt_regs *)(eregs->rsp -= sizeof(struct pt_regs));
	if (eregs != regs)
		*regs = *eregs;
	return regs;
}

/* runs on IST stack. */
asmlinkage void do_debug(struct pt_regs * regs, unsigned long error_code)
{
	unsigned long condition;
	struct task_struct *tsk = current;
	siginfo_t info;

#ifdef CONFIG_CHECKING
       { 
	       /* RED-PEN interaction with debugger - could destroy gs */
               unsigned long gs; 
               struct x8664_pda *pda = cpu_pda + safe_smp_processor_id(); 
               rdmsrl(MSR_GS_BASE, gs); 
               if (gs != (unsigned long)pda) { 
                       wrmsrl(MSR_GS_BASE, pda); 
                       printk("debug handler: wrong gs %lx expected %p\n", gs, pda);
               }
       }
#endif

	asm("movq %%db6,%0" : "=r" (condition));

	if (notify_die(DIE_DEBUG, "debug", regs, condition, error_code,
						SIGTRAP) == NOTIFY_STOP)
		return;

	conditional_sti(regs);

	/* Mask out spurious debug traps due to lazy DR7 setting */
	if (condition & (DR_TRAP0|DR_TRAP1|DR_TRAP2|DR_TRAP3)) {
		if (!tsk->thread.debugreg7) { 
			goto clear_dr7;
		}
	}

	tsk->thread.debugreg6 = condition;

	/* Mask out spurious TF errors due to lazy TF clearing */
	if (condition & DR_STEP) {
		/*
		 * The TF error should be masked out only if the current
		 * process is not traced and if the TRAP flag has been set
		 * previously by a tracing process (condition detected by
		 * the PT_DTRACE flag); remember that the i386 TRAP flag
		 * can be modified by the process itself in user mode,
		 * allowing programs to debug themselves without the ptrace()
		 * interface.
		 */
                if ((regs->cs & 3) == 0)
                       goto clear_TF_reenable;
		/*
		 * Was the TF flag set by a debugger? If so, clear it now,
		 * so that register information is correct.
		 */
		if (tsk->ptrace & PT_DTRACE) {
			regs->eflags &= ~TF_MASK;
			tsk->ptrace &= ~PT_DTRACE;
		}
	}

	/* Ok, finally something we can handle */
	tsk->thread.trap_no = 1;
	tsk->thread.error_code = error_code;
	info.si_signo = SIGTRAP;
	info.si_errno = 0;
	info.si_code = TRAP_BRKPT;
	if ((regs->cs & 3) == 0) 
		goto clear_dr7; 

	info.si_addr = (void __user *)regs->rip;
	force_sig_info(SIGTRAP, &info, tsk);	
clear_dr7:
	asm volatile("movq %0,%%db7"::"r"(0UL));
	return;

clear_TF_reenable:
	set_tsk_thread_flag(tsk, TIF_SINGLESTEP);
	regs->eflags &= ~TF_MASK;
}

static int kernel_math_error(struct pt_regs *regs, char *str)
{
	const struct exception_table_entry *fixup;
	fixup = search_exception_tables(regs->rip);
	if (fixup) {
		regs->rip = fixup->fixup;
		return 1;
	}
	notify_die(DIE_GPF, str, regs, 0, 16, SIGFPE);
	/* Illegal floating point operation in the kernel */
	die(str, regs, 0);
	return 0;
}

/*
 * Note that we play around with the 'TS' bit in an attempt to get
 * the correct behaviour even in the presence of the asynchronous
 * IRQ13 behaviour
 */
asmlinkage void do_coprocessor_error(struct pt_regs *regs)
{
	void __user *rip = (void __user *)(regs->rip);
	struct task_struct * task;
	siginfo_t info;
	unsigned short cwd, swd;

	conditional_sti(regs);
	if ((regs->cs & 3) == 0 &&
	    kernel_math_error(regs, "kernel x87 math error"))
		return;

	/*
	 * Save the info for the exception handler and clear the error.
	 */
	task = current;
	save_init_fpu(task);
	task->thread.trap_no = 16;
	task->thread.error_code = 0;
	info.si_signo = SIGFPE;
	info.si_errno = 0;
	info.si_code = __SI_FAULT;
	info.si_addr = rip;
	/*
	 * (~cwd & swd) will mask out exceptions that are not set to unmasked
	 * status.  0x3f is the exception bits in these regs, 0x200 is the
	 * C1 reg you need in case of a stack fault, 0x040 is the stack
	 * fault bit.  We should only be taking one exception at a time,
	 * so if this combination doesn't produce any single exception,
	 * then we have a bad program that isn't synchronizing its FPU usage
	 * and it will suffer the consequences since we won't be able to
	 * fully reproduce the context of the exception
	 */
	cwd = get_fpu_cwd(task);
	swd = get_fpu_swd(task);
	switch (((~cwd) & swd & 0x3f) | (swd & 0x240)) {
		case 0x000:
		default:
			break;
		case 0x001: /* Invalid Op */
		case 0x041: /* Stack Fault */
		case 0x241: /* Stack Fault | Direction */
			info.si_code = FPE_FLTINV;
			break;
		case 0x002: /* Denormalize */
		case 0x010: /* Underflow */
			info.si_code = FPE_FLTUND;
			break;
		case 0x004: /* Zero Divide */
			info.si_code = FPE_FLTDIV;
			break;
		case 0x008: /* Overflow */
			info.si_code = FPE_FLTOVF;
			break;
		case 0x020: /* Precision */
			info.si_code = FPE_FLTRES;
			break;
	}
	force_sig_info(SIGFPE, &info, task);
}

asmlinkage void bad_intr(void)
{
	printk("bad interrupt"); 
}

asmlinkage void do_simd_coprocessor_error(struct pt_regs *regs)
{
	void __user *rip = (void __user *)(regs->rip);
	struct task_struct * task;
	siginfo_t info;
	unsigned short mxcsr;

	conditional_sti(regs);
	if ((regs->cs & 3) == 0 &&
        	kernel_math_error(regs, "kernel simd math error"))
		return;

	/*
	 * Save the info for the exception handler and clear the error.
	 */
	task = current;
	save_init_fpu(task);
	task->thread.trap_no = 19;
	task->thread.error_code = 0;
	info.si_signo = SIGFPE;
	info.si_errno = 0;
	info.si_code = __SI_FAULT;
	info.si_addr = rip;
	/*
	 * The SIMD FPU exceptions are handled a little differently, as there
	 * is only a single status/control register.  Thus, to determine which
	 * unmasked exception was caught we must mask the exception mask bits
	 * at 0x1f80, and then use these to mask the exception bits at 0x3f.
	 */
	mxcsr = get_fpu_mxcsr(task);
	switch (~((mxcsr & 0x1f80) >> 7) & (mxcsr & 0x3f)) {
		case 0x000:
		default:
			break;
		case 0x001: /* Invalid Op */
			info.si_code = FPE_FLTINV;
			break;
		case 0x002: /* Denormalize */
		case 0x010: /* Underflow */
			info.si_code = FPE_FLTUND;
			break;
		case 0x004: /* Zero Divide */
			info.si_code = FPE_FLTDIV;
			break;
		case 0x008: /* Overflow */
			info.si_code = FPE_FLTOVF;
			break;
		case 0x020: /* Precision */
			info.si_code = FPE_FLTRES;
			break;
	}
	force_sig_info(SIGFPE, &info, task);
}

asmlinkage void do_spurious_interrupt_bug(struct pt_regs * regs)
{
}

#if 0
asmlinkage void __attribute__((weak)) smp_thermal_interrupt(void)
{
}
#endif

/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 *
 * Careful.. There are problems with IBM-designed IRQ13 behaviour.
 * Don't touch unless you *really* know how it works.
 */
asmlinkage void math_state_restore(void)
{
	struct task_struct *me = current;
        
        /* clts(); */ /* 'clts' is done for us by Xen during virtual trap. */

	if (!used_math())
		init_fpu(me);
	restore_fpu_checking(&me->thread.i387.fxsave);
	me->thread_info->status |= TS_USEDFPU;
}

void do_call_debug(struct pt_regs *regs) 
{ 
	notify_die(DIE_CALL, "debug call", regs, 0, 255, SIGINT); 
}


/*
 * NB. All these are "interrupt gates" (i.e. events_mask is set) because we
 * specify <dpl>|4 in the second field.
 */
static trap_info_t trap_table[] = {
        {  0, 0|4, (__KERNEL_CS|0x3), (unsigned long)divide_error               },
        {  1, 0|4, (__KERNEL_CS|0x3), (unsigned long)debug                      },
        {  3, 3|4, (__KERNEL_CS|0x3), (unsigned long)int3                       },
        {  4, 3|4, (__KERNEL_CS|0x3), (unsigned long)overflow                   },
        {  5, 3|4, (__KERNEL_CS|0x3), (unsigned long)bounds                     },
        {  6, 0|4, (__KERNEL_CS|0x3), (unsigned long)invalid_op                 },
        {  7, 0|4, (__KERNEL_CS|0x3), (unsigned long)device_not_available       },
        {  9, 0|4, (__KERNEL_CS|0x3), (unsigned long)coprocessor_segment_overrun},
        { 10, 0|4, (__KERNEL_CS|0x3), (unsigned long)invalid_TSS                },
        { 11, 0|4, (__KERNEL_CS|0x3), (unsigned long)segment_not_present        },
        { 12, 0|4, (__KERNEL_CS|0x3), (unsigned long)stack_segment              },
        { 13, 0|4, (__KERNEL_CS|0x3), (unsigned long)general_protection         },
        { 14, 0|4, (__KERNEL_CS|0x3), (unsigned long)page_fault                 },
        { 15, 0|4, (__KERNEL_CS|0x3), (unsigned long)spurious_interrupt_bug     },
        { 16, 0|4, (__KERNEL_CS|0x3), (unsigned long)coprocessor_error          },
        { 17, 0|4, (__KERNEL_CS|0x3), (unsigned long)alignment_check            },
#ifdef CONFIG_X86_MCE
        { 18, 0|4, (__KERNEL_CS|0x3), (unsigned long)machine_check              },
#endif
        { 19, 0|4, (__KERNEL_CS|0x3), (unsigned long)simd_coprocessor_error     },
#ifdef CONFIG_IA32_EMULATION
	{ IA32_SYSCALL_VECTOR, 3|4, (__KERNEL_CS|0x3), (unsigned long)ia32_syscall},
#endif
        {  0, 0,           0, 0                                              }
};

void __init trap_init(void)
{
        int ret;

        ret = HYPERVISOR_set_trap_table(trap_table);
        
        if (ret) 
                printk("HYPERVISOR_set_trap_table faild: error %d\n",
                       ret);

#ifdef CONFIG_IA32_EMULATION
	set_system_gate(IA32_SYSCALL_VECTOR, ia32_syscall);
#endif
       
	/*
	 * Should be a barrier for any external CPU state.
	 */
	cpu_init();
}

void smp_trap_init(trap_info_t *trap_ctxt)
{
	trap_info_t *t = trap_table;
	int i;

	for (i = 0; i < 256; i++) {
		trap_ctxt[i].vector = i;
		trap_ctxt[i].cs     = FLAT_KERNEL_CS;
	}

	for (t = trap_table; t->address; t++) {
		trap_ctxt[t->vector].flags = t->flags;
		trap_ctxt[t->vector].cs = t->cs;
		trap_ctxt[t->vector].address = t->address;
	}
}


/* Actual parsing is done early in setup.c. */
static int __init oops_dummy(char *s)
{ 
	panic_on_oops = 1;
	return -1; 
} 
__setup("oops=", oops_dummy); 

static int __init kstack_setup(char *s)
{
	kstack_depth_to_print = simple_strtoul(s,NULL,0);
	return 0;
}
__setup("kstack=", kstack_setup);

