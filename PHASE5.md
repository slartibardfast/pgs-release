# Phase 5: Algorithm Integration

## Status: DONE

## Goal

Extract Median Cut quantization algorithm into the shared `av_quantize_*`
API, then refactor `vf_palettegen` to use it.

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

## ELBG — Dropped

ELBG was originally planned as Patch 3 but was dropped:

- **Architectural blocker:** ELBG is `avpriv_` in libavcodec. Wrapping it in
  libavutil's quantize.c creates a wrong-direction dependency (libavutil→libavcodec).
  The only clean fix is moving elbg.{h,c} to libavutil, which touches 4 upstream
  encoders (cinepakenc, a64multienc, roqvideoenc, msvideo1enc) — a large
  cross-cutting refactor.

- **Poor fit:** ELBG is a general-purpose N-dimensional vector quantizer operating
  on int arrays. Wrapping it as a color quantizer (RGBA→int vectors→codebook→palette)
  adds complexity for marginal value when NeuQuant and Median Cut are purpose-built
  for the task.

- **Not needed:** Our PGS use case is well-served by the existing two algorithms.
  If upstream wants ELBG in the quantize API later, the right approach is a separate
  series moving elbg to libavutil first.

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
