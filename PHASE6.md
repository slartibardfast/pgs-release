# Phase 6: GIF Encoder RGBA Quantization

## Status: IN PROGRESS

## Goal

Add `AV_PIX_FMT_RGB32` (RGBA) support to the GIF encoder so users can
encode GIF directly from RGBA without the `palettegen`+`paletteuse`
filter pipeline.

## Patches

1. `lavc/gif: add RGBA input with built-in quantization` (pending)
   - Add `AV_PIX_FMT_RGB32` to accepted pixel formats
   - When input is RGBA, quantize each frame via `av_quantize_*`
   - Map pixels via `ff_palette_map_apply()` with dithering
   - Add `quantize_method` option (elbg/mediancut/neuquant, default mediancut)
   - Add `dither` option (9 modes from palettemap, default floyd_steinberg)
   - Per-frame palette (no cross-frame optimization)

## Design Decisions

### Why Median Cut as default (not NeuQuant)

For PGS subtitles (Phase 5), NeuQuant is the right default: subtitles
have few colors and NeuQuant is fastest. For GIF encoding of video
frames, the input has thousands of unique colors. Median Cut produces
significantly better palettes on high-color images (69.20 vs 41.76 dB
on multicolour tests at 256 colors) and is deterministic.

### Single patch (not two)

The plan originally called for two patches: one for quantization,
one for dithering. But `ff_palette_map_apply()` already does both
mapping and dithering in a single call. Splitting would mean Patch 1
uses `av_quantize_apply()` (no dithering) and Patch 2 replaces it
with `ff_palette_map_apply()`. That's churn with no intermediate
value — the non-dithered path would produce terrible GIF quality.
One patch is cleaner.

### Per-frame vs global palette

Each frame gets an independent palette. This is simpler and correct
for most content. The `palettegen`+`paletteuse` pipeline remains
available for users who need cross-frame palette optimization
(`stats_mode=full`) or other advanced options.

### Integration with existing encoder

The GIF encoder currently accepts PAL8 (palette + indices) and several
RGB pixel formats that get converted via `avpriv_set_systematic_pal2`.
RGBA input is new: the encoder quantizes RGBA to a palette + indices
internally, then feeds the existing LZW pipeline.

Key integration points:
- `gif_encode_init`: allocate quantize + palettemap contexts
- `gif_encode_frame`: detect RGBA, quantize, map, then continue
  existing path with the generated PAL8 data
- `gif_encode_close`: free quantize + palettemap contexts
- `gif_image_write_image`: receives palette + indices as before

### Transparency handling

RGBA pixels with alpha < 128 are mapped to a transparent index.
After quantization, we reserve palette slot 255 for transparency
and set `transparent_index` so the existing GCE transparency
logic applies.
