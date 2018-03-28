// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <arch/arch.h>
#include <stab.h>
#include <smp.h>
#include <arch/console.h>

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <monitor.h>
#include <trap.h>
#include <pmap.h>
#include <kdebug.h>
#include <testing.h>
#include <manager.h>
#include <schedule.h>
#include <kdebug.h>
#include <syscall.h>
#include <kmalloc.h>
#include <elf.h>
#include <event.h>
#include <trap.h>
#include <time.h>
#include <percpu.h>

#include <ros/memlayout.h>
#include <ros/event.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line

typedef struct command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char **argv, struct hw_trapframe *hw_tf);
} command_t;

static command_t commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Dump a backtrace", mon_backtrace },
	{ "bt", "Dump a backtrace", mon_backtrace },
	{ "reboot", "Take a ride to the South Bay", mon_reboot },
	{ "showmapping", "Shows VA->PA mappings", mon_showmapping},
	{ "sm", "Shows VA->PA mappings", mon_sm},
	{ "cpuinfo", "Prints CPU diagnostics", mon_cpuinfo},
	{ "ps", "Prints process list", mon_ps},
	{ "nanwan", "Meet Nanwan!!", mon_nanwan},
	{ "bin_ls", "List files in /bin", mon_bin_ls},
	{ "bin_run", "Create and run a program from /bin", mon_bin_run},
	{ "manager", "Run the manager", mon_manager},
	{ "procinfo", "Show information about processes", mon_procinfo},
	{ "pip", "Shorthand for procinfo pid", mon_pip},
	{ "kill", "Kills a process", mon_kill},
	{ "exit", "Leave the monitor", mon_exit},
	{ "e", "Leave the monitor", mon_exit},
	{ "kfunc", "Run a kernel function directly (!!!)", mon_kfunc},
	{ "notify", "Notify a process.  Vcoreid will skip their prefs", mon_notify},
	{ "measure", "Run a specific measurement", mon_measure},
	{ "trace", "Run some tracing functions", mon_trace},
	{ "monitor", "Run the monitor on another core", mon_monitor},
	{ "fs", "Filesystem Diagnostics", mon_fs},
	{ "sh", "Try to run a shell (bash)", mon_shell},
	{ "bash", "Try to run a shell (bash)", mon_shell},
	{ "bb", "Try to run a shell (bash)", mon_shell},
	{ "alarm", "Alarm Diagnostics", mon_alarm},
	{ "msr", "read/write msr: msr msr [value]", mon_msr},
	{ "db", "Misc debugging", mon_db},
	{ "px", "Toggle printx", mon_px},
	{ "kpfret", "Attempt to idle after a kernel fault", mon_kpfret},
	{ "ks", "Kernel scheduler hacks", mon_ks},
	{ "coreinfo", "Print diagnostics for a core", mon_coreinfo},
	{ "hexdump", "Hexdump PID's memory (0 for kernel)", mon_hexdump},
	{ "hd", "Hexdump PID's memory (0 for kernel)", mon_hexdump},
	{ "pahexdump", "Hexdump physical memory", mon_pahexdump},
	{ "phd", "Hexdump physical memory", mon_pahexdump},
};
#define NCOMMANDS (sizeof(commands)/sizeof(commands[0]))

/***** Implementations of basic kernel monitor commands *****/

