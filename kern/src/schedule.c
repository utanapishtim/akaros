/* Copyright (c) 2009, 2012 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * Scheduling and dispatching. */

#ifdef __SHARC__
#pragma nosharc
#endif

#include <schedule.h>
#include <process.h>
#include <monitor.h>
#include <stdio.h>
#include <assert.h>
#include <atomic.h>
#include <smp.h>
#include <manager.h>
#include <alarm.h>
#include <sys/queue.h>

/* Process Lists */
struct proc_list runnable_scps = TAILQ_HEAD_INITIALIZER(runnable_scps);
struct proc_list all_mcps = TAILQ_HEAD_INITIALIZER(all_mcps);
spinlock_t sched_lock = SPINLOCK_INITIALIZER;

// This could be useful for making scheduling decisions.  
/* Physical coremap: each index is a physical core id, with a proc ptr for
 * whoever *should be or is* running.  Very similar to current, which is what
 * process is *really* running there. */
struct proc *pcoremap[MAX_NUM_CPUS];

/* Tracks which cores are idle, similar to the vcoremap.  Each value is the
 * physical coreid of an unallocated core. */
spinlock_t idle_lock = SPINLOCK_INITIALIZER;
uint32_t idlecoremap[MAX_NUM_CPUS];
uint32_t num_idlecores = 0;
uint32_t num_mgmtcores = 1;

/* Helper, defined below */
static void __core_request(struct proc *p);

/* Alarm struct, for our example 'timer tick' */
struct alarm_waiter ksched_waiter;

#define TIMER_TICK_USEC 10000 	/* 10msec */

/* Helper: Sets up a timer tick on the calling core to go off 10 msec from now.
 * This assumes the calling core is an LL core, etc. */
static void set_ksched_alarm(void)
{
	set_awaiter_rel(&ksched_waiter, TIMER_TICK_USEC);
	set_alarm(&per_cpu_info[core_id()].tchain, &ksched_waiter);
}

/* Kmsg, to run the scheduler tick (not in interrupt context) and reset the
 * alarm.  Note that interrupts will be disabled, but this is not the same as
 * interrupt context.  We're a routine kmsg, which means the core is in a
 * quiescent state. */
static void __ksched_tick(struct trapframe *tf, uint32_t srcid, long a0,
                          long a1, long a2)
{
	/* TODO: imagine doing some accounting here */
	schedule();
	/* Set our alarm to go off, incrementing from our last tick (instead of
	 * setting it relative to now, since some time has passed since the alarm
	 * first went off.  Note, this may be now or in the past! */
	set_awaiter_inc(&ksched_waiter, TIMER_TICK_USEC);
	set_alarm(&per_cpu_info[core_id()].tchain, &ksched_waiter);
}

/* Interrupt/alarm handler: tells our core to run the scheduler (out of
 * interrupt context). */
static void __kalarm(struct alarm_waiter *waiter)
{
	send_kernel_message(core_id(), __ksched_tick, 0, 0, 0, KMSG_ROUTINE);
}

void schedule_init(void)
{
	TAILQ_INIT(&runnable_scps);
	TAILQ_INIT(&all_mcps);
	assert(!core_id());		/* want the alarm on core0 for now */
	init_awaiter(&ksched_waiter, __kalarm);
	set_ksched_alarm();

	/* Ghetto old idle core init */
	/* Init idle cores. Core 0 is the management core. */
	spin_lock(&idle_lock);
#ifdef __CONFIG_DISABLE_SMT__
	/* assumes core0 is the only management core (NIC and monitor functionality
	 * are run there too.  it just adds the odd cores to the idlecoremap */
	assert(!(num_cpus % 2));
	// TODO: consider checking x86 for machines that actually hyperthread
	num_idlecores = num_cpus >> 1;
 #ifdef __CONFIG_ARSC_SERVER__
	// Dedicate one core (core 2) to sysserver, might be able to share wit NIC
	num_mgmtcores++;
	assert(num_cpus >= num_mgmtcores);
	send_kernel_message(2, (amr_t)arsc_server, 0,0,0, KMSG_ROUTINE);
 #endif
	for (int i = 0; i < num_idlecores; i++)
		idlecoremap[i] = (i * 2) + 1;
#else
	// __CONFIG_DISABLE_SMT__
	#ifdef __CONFIG_NETWORKING__
	num_mgmtcores++; // Next core is dedicated to the NIC
	assert(num_cpus >= num_mgmtcores);
	#endif
	#ifdef __CONFIG_APPSERVER__
	#ifdef __CONFIG_DEDICATED_MONITOR__
	num_mgmtcores++; // Next core dedicated to running the kernel monitor
	assert(num_cpus >= num_mgmtcores);
	// Need to subtract 1 from the num_mgmtcores # to get the cores index
	send_kernel_message(num_mgmtcores-1, (amr_t)monitor, 0,0,0, KMSG_ROUTINE);
	#endif
	#endif
 #ifdef __CONFIG_ARSC_SERVER__
	// Dedicate one core (core 2) to sysserver, might be able to share with NIC
	num_mgmtcores++;
	assert(num_cpus >= num_mgmtcores);
	send_kernel_message(num_mgmtcores-1, (amr_t)arsc_server, 0,0,0, KMSG_ROUTINE);
 #endif
	num_idlecores = num_cpus - num_mgmtcores;
	for (int i = 0; i < num_idlecores; i++)
		idlecoremap[i] = i + num_mgmtcores;
#endif /* __CONFIG_DISABLE_SMT__ */
	spin_unlock(&idle_lock);
	return;
}

