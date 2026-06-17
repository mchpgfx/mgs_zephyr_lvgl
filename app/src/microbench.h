/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPU-vs-CPU per-op microbenchmark. Times nano2D GPU ops against a software
 * reference across op types and sizes to find the size at which the GPU beats
 * the CPU (if ever). Results are printed over the console as a CSV table.
 */

#ifndef MICROBENCH_H_
#define MICROBENCH_H_

/* Run the microbenchmark once and print results. The GC520 must be initialised
 * (the driver does n2d_init/n2d_open at boot). Does not use LVGL. */
void microbench_run(void);

#endif /* MICROBENCH_H_ */