int mon_help(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	int i;

	for (i = 0; i < NCOMMANDS; i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int mon_ps(int argc, char** argv, struct hw_trapframe *hw_tf)
{
	print_allpids();
	return 0;
}

int mon_kerninfo(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	extern char _start[], etext[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start %016x (virt)  %016x (phys)\n", _start, (uintptr_t)(_start - KERNBASE));
	cprintf("  etext  %016x (virt)  %016x (phys)\n", etext, (uintptr_t)(etext - KERNBASE));
	cprintf("  end    %016x (virt)  %016x (phys)\n", end, (uintptr_t)(end - KERNBASE));
	cprintf("Kernel executable memory footprint: %dKB\n",
		(uint32_t)(end-_start+1023)/1024);
	return 0;
}

static int __backtrace(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	uintptr_t pc, fp;
	if (argc == 1) {
		backtrace();
		return 0;
	}
	if (argc != 3) {
		printk("Need either no arguments, or two (PC and FP) in hex\n");
		return 1;
	}
	pc = strtol(argv[1], 0, 16);
	fp = strtol(argv[2], 0, 16);
	printk("Backtrace from instruction %p, with frame pointer %p\n", pc, fp);
	backtrace_frame(pc, fp);
	return 0;
}

int mon_backtrace(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	return __backtrace(argc, argv, hw_tf);
}

int mon_reboot(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	cprintf("[Scottish Accent]: She's goin' down, Cap'n!\n");
	reboot();

	// really, should never see this
	cprintf("Sigh....\n");
	return 0;
}

static int __showmapping(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	struct proc *p = NULL;
	uintptr_t start;
	size_t size;
	pgdir_t pgdir;
	pid_t pid;

	if (argc < 3) {
		printk("Shows virtual -> physical mappings for a virt addr range.\n");
		printk("Usage: showmapping PID START_ADDR [END_ADDR]\n");
		printk("    PID == 0 for the boot pgdir\n");
		return 1;
	}
	pid = strtol(argv[1], 0, 10);
	if (!pid) {
		pgdir = boot_pgdir;
	} else {
		p = pid2proc(pid);
		if (!p) {
			printk("No proc with pid %d\n", pid);
			return 1;
		}
		pgdir = p->env_pgdir;
	}
	start = ROUNDDOWN(strtol(argv[2], 0, 16), PGSIZE);
	size = (argc == 3) ? 1 : strtol(argv[3], 0, 16) - start;
	if (size/PGSIZE > 512) {
		cprintf("Not going to do this for more than 512 items\n");
		return 1;
	}
	show_mapping(pgdir, start, size);
	if (p)
		proc_decref(p);
	return 0;
}

int mon_showmapping(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	return __showmapping(argc, argv, hw_tf);
}

int mon_sm(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	return __showmapping(argc, argv, hw_tf);
}

static spinlock_t print_info_lock = SPINLOCK_INITIALIZER_IRQSAVE;

static void print_info_handler(struct hw_trapframe *hw_tf, void *data)
{
	uint64_t tsc = read_tsc();

	spin_lock_irqsave(&print_info_lock);
	cprintf("----------------------------\n");
	cprintf("This is Core %d\n", core_id());
	cprintf("Timestamp = %lld\n", tsc);
#ifdef CONFIG_X86
	cprintf("Hardware core %d\n", hw_core_id());
	cprintf("MTRR_DEF_TYPE = 0x%08x\n", read_msr(IA32_MTRR_DEF_TYPE));
	cprintf("MTRR Phys0 Base = 0x%016llx, Mask = 0x%016llx\n",
	        read_msr(0x200), read_msr(0x201));
	cprintf("MTRR Phys1 Base = 0x%016llx, Mask = 0x%016llx\n",
	        read_msr(0x202), read_msr(0x203));
	cprintf("MTRR Phys2 Base = 0x%016llx, Mask = 0x%016llx\n",
	        read_msr(0x204), read_msr(0x205));
	cprintf("MTRR Phys3 Base = 0x%016llx, Mask = 0x%016llx\n",
	        read_msr(0x206), read_msr(0x207));
	cprintf("MTRR Phys4 Base = 0x%016llx, Mask = 0x%016llx\n",
	        read_msr(0x208), read_msr(0x209));
	cprintf("MTRR Phys5 Base = 0x%016llx, Mask = 0x%016llx\n",
	        read_msr(0x20a), read_msr(0x20b));
	cprintf("MTRR Phys6 Base = 0x%016llx, Mask = 0x%016llx\n",
	        read_msr(0x20c), read_msr(0x20d));
	cprintf("MTRR Phys7 Base = 0x%016llx, Mask = 0x%016llx\n",
	        read_msr(0x20e), read_msr(0x20f));
#endif // CONFIG_X86
	cprintf("----------------------------\n");
	spin_unlock_irqsave(&print_info_lock);
}

static bool print_all_info(void)
{
	cprintf("\nCORE 0 asking all cores to print info:\n");
	smp_call_function_all(print_info_handler, NULL, 0);
	cprintf("\nDone!\n");
	return true;
}

int mon_cpuinfo(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	cprintf("Number of Cores detected: %d\n", num_cores);
	cprintf("Calling CPU's ID: 0x%08x\n", core_id());

	if (argc < 2)
		smp_call_function_self(print_info_handler, NULL, 0);
	else
		smp_call_function_single(strtol(argv[1], 0, 10),
		                         print_info_handler, NULL, 0);
	return 0;
}

int mon_manager(int argc, char** argv, struct hw_trapframe *hw_tf)
{
	manager();
	panic("should never get here");
	return 0;
}

int mon_nanwan(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	/* Borrowed with love from http://www.geocities.com/SoHo/7373/zoo.htm
	 * (http://www.ascii-art.com/).  Slightly modified to make it 25 lines tall.
	 */
	printk("\n");
	printk("             .-.  .-.\n");
	printk("             |  \\/  |\n");
	printk("            /,   ,_  `'-.\n");
	printk("          .-|\\   /`\\     '. \n");
	printk("        .'  0/   | 0\\  \\_  `\".  \n");
	printk("     .-'  _,/    '--'.'|#''---'\n");
	printk("      `--'  |       /   \\#\n");
	printk("            |      /     \\#\n");
	printk("            \\     ;|\\    .\\#\n");
	printk("            |' ' //  \\   ::\\# \n");
	printk("            \\   /`    \\   ':\\#\n");
	printk("             `\"`       \\..   \\#\n");
	printk("                        \\::.  \\#\n");
	printk("                         \\::   \\#\n");
	printk("                          \\'  .:\\#\n");
	printk("                           \\  :::\\#\n");
	printk("                            \\  '::\\#\n");
	printk("                             \\     \\#\n");
	printk("                              \\:.   \\#\n");
	printk("                               \\::   \\#\n");
	printk("                                \\'   .\\#\n");
	printk("                             jgs \\   ::\\#\n");
	printk("                                  \\      \n");
	return 0;
}

int mon_bin_ls(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	struct kdirent dir = {0};
	struct file *bin_dir;
	int retval = 0;

	bin_dir = do_file_open("/bin", O_EXEC | O_READ, 0);
	if (!bin_dir) {
		printk("No /bin directory!\n");
		return 1;
	}
	printk("Files in /bin:\n-------------------------------\n");
	do {
		retval = bin_dir->f_op->readdir(bin_dir, &dir);
		printk("%s\n", dir.d_name);
	} while (retval == 1);
	kref_put(&bin_dir->f_kref);
	return 0;
}

int mon_bin_run(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	if (argc < 2) {
		printk("Usage: bin_run FILENAME\n");
		return 1;
	}
	struct file_or_chan *program;
	int retval = 0;
	char buf[5 + MAX_FILENAME_SZ + 1] = "/bin/";	/* /bin/ + max + \0 */

	strlcpy(buf, "/bin/", sizeof(buf));
	if (strlcat(buf, argv[1], sizeof(buf)) > sizeof(buf)) {
		printk("Filename '%s' too long!\n", argv[1]);
		return 1;
	}
	program = foc_open(buf, O_EXEC | O_READ, 0);
	if (!program) {
		printk("No such program!\n");
		return 1;
	}
	char **p_argv = kmalloc(sizeof(char*) * argc, 0);	/* bin_run's argc */
	for (int i = 0; i < argc - 1; i++)
		p_argv[i] = argv[i + 1];
	p_argv[argc - 1] = 0;
	/* super ugly: we need to stash current, so that proc_create doesn't pick up
	 * on random processes running here and assuming they are the parent */
	struct proc *old_cur = current;
	current = 0;
	struct proc *p = proc_create(program, p_argv, NULL);
	current = old_cur;
	kfree(p_argv);
	proc_wakeup(p);
	proc_decref(p); /* let go of the reference created in proc_create() */
	foc_decref(program);
	/* Make a scheduling decision.  You might not get the process you created,
	 * in the event there are others floating around that are runnable */
	run_scheduler();
	/* want to idle, so we un the process we just selected.  this is a bit
	 * hackish, but so is the monitor. */
	smp_idle();
	assert(0);
	return 0;
}

int mon_procinfo(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	int verbosity = 0;

	if (argc < 2) {
		printk("Usage: procinfo OPTION\n");
		printk("\tall: show all active pids\n");
		printk("\tpid NUM: show a lot of info for proc NUM\n");
		printk("\tunlock: unlock the lock for the ADDR (OMG!!!)\n");
		printk("\tkill NUM: destroy proc NUM\n");
		return 1;
	}
	if (!strcmp(argv[1], "all")) {
		print_allpids();
	} else if (!strcmp(argv[1], "pid")) {
		if (argc < 3) {
			printk("Give me a pid number.\n");
			return 1;
		}
		if (argc >= 4)
			verbosity = strtol(argv[3], 0, 0);
		print_proc_info(strtol(argv[2], 0, 0), verbosity);
	} else if (!strcmp(argv[1], "unlock")) {
		if (argc != 3) {
			printk("Gimme lock address!  Me want lock address!.\n");
			return 1;
		}
		spinlock_t *lock = (spinlock_t*)strtol(argv[2], 0, 16);
		if (!lock) {
			printk("Null address...\n");
			return 1;
		}
		spin_unlock(lock);
	} else if (!strcmp(argv[1], "kill")) {
		if (argc != 3) {
			printk("Give me a pid number.\n");
			return 1;
		}
		struct proc *p = pid2proc(strtol(argv[2], 0, 0));
		if (!p) {
			printk("No such proc\n");
			return 1;
		}
		proc_destroy(p);
		proc_decref(p);
	} else {
		printk("Bad option\n");
		return 1;
	}
	return 0;
}

int mon_pip(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	int verbosity = 0;

	if (argc < 2) {
		printk("Give me a pid number.\n");
		return 1;
	}
	if (argc >= 3)
		verbosity = strtol(argv[2], 0, 0);
	print_proc_info(strtol(argv[1], 0, 0), verbosity);
	return 0;
}

int mon_kill(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	struct proc *p;

	if (argc < 2) {
		printk("Usage: kill PID\n");
		return 1;
	}
	p = pid2proc(strtol(argv[1], 0, 0));
	if (!p) {
		printk("No such proc\n");
		return 1;
	}
	p->exitcode = 1;	/* typical EXIT_FAILURE */
	proc_destroy(p);
	proc_decref(p);
	return 0;
}

int mon_exit(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	return -1;
}

int mon_kfunc(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	long ret;
	long (*func)(void *arg, ...);

	if (argc < 2) {
		printk("Usage: kfunc FUNCTION [arg1] [arg2] [etc]\n");
		printk("Use 0x with hex arguments.  Can take 6 args.\n");
		return 1;
	}
	func = (void*)get_symbol_addr(argv[1]);
	if (!func) {
		printk("Function not found.\n");
		return 1;
	}
	/* Not elegant, but whatever.  maybe there's a better syntax, or we can do
	 * it with asm magic. */
	switch (argc) {
	case 2: /* have to fake one arg */
		ret = func((void*)0);
		break;
	case 3: /* the real first arg */
		ret = func((void*)strtol(argv[2], 0, 0));
		break;
	case 4:
		ret = func((void*)strtol(argv[2], 0, 0),
		                  strtol(argv[3], 0, 0));
		break;
	case 5:
		ret = func((void*)strtol(argv[2], 0, 0),
		                  strtol(argv[3], 0, 0),
		                  strtol(argv[4], 0, 0));
		break;
	case 6:
		ret = func((void*)strtol(argv[2], 0, 0),
		                  strtol(argv[3], 0, 0),
		                  strtol(argv[4], 0, 0),
		                  strtol(argv[5], 0, 0));
		break;
	case 7:
		ret = func((void*)strtol(argv[2], 0, 0),
		                  strtol(argv[3], 0, 0),
		                  strtol(argv[4], 0, 0),
		                  strtol(argv[5], 0, 0),
		                  strtol(argv[6], 0, 0));
		break;
	case 8:
		ret = func((void*)strtol(argv[2], 0, 0),
		                  strtol(argv[3], 0, 0),
		                  strtol(argv[4], 0, 0),
		                  strtol(argv[5], 0, 0),
		                  strtol(argv[6], 0, 0),
		                  strtol(argv[7], 0, 0));
		break;
	default:
		printk("Bad number of arguments.\n");
		return -1;
	}
	printk("%s (might have) returned %p\n", argv[1], ret);
	return 0;
}

/* Sending a vcoreid forces an event and an IPI/notification */
int mon_notify(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	struct proc *p;
	uint32_t vcoreid;
	struct event_msg msg = {0};

	if (argc < 3) {
		printk("Usage: notify PID NUM [VCOREID]\n");
		return 1;
	}
	p = pid2proc(strtol(argv[1], 0, 0));
	if (!p) {
		printk("No such proc\n");
		return 1;
	}
	msg.ev_type = strtol(argv[2], 0, 0);
	if (argc == 4) {
		vcoreid = strtol(argv[3], 0, 0);
		/* This will go to the private mbox */
		post_vcore_event(p, &msg, vcoreid, EVENT_VCORE_PRIVATE);
		proc_notify(p, vcoreid);
	} else {
		/* o/w, try and do what they want */
		send_kernel_event(p, &msg, 0);
	}
	proc_decref(p);
	return 0;
}

/* Micro-benchmarky Measurements.  This is really fragile code that probably
 * won't work perfectly, esp as the kernel evolves. */
int mon_measure(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	uint64_t begin = 0, diff = 0;
	uint32_t end_refcnt = 0;

	if (argc < 2) {
		printk("Usage: measure OPTION\n");
		printk("\tkill PID : kill proc PID\n");
		printk("\tpreempt PID : preempt proc PID (no delay)\n");
		printk("\tpreempt PID [pcore] : preempt PID's pcore (no delay)\n");
		printk("\tpreempt-warn PID : warn-preempt proc PID (pending)\n");
		printk("\tpreempt-warn PID [pcore] : warn-preempt proc PID's pcore\n");
		printk("\tpreempt-raw PID : raw-preempt proc PID\n");
		printk("\tpreempt-raw PID [pcore] : raw-preempt proc PID's pcore\n");
		return 1;
	}
	if (!strcmp(argv[1], "kill")) {
		if (argc < 3) {
			printk("Give me a pid number.\n");
			return 1;
		}
		struct proc *p = pid2proc(strtol(argv[2], 0, 0));
		if (!p) {
			printk("No such proc\n");
			return 1;
		}
		begin = start_timing();
#ifdef CONFIG_APPSERVER
		printk("Warning: this will be inaccurate due to the appserver.\n");
		end_refcnt = kref_refcnt(&p->p_kref) - p->procinfo->num_vcores - 1;
#endif /* CONFIG_APPSERVER */
		proc_destroy(p);
		proc_decref(p);
#ifdef CONFIG_APPSERVER
		/* Won't be that accurate, since it's not actually going through the
		 * __proc_free() path. */
		spin_on(kref_refcnt(&p->p_kref) != end_refcnt);
#else
		/* this is a little ghetto. it's not fully free yet, but we are also
		 * slowing it down by messing with it, esp with the busy waiting on a
		 * hyperthreaded core. */
		spin_on(p->env_cr3);
#endif /* CONFIG_APPSERVER */
		/* No noticeable difference using stop_timing instead of read_tsc() */
		diff = stop_timing(begin);
	} else if (!strcmp(argv[1], "preempt")) {
		if (argc < 3) {
			printk("Give me a pid number.\n");
			return 1;
		}
		struct proc *p = pid2proc(strtol(argv[2], 0, 0));
		if (!p) {
			printk("No such proc\n");
			return 1;
		}
		if (argc == 4) { /* single core being preempted, warned but no delay */
			uint32_t pcoreid = strtol(argv[3], 0, 0);
			begin = start_timing();
			if (proc_preempt_core(p, pcoreid, 1000000)) {
				__sched_put_idle_core(p, pcoreid);
				/* done when unmapped (right before abandoning) */
				spin_on(p->procinfo->pcoremap[pcoreid].valid);
			} else {
				printk("Core %d was not mapped to proc\n", pcoreid);
			}
			diff = stop_timing(begin);
		} else { /* preempt all cores, warned but no delay */
			end_refcnt = kref_refcnt(&p->p_kref) - p->procinfo->num_vcores;
			begin = start_timing();
			proc_preempt_all(p, 1000000);
			/* a little ghetto, implies no one is using p */
			spin_on(kref_refcnt(&p->p_kref) != end_refcnt);
			diff = stop_timing(begin);
		}
		proc_decref(p);
	} else if (!strcmp(argv[1], "preempt-warn")) {
		if (argc < 3) {
			printk("Give me a pid number.\n");
			return 1;
		}
		struct proc *p = pid2proc(strtol(argv[2], 0, 0));
		if (!p) {
			printk("No such proc\n");
			return 1;
		}
		printk("Careful: if this hangs, then the process isn't responding.\n");
		if (argc == 4) { /* single core being preempted-warned */
			uint32_t pcoreid = strtol(argv[3], 0, 0);
			spin_lock(&p->proc_lock);
			uint32_t vcoreid = p->procinfo->pcoremap[pcoreid].vcoreid;
			if (!p->procinfo->pcoremap[pcoreid].valid) {
				printk("Pick a mapped pcore\n");
				spin_unlock(&p->proc_lock);
				return 1;
			}
			begin = start_timing();
			__proc_preempt_warn(p, vcoreid, 1000000); // 1 sec
			spin_unlock(&p->proc_lock);
			/* done when unmapped (right before abandoning) */
			spin_on(p->procinfo->pcoremap[pcoreid].valid);
			diff = stop_timing(begin);
		} else { /* preempt-warn all cores */
			printk("Warning, this won't work if they can't yield their "
			       "last vcore, will stop at 1!\n");
			spin_lock(&p->proc_lock);
			begin = start_timing();
			__proc_preempt_warnall(p, 1000000);
			spin_unlock(&p->proc_lock);
			/* target cores do the unmapping / changing of the num_vcores */
			spin_on(p->procinfo->num_vcores > 1);
			diff = stop_timing(begin);
		}
		proc_decref(p);
	} else if (!strcmp(argv[1], "preempt-raw")) {
		if (argc < 3) {
			printk("Give me a pid number.\n");
			return 1;
		}
		struct proc *p = pid2proc(strtol(argv[2], 0, 0));
		if (!p) {
			printk("No such proc\n");
			return 1;
		}
		if (argc == 4) { /* single core preempted, no warning or waiting */
			uint32_t pcoreid = strtol(argv[3], 0, 0);
			spin_lock(&p->proc_lock);
			if (!p->procinfo->pcoremap[pcoreid].valid) {
				printk("Pick a mapped pcore\n");
				spin_unlock(&p->proc_lock);
				return 1;
			}
			begin = start_timing();
			__proc_preempt_core(p, pcoreid);
			if (!p->procinfo->num_vcores)
				__proc_set_state(p, PROC_RUNNABLE_M);
			spin_unlock(&p->proc_lock);
			/* ghetto, since the ksched should be calling all of this */
			__sched_put_idle_core(p, pcoreid);
			/* done when unmapped (right before abandoning) */
			spin_on(p->procinfo->pcoremap[pcoreid].valid);
			diff = stop_timing(begin);
		} else { /* preempt all cores, no warning or waiting */
			spin_lock(&p->proc_lock);
			uint32_t pc_arr[p->procinfo->num_vcores];
			uint32_t num_revoked;
			end_refcnt = kref_refcnt(&p->p_kref) - p->procinfo->num_vcores;
			begin = start_timing();
			num_revoked = __proc_preempt_all(p, pc_arr);
			__proc_set_state(p, PROC_RUNNABLE_M);
			spin_unlock(&p->proc_lock);
			if (num_revoked)
				__sched_put_idle_cores(p, pc_arr, num_revoked);
			/* a little ghetto, implies no one else is using p */
			spin_on(kref_refcnt(&p->p_kref) != end_refcnt);
			diff = stop_timing(begin);
		}
		proc_decref(p);
	} else {
		printk("Bad option\n");
		return 1;
	}
	printk("[Tired Giraffe Accent] Took %llu usec (%llu nsec) to finish.\n",
	       tsc2usec(diff), tsc2nsec(diff));
	return 0;
}

static bool mon_verbose_trace = FALSE;
static DEFINE_PERCPU(bool, mon_nmi_trace);

static void emit_hwtf_backtrace(struct hw_trapframe *hw_tf)
{
	char *fn_name;

	if (mon_verbose_trace) {
		print_trapframe(hw_tf);
		backtrace_hwtf(hw_tf);
	}
	fn_name = get_fn_name(get_hwtf_pc(hw_tf));
	printk("Core %d is at %p (%s)\n", core_id(), get_hwtf_pc(hw_tf),
	       fn_name);
	kfree(fn_name);
}

static void emit_vmtf_backtrace(struct vm_trapframe *vm_tf)
{
	if (mon_verbose_trace)
		print_vmtrapframe(vm_tf);
	printk("Core %d is at %p\n", core_id(), get_vmtf_pc(vm_tf));
}

/* This is dangerous and could cause a deadlock, since it runs in NMI context.
 * It's only for monitor debugging, so YMMV.  We pass the type since the kernel
 * doesn't deal in contexts (yet) */
void emit_monitor_backtrace(int type, void *tf)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];

	if (!PERCPU_VAR(mon_nmi_trace))
		return;
	/* To prevent a spew of output during a lot of perf NMIs, we'll turn off the
	 * monitor output as soon as any NMI hits our core. */
	PERCPU_VAR(mon_nmi_trace) = FALSE;
	/* Temporarily disable deadlock detection when we print.  We could
	 * deadlock if we were printing when we NMIed. */
	pcpui->__lock_checking_enabled--;
	if (type == ROS_HW_CTX)
		emit_hwtf_backtrace((struct hw_trapframe*)tf);
	else
		emit_vmtf_backtrace((struct vm_trapframe*)tf);
	print_kmsgs(core_id());
	pcpui->__lock_checking_enabled++;
}


int mon_trace(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	int core;
	if (argc < 2) {
		printk("Usage: trace OPTION\n");
		printk("\tsyscall start [silent (0 or non-zero, NOT the word silent)] [pid]: starts tracing\n");
		printk("\tsyscall stop: stops tracing.\n");
		printk("\tcoretf COREID: prints PC, -1 for all cores, verbose => TF\n");
		printk("\tpcpui [type [coreid]]: runs pcpui trace ring handlers\n");
		printk("\tpcpui-reset [noclear]: resets/clears pcpui trace ring\n");
		printk("\tverbose: toggles verbosity, depends on trace command\n");
		return 1;
	}
	if (!strcmp(argv[1], "syscall")) {
		if (argc < 3) {
			printk("Need a start or stop.\n");
			return 1;
		}
		if (!strcmp(argv[2], "start")) {
			systrace_loud = TRUE;
		} else if (!strcmp(argv[2], "stop")) {
			systrace_loud = FALSE;
		} else {
			printk("Need a start or stop.\n");
			return 1;
		}
	} else if (!strcmp(argv[1], "coretf")) {
		if (argc != 3) {
			printk("Need a coreid, fool.\n");
			return 1;
		}
		core = strtol(argv[2], 0, 0);
		if (core < 0) {
			printk("Sending NMIs to all cores:\n");
			for (int i = 0; i < num_cores; i++) {
				_PERCPU_VAR(mon_nmi_trace, i) = TRUE;
				send_nmi(i);
				udelay(1000000);
			}
		} else {
			printk("Sending NMI core %d:\n", core);
			if (core >= num_cores) {
				printk("No such core!  Maybe it's in another cell...\n");
				return 1;
			}
			_PERCPU_VAR(mon_nmi_trace, core) = TRUE;
			send_nmi(core);
		}
		udelay(1000000);
	} else if (!strcmp(argv[1], "pcpui")) {
		int pcpui_type, pcpui_coreid;
		if (argc >= 3)
			pcpui_type = strtol(argv[2], 0, 0);
		else
			pcpui_type = 0;
		printk("\nRunning PCPUI Trace Ring handlers for type %d\n", pcpui_type);
		if (argc >= 4) {
			pcpui_coreid = strtol(argv[3], 0, 0);
			pcpui_tr_foreach(pcpui_coreid, pcpui_type);
		} else {
			pcpui_tr_foreach_all(pcpui_type);
		}
	} else if (!strcmp(argv[1], "pcpui-reset")) {
		if (argc >= 3) {
			printk("\nResetting all PCPUI Trace Rings\n");
			pcpui_tr_reset_all();
		} else {
			printk("\nResetting and clearing all PCPUI Trace Rings\n");
			pcpui_tr_reset_and_clear_all();
		}
	} else if (!strcmp(argv[1], "verbose")) {
		if (mon_verbose_trace) {
			printk("Turning trace verbosity off\n");
			mon_verbose_trace = FALSE;
		} else {
			printk("Turning trace verbosity on\n");
			mon_verbose_trace = TRUE;
		}
	} else if (!strcmp(argv[1], "opt2")) {
		if (argc != 3) {
			printk("ERRRRRRRRRR.\n");
			return 1;
		}
		print_proc_info(strtol(argv[2], 0, 0), 0);
	} else {
		printk("Bad option\n");
		return 1;
	}
	return 0;
}

int mon_monitor(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	if (argc < 2) {
		printk("Usage: monitor COREID\n");
		return 1;
	}
	uint32_t core = strtol(argv[1], 0, 0);
	if (core >= num_cores) {
		printk("No such core!  Maybe it's in another cell...\n");
		return 1;
	}
	send_kernel_message(core, __run_mon, 0, 0, 0, KMSG_ROUTINE);
	return 0;
}

/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16


int onecmd(int argc, char *argv[], struct hw_trapframe *hw_tf) {
	int i;
	if (!argc)
		return -1;
	for (i = 0; i < NCOMMANDS; i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, hw_tf);
	}
	return -1;
}

