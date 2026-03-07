# Phase 5: Algorithm Integration

## Status: DONE

## Goal

Extract Median Cut and ELBG quantization algorithms into the shared
`av_quantize_*` API, then refactor `vf_palettegen` to use it.

## Patches

1. `lavu: add Median Cut quantizer algorithm` (d42088c3e3)
   - Extracted core Median Cut from vf_palettegen into libavutil/mediancut.{h,c}
   - Added AV_QUANTIZE_MEDIAN_CUT to public enum in quantize.h
   - Wired through quantize.c (including region-weighted path)
   - Added 3 FATE tests (basic, regions, vs_neuquant)
   - Bumped lavu version 60.27→60.28, updated APIchanges

2. `lavfi/vf_palettegen: use libavutil Median Cut API` (8209b94852)
   - Replaced ~260 lines of inline algorithm code with ff_mediancut_* calls
   - Added incremental API: ff_mediancut_add_color(), ff_mediancut_build_palette(),
     ff_mediancut_nb_colors(), ff_mediancut_reset()
   - Filter retains AVFilter boilerplate, stats_mode, reserve_transparent
   - All three modes verified: full/diff/single

3. `lavu: move ELBG algorithm from libavcodec to libavutil` (552b923ffe)
   - ELBG has zero libavcodec dependencies (only uses libavutil types)
   - Moved elbg.{h,c} to libavutil, updated include guard
   - Updated 5 consumers: cinepakenc, a64multienc, msvideo1enc, roqvideoenc, vf_elbg
   - Updated both Makefiles, removed empty libavfilter dependencies section

4. `lavu: add ELBG quantizer algorithm` (eff80d0f46)
   - Added AV_QUANTIZE_ELBG wrapping avpriv_elbg_do as color quantizer
   - RGBA pixels → 4D int vectors → ELBG codebook → palette conversion
   - Sorted AVQuantizeAlgorithm enum alphabetically with author/date citations
   - Quality maps to ELBG steps: 1-10→1, 11-20→2, 21-30→3
   - Added FATE test (4 color clusters), bumped lavu 60.28→60.29

5. `lavc/pgssubenc, fftools: add quantize_method option` (pending)
   - Added `quantize_method` AVOption to PGS encoder (elbg/mediancut/neuquant)
   - fftools reads option via `av_opt_get_int` from encoder's priv_data
   - All 3 quantize call sites in ffmpeg_enc.c now use selected algorithm
   - Default remains NeuQuant for backward compatibility
   - Encoders without the option fall back to NeuQuant

## Design Decisions

### Median Cut extraction

Core algorithm (~150 lines): OkLab color histogram, weighted box
splitting along major variance axis, median-weight split points.

**What moved to libavutil:**
- Histogram building (color_inc / hash table)
- Box statistics (compute_box_stats)
- Box splitting (split_box, get_next_box_id_to_split)
- Palette extraction (average color per box)

**What stays in vf_palettegen:**
- AVFilter boilerplate (filter_frame, request_frame, config_output)
- stats_mode (all_frames/diff_frames/single_frames) — filter-level concern
- reserve_transparent — filter option, applied post-quantization
- Metadata logging (lavfi.color_quant_ratio)

### Incremental API for filter integration

The filter builds histograms incrementally across frames, while
`ff_mediancut_learn()` takes a flat RGBA buffer. To bridge this gap,
Patch 2 added lower-level building blocks:

```c
int ff_mediancut_add_color(ctx, color);     // histogram increment
int ff_mediancut_build_palette(ctx);        // run algorithm on accumulated histogram
int ff_mediancut_nb_colors(ctx);            // distinct colors in histogram
void ff_mediancut_reset(ctx);               // reset for reuse
```

`ff_mediancut_learn()` was refactored to be a convenience wrapper
calling reset → add_color loop → build_palette.

### ELBG move rationale

ELBG was in libavcodec with `avpriv_` prefix but had zero libavcodec
dependencies — only libavutil types (AVLFG, av_malloc, av_assert).
Moving to libavutil corrects the dependency direction and enables
wrapping in the quantize API without cross-library violations.

### ELBG as color quantizer

ELBG is a general N-dimensional vector quantizer. Wrapping it for
RGBA color quantization converts pixels to 4D int vectors (R,G,B,A),
runs ELBG to find codebook entries, then converts back to palette.
Nearest-neighbor mapping uses simple squared Euclidean distance in
RGBA space.

### Quantizer selectability

The `quantize_method` AVOption on the PGS encoder stores the user's
choice. fftools reads it from `enc_ctx->priv_data` via `av_opt_get_int`
with a NeuQuant fallback for encoders that don't have the option (e.g.
DVB sub). This keeps the encoder and fftools loosely coupled — the
encoder declares its preferences, fftools honors them.
