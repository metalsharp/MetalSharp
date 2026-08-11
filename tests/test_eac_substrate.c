/*
 * Regression test for the MetalSharp Linux EAC substrate's Mach port
 * lifecycle (metalsharp/MetalSharp#442).
 *
 * Every guest errno/pthread/mutex operation obtains the current thread's
 * Mach port name through mach_thread_self(), which creates a fresh send
 * right.  If that right is not deallocated, the send-right reference count
 * on the thread's self port grows without bound and long-running protected
 * launchers exhaust the process Mach port budget.
 *
 * The substrate is x86_64/Rosetta-only, so this test is built with the
 * project default (Wine) architecture, exactly like the substrate target.
 * It includes the substrate source directly so the static hot-path entry
 * points can be exercised, then asserts that the send-right count on the
 * current thread's self port is unchanged after hammering every path that
 * used to leak.
 */

#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/anticheat/linux_substrate.c"

#define MS442_ITERATIONS       100000u
#define MS442_MUTEX_ITERATIONS 1000u

static mach_msg_type_number_t send_right_count(void) {
    mach_port_t self = mach_thread_self();
    mach_msg_type_number_t refs = 0;
    kern_return_t result = mach_port_get_refs(mach_task_self(), self, MACH_PORT_RIGHT_SEND, &refs);
    mach_port_deallocate(mach_task_self(), self);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "eac_substrate: mach_port_get_refs failed: %d\n", result);
        exit(2);
    }
    return refs;
}

int main(void) {
    mach_msg_type_number_t baseline = send_right_count();

    uint32_t key = 0;
    (void)metalsharp_eac_pthread_key_create(&key, NULL);

    /* errno, TSD, and pthread_self lookups are the per-operation hot paths
     * and do not log. */
    for (uint32_t iteration = 0; iteration < MS442_ITERATIONS; iteration++) {
        (void)metalsharp_eac_linux_errno_location();
        (void)metalsharp_eac_pthread_setspecific(key, (const void*)(uintptr_t)iteration);
        (void)metalsharp_eac_pthread_getspecific(key);
        (void)metalsharp_eac_pthread_self();
        (void)metalsharp_eac_thread_token();
    }

    /* The mutex paths log through ms_log, which opens/writes/closes a file
     * per call; keep their share of the loop small. */
    for (uint32_t iteration = 0; iteration < MS442_MUTEX_ITERATIONS; iteration++) {
        (void)metalsharp_eac_pthread_mutex_lock((void*)(uintptr_t)0x442u);
        (void)metalsharp_eac_pthread_mutex_unlock((void*)(uintptr_t)0x442u);
        (void)metalsharp_eac_pthread_mutex_trylock((void*)(uintptr_t)0x442u);
        (void)metalsharp_eac_pthread_mutex_unlock((void*)(uintptr_t)0x442u);
    }

    mach_msg_type_number_t after = send_right_count();
    if (after != baseline) {
        fprintf(stderr, "eac_substrate: Mach send-right leak: baseline=%u after=%u (delta=%d)\n", (unsigned)baseline,
                (unsigned)after, (int)(after - baseline));
        return 1;
    }
    printf("eac_substrate: OK send-right count stable (%u -> %u)\n", (unsigned)baseline, (unsigned)after);
    return 0;
}