void __run_mon(uint32_t srcid, long a0, long a1, long a2)
{
	monitor(0);
}

static int runcmd(char *real_buf, struct hw_trapframe *hw_tf) {
	char * buf = real_buf;
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	/* Discard initial 'm ', which is a common mistake when using 'm' a lot */
	if ((buf[0] == 'm') && (buf[1] == ' '))
		buf += 2;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		//This will get fucked at runtime..... in the ASS
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < NCOMMANDS; i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, hw_tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void monitor(struct hw_trapframe *hw_tf)
{
	#define MON_CMD_LENGTH 256
	char buf[MON_CMD_LENGTH];
	int cnt;
	int coreid = core_id_early();

	/* they are always disabled, since we have this irqsave lock */
	if (irq_is_enabled())
		printk("Entering Nanwan's Dungeon on Core %d (Ints on):\n", coreid);
	else
		printk("Entering Nanwan's Dungeon on Core %d (Ints off):\n", coreid);
	printk("Type 'help' for a list of commands.\n");

	if (hw_tf != NULL)
		print_trapframe(hw_tf);

	while (1) {
		/* on occasion, the kernel monitor can migrate (like if you run
		 * something that blocks / syncs and wakes up on another core) */
		cmb();
		cnt = readline(buf, MON_CMD_LENGTH, "ROS(Core %d)> ", core_id_early());
		if (cnt > 0) {
			buf[cnt] = 0;
			if (runcmd(buf, hw_tf) < 0)
				break;
		}
	}
}

static void pm_flusher(void *unused)
{
	struct super_block *sb;
	struct inode *inode;
	unsigned long nr_pages;

	/* could also put the delay between calls, or even within remove, during the
	 * WB phase. */
	printk("GIANT WARNING: the pm_flusher is running and will never stop!\n");
	while (1) {
		kthread_usleep(5000);
		TAILQ_FOREACH(sb, &super_blocks, s_list) {
			TAILQ_FOREACH(inode, &sb->s_inodes, i_sb_list) {
				nr_pages = ROUNDUP(inode->i_size, PGSIZE) >> PGSHIFT;
				if (nr_pages)
					pm_remove_contig(inode->i_mapping, 0, nr_pages);
			}
		}
	}
}

int mon_fs(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	/* this assumes one mounted FS at the NS root */
	struct super_block *sb;
	struct file *file;
	struct inode *inode;
	struct dentry *dentry;
	if (argc < 2) {
		printk("Usage: fs OPTION\n");
		printk("\topen: show all open files\n");
		printk("\tinodes: show all inodes\n");
		printk("\tdentries [lru|prune]: show all dentries, opt LRU/prune\n");
		printk("\tls DIR: print the dir tree starting with DIR\n");
		printk("\tpid: proc PID's fs crap placeholder\n");
		printk("\tpmflusher: start a ktask to keep flushing all PMs\n");
		return 1;
	}
	if (!strcmp(argv[1], "open")) {
		printk("Open Files:\n----------------------------\n");
		TAILQ_FOREACH(sb, &super_blocks, s_list) {
			printk("Superblock for %s\n", sb->s_name);
			TAILQ_FOREACH(file, &sb->s_files, f_list)
				printk("File: %p, %s, Refs: %d, Drefs: %d, Irefs: %d PM: %p\n",
				       file, file_name(file), kref_refcnt(&file->f_kref),
				       kref_refcnt(&file->f_dentry->d_kref),
				       kref_refcnt(&file->f_dentry->d_inode->i_kref),
					   file->f_mapping);
		}
	} else if (!strcmp(argv[1], "inodes")) {
		printk("Mounted FS Inodes:\n----------------------------\n");
		TAILQ_FOREACH(sb, &super_blocks, s_list) {
			printk("Superblock for %s\n", sb->s_name);
			TAILQ_FOREACH(inode, &sb->s_inodes, i_sb_list) {
				printk("Inode: %p, Refs: %d, Nlinks: %d, Size(B): %d\n",
				       inode, kref_refcnt(&inode->i_kref), inode->i_nlink,
				       inode->i_size);
				TAILQ_FOREACH(dentry, &inode->i_dentry, d_alias)
					printk("\t%s: Dentry: %p, Refs: %d\n",
					       dentry->d_name.name, dentry,
					       kref_refcnt(&dentry->d_kref));
			}
		}
	} else if (!strcmp(argv[1], "dentries")) {
		printk("Dentry Cache:\n----------------------------\n");
		TAILQ_FOREACH(sb, &super_blocks, s_list) {
			printk("Superblock for %s\n", sb->s_name);
			printk("DENTRY     FLAGS      REFCNT NAME\n");
			printk("--------------------------------\n");
			/* Hash helper */
			void print_dcache_entry(void *item, void *opaque)
			{
				struct dentry *d_i = (struct dentry*)item;
				printk("%p %p %02d     %s\n", d_i, d_i->d_flags,
				       kref_refcnt(&d_i->d_kref), d_i->d_name.name);
			}
			hash_for_each(sb->s_dcache, print_dcache_entry, NULL);
		}
		if (argc < 3)
			return 0;
		if (!strcmp(argv[2], "lru")) {
			printk("LRU lists:\n");
			TAILQ_FOREACH(sb, &super_blocks, s_list) {
				printk("Superblock for %s\n", sb->s_name);
				TAILQ_FOREACH(dentry, &sb->s_lru_d, d_lru)
					printk("Dentry: %p, Name: %s\n", dentry,
					       dentry->d_name.name);
			}
		} else if (!strcmp(argv[2], "prune")) {
			printk("Pruning unused dentries\n");
			TAILQ_FOREACH(sb, &super_blocks, s_list)
				dcache_prune(sb, FALSE);
		}
	} else if (!strcmp(argv[1], "ls")) {
		if (argc != 3) {
			printk("Give me a dir.\n");
			return 1;
		}
		if (argv[2][0] != '/') {
			printk("Dear fellow giraffe lover, Use absolute paths.\n");
			return 1;
		}
		ls_dash_r(argv[2]);
		/* whatever.  placeholder. */
	} else if (!strcmp(argv[1], "pid")) {
		if (argc != 3) {
			printk("Give me a pid number.\n");
			return 1;
		}
		/* whatever.  placeholder. */
	} else if (!strcmp(argv[1], "pmflusher")) {
		ktask("pm_flusher", pm_flusher, 0);
	} else {
		printk("Bad option\n");
		return 1;
	}
	return 0;
}

int mon_shell(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	char *l_argv[2] = {"/bin/bash", "bash"};
	return mon_bin_run(2, l_argv, hw_tf);
}

int mon_alarm(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	if (argc < 2) {
		printk("Usage: alarm OPTION\n");
		printk("\tpcpu: print full alarm tchains from every core\n");
		return 1;
	}
	if (!strcmp(argv[1], "pcpu")) {
		print_pcpu_chains();
	} else {
		printk("Bad option\n");
		return 1;
	}
	return 0;
}

static void show_msr(struct hw_trapframe *unused, void *v)
{
	int core = core_id();
	uint64_t val;
	uint32_t msr = *(uint32_t *)v;
	val = read_msr(msr);
	printk("%d: %08x: %016llx\n", core, msr, val);
}

struct set {
	uint32_t msr;
	uint64_t val;
};

static void set_msr(struct hw_trapframe *unused, void *v)
{
	int core = core_id();
	struct set *s = v;
	uint32_t msr = s->msr;
	uint64_t val = s->val;
	write_msr(msr, val);
	val = read_msr(msr);
	printk("%d: %08x: %016llx\n", core, msr, val);
}

int mon_msr(int argc, char **argv, struct hw_trapframe *hw_tf)
{
#ifndef CONFIG_X86
	cprintf("Not on this architecture\n");
	return 1;
#else
	uint64_t val;
	uint32_t msr;
	if (argc < 2 || argc > 3) {
		printk("Usage: msr register [value]\n");
		return 1;
	}
	msr = strtoul(argv[1], 0, 16);
	handler_wrapper_t *w;
	smp_call_function_all(show_msr, &msr, &w);
	smp_call_wait(w);

	if (argc < 3)
		return 0;
	/* somewhat bogus on 32 bit. */
	val = strtoul(argv[2], 0, 16);

	struct set set;
	set.msr = msr;
	set.val = val;
	smp_call_function_all(set_msr, &set, &w);
	smp_call_wait(w);
	return 0;
#endif
}

int mon_db(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	pid_t pid = -1;

	if (argc < 2) {
		printk("Usage: db OPTION\n");
		printk("\tsem [PID]: print all semaphore info\n");
		printk("\taddr PID 0xADDR: for PID lookup ADDR's file/vmr info\n");
		return 1;
	}
	if (!strcmp(argv[1], "sem")) {
		if (argc > 2)
			pid = strtol(argv[2], 0, 0);
		print_all_sem_info(pid);
	} else if (!strcmp(argv[1], "addr")) {
		if (argc < 4) {
			printk("Usage: db addr PID 0xADDR\n");
			return 1;
		}
		debug_addr_pid(strtol(argv[2], 0, 10), strtol(argv[3], 0, 16));
	} else {
		printk("Bad option\n");
		return 1;
	}
	return 0;
}

int mon_px(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	pid_t pid = 0;
	struct proc *p;

	if (argc == 2)
		pid = strtol(argv[1], 0, 0);
	if (!pid) {
		set_printx(2);
		printk("Printxing is now %sabled\n", printx_on ? "en" : "dis");
		return 0;
	}
	p = pid2proc(pid);
	if (!p) {
		printk("No proc with pid %d\n", pid);
		return 1;
	}
	p->procdata->printx_on = !p->procdata->printx_on;
	proc_decref(p);
	return 0;
}

/* Super hack.  Given a kernel hw_tf, we hack the RIP to smp_idle, then return
 * to it.  Any locks or other stuff being done is completely lost, so you could
 * deadlock.  This gets out of the "we're totall screwed, but don't want to
 * reboot right now", typically caused by screw-ups from the monitor. */
int mon_kpfret(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];

	/* if monitor had a TF, try to use that */
	if (!hw_tf) {
		if (argc < 2) {
			printk("Usage: kpfret HW_TF\n");
			return 1;
		}
		/* the hw_tf passed in is the one we got from monitor, which is 0 from
		 * panics. */
		hw_tf = (struct hw_trapframe*)strtol(argv[1], 0, 16);
	}

	if (!in_kernel(hw_tf)) {
		printk("hw_tf %p was not a kernel tf!\n", hw_tf);
		return -1;
	}

#ifdef CONFIG_X86
	hw_tf->tf_rip = (uintptr_t)smp_idle;
	dec_ktrap_depth(pcpui);

	asm volatile("mov %0, %%rsp;"
	             "addq $0x10, %%rsp;"
	             "popq %%rax;"
	             "popq %%rbx;"
	             "popq %%rcx;"
	             "popq %%rdx;"
	             "popq %%rbp;"
	             "popq %%rsi;"
	             "popq %%rdi;"
	             "popq %%r8;"
	             "popq %%r9;"
	             "popq %%r10;"
	             "popq %%r11;"
	             "popq %%r12;"
	             "popq %%r13;"
	             "popq %%r14;"
	             "popq %%r15;"
	             "addq $0x10, %%rsp;"
	             "iretq;"
				 : : "r"(hw_tf));
	assert(0);
#else
	printk("KPF return not supported\n");
	return -1;
#endif /* CONFIG_X86 */
}

