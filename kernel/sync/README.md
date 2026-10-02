Ticket spinlocks (`spinlock.c`), preemptCount (`preempt.c`) and the lock validator
(`lockdep-core.c`, the pure core host-tested in `tests/host/kernel_lockdep_core_test.c`, and
`lockdep.c`, debug builds only). See ARCHITECTURE §7.6 and D-183..D-189. `Mutex`, `RwLock`,
`Semaphore`, `WaitQueue` and `Completion` arrive with the scheduler (M4).
