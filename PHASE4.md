# Phase 4: Region-Weighted Quantization

## Status: DONE

Two commits on `pgs-series` branch:
- `cb332e7d7b` lavu/quantize: add region-weighted palette generation
- `2b03815c86` fftools: use region-weighted quantization for coalesced subtitles

Also squashed a pre-existing Phase 3a bug fix (missing `!CONFIG_LIBASS`
stub for `avfilter_subtitle_render_add_event`) into `9f1dfb5401`.

### Series review cleanup (2026-03-07)

During a full 14-commit series review, the following cross-cutting issues
were found and fixed via rebase:

- **FFPaletteMapContext made opaque** (Phase 2b) — struct moved from
  palettemap.h to palettemap.c, added `ff_palette_map_get_palette()` and
  `ff_palette_map_get_nodes()` accessors, updated vf_paletteuse.c consumer
- **APIchanges completed** (Phase 3) — added missing `init_event`,
  `add_event`, `sample` entries to doc/APIchanges
- **Doxygen consistency** (Phase 3) — added `[in]/[out]` parameter
  direction markers to all functions in subtitle_render.h
- **Commit reordering** (Phase 3/3a) — core text-to-bitmap now precedes
  animation utilities to match PLAN.md phase dependency structure
- **Ticket references** (Phase 3) — added `Ref: trac.ffmpeg.org/ticket/3819`
  to subtitle rendering and text-to-bitmap commit trailers

## Problem

PGS Display Sets use a single 256-color palette (PDS) shared across all
composition objects. When multiple ASS events overlap in time — e.g., a
large monochrome dialogue region plus a small colorful karaoke region —
NeuQuant training on the concatenated pixel buffer is dominated by the
larger region. The karaoke colors get starved of palette entries, causing
visible color loss.

## Solution

Add `av_quantize_add_region()` to the existing quantizer API. Each call
registers a region of RGBA pixels. When `av_quantize_generate_palette()`
is called with regions present, it builds an interleaved sample buffer
drawing an equal number of pixels from each region, regardless of region
size. Small regions wrap around to fill their quota.

### Key design decisions

**Copy semantics:** `add_region()` copies pixel data internally via
`av_memdup()`. The caller may free immediately. Regions are consumed
(freed) by the next `generate_palette()` call.

**Fixed region limit:** `MAX_REGIONS = 16` with inline array (no heap
allocation for the region list). Returns `AVERROR(ENOSPC)` on overflow.
16 is more than sufficient — PGS supports at most 2 composition objects
per Display Set, and even extreme karaoke rarely exceeds 5 overlapping
events.

**Uniform sampling:** Every region contributes exactly `per_region`
pixels, computed as `FFMIN(max_region_size, SAMPLES_PER_REGION)` where
`SAMPLES_PER_REGION = 8192`. Regions smaller than `per_region` wrap
around. This bounds the interleaved buffer at `8192 * 16 * 4 = 512 KB`
regardless of input size.

**Backward compatible:** If no regions are added, `generate_palette()`
operates on the flat `rgba`/`nb_pixels` parameters as before.

### What we dropped

**HyAB distance metric:** PLAN.md proposed HyAB (Abasi et al. 2020) for
quality validation. After implementation and testing, SSE (sum of squared
Euclidean error in RGBA space) proved sufficient to demonstrate the
improvement. HyAB added complexity without changing the conclusion. The
region-weighted approach reduces karaoke SSE by 76% in our test — the
signal is unambiguous.

## API

```c
int av_quantize_add_region(AVQuantizeContext *ctx,
                            const uint8_t *rgba, int nb_pixels);
```

- `ctx`: quantization context from `av_quantize_alloc()`
- `rgba`: RGBA pixel data (4 bytes per pixel), copied internally
- `nb_pixels`: number of pixels in this region
- Returns 0 on success, negative `AVERROR` on failure

## Consumer

In `fftools/ffmpeg_enc.c`, `flush_coalesced_subtitles()` now renders each
event individually via `avfilter_subtitle_render_frame()`, adds it as a
region, then calls `generate_palette()` on the composite RGBA. This only
activates when `nb > 1` (multiple overlapping events).

## Tests

Six new tests in `libavutil/tests/quantize.c`:

| Test | What it proves |
|------|---------------|
| `test_region_weighted` | 4000 white + 100 red pixels: red survives in palette with regions |
| `test_region_backward_compat` | No regions → flat path works unchanged |
| `test_region_starvation_comparison` | Flat SSE=43425 vs region SSE=24336 (44% improvement) |
| `test_region_multi_diversity` | 3 regions (cyan/magenta/yellow) all represented |
| `test_region_single_equivalence` | Single region SSE within 20% of flat |
| `test_region_karaoke` | Real-world karaoke: flat SSE=6083340 vs region SSE=1438620 (76% improvement) |

## Security considerations

- `nb_pixels > INT_MAX / 4` guard before `* 4` byte calculations in `add_region()`
- `int64_t` intermediate for `ev_w * ev_h` dimension product in `ffmpeg_enc.c`
- Region cleanup on all error paths in `generate_palette()`

## Files changed

| File | Change |
|------|--------|
| `libavutil/quantize.h` | Add `av_quantize_add_region()` declaration, update `generate_palette()` docs |
| `libavutil/quantize.c` | Region storage, `build_region_samples()`, region path in `generate_palette()` |
| `libavutil/tests/quantize.c` | 6 new tests + extended error handling test |
| `libavutil/version.h` | MINOR bump 26 → 27 |
| `doc/APIchanges` | New API entry |
| `tests/ref/fate/source` | Add pre-existing neuquant.c av_clip lines |
| `fftools/ffmpeg_enc.c` | Region-weighted path in coalescing |
| `libavfilter/subtitle_render.c` | Squashed: missing `!CONFIG_LIBASS` stub |
