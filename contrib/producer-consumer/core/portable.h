#ifndef PC_CORE_PORTABLE_H
#define PC_CORE_PORTABLE_H

/*
 * Includes
 */

#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/compiler.h>

#include <asm/tsc.h>

#else // Userspace

#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#endif // __KERNEL__

/*
 * Basic Types
 */

#ifdef __KERNEL__

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

/*
 * Included from stdint and stdbool
 */

#endif

#define true 1
#define false 0

/*
 * Atomics
 */

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

/*
 * Timestamps
 */

typedef uint64_t tsc_t;

#ifdef KERNEL

static inline tsc_t read_tsc() { rdtsc(); }

#else

#define mbarrier() __asm__ __volatile__("" : : : "memory")

static inline tsc_t read_tsc(void) {
  uint32_t hi, lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return (uint64_t)lo | ((uint64_t)hi << 32);
}

#endif // KERNEL

/*
 * Utils
 */

#define declare_trait(TraitName, OpsStruct)                                    \
  typedef struct TraitName##Const {                                            \
    const OpsStruct *ops;                                                      \
    const void *data;                                                          \
  } TraitName##Const;                                                          \
  typedef struct TraitName##Mut {                                              \
    const OpsStruct *ops;                                                      \
    void *data;                                                                \
  } TraitName##Mut;                                                            \
  static inline TraitName##Const As##TraitName##Const(                         \
      TraitName##Mut fat_ptr) {                                                \
    return (TraitName##Const){.ops = fat_ptr.ops, .data = fat_ptr.data};       \
  }

#define declare_impl(ImplName, TraitName, impl_ops)                            \
  static inline TraitName##Const ImplName##As##TraitName##Const(               \
      const ImplName *data) {                                                  \
    return (TraitName##Const){.ops = &impl_ops, .data = data};                 \
  }                                                                            \
  static inline TraitName##Mut ImplName##As##TraitName##Mut(ImplName *data) {  \
    return (TraitName##Mut){.ops = &impl_ops, .data = data};                   \
  }

#endif // !
