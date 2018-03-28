#pragma once
#ifndef ROS_KERNEL
# error "This is a ROS kernel header; user programs should not #include it"
#endif

// Activate the kernel monitor,
// optionally providing a trap frame indicating the current state
// (NULL if none).
void monitor(struct hw_trapframe *hw_tf);
void emit_monitor_backtrace(int type, void *tf);
int onecmd(int argc, char *argv[], struct hw_trapframe *hw_tf);
void __run_mon(uint32_t srcid, long a0, long a1, long a2);

// Functions implementing monitor commands.
int mon_help(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_kerninfo(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_backtrace(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_ps(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_reboot(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_showmapping(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_sm(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_cpuinfo(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_nanwan(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_bin_run(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_manager(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_procinfo(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_pip(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_kill(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_exit(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_kfunc(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_notify(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_measure(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_trace(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_monitor(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_shell(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_alarm(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_msr(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_db(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_px(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_kpfret(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_ks(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_coreinfo(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_hexdump(int argc, char **argv, struct hw_trapframe *hw_tf);
int mon_pahexdump(int argc, char **argv, struct hw_trapframe *hw_tf);
