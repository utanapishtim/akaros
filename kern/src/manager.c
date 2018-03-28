/*
 * Copyright (c) 2009 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 */


#include <ros/common.h>
#include <smp.h>
#include <arch/init.h>
#include <mm.h>
#include <elf.h>

#include <kmalloc.h>
#include <assert.h>
#include <manager.h>
#include <process.h>
#include <schedule.h>
#include <syscall.h>
#include <ktest.h>
#include <stdio.h>
#include <time.h>
#include <monitor.h>
#include <string.h>
#include <pmap.h>
#include <arch/console.h>
#include <time.h>
#include <ros/arch/membar.h>

/*
 * Currently, if you leave this function by way of proc_run (process_workqueue
 * that proc_runs), you will never come back to where you left off, and the
 * function will start from the top.  Hence the hack 'progress'.
 */
void manager(void)
{
	// LoL
	#define PASTE(s1,s2) s1 ## s2
	#define MANAGER_FUNC(dev) PASTE(manager_,dev)

	#if !defined(DEVELOPER_NAME) && \
	    (defined(CONFIG_KERNEL_TESTING) || \
	     defined(CONFIG_USERSPACE_TESTING))
		#define DEVELOPER_NAME jenkins
	#endif

	#ifndef DEVELOPER_NAME
		#define DEVELOPER_NAME brho
	#endif

	void MANAGER_FUNC(DEVELOPER_NAME)(void);
	MANAGER_FUNC(DEVELOPER_NAME)();
}

char *p_argv[] = {0, 0, 0};
/* Helper macro for quickly running a process.  Pass it a string, *file, and a
 * *proc. */
#define quick_proc_run(x, p, f)                                                  \
	(f) = do_file_open((x), O_READ, 0);                                          \
	assert((f));                                                                 \
	p_argv[0] = file_name((f));                                                  \
	(p) = proc_create((f), p_argv, NULL);                                        \
	kref_put(&(f)->f_kref);                                                      \
	spin_lock(&(p)->proc_lock);                                                  \
	__proc_set_state((p), PROC_RUNNABLE_S);                                      \
	spin_unlock(&(p)->proc_lock);                                                \
	proc_run_s((p));                                                             \
	proc_decref((p));

#define quick_proc_create(x, p, f)                                               \
	(f) = do_file_open((x), O_READ, 0);                                          \
	assert((f));                                                                 \
	p_argv[0] = file_name((f));                                                  \
	(p) = proc_create((f), p_argv, NULL);                                        \
	kref_put(&(f)->f_kref);                                                      \
	spin_lock(&(p)->proc_lock);                                                  \
	__proc_set_state((p), PROC_RUNNABLE_S);                                      \
	spin_unlock(&(p)->proc_lock);

#define quick_proc_color_run(x, p, c, f)                                         \
	(f) = do_file_open((x), O_READ, 0);                                          \
	assert((f));                                                                 \
	p_argv[0] = file_name((f));                                                  \
	(p) = proc_create((f), p_argv, NULL);                                        \
	kref_put(&(f)->f_kref);                                                      \
	spin_lock(&(p)->proc_lock);                                                  \
	__proc_set_state((p), PROC_RUNNABLE_S);                                      \
	spin_unlock(&(p)->proc_lock);                                                \
	p->cache_colors_map = cache_colors_map_alloc();                              \
	for (int i = 0; i < (c); i++)                                                \
		cache_color_alloc(llc_cache, p->cache_colors_map);                       \
	proc_run_s((p));                                                             \
	proc_decref((p));

#define quick_proc_color_create(x, p, c, f)                                      \
	(f) = do_file_open((x), O_READ, 0);                                          \
	assert((f));                                                                 \
	p_argv[0] = file_name((f));                                                  \
	(p) = proc_create((f), p_argv, NULL);                                        \
	kref_put(&(f)->f_kref);                                                      \
	spin_lock(&(p)->proc_lock);                                                  \
	__proc_set_state((p), PROC_RUNNABLE_S);                                      \
	spin_unlock(&(p)->proc_lock);                                                \
	p->cache_colors_map = cache_colors_map_alloc();                              \
	for (int i = 0; i < (c); i++)                                                \
		cache_color_alloc(llc_cache, p->cache_colors_map);

void manager_brho(void)
{
	static bool first = TRUE;
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];

	if (first) {
		printk("*** IRQs must be enabled for input emergency codes ***\n");
		#ifdef CONFIG_X86
		printk("*** Hit ctrl-g to enter the monitor. ***\n");
		printk("*** Hit ctrl-q to force-enter the monitor. ***\n");
		printk("*** Hit ctrl-b for a backtrace of core 0 ***\n");
		#else
		printk("*** Hit ctrl-g to enter the monitor. ***\n");
		#warning "***** ctrl-g untested on riscv, check k/a/r/trap.c *****"
		#endif
		first = FALSE;
	}
	/* just idle, and deal with things via interrupts.  or via face. */
	smp_idle();
	/* whatever we do in the manager, keep in mind that we need to not do
	 * anything too soon (like make processes), since we'll drop in here during
	 * boot if the boot sequence required any I/O (like EXT2), and we need to
	 * PRKM() */
	assert(0);

