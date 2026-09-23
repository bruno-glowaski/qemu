#ifndef PC_LINUX_KERNEL_COMMON_H
#define PC_LINUX_KERNEL_COMMON_H

#ifdef __KERNEL__

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/sched.h>

#include "../core/common.h"

typedef void LinuxRuntime;

static bool linux_runtime_is_interrupted(RuntimeConst self) {
  return signal_pending(current);
}

static void linux_runtime_fix_hart(RuntimeMut self) {
  int cpu;
  int ret;

  cpu = get_cpu();
  ret = set_cpus_allowed_ptr(current, cpumask_of(cpu));
  put_cpu();

  if (ret)
    do_exit(SIGKILL);
}

static const RuntimeOps linux_runtime_ops = {
    .fix_hart = linux_runtime_fix_hart,
    .is_interrupted = linux_runtime_is_interrupted,
};

declare_impl(LinuxRuntime, Runtime, &linux_runtime_ops);

#endif // !__KERNEL__

#endif // !PC_LINUX_KERNEL_COMMON_H
