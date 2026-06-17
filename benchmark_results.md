# LVGL 9.5.0 Benchmark — GC520UL GPU vs SW

Board: `sama7d65_curiosity/sama7d65`, 800x480 RGB565, LVGL full-refresh double-VDB.
Toggle: `USE_NANO2D_DRAW_UNIT` in `app/src/main.c` (1 = GPU draw unit, 0 = pure SW).
GPU core clock = 532 MHz (GPUPLL/2, the GC520UL's rated max).

Columns: Avg. CPU %, Avg. FPS, render time (ms), flush time (ms).
(Scene names reconstructed — the Zephyr LVGL log shim strips the first 7 chars
of each line; numeric columns are intact.)

## GPU @ 532 MHz (GCLKDIV ÷2, in-spec, USE_NANO2D_DRAW_UNIT = 1)

| Scene | CPU % | FPS | render ms | flush ms |
|---|---|---|---|---|
| Empty screen | 19 | 20 | 9 | 0 |
| Moving wallpaper | 28 | 21 | 13 | 0 |
| Single rectangle | 25 | 22 | 10 | 0 |
| Multiple rectangles | 31 | 20 | 15 | 0 |
| Multiple RGB images | 29 | 21 | 13 | 0 |
| Multiple ARGB images | 29 | 20 | 13 | 0 |
| Rotated ARGB images | 33 | 19 | 16 | 0 |
| Multiple labels | 30 | 20 | 13 | 0 |
| Multiple sized text | 40 | 17 | 22 | 0 |
| Multiple arcs | 29 | 21 | 12 | 0 |
| Containers | 31 | 20 | 15 | 0 |
| Containers with overlay | 41 | 17 | 24 | 0 |
| Containers with opa | 36 | 19 | 18 | 0 |
| Containers with opa_layer | 47 | 15 | 30 | 0 |
| Containers with scrolling | 32 | 20 | 14 | 0 |
| Widgets demo | 51 | 14 | 29 | 0 |
| **All scenes avg.** | **33** | **19** | **16** | **0** |

## SW (USE_NANO2D_DRAW_UNIT = 0)

| Scene | CPU % | FPS | render ms | flush ms |
|---|---|---|---|---|
| Empty screen | 3 | 25 | 1 | 0 |
| Moving wallpaper | 13 | 25 | 4 | 0 |
| Single rectangle | 5 | 28 | 1 | 0 |
| Multiple rectangles | 7 | 27 | 2 | 0 |
| Multiple RGB images | 8 | 27 | 2 | 0 |
| Multiple ARGB images | 13 | 25 | 4 | 0 |
| Rotated ARGB images | 19 | 23 | 7 | 0 |
| Multiple labels | 15 | 25 | 4 | 0 |
| Multiple sized text | 30 | 20 | 14 | 0 |
| Multiple arcs | 13 | 25 | 3 | 0 |
| Containers | 16 | 24 | 6 | 0 |
| Containers with overlay | 25 | 21 | 11 | 0 |
| Containers with opa | 22 | 23 | 9 | 0 |
| Containers with opa_layer | 40 | 17 | 22 | 0 |
| Containers with scrolling | 14 | 25 | 5 | 0 |
| Widgets demo | 32 | 19 | 10 | 0 |
| **All scenes avg.** | **17** | **23** | **6** | **0** |

## Comparison (GPU @532 vs SW)

| Metric (all-scenes avg.) | SW | GPU @532 |
|---|---|---|
| Avg. FPS | **23** | 19 |
| Avg. render time | **6 ms** | 16 ms |
| Avg. CPU | **17%** | 33% |

**The GPU draw unit is net-negative on this benchmark/config** — every scene is
slower and more CPU-hungry with the GPU enabled. Even purely software scenes
(`Multiple sized text` 14→22 ms, `Multiple arcs` 3→12 ms — text/arcs are never
GPU-accelerated) regress, because each frame's full-screen background *fill*
goes through the GPU path and adds fixed per-frame cost.

### Why (cached Cortex-A7 + NEON SW renderer vs a small blitter GPU)
1. **Per-op overhead dominates small ops** — GPU state setup (`n2d_set`),
   command queue, and a blocking `n2d_commit` (IRQ wait) per batch. The SW NEON
   fill/blit has none of that.
2. **Cache-coherency tax** — every GPU op cleans source + destination and
   invalidates afterward, on top of the display flush's own 768 KB full-buffer
   clean.
3. **Synchronous commit** — CPU blocks on the GPU; no CPU/GPU overlap (and on
   single-core / `LV_OS_NONE` overlap is limited anyway).
4. **The SW path is genuinely fast** here — Cortex-A7 + NEON handles these
   small/medium fills and blits quickly with no setup or coherency cost.

This is the common outcome for blitter-class 2D GPUs against a fast cached CPU
renderer on typical UI: the GPU only wins when ops are large, the CPU renderer
is slow, or the GPU runs asynchronously overlapping CPU work.

### Levers tried / considered
- **Clock**: already at the rated max (532 MHz). Halving to 266 only slowed it;
  not the bottleneck.
- **Cache reduction** (single-range full-width ops, invalidate-not-clean for
  opaque fills): no measurable change — the pre-clean wasn't writing back dirty
  lines, so there was nothing to save.
- **Async commit**: a first attempt (idle-register busy-poll) regressed to
  single-digit FPS. Reworked cleanly with upstream `n2d_commit_ex(FALSE)` +
  `n2d_wait()` (ported into our nano2D; sleeps on the completion IRQ) — stable
  and regression-free, but **flat**: identical to the blocking 532 numbers.
  Overlap is blocked by the full-screen background dependency (nothing
  independent for SW to render while the GPU runs). Kept behind
  `NANO2D_ASYNC_COMMIT` (off by default); would pay off only with threaded SW
  (`LV_USE_OS`) or a GPU-favorable workload.
- **Size-gated offload**: ruled out by the per-op microbenchmark below — there
  is no size at which the GPU beats the NEON software renderer here.

## Per-op microbenchmark (GPU vs CPU)

Direct per-op timing (`src/microbench.c`, behind `RUN_MICROBENCH`), GPU @532 MHz,
including the same cache maintenance the draw unit does. "SW" is plain C — a
*lower bound* on software speed (LVGL's NEON renderer is faster), so a GPU loss
here is a GPU loss against LVGL too.

| Op | SW (16px → 256px) | GPU marginal (16px → 256px) | GPU beats SW above |
|---|---|---|---|
| fill | 1.6 µs → 332 µs (1.9 ms full-screen) | 142 µs → 1.38 ms (7.8 ms full) | never |
| copy (RGB565) | 2.3 µs → 516 µs | 153 µs → 1.41 ms | never |
| scale2x (RGB565) | 9 µs → 2.43 ms | 174 µs → 5.5 ms | never |
| alpha (ARGB8888) | 15 µs → 4.85 ms | 169 µs → 1.57 ms | ~64 px (but see below) |

Findings:
1. **~140 µs per-op GPU floor.** Even batched (commit amortised), each GPU op
   costs ~140–170 µs of *CPU* to build the command + maintain caches before the
   GPU runs. A 16 px SW fill is 1.6 µs; queuing it on the GPU is 142 µs (~90×).
   fill/copy/scale never recover, even full-screen.
2. **The alpha "crossover" is an artifact.** The naive-C reference does 3 integer
   divides per pixel (`/255`), ~10× slower than LVGL's divide-free, NEON alpha
   blend. The real benchmark confirms it: the *Multiple ARGB images* scene
   (~100 px ARGB blits) measured SW 4 ms vs GPU 13 ms — LVGL's NEON SW beat the
   GPU at the very size the microbench flags as a GPU win. Against the real
   renderer the crossover is far higher or nonexistent.

**Verdict: no size-gate sweet spot on this platform.** The GC520 at 532 MHz is
slower than Cortex-A7 + NEON for 2D across the board: the ~140 µs/op command+cache
overhead plus the CPU's fast vectorized blends leave no window. The GPU would
only win with a slower CPU, a coherent (no-cache-maintenance) GPU path, or much
heavier per-pixel work than this UI workload presents.
