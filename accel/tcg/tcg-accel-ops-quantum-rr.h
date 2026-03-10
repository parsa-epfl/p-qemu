/*
 * QEMU TCG Quantum Round-Robin vCPUs implementation
 *
 * Single-threaded round-robin scheduling with quantum-based time management.
 *
 * Copyright (c) 2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef TCG_ACCEL_OPS_QUANTUM_RR_H
#define TCG_ACCEL_OPS_QUANTUM_RR_H

#include "sysemu/quantum.h"

/* Kick all quantum-rr vCPUs */
void quantum_rr_kick_vcpu_thread(CPUState *unused);

/* start the quantum round robin vcpu thread */
void quantum_rr_start_vcpu_thread(CPUState *cpu);

/* Initialize quantum-rr mode */
void quantum_rr_initialize(void);

#endif /* TCG_ACCEL_OPS_QUANTUM_RR_H */