/* _S procs are scheduled like in traditional systems */
void schedule_scp(struct proc *p)
{
	/* up the refcnt since we are storing the reference */
	proc_incref(p, 1);
	spin_lock(&sched_lock);
	printd("Scheduling PID: %d\n", p->pid);
	TAILQ_INSERT_TAIL(&runnable_scps, p, proc_link);
	spin_unlock(&sched_lock);
}

/* important to only call this on RUNNING_S, for now */
void register_mcp(struct proc *p)
{
	proc_incref(p, 1);
	spin_lock(&sched_lock);
	TAILQ_INSERT_TAIL(&all_mcps, p, proc_link);
	spin_unlock(&sched_lock);
	//poke_ksched(p, RES_CORES);
}

/* Something has changed, and for whatever reason the scheduler should
 * reevaluate things. 
 *
 * Don't call this from interrupt context (grabs proclocks). */
void schedule(void)
{
	struct proc *p, *temp;
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	spin_lock(&sched_lock);
	/* trivially try to handle the needs of all our MCPS.  smarter schedulers
	 * would do something other than FCFS */
	TAILQ_FOREACH_SAFE(p, &all_mcps, proc_link, temp) {
		printd("Ksched has MCP %08p (%d)\n", p, p->pid);
		/* If they are dying, abort.  There's a bit of a race here.  If they
		 * start dying right after the check, core_request/give_cores would
		 * start dealing with a DYING proc.  The code can handle it, but this
		 * will probably change. */
		if (p->state == PROC_DYING) {
			TAILQ_REMOVE(&all_mcps, p, proc_link);
			proc_decref(p);
			continue;
		}
		if (!num_idlecores)
			break;
		/* TODO: might use amt_wanted as a proxy.  right now, they have
		 * amt_wanted == 1, even though they are waiting.
		 * TODO: this is RACY too - just like with DYING. */
		if (p->state == PROC_WAITING)
			continue;
		__core_request(p);
	}
	/* prune any dying SCPs at the head of the queue and maybe sched our core */
	while ((p = TAILQ_FIRST(&runnable_scps))) {
		if (p->state == PROC_DYING) {
			TAILQ_REMOVE(&runnable_scps, p, proc_link);
			proc_decref(p);
		} else {
			/* check our core to see if we can give it out to an SCP */
			if (management_core() && (!pcpui->owning_proc)) {
				TAILQ_REMOVE(&runnable_scps, p, proc_link);
				printd("PID of the SCP i'm running: %d\n", p->pid);
				proc_run_s(p);	/* gives it core we're running on */
				proc_decref(p);
			}
			break;
		}
	}
	spin_unlock(&sched_lock);
}

/* A process is asking the ksched to look at its resource desires.  The
 * scheduler is free to ignore this, for its own reasons, so long as it
 * eventually gets around to looking at resource desires. */
void poke_ksched(struct proc *p, int res_type)
{
	/* TODO: probably want something to trigger all res_types */
	spin_lock(&sched_lock);
	switch (res_type) {
		case RES_CORES:
			/* ignore core requests from non-mcps (note we have races if we ever
			 * allow procs to switch back). */
			if (!__proc_is_mcp(p))
				break;
			__core_request(p);
			break;
		default:
			break;
	}
	spin_unlock(&sched_lock);
}

/* The calling cpu/core has nothing to do and plans to idle/halt.  This is an
 * opportunity to pick the nature of that halting (low power state, etc), or
 * provide some other work (_Ss on LL cores).  Note that interrupts are
 * disabled, and if you return, the core will cpu_halt(). */
void cpu_bored(void)
{
	if (!management_core())
		return;
	/* TODO: something smart.  For now, do what smp_idle did */
	manager();
	assert(0);
	/* TODO run a process, and if none exist at all and we're core 0, bring up
	 * the monitor/manager */
}

/* Helper function to return a core to the idlemap.  It causes some more lock
 * acquisitions (like in a for loop), but it's a little easier.  Plus, one day
 * we might be able to do this without locks (for the putting).
 *
 * TODO: this is a trigger, telling us we have more cores.  Could/should make a
 * scheduling decision (or at least plan to).  Note that right now, the proc's
 * lock is still being held, so we can't do anything in this context. */
