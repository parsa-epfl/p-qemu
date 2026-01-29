#ifndef QEMU_DYNAMIC_BARRIER_H
#define QEMU_DYNAMIC_BARRIER_H

#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>


#include "histogram.h"
#include <stdbool.h>

typedef struct IRQState *qemu_irq;

typedef struct delayed_interrupt_t {
    qemu_irq irq;
    int level;
} delayed_interrupt_t;

// The return result of the barrier.
typedef struct {
    uint32_t stop_request;
    uint32_t generation;
} barrier_result_t;

typedef struct {
    struct {
        atomic_uint_fast64_t next_ticket;
        atomic_uint_fast64_t now_serving;
    } lock;
    uint64_t __padding1__[6];
    uint64_t threshold;
    uint64_t __padding2__[7];
    uint64_t count;
    uint64_t __padding3__[7];
    union {
        barrier_result_t two_32;
        atomic_uint_fast64_t one_64;
    } return_value;
    uint64_t __padding5__[7];

    uint64_t timer_update_request;
    int64_t next_virtual_time_deadline_in_ns;

    uint64_t last_timestamp;
    uint64_t total_diff;
    time_histogram_t *histogram[128]; // each core has its own histogram.

    uint64_t next_check_threshold;
    uint64_t current_cycle;

    uint64_t handling_interrupts;

    GQueue *delayed_interrupts;

    volatile uint64_t *plugin_quantum_generation;

    bool stop_request;
} dynamic_barrier_polling_t;

extern dynamic_barrier_polling_t quantum_barrier;

int dynamic_barrier_polling_init(dynamic_barrier_polling_t *barrier, int initial_threshold);
int dynamic_barrier_polling_destroy(dynamic_barrier_polling_t *barrier);
uint32_t dynamic_barrier_polling_wait(dynamic_barrier_polling_t *barrier, uint32_t private_generation, int *stop_request, bool check_time); // return the current quantum generation after waiting for the barrier.
uint32_t dynamic_barrier_polling_increase_by_1(dynamic_barrier_polling_t *barrier); // return the current generation while this thread is added.
int dynamic_barrier_polling_decrease_by_1(dynamic_barrier_polling_t *barrier);
void dynamic_barrier_polling_reset(dynamic_barrier_polling_t *barrier);
void dynamic_barrier_push_delayed_interrupt(dynamic_barrier_polling_t *barrier, qemu_irq irq, int level);
void dynamic_barrier_broadcast_pause_all_cpu_requests(dynamic_barrier_polling_t *barrier);

#endif
