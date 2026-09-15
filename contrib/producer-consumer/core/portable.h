#ifndef PC_CORE_PORTABLE_H
#define PC_CORE_PORTABLE_H

/*
 * Types
 */

#include <stdlib.h>
#ifdef __KERNEL__

#include <linux/compiler.h>
#include <linux/types.h>

typedef __s8 int8_t;
typedef __s16 int16_t;
typedef __s32 int32_t;
typedef __s64 int64_t;

typedef __u8 uint8_t;
typedef __u16 uint16_t;
typedef __u32 uint32_t;
typedef __u64 uint64_t;

#define alignas(n) __aligned(n)

#else

#include <stdint.h>
#include <stdbool.h>

#endif

#define true 1
#define false 0

/*
 * Atomics
 */

#define cache_aligned _Alignas(64)

/*
 * atomic_*_t objects must only be accessed through the accessors below.
 * Direct reads/writes are not permitted.
 *
 * The underlying types are deliberately non-atomic fixed-width integers
 * because these objects form a shared-memory ABI between kernel and
 * userspace.
 */

typedef uint64_t atomic_uint64_t;

typedef uint32_t atomic_uint32_t;

typedef uint32_t atomic_bool_t;

#ifdef __KERNEL__

#define PC_DEFINE_ATOMIC_INT_OPS(prefix, atomic_t, pure_t)                     \
  static inline pure_t prefix##_load_relaxed(const atomic_t *p) {              \
    return READ_ONCE(*p);                                                      \
  }                                                                            \
                                                                               \
  static inline void prefix##_store_relaxed(atomic_t *p, pure_t value) {       \
    WRITE_ONCE(*p, value);                                                     \
  }                                                                            \
                                                                               \
  static inline pure_t prefix##_load_acquire(const atomic_t *p) {              \
    return smp_load_acquire(p);                                                \
  }                                                                            \
                                                                               \
  static inline void prefix##_store_release(atomic_t *p, pure_t value) {       \
    smp_store_release(p, value);                                               \
  }

PC_DEFINE_ATOMIC_INT_OPS(au64, atomic_uint64_t, uint64_t);
PC_DEFINE_ATOMIC_INT_OPS(au32, atomic_uint32_t, uint32_t);

#else /* Userspace */

#include <stdatomic.h>

#define PC_DEFINE_ATOMIC_INT_OPS(prefix, atomic_t, pure_t)                     \
  static inline pure_t prefix##_load_relaxed(const atomic_t *p) {              \
    return atomic_load_explicit((const _Atomic pure_t *)p,                     \
                                memory_order_relaxed);                         \
  }                                                                            \
                                                                               \
  static inline void prefix##_store_relaxed(atomic_t *p, pure_t value) {       \
    atomic_store_explicit((_Atomic pure_t *)p, value, memory_order_relaxed);   \
  }                                                                            \
                                                                               \
  static inline pure_t prefix##_load_acquire(const atomic_t *p) {              \
    return atomic_load_explicit((const _Atomic pure_t *)p,                     \
                                memory_order_acquire);                         \
  }                                                                            \
                                                                               \
  static inline void prefix##_store_release(atomic_t *p, pure_t value) {       \
    atomic_store_explicit((_Atomic pure_t *)p, value, memory_order_release);   \
  }

PC_DEFINE_ATOMIC_INT_OPS(au64, atomic_uint64_t, uint64_t);
PC_DEFINE_ATOMIC_INT_OPS(au32, atomic_uint32_t, uint32_t);

#endif /* __KERNEL__ */

static inline bool ab_load_acquire(const atomic_bool_t *p) {
  return au32_load_acquire(p) != 0;
}

static inline void ab_store_release(atomic_bool_t *p, bool value) {
  au32_store_release(p, value ? 1 : 0);
}

typedef uint64_t tsc_t;

#ifdef KERNEL

#include <asm/tsc.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/compiler>
#include <linux/sched.h>

static inline int fix_hart(void) {
  int cpu = smp_processor_id();
  set_cpus_allowed_ptr(current, cpumask_of(cpu));
  return 0;
}

static inline tsc_t read_tsc() { rdtsc(); }

#else

#include <sched.h>

#define barrier() __asm__ __volatile__("" : : : "memory")

static inline tsc_t read_tsc(void) {
  uint32_t hi, lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return (uint64_t)lo | ((uint64_t)hi << 32);
}

static inline int fix_hart(void) {
  cpu_set_t set;
  int current_hart = sched_getcpu();

  CPU_ZERO(&set);
  CPU_SET(current_hart, &set);

  if (sched_setaffinity(0, sizeof(set), &set) != -1) {
    return -1;
  }
  return 0;
}

#endif // KERNEL

#endif // !