int mon_ks(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	if (argc < 2) {
usage:
		printk("Usage: ks OPTION\n");
		printk("\tidles: show idle core map\n");
		printk("\tdiag: scheduler diagnostic report\n");
		printk("\tresources: show resources wanted/granted for all procs\n");
		printk("\tsort: sorts the idlecoremap, 1..n\n");
		printk("\tnc PCOREID: sets the next CG core allocated\n");
		return 1;
	}
	if (!strcmp(argv[1], "idles")) {
		print_idle_core_map();
	} else if (!strcmp(argv[1], "diag")) {
		sched_diag();
	} else if (!strcmp(argv[1], "resources")) {
		print_all_resources();
	} else if (!strcmp(argv[1], "sort")) {
		sort_idle_cores();
	} else if (!strcmp(argv[1], "nc")) {
		if (argc != 3) {
			printk("Need a pcore number.\n");
			return 1;
		}
		next_core_to_alloc(strtol(argv[2], 0, 0));
	} else {
		printk("Bad option %s\n", argv[1]);
		goto usage;
	}
	return 0;
}

/* Prints info about a core.  Optional first arg == coreid. */
int mon_coreinfo(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	struct per_cpu_info *pcpui;
	struct kthread *kth;
	int coreid = core_id();

	if (argc >= 2)
		coreid = strtol(argv[1], 0, 0);
	pcpui = &per_cpu_info[coreid];
	printk("Core %d:\n\tcur_proc %d\n\towning proc %d, owning vc %d\n",
	       coreid, pcpui->cur_proc ? pcpui->cur_proc->pid : 0,
	       pcpui->owning_proc ? pcpui->owning_proc->pid : 0,
	       pcpui->owning_vcoreid != 0xdeadbeef ? pcpui->owning_vcoreid : 0);
	kth = pcpui->cur_kthread;
	if (kth) {
		/* kth->proc is only used when the kthread is sleeping.  when it's
		 * running, we care about cur_proc.  if we're here, proc should be 0
		 * unless the kth is concurrently sleeping (we called this remotely) */
		printk("\tkthread %p (%s), sysc %p (%d)\n", kth, kth->name,
		       kth->sysc, kth->sysc ? kth->sysc->num : -1);
	} else {
		/* Can happen during early boot */
		printk("\tNo kthread!\n");
	}
	return 0;
}

