#ifndef __ASM_SPINLOCK_H
#define __ASM_SPINLOCK_H

#include <lib.h>

/*
 * Your basic SMP spinlocks, allowing only a single CPU anywhere
 */

typedef struct {
	volatile unsigned int slock;
} spinlock_t;

#define SPINLOCK_MAGIC	0xdead4ead

#define SPIN_LOCK_UNLOCKED (spinlock_t) { 1 }

#define spin_lock_init(x)	do { *(x) = SPIN_LOCK_UNLOCKED; } while(0)

/*
 * Simple spin lock operations.  There are two variants, one clears IRQ's
 * on the local processor, one does not.
 *
 * We make no fairness assumptions. They have a cost.
 */

#define spin_is_locked(x)	(*(volatile signed char *)(&(x)->slock) <= 0)
#define spin_unlock_wait(x)	do { barrier(); } while(spin_is_locked(x))

#define spin_lock_string \
        "1:\n" \
	LOCK \
	"decb %0\n\t" \
	"jns 3f\n" \
	"2:\t" \
	"rep;nop\n\t" \
	"cmpb $0,%0\n\t" \
	"jle 2b\n\t" \
	"jmp 1b\n" \
	"3:\n\t"

#define spin_lock_string_flags \
        "1:\n" \
	LOCK \
	"decb %0\n\t" \
	"jns 4f\n\t" \
	"2:\t" \
	"testl $0x200, %1\n\t" \
	"jz 3f\n\t" \
	"#sti\n\t" \
	"3:\t" \
	"rep;nop\n\t" \
	"cmpb $0, %0\n\t" \
	"jle 3b\n\t" \
	"#cli\n\t" \
	"jmp 1b\n" \
	"4:\n\t"

/*
 * This works. Despite all the confusion.
 * (except on PPro SMP or if we are using OOSTORE)
 * (PPro errata 66, 92)
 */

#define spin_unlock_string \
	"xchgb %b0, %1" \
		:"=q" (oldval), "=m" (lock->slock) \
		:"0" (oldval) : "memory"

static inline void _raw_spin_unlock(spinlock_t *lock)
{
	char oldval = 1;
	__asm__ __volatile__(
		spin_unlock_string
	);
}

static inline int _raw_spin_trylock(spinlock_t *lock)
{
	char oldval;
	__asm__ __volatile__(
		"xchgb %b0,%1\n"
		:"=q" (oldval), "=m" (lock->slock)
		:"0" (0) : "memory");
	return oldval > 0;
}

static inline void _raw_spin_lock(spinlock_t *lock)
{
	__asm__ __volatile__(
		spin_lock_string
		:"=m" (lock->slock) : : "memory");
}

static inline void _raw_spin_lock_flags (spinlock_t *lock, unsigned long flags)
{
	__asm__ __volatile__(
		spin_lock_string_flags
		:"=m" (lock->slock) : "r" (flags) : "memory");
}

#define _spin_trylock(lock)     ({_raw_spin_trylock(lock) ? \
                                1 : ({ 0;});})

#define _spin_lock(lock)        \
do {                            \
        _raw_spin_lock(lock);   \
} while(0)

#define _spin_unlock(lock)      \
do {                            \
        _raw_spin_unlock(lock); \
} while (0)


#define spin_lock(lock)       _spin_lock(lock)
#define spin_unlock(lock)       _spin_unlock(lock)

#define DEFINE_SPINLOCK(x) spinlock_t x = SPIN_LOCK_UNLOCKED

#endif