#if 0 /* ancient tests below: (keeping around til we ditch the manager) */
	// for testing taking cores, check in case 1 for usage
	uint32_t corelist[MAX_NUM_CORES];
	uint32_t num = 3;
	struct file *temp_f;
	static struct proc *p;

	static uint8_t RACY progress = 0;	/* this will wrap around. */
	switch (progress++) {
		case 0:
			printk("Top of the manager to ya!\n");
			/* 124 is half of the available boxboro colors (with the kernel
			 * getting 8) */
			//quick_proc_color_run("msr_dumb_while", p, 124, temp_f);
			quick_proc_run("/bin/hello", p, temp_f);
			#if 0
			// this is how you can transition to a parallel process manually
			// make sure you don't proc run first
			__proc_set_state(p, PROC_RUNNING_S);
			__proc_set_state(p, PROC_RUNNABLE_M);
			p->resources[RES_CORES].amt_wanted = 5;
			spin_unlock(&p->proc_lock);
			core_request(p);
			panic("This is okay");
			#endif
			break;
		case 1:
			#if 0
			udelay(10000000);
			// this is a ghetto way to test restarting an _M
				printk("\nattempting to ghetto preempt...\n");
				spin_lock(&p->proc_lock);
				proc_take_allcores(p, __death);
				__proc_set_state(p, PROC_RUNNABLE_M);
				spin_unlock(&p->proc_lock);
				udelay(5000000);
				printk("\nattempting to restart...\n");
				core_request(p); // proc still wants the cores
			panic("This is okay");
			// this tests taking some cores, and later killing an _M
				printk("taking 3 cores from p\n");
				for (int i = 0; i < num; i++)
					corelist[i] = 7-i; // 7, 6, and 5
				spin_lock(&p->proc_lock);
				proc_take_cores(p, corelist, &num, __death);
				spin_unlock(&p->proc_lock);
				udelay(5000000);
				printk("Killing p\n");
				proc_destroy(p);
				printk("Killed p\n");
			panic("This is okay");

			envs[0] = kfs_proc_create(kfs_lookup_path("roslib_hello"));
			__proc_set_state(envs[0], PROC_RUNNABLE_S);
			proc_run(envs[0]);
			warn("DEPRECATED");
			break;
			#endif
		case 2:
			/*
			test_smp_call_functions();
			test_checklists();
			test_barrier();
			test_print_info();
			test_lapic_status_bit();
			test_ipi_sending();
			test_pit();
			*/
		default:
			printd("Manager Progress: %d\n", progress);
			// delay if you want to test rescheduling an MCP that yielded
			//udelay(15000000);
			run_scheduler();
	}
	panic("If you see me, then you probably screwed up");
	monitor(0);

	/*
	printk("Servicing syscalls from Core 0:\n\n");
	while (1) {
		process_generic_syscalls(&envs[0], 1);
		cpu_relax();
	}
	*/
	return;
#endif
}

void manager_jenkins()
{
	#ifdef CONFIG_KERNEL_TESTING
		printk("<-- BEGIN_KERNEL_TESTS -->\n");
		run_registered_ktest_suites();
		printk("<-- END_KERNEL_TESTS -->\n");
	#endif

	// Run userspace tests (from config specified path).
	#ifdef CONFIG_USERSPACE_TESTING
	if (strlen(CONFIG_USERSPACE_TESTING_SCRIPT) != 0) {
		char exec[] = "/bin/ash";
		char *p_argv[] = {exec, CONFIG_USERSPACE_TESTING_SCRIPT, 0};

		struct file *program = do_file_open(exec, O_READ, 0);
		struct proc *p = proc_create(program, p_argv, NULL);
		proc_wakeup(p);
		proc_decref(p); /* let go of the reference created in proc_create() */
		kref_put(&program->f_kref);
		run_scheduler();
	    // Need a way to wait for p to finish
	} else {
		printk("No user-space launcher file specified.\n");
	}
	#endif
	smp_idle();
	assert(0);
}

void manager_klueska()
{
	static struct proc *envs[256];
	static volatile uint8_t progress = 0;

	if (progress == 0) {
		progress++;
		panic("what do you want to do?");
		//envs[0] = kfs_proc_create(kfs_lookup_path("fillmeup"));
		__proc_set_state(envs[0], PROC_RUNNABLE_S);
		proc_run_s(envs[0]);
		warn("DEPRECATED");
	}
	run_scheduler();

	panic("DON'T PANIC");
}

void manager_waterman()
{
	static bool first = true;
	if (first)
		mon_shell(0, 0, 0);
	smp_idle();
	assert(0);
}

void manager_yuzhu()
{

	static uint8_t progress = 0;
	static struct proc *p;

	// for testing taking cores, check in case 1 for usage
	uint32_t corelist[MAX_NUM_CORES];
	uint32_t num = 3;

	//create_server(init_num_cores, loop);

	monitor(0);

	// quick_proc_run("hello", p);

}
