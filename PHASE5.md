# Phase 5: Algorithm Integration

## Status: IN PROGRESS

## Goal

Extract Median Cut and ELBG quantization algorithms into the shared
`av_quantize_*` API, then refactor `vf_palettegen` to use it.

## Patches

1. `lavu: add Median Cut quantizer algorithm` — extract core from vf_palettegen
2. `lavfi/vf_palettegen: use libavutil quantizer API` — replace inline algorithm
3. `lavu: add ELBG quantizer algorithm` — wrap avpriv_elbg_do

## Design Decisions

### Median Cut extraction

Core algorithm (~150 lines): OkLab color histogram, weighted box
splitting along major variance axis, median-weight split points.

**What moves to libavutil:**
- Histogram building (color_inc / hash table)
- Box statistics (compute_box_stats)
- Box splitting (split_box, get_next_box_id_to_split)
- Palette extraction (average color per box)

**What stays in vf_palettegen:**
- AVFilter boilerplate (filter_frame, request_frame, config_output)
- stats_mode (all_frames/diff_frames/single_frames) — filter-level concern
- reserve_transparent — filter option, applied post-quantization
- Metadata logging (lavfi.color_quant_ratio)

**Internal API pattern (matches neuquant.h):**
```c
typedef struct MedianCutContext MedianCutContext;
MedianCutContext *ff_mediancut_alloc(int max_colors);
void ff_mediancut_free(MedianCutContext **ctx);
int ff_mediancut_learn(MedianCutContext *ctx, const uint8_t *rgba,
                       int nb_pixels);
void ff_mediancut_get_palette(MedianCutContext *ctx, uint32_t *palette);
int ff_mediancut_map_pixel(MedianCutContext *ctx,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a);
```

### ELBG wrapping

ELBG is already library-level code in libavcodec/elbg.{h,c}. Wrapping
requires RGBA-to-int translation and codebook-to-palette conversion.

**Dependencies:** AVLFG (lagged Fibonacci PRNG) from libavutil/lfg.h.
Already in libavutil, no cross-library dependency issues.

**Note:** ELBG uses `avpriv_` prefix — it's internal to libavcodec.
Wrapping it in libavutil's quantize.c creates a libavutil→libavcodec
dependency, which is architecturally wrong (libavutil is the base layer).
Options:
1. Move elbg.{h,c} to libavutil (clean but touches upstream files)
2. Keep ELBG wrapper in a higher layer
3. Duplicate the algorithm in libavutil

Decision deferred until implementation reveals the cleanest path.