void put_idle_core(uint32_t coreid)
{
	spin_lock(&idle_lock);
	idlecoremap[num_idlecores++] = coreid;
	spin_unlock(&idle_lock);
}

/* Bulk interface for put_idle */
void put_idle_cores(uint32_t *pc_arr, uint32_t num)
{
	spin_lock(&idle_lock);
	for (int i = 0; i < num; i++)
		idlecoremap[num_idlecores++] = pc_arr[i];
	spin_unlock(&idle_lock);
}

/* Available resources changed (plus or minus).  Some parts of the kernel may
 * call this if a particular resource that is 'quantity-based' changes.  Things
 * like available RAM to processes, bandwidth, etc.  Cores would probably be
 * inappropriate, since we need to know which specific core is now free. */
void avail_res_changed(int res_type, long change)
{
	printk("[kernel] ksched doesn't track any resources yet!\n");
}

/* Normally it'll be the max number of CG cores ever */
uint32_t max_vcores(struct proc *p)
{
#ifdef __CONFIG_DISABLE_SMT__
	return num_cpus >> 1;
#else
	return MAX(1, num_cpus - num_mgmtcores);
#endif /* __CONFIG_DISABLE_SMT__ */
}

/* Ghetto helper, just hands out up to 'amt_new' cores (no sense of locality or
 * anything) */
static uint32_t get_idle_cores(struct proc *p, uint32_t *pc_arr,
                               uint32_t amt_new)
{
	uint32_t num_granted = 0;
	spin_lock(&idle_lock);
	for (int i = 0; i < num_idlecores && i < amt_new; i++) {
		/* grab the last one on the list */
		pc_arr[i] = idlecoremap[num_idlecores - 1];
		num_idlecores--;
		num_granted++;
	}
	spin_unlock(&idle_lock);
	return num_granted;
}

/* This deals with a request for more cores.  The request is already stored in
 * the proc's amt_wanted (it is compared to amt_granted). */
static void __core_request(struct proc *p)
{
	uint32_t num_granted, amt_wanted, amt_granted;
	uint32_t corelist[num_cpus];

	/* TODO: consider copy-in for amt_wanted too. */
	amt_wanted = p->procdata->res_req[RES_CORES].amt_wanted;
	amt_granted = p->procinfo->res_grant[RES_CORES];

	/* Help them out - if they ask for something impossible, give them 1 so they
	 * can make some progress. (this is racy). */
	if (amt_wanted > p->procinfo->max_vcores) {
		p->procdata->res_req[RES_CORES].amt_wanted = 1;
	}
	/* if they are satisfied, we're done.  There's a slight chance they have
	 * cores, but they aren't running (sched gave them cores while they were
	 * yielding, and now we see them on the run queue). */
	if (amt_wanted <= amt_granted)
		return;
	/* Otherwise, see what they want, and try to give out as many as possible.
	 * Current models are simple - it's just a raw number of cores, and we just
	 * give out what we can. */
	num_granted = get_idle_cores(p, corelist, amt_wanted - amt_granted);
	/* Now, actually give them out */
	if (num_granted) {
		/* give them the cores.  this will start up the extras if RUNNING_M. */
		spin_lock(&p->proc_lock);
		__proc_give_cores(p, corelist, num_granted);
		/* at some point after giving cores, call proc_run_m() (harmless on
		 * RUNNING_Ms).  You can give small groups of cores, then run them
		 * (which is more efficient than interleaving runs with the gives for
		 * bulk preempted processes). */
		__proc_run_m(p); /* harmless to call this on RUNNING_Ms */
		spin_unlock(&p->proc_lock);
	}
}

/************** Debugging **************/
void sched_diag(void)
{
	struct proc *p;
	TAILQ_FOREACH(p, &runnable_scps, proc_link)
		printk("_S PID: %d\n", p->pid);
	TAILQ_FOREACH(p, &all_mcps, proc_link)
		printk("MCP PID: %d\n", p->pid);
	return;
}

void print_idlecoremap(void)
{
	spin_lock(&idle_lock);
	printk("There are %d idle cores.\n", num_idlecores);
	for (int i = 0; i < num_idlecores; i++)
		printk("idlecoremap[%d] = %d\n", i, idlecoremap[i]);
	spin_unlock(&idle_lock);
}

void print_resources(struct proc *p)
{
	printk("--------------------\n");
	printk("PID: %d\n", p->pid);
	printk("--------------------\n");
	for (int i = 0; i < MAX_NUM_RESOURCES; i++)
		printk("Res type: %02d, amt wanted: %08d, amt granted: %08d\n", i,
		       p->procdata->res_req[i].amt_wanted, p->procinfo->res_grant[i]);
}

void print_all_resources(void)
{
	/* Hash helper */
	void __print_resources(void *item)
	{
		print_resources((struct proc*)item);
	}
	spin_lock(&pid_hash_lock);
	hash_for_each(pid_hash, __print_resources);
	spin_unlock(&pid_hash_lock);
}
