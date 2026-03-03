# Text-to-Bitmap Subtitle Conversion for FFmpeg

FFmpeg can transcode between virtually any audio and video format. Subtitles are the exception. Converting text subtitles (SRT, ASS, WebVTT) to bitmap format (PGS, DVB, DVD, XSUB) fails with:

> *"Subtitle encoding currently only possible from text to text or bitmap to bitmap"*

This blocks 72 subtitle conversion pairs (18 text decoders × 4 bitmap encoders) and is tracked as [FFmpeg bug #3819](https://trac.ffmpeg.org/ticket/3819). This repository contains a patch series that fixes it.

## The approach

Three prior attempts have perhaps stalled:

- **Coza (2022)**: Put libass rendering in libavcodec. The feedback was clear: libavcodec→libass is a dependency violation; the work belongs in libavfilter.
- **softworkz (2021–2025)**: Full subtitle filter infrastructure — 25 patches, 89 files, new AVFrame fields, new buffer sources. Fundamental API design questions remain unresolved after four years.

We take the minimal path. libass already lives in libavfilter. A small rendering utility there, called from fftools in the same pattern as `sub2video`, is all that's needed. No filter infrastructure. No AVFrame changes. The whole thing fits in 12 patches.

## The patches

Branch: [`pgs-series`](https://github.com/slartibardfast/FFmpeg/tree/pgs-series) — four independent series, A/B/C reviewable in parallel, D after all three land.

### A — PGS encoder (2 patches)

A working HDMV PGS subtitle encoder with a full composition state machine:

- Automatic Display Set type selection (Epoch Start / Normal / palette-only)
- `palette_update_flag` for palette-only updates (no bitmap retransmission)
- Correct `palette_version` sequencing before segment writes
- FATE test: encode/decode roundtrip with palette versioning verification

### B — Quantization API (4 patches)

NeuQuant neural-network colour quantization extracted as a reusable public API:

- `av_quantize_alloc` / `av_quantize_generate_palette` / `av_quantize_apply`
- OkLab perceptual colour space utilities for palette operations
- Palette index remapping and dithering extracted from `vf_paletteuse`
- Existing `vf_paletteuse` FATE tests remain bit-for-bit identical

### C — Subtitle renderer (2 patches)

A public `avfilter_subtitle_render_*` API for rendering text subtitle events to cropped RGBA bitmaps via libass:

- `alloc` / `set_header` / `add_font` / `render_frame`
- `init_event` + `sample` for multi-timepoint rendering (the animation path)
- Stub implementation when `CONFIG_LIBASS` is unset — symbols always exported
- Lives in libavfilter alongside the existing `vf_subtitles.c` libass integration

### D — fftools orchestration (4 patches)

Wires everything together in `fftools/ffmpeg_enc.c`:

- **Text-to-bitmap conversion**: detects text→bitmap mux, renders via Series C, quantizes via Series B, encodes via Series A
- **Rect splitting**: when a subtitle spans top and bottom of screen with a transparent gap, splits into two composition objects. Both halves share one quantized palette — PGS allows only one PDS per Display Set, so independent quantization per half produces wrong colours
- **Animation detection**: renders at every frame interval, classifies pixel changes without parsing any format-specific tags:
  - Alpha-only change → palette-only Normal Display Set (one bitmap, N palette updates)
  - Position-only change → position-only Normal Display Set (no bitmap retransmission)
  - Content change → new Epoch Start per frame
- **Event coalescing**: same-PTS ASS events (common in MKV) buffered and composited before encoding

## Quality

Palette quantization on a representative set of subtitle styles:

| | PSNR |
|-|------|
| Simple white text | 43.86 dB |
| Text with border and shadow | 36.97 dB |
| Multicolour (R/G/B) | **42.37 dB** |
| Gradient stress test | 31.01 dB |

## Animation

A `\fad(300,500)` fade on a 3-second event at 24fps:

```
Pass 1 — scan 72 frames (~2ms): peak opacity found, classified ALPHA
Pass 2 — encode:
  1.000s  Epoch Start: reference bitmap + scaled palette (α ≈ 0%)
  1.042s  Normal PDS:  α ≈ 14%
  ...
  1.292s  Normal PDS:  α ≈ 100%   ← peak frame
  (static frames skipped)
  3.500s  Normal PDS:  α ≈ 80%
  ...
  3.958s  Normal PDS:  α ≈ 8%
  4.000s  Clear
```

All animation types in all text subtitle formats are handled without parsing tags — the renderer output tells us everything.

## Spec compliance

The PGS decoder model is grounded in the Panasonic and Sony HDMV patents (US20090185789A1, US8638861B2, US7620297B2). Buffer sizes, transfer rates, and palette limits are taken from the patents, not reverse-engineered from player behaviour.

[SUPer](https://github.com/cubicibo/SUPer) by cubicibo was used as a hardware-validated reference for palette animation sequences and decoder model compliance. No code or idioms were taken from it.

## FATE tests

11 tests, all passing:

| Test | Covers |
|------|--------|
| `fate-sub-pgs` | Encode/decode roundtrip |
| `fate-quantize` | NeuQuant API |
| `fate-api-pgs-fade` | Encoder state machine (6 scenarios) |
| `fate-api-pgs-animation-util` | Change classifier |
| `fate-api-pgs-animation-timing` | Fade timing model |
| `fate-api-pgs-coalesce` | Event coalescing |
| `fate-api-pgs-rectsplit` | Rect splitting with shared palette |
| `fate-filter-paletteuse-{nodither,bayer,bayer0,sierra2_4a}` | vf_paletteuse unchanged |

## Status

The series is complete and under review. Whether and when it moves upstream is an open question.

## Acknowledgements

Developed with assistance from [Claude](https://claude.ai) (Anthropic).

---

## Historical note

This work grew out of [PunkGraphicStream](punkgraphicstream/), a Java application that converts ASS subtitles to PGS/SUP for Blu-ray authoring. The Java encoder proved the approach and informed the FFmpeg implementation, particularly the PGS composition state machine and decoder model compliance.