int mon_hexdump(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	struct proc *p = NULL;
	uintptr_t switch_state;
	pid_t pid;
	uintptr_t start;
	size_t len;

	assert(argc >= 1);
	if (argc < 4) {
		printk("Usage: %s PID ADDR LEN\n", argv[0]);
		printk("    PID == 0 for kernel / don't care\n");
		return 1;
	}
	pid = strtol(argv[1], 0, 0);
	start = strtoul(argv[2], 0, 0);
	len = strtoul(argv[3], 0, 0);
	if (pid) {
		p = pid2proc(pid);
		if (!p) {
			printk("No proc with pid %d\n", pid);
			return 1;
		}
		switch_state = switch_to(p);
	}
	hexdump((void*)start, len);
	if (p) {
		switch_back(p, switch_state);
		proc_decref(p);
	}
	return 0;
}

int mon_pahexdump(int argc, char **argv, struct hw_trapframe *hw_tf)
{
	uintptr_t start;
	size_t len;

	assert(argc >= 1);
	if (argc < 3) {
		printk("Usage: %s PHYS_ADDR LEN\n", argv[0]);
		return 1;
	}
	start = strtoul(argv[1], 0, 0);
	len = strtoul(argv[2], 0, 0);
	pahexdump(start, len);
	return 0;
}
