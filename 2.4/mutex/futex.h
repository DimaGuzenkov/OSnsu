#ifndef _FUTEX_H_
#define _FUTEX_H_

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>

static inline long futex(uint32_t *uaddr, int futex_op, uint32_t val,
                        const struct timespec *timeout, uint32_t *uaddr2,
                        uint32_t val3) {
    return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

static inline void futex_wait(uint32_t *addr, uint32_t val) {
    futex(addr, FUTEX_WAIT, val, NULL, NULL, 0);
}

static inline void futex_wake(uint32_t *addr, int count) {
    futex(addr, FUTEX_WAKE, count, NULL, NULL, 0);
}

#endif