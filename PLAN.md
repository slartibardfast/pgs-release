# Plan: NeuQuant Quantizer + Text-to-Bitmap for FFmpeg (Upstream Focus)

## Context

FFmpeg has no PGS subtitle encoder (ticket #6843) and no way to convert
text subtitles to bitmap format (ticket #3819). This blocks 72 subtitle
conversion pairs (18 text decoders × 4 bitmap encoders).

We build five things in phases:
1. PGS encoder (self-contained, no dependencies on the rest)
2. Generic color quantization API (pure utility in libavutil)
3. Text-to-bitmap conversion (rendering in libavfilter, orchestrated by fftools)
4. DVD subtitle encoder consolidation (first consumer of shared API)
5. Median Cut + ELBG algorithm integration + GIF cleanup (complete unification)

## Upstream Acceptance Intelligence

### Key risks

| Risk | Evidence | Mitigation |
|------|----------|-----------|
| **Text-to-bitmap rejected in 2022** | [Coza's 12-patch series](https://patchwork.ffmpeg.org/project/ffmpeg/cover/20220503161328.842587-1-traian.coza@gmail.com/) — wrong location (libavcodec), called "hacky" | Rendering in libavfilter (where libass lives); send RFC first |
| **Subtitle filtering is contentious** | Active [RFC](https://www.mail-archive.com/ffmpeg-devel@ffmpeg.org/msg181951.html) (May 2025) + [$1000 bounty dispute](https://www.mail-archive.com/ffmpeg-devel@ffmpeg.org/msg181991.html) | Don't build subtitle filter infrastructure; use utility function pattern |
| **Large series get stuck** | softworkz's 25-patch set: 9 versions, never merged | 2-4 patches per phase |
| **AI code policy** | [RFC July 2025](https://www.mail-archive.com/ffmpeg-devel@ffmpeg.org/msg183437.html) — AMD patch rejected as "AI slop" | Disclose assistance; thorough human review |
| **Ticket #6843** | [PGS encoder requested](https://trac.ffmpeg.org/ticket/6843) | Reference in commits |
| **Ticket #3819** | [Subtitle type incompatibility](https://trac.ffmpeg.org/ticket/3819) | Phase 3 addresses directly |

### Key reviewers

- **Lynne** — skeptical of subtitle architecture changes
- **Anton Khirnov** — design purity
- **Hendrik Leppkes** — subtitle architecture
- **Michael Niedermayer** — security, edge cases

## Architecture

### Library boundaries

```
libavutil    ← quantizer API, OkLab, dithering, palette mapping
libavcodec   ← encoders (accept SUBTITLE_BITMAP only, unchanged)
libavfilter  ← text-to-RGBA rendering (where libass already lives)
fftools      ← detect type mismatch, orchestrate render + quantize
```

Dependency order: `libavutil ← libavcodec ← libavformat ← libavfilter ← fftools`

- libavfilter already has libass (`vf_subtitles.c`)
- libavcodec CANNOT depend on libavfilter
- Putting libass in libavcodec was the 2022 rejection reason
- fftools already orchestrates sub2video — same pattern

### Data flow (Phase 3)

```
AVSubtitleRect (SUBTITLE_ASS or SUBTITLE_TEXT)
  │
  ▼ fftools/ffmpeg_enc.c detects type mismatch
  │
  ├─ avfilter_subtitle_render_frame()         ← libavfilter (libass)
  │   └─ rasterize → composite → crop → RGBA buffer
  ├─ av_quantize_generate_palette()          ← libavutil
  ├─ av_quantize_apply()                     ← libavutil
  └─ rewrite rect: type=BITMAP, data[0]=indices, data[1]=palette
  │
  ▼ AVSubtitleRect (type=SUBTITLE_BITMAP)
  │
  ▼ any bitmap encoder (pgssub, dvbsub, dvdsub, xsub)
```

## Submission Strategy

Tests are included in the same patch as the code they test (supporting evidence, not separate patches).

```
Phase 0:  RFC email
Phase 1:  [PATCH 1/1] PGS encoder + composition states    ← DONE (2cc882f669), includes state machine
Phase 2a: [PATCH 1/2] OkLab move + Quantizer API          ← DONE (8e60ec654f, 8d7abb5328)
Phase 2b: [PATCH 1/2] Palette mapping extraction           ← DONE (3326aa9602, 557d01153a)
Phase 3:  [PATCH 1/2] Text-to-bitmap + rect splitting     ← DONE
Phase 3a: [PATCH 1/1] Text-to-bitmap: universal animation ← DONE
Phase 4:  [PATCH 1/1] DVD subtitle consolidation           ← first consumer of shared API
Phase 5:  [PATCH 1/4] Median Cut + ELBG + GIF cleanup      ← complete unification
```

Total: ~16 patches across 7 submissions. Each phase is independent.

### Phase dependency for animation

Animation support spans Phase 1 (encoder state machine, now done) and
Phase 3a (animation-aware conversion). Phase 3a calls the encoder with
palette-only Normal Display Sets to produce fade effects.

```
Phase 1 (encoder + composition states) ← DONE
                        │
Phase 3 (text-to-bitmap)  ──→ Phase 3a (universal animation pipeline)
```

Phases 2a, 2b, 4, 5 are unaffected by animation work.

### Phase 0: RFC email (before any patches)

```
Subject: [RFC] PGS subtitle encoder, quantization API,
         and text-to-bitmap subtitle conversion

I'm working on a PGS subtitle encoder (ticket #6843) and
text-to-bitmap subtitle conversion (ticket #3819) that would
unlock 72 currently broken decoder-encoder pairs.

Architecture:
- Color quantization API in libavutil (NeuQuant with OkLab,
  variable palette 2-256, dithering extracted from vf_paletteuse)
- Text rendering utility in libavfilter (where libass already
  lives via vf_subtitles.c — no new external dependencies)
- Orchestration in fftools (same pattern as sub2video)
- Encoders unchanged — still accept SUBTITLE_BITMAP only

The rendering lives in libavfilter because libavcodec cannot
depend on libass (Coza's 2022 series was rejected for this).
The API is public (avfilter_subtitle_render_*) because ff_
symbols are invisible to fftools in shared builds.

I deliberately avoided building subtitle filter infrastructure
(buffersrc/sink, AVFrame subtitle support). The subtitle
filtering discussion has fundamental unresolved design
questions (sparse/overlapping event timing vs contiguous
frame scheduling). Our utility function approach is orthogonal
— it works with existing AVSubtitle, requires no AVFrame
changes, and can serve as a building block for a future
text2graphicsub filter if that infrastructure lands.

The implementation is 2 patches, ~500 lines. Each phase in
the series is independently useful and independently testable.

Code at [repo URL]. Tested with roundtrip encode/decode.
```

### Phase 1: PGS encoder — DONE, amendment pending

```
[PATCH 1/1] lavc/pgssubenc: add HDMV PGS subtitle encoder
```

Includes encoder, FATE test, reference CRC. Committed `2cc882f669`.

Composition state machine included: automatic state detection (Epoch Start,
Normal, Acquisition Point), palette_update_flag, palette_version tracking.
See PHASE1.md for design details grounded in patents US20090185789A1,
US8638861B2, and US7620297B2.

### Phase 2a: Quantizer API + NeuQuant — DONE

```
[PATCH 1/2] lavu: move OkLab palette utilities from libavfilter  (8e60ec654f)
[PATCH 2/2] lavu: add color quantization API with NeuQuant       (8d7abb5328)
```

Patch 1 is a pure refactor (palette.{h,c} move, include updates, no functional change).
Patch 2 includes quantize.h, quantize.c, neuquant.{h,c}, tests/quantize.c,
version bump (MINOR 25→26), and APIchanges — one logical unit with its test.

### Phase 2b: Palette mapping extraction — DONE

```
[PATCH 1/2] lavu: extract palette mapping and dithering from vf_paletteuse  (3326aa9602)
[PATCH 2/2] lavfi/vf_paletteuse: use libavutil palette mapping              (557d01153a)
```

Patch 1 creates palettemap.{h,c} with KD-tree colormap, hash cache, and 9 dithering
algorithms. Internal API only (ff_ prefix). Struct names kept verbatim from original;
FFColorInfo etc. noted as future work.
Patch 2 removes ~570 lines from vf_paletteuse.c, replacing with ff_palette_map_*() calls.
All 4 paletteuse FATE tests produce bit-for-bit identical output.

### Phase 3: Universal text-to-bitmap — DONE, amendment pending

Rendering in libavfilter, orchestration in fftools. Requires `--enable-libass`.

```
[PATCH 1/2] lavfi: add text subtitle rendering utility (via libass)   (0b803170ab)
[PATCH 2/2] fftools: auto-convert text subtitles to bitmap for encoding (9c953175c6)
```

Unlocks 72 text→bitmap subtitle conversion pairs (e.g. SRT→PGS, ASS→DVB).
FATE tests pending (font rendering is platform-dependent; structural test needed).

**Rect splitting (uncommitted):** Scan rendered RGBA for transparent gaps,
split into 2 composition objects when gap > 32 rows. Implemented in
`convert_text_to_bitmap()`, needs FATE coverage.

**Amendment (Phase 3a):** Universal animation pipeline — multi-timepoint
rendering via `init_event()`/`sample()` API, format-agnostic change
classification (ALPHA/POSITION/CONTENT), and optimal PGS Display Set
encoding per change type. Handles fades, motion, and complex transforms
without parsing format-specific tags. See PHASE3.md for full details.

### Phase 4: DVD subtitle consolidation (after Phase 2)

```
[PATCH 1/1] lavc/dvdsubenc: use libavutil quantizer and palette mapping
```

Single patch replacing ~130 lines of ad-hoc code with shared API calls.

### Phase 5: Algorithm integration + GIF cleanup (after Phase 2)

```
[PATCH 1/4] libavutil: add Median Cut quantizer algorithm
[PATCH 2/4] lavfi/vf_palettegen: use libavutil quantizer API
[PATCH 3/4] libavutil: add ELBG quantizer algorithm
[PATCH 4/4] lavc/gif: use libavutil palette mapping
```

### Patch discipline

- 4-space indent, no tabs, 80-char lines
- K&R style, `snake_case` functions, `CamelCase` types
- Each patch compiles and passes `make fate` independently
- Commit messages: `area: short description\n\ndetails`
- No cosmetic + functional changes mixed
- Tests included with the code they test (not separate patches)

### Upstream requirements for new public API (Phase 2a)

| Requirement | Action |
|-------------|--------|
| Version bump | `libavutil/version.h`: MINOR 25→26, MICRO→100 |
| APIchanges | `doc/APIchanges`: list new public functions |
| Header guard | `AVUTIL_QUANTIZE_H` |
| Free naming | `av_quantize_freep()` (`**ptr` pattern) |
| Opaque context | Struct definition in .c only |
| Errors | Alloc returns NULL; operations return negative `AVERROR` |
| Doxygen | `@param[in]`, `@param[out]`, `@return`, `@note` |

Public API required: Phases 3, 4, 5 all call `av_quantize_*` cross-library.

---

## Phase 1 Detail: PGS Encoder

**DONE — committed `2cc882f669`.**

Implemented in `ffmpeg/libavcodec/pgssubenc.c`:
- PGS RLE encoding per HDMV spec, ODS fragmentation for >64KB
- Up to 2 composition objects, AABB overlap rejection
- Frame rate from `avctx->framerate` with AVOption override
- Color space from `avctx->colorspace` (BT.709/BT.601), height fallback
- Forced subtitle flag, epoch-start composition state
- FATE test: `fate-sub-pgs` using `pgs_sub.sup` FATE sample

---

## Phase 2 Detail: Quantizer API

### API (`libavutil/quantize.h`)

```c
#ifndef AVUTIL_QUANTIZE_H
#define AVUTIL_QUANTIZE_H

#include <stdint.h>

enum AVQuantizeAlgorithm {
    AV_QUANTIZE_NEUQUANT,
};

enum AVDitheringMode {
    AV_DITHER_NONE,
    AV_DITHER_BAYER,
    AV_DITHER_HECKBERT,
    AV_DITHER_FLOYD_STEINBERG,
    AV_DITHER_SIERRA2,
    AV_DITHER_SIERRA2_4A,
    AV_DITHER_SIERRA3,
    AV_DITHER_BURKES,
    AV_DITHER_ATKINSON,
    AV_DITHER_NB
};

typedef struct AVQuantizeContext AVQuantizeContext;

AVQuantizeContext *av_quantize_alloc(enum AVQuantizeAlgorithm algorithm,
                                     int max_colors);
void av_quantize_freep(AVQuantizeContext **ctx);

int av_quantize_generate_palette(AVQuantizeContext *ctx,
                                  const uint8_t *rgba, int nb_pixels,
                                  uint32_t *palette, int quality);

int av_quantize_apply(AVQuantizeContext *ctx,
                       const uint8_t *rgba, uint8_t *indices,
                       int nb_pixels);

int av_palette_apply(const uint32_t *palette, int nb_colors,
                      uint8_t *rgba, uint8_t *indices,
                      int w, int h, enum AVDitheringMode dither);

#endif /* AVUTIL_QUANTIZE_H */
```

### NeuQuant port (from neuquant32/pngnq)

| Enhancement | Rationale |
|-------------|-----------|
| OkLab color space | Perceptual distance, reuse moved `palette.c` |
| Heap-allocated state | Thread-safe, no globals |
| RGBA byte order | FFmpeg convention (not ABGR) |
| `max_colors` 2–256 | DVD=4, DVB=16, PGS=256 |
| Alpha-aware distance | `colorimportance()` weighting |

### NeuQuant copyright (must preserve)

```
Copyright (c) 1994 Anthony Dekker
Modified for RGBA: Copyright (c) 2004-2006 Stuart Coyle
Rewritten: Copyright (c) 2009 Kornel Lesiński
Adapted for FFmpeg: Copyright (c) 2026 David Connolly
[Original permissive license — attribution only]
```

### Palette mapping extraction from `vf_paletteuse.c` (Phase 2b)

| Component | Lines | Destination |
|-----------|-------|-------------|
| `dither_color()` | 157-164 | `palettemap.c` |
| `diff()` (OkLab distance) | 166-182 | `palettemap.c` |
| `colormap_nearest_node()` | 196-224 | `palettemap.c` |
| `colormap_nearest()` | 226-231 | `palettemap.c` |
| `color_get()` | 242-269 | `palettemap.c` |
| `colormap_insert()` | 595+ | `palettemap.c` |
| `set_frame()` (9 dithering) | 292-420 | `palettemap.c` |

---

## Phase 3 Detail: Universal Text→Bitmap

### The problem: 72 broken pairs

18 text decoders (all produce `SUBTITLE_ASS`) × 4 bitmap encoders
(all require `SUBTITLE_BITMAP`) = 72 pairs that fail at runtime.
Tracked as ticket #3819.

### Prior art

Three prior attempts inform our design:

**1. Clement Boesch's AVFrame subtitle WIP (Nov 2016)**
Proposed putting subtitles into AVFrame via `extended_data` pointers.
wm4 raised concerns about memory management, ABI stability, and display
time semantics. Never completed. Established the idea but exposed the
fundamental mismatch: subtitle events are sparse and overlapping, unlike
contiguous audio/video frames.

**2. Coza's text-to-bitmap series (May 2022, 12 patches, rejected)**
Put libass rendering directly in libavcodec. Paul B Mahol: "this needs to
be in libavfilter instead of libavcodec." Rejection was clear: libavcodec
is the wrong place for libass.

**3. softworkz's subtitle filtering (Sep 2021–Jun 2025, 25 patches, 9+ versions, unmerged)**
Full subtitle filter infrastructure: AVFrame subtitle fields, frame-based
codec API, sbuffersrc/sbuffersink, ~15 subtitle filters. Includes
`text2graphicsub` which does exactly what Phase 3 does — as a filter node.

Why it hasn't merged after 4+ years:
- The timing problem: subtitle events are sparse (gaps) and non-exclusive
  (overlapping). softworkz solved with dual timing fields on AVFrame.
  Hendrik Leppkes, Anton Khirnov, and Lynne objected to the API design.
- Series size: 25 patches touching 89 files.
- softworkz offered $1,000 bounty for alternative without dual timing.
  No taker as of Jun 2025. Status: deadlocked.

### Architecture: utility function, not subtitle filters

Our approach sidesteps the subtitle filtering debate:
- **No AVFrame changes.** We work with existing `AVSubtitle`/`AVSubtitleRect`.
- **No filter graph involvement.** Same pattern as sub2video (~200 lines in
  `ffmpeg_filter.c` that convert bitmap subtitles to video frames entirely
  in fftools, without formal filter infrastructure).
- **No opinion on the timing debate.** No subtitle frame scheduling needed.
- **Compatible with future subtitle filters.** Our rendering utility becomes
  a building block for a future `text2graphicsub` filter if one lands.

**Why not libavcodec?** Coza's 2022 rejection. libavcodec→libass is a
dependency violation.

**Why public API (`avfilter_subtitle_render_*`)?** `ff_` symbols are invisible
to fftools in shared builds. fftools must call the renderer. Requires version
bump + APIchanges.

### Rendering utility (`libavfilter/subtitle_render.{h,c}`)

Public API, CONFIG_LIBASS gated (stubs return AVERROR(ENOSYS) without libass):

```c
AVSubtitleRenderContext *avfilter_subtitle_render_alloc(int canvas_w,
                                                        int canvas_h);
void avfilter_subtitle_render_freep(AVSubtitleRenderContext **ctx);
int avfilter_subtitle_render_set_header(AVSubtitleRenderContext *ctx,
                                         const char *header);
int avfilter_subtitle_render_add_font(AVSubtitleRenderContext *ctx,
                                       const char *name,
                                       const uint8_t *data, int size);
int avfilter_subtitle_render_frame(AVSubtitleRenderContext *ctx,
                                    const char *text,
                                    int64_t start_ms, int64_t duration_ms,
                                    uint8_t **rgba, int *linesize,
                                    int *x, int *y, int *w, int *h);
```

Rendering flow: ASS event → `ass_process_chunk()` → `ass_render_frame()` →
`ASS_Image` linked list → composite alpha masks onto transparent RGBA canvas →
crop bounding box → return RGBA + position. Caller (fftools) quantizes to
palette using `av_quantize_*`.

### Orchestration (`fftools/ffmpeg_enc.c`)

In `do_subtitle_out()`, detect text→bitmap mismatch before encoding:
1. Lazy-init renderer (canvas_size, ASS header, font attachments)
2. Render each text rect → RGBA
3. Quantize RGBA → palette + indices via `av_quantize_*`
4. Rewrite rect: type=BITMAP, data[0]=indices, data[1]=palette
5. Continue to `avcodec_encode_subtitle()`

Relax text→bitmap gate in `fftools/ffmpeg_mux_init.c` (probe-and-free pattern
to detect libass availability at runtime without preprocessor conditionals).

### Clip-box splitting (top/bottom composition objects)

PGS supports up to 2 non-overlapping composition objects per display set.
When a rendered bitmap contains a horizontal transparent gap (e.g. text
at top and bottom of screen), the conversion splits it into 2 rects.
This avoids wasting bandwidth on transparent pixels spanning the gap.

Algorithm: scan rows of rendered RGBA for fully-transparent runs. If a
gap exceeds a threshold (32 rows), quantize the full RGBA image first,
then split the index buffer into top and bottom halves. Both halves share
one palette — PGS allows only one PDS (palette) per Display Set, so
independent quantization per half would produce incorrect colors for the
second rect (the encoder writes only `rects[0]->data[1]` as the PDS).

### Animation pipeline (Phase 3a)

Palette animation and position animation are core to the text-to-bitmap
layer — they determine output quality for common ASS effects. The encoder
composition state machine (Phase 1, done) provides the foundation —
Phase 3a builds the animation-aware conversion layer on top.

**Encoder support (done in Phase 1):**
- Composition states: Epoch Start / Acquisition Point / Normal
- palette_update_flag: emit PDS-only Display Sets (no WDS or ODS)
- palette_version: increment within epoch
- See PHASE1.md for encoder specification

**Phase 3a — Universal subtitle animation:**
- Multi-timepoint rendering via `init_event()` + `sample()` API
- Every-frame scan gated by format hint (SUBTITLE_ASS with `{`)
- Classify changes: ALPHA (fade), POSITION (motion), CONTENT (complex)
- ALPHA: quantize peak frame, palette-only Normal DS chain
- POSITION: quantize once, position-only Normal DS chain
- CONTENT: independent quantization per frame, Epoch Start each
- See PHASE3.md for full animation pipeline specification

**Decoder model constants (from patents, validated by hardware testing):**

| Constant | Value | Source |
|----------|-------|--------|
| Rx | 2 MB/s (16 Mbps) | US7620297B2 |
| Rd | 16 MB/s (128 Mbps) | US7620297B2 |
| Rc | 32 MB/s (256 Mbps) | US7620297B2 |
| Coded Data Buffer | 1 MB | US8638861B2 |
| Decoded Object Buffer | 4 MB | US8638861B2 |
| Max objects/epoch | 64 | US8638861B2 |
| Max palettes/epoch | 8 | US8638861B2 |

A palette-only Display Set (~1300 bytes) takes <0.65 ms at Rx = 2 MB/s,
enabling 60+ palette updates/second without buffer overflow. This is the
foundation for smooth fade animation.

### FATE testing

Font rendering is platform-dependent (FreeType version, fontconfig).
Structural unit test validates render API and rect splitting. Integration
test verifies SRT→PGS pipeline produces valid output. Gated on CONFIG_LIBASS.

### Usage

```bash
ffmpeg -i input.srt -c:s pgssub -s 1920x1080 output.sup
ffmpeg -i input.ass -c:s pgssub -s 1920x1080 output.sup
ffmpeg -i input.webvtt -c:s dvbsub output.ts
ffmpeg -i movie.mkv -map 0:s -c:s pgssub -s 1920x1080 output.sup
```

---

## Phase 4 Detail: DVD Subtitle Consolidation

Replace `dvdsubenc.c`'s ad-hoc color matching with shared quantizer API.
Replaces ~130 LOC (`color_distance`, `count_colors`, `select_palette`,
`build_color_map`) with `av_quantize_generate_palette()` + `av_palette_apply()`.
Upgrades to OkLab perceptual distance, adds dithering (critical at 4 colors).

---

## Phase 5 Detail: Algorithm Integration + GIF

- Wrap Median Cut from `vf_palettegen.c` as `AV_QUANTIZE_MEDIAN_CUT`
- Wrap ELBG from `libavcodec/elbg.{h,c}` as `AV_QUANTIZE_ELBG`
- Replace `gif.c` palette code with shared API

---

## Existing FFmpeg Code to Reuse

| Component | Location | Phase |
|-----------|----------|-------|
| OkLab ↔ sRGB | `libavutil/palette.{h,c}` | 2a — DONE (moved from libavfilter/) |
| KD-Tree + 9 dithering | `libavfilter/vf_paletteuse.c` | 2b — extract to `libavutil/palettemap.c` |
| libass rendering | `libavfilter/vf_subtitles.c` | 3 — pattern for `subtitle_render.c` |
| ELBG quantizer | `libavcodec/elbg.{h,c}` | 5 — wrap as `AV_QUANTIZE_ELBG` |
| Median Cut | `libavfilter/vf_palettegen.c` | 5 — wrap as `AV_QUANTIZE_MEDIAN_CUT` |

### Code to consolidate

| Location | Current code | Replacement | Phase |
|----------|-------------|-------------|-------|
| `dvdsubenc.c:100-235` | color_distance, count_colors, select_palette, build_color_map | `av_quantize_*` + `av_palette_apply()` | 4 |
| `gif.c:67-97` | shrink_palette, remap_frame_to_palette | `av_palette_apply()` | 5 |
| `vf_palettegen.c:319-392` | get_palette_frame (median cut) | `AV_QUANTIZE_MEDIAN_CUT` | 5 |
| `vf_elbg.c` + `elbg.{h,c}` | ELBG vector quantizer | `AV_QUANTIZE_ELBG` | 5 |

---

## Complete File Manifest

### New files

| Phase | File | Library |
|-------|------|---------|
| 2a | `libavutil/palette.{h,c}` | libavutil (moved from libavfilter/) |
| 2a | `libavutil/quantize.h` | libavutil |
| 2a | `libavutil/quantize.c` | libavutil |
| 2a | `libavutil/neuquant.{h,c}` | libavutil |
| 2a | `libavutil/tests/quantize.c` | libavutil |
| 2b | `libavutil/palettemap.c` | libavutil |
| 3 | `libavfilter/subtitle_render.{h,c}` | libavfilter |
| 3a | `fftools/ffmpeg_subtitle_animation.c` | fftools |
| 3a | `tests/api/api-pgs-fade-test.c` | tests |
| 3a | `tests/api/api-pgs-animation-util-test.c` | tests |
| 5 | `libavutil/mediancut.c` | libavutil |

### Modified files

| Phase | File | Change |
|-------|------|--------|
| 1 | `libavcodec/{pgssubenc.c,Makefile,allcodecs.c}` | DONE |
| 1 | `tests/{fate/subtitles.mak,ref/fate/sub-pgs}` | DONE |
| 2a | `libavutil/{Makefile,version.h}`, `doc/APIchanges` | DONE |
| 2a | `libavfilter/{Makefile,vf_palettegen.c,vf_paletteuse.c}` | DONE |
| 2b | `libavfilter/vf_paletteuse.c` | Use `av_palette_apply()` |
| 3 | `libavfilter/Makefile`, `fftools/ffmpeg_enc.c` | Rendering + orchestration |
| 3a | `libavfilter/subtitle_render.{h,c}` | Add init_event + sample API |
| 3a | `libavcodec/pgssubenc.c` | palette_version ordering fix |
| 3a | `fftools/ffmpeg_enc.c` | Animation dispatch + orchestration |
| 3a | `tests/api/Makefile`, `tests/fate/api.mak` | FATE test targets |
| 4 | `libavcodec/dvdsubenc.c` | Use shared quantizer |
| 5 | `libavutil/{quantize.c,quantize.h}` | Add algorithms |
| 5 | `libavfilter/{vf_palettegen.c,vf_elbg.c}`, `libavcodec/gif.c` | Use shared API |

---

## Implementation Order

### Phase 1: DONE (encoder + composition state machine)
Committed `2cc882f669` in ffmpeg submodule. Includes composition state
machine, palette_update_flag, palette_version tracking, and Acquisition
Point support.

### Phase 2a: DONE
Committed `8e60ec654f` (palette move) and `8d7abb5328` (quantizer API) in ffmpeg submodule.

### Phase 2b: DONE
Committed `3326aa9602` (extract) and `557d01153a` (refactor filter).

### Phase 3 + 3a: DONE
All committed on `pgs-series` branch, reorganized into 4 independent
submission series (A: PGS encoder, B: quantization, C: renderer,
D: text-to-bitmap). See plan file for series details.

### Phase 4: DVD subtitle consolidation
8. Replace dvdsubenc.c with shared API calls
9. Verify `make fate`

### Phase 5: Algorithm integration + GIF
10. Add Median Cut + ELBG algorithms, refactor palettegen + elbg + gif
11. Verify `make fate`

## Verification

```bash
# Phase 1 (done)
FATE_SAMPLES=/tmp/fate-samples make fate-sub-pgs

# Phase 1 (encoder + composition states, done)
FATE_SAMPLES=/tmp/fate-samples make fate-sub-pgs  # passes

# Phase 2 (done)
make -j$(nproc) && make fate  # no behavior change

# Phase 3 (requires --enable-libass)
./configure --enable-libass --disable-doc && make -j$(nproc)
./ffmpeg -i test.srt -c:s pgssub -s 1920x1080 /tmp/t2b.sup
./ffprobe -v error -show_streams /tmp/t2b.sup | grep hdmv_pgs

# Phase 3a (animation pipeline)
FATE_SAMPLES=/tmp/fate-samples make fate-api-pgs-fade fate-api-pgs-animation-util
./ffmpeg -i test_fade.ass -c:s pgssub -s 1920x1080 /tmp/fade.sup
./ffprobe -v error -show_packets /tmp/fade.sup  # verify multiple display sets

# Phase 4-5
make -j$(nproc) && make fate  # all quantizers unified
```

## Detailed Phase Documentation

| Phase | Document | Status |
|-------|----------|--------|
| Phase 1 + 1a | [PHASE1.md](PHASE1.md) | Retrospective + amendment plan |
| Phase 2a + 2b | [PHASE2.md](PHASE2.md) | Retrospective (complete) |
| Phase 3 + 3a | [PHASE3.md](PHASE3.md) | Retrospective + animation plan |

## References

### Patents (specification basis)

| Patent | Assignee | Covers |
|--------|----------|--------|
| US20090185789A1 | Panasonic | Stream shaping, decoder model, composition states |
| US8638861B2 | Sony | Segment syntax, buffering model, timing constraints |
| US7620297B2 | Panasonic | Decoder model, object buffer management, transfer rates |

### Compiled specification

- `docs/pgs-specification.md` — Synthesized from patents + reverse engineering

### Reference implementations (cited for spec interpretation, not code)

- FFmpeg `libavcodec/pgssubdec.c` — Reference decoder
- SUPer — Hardware-validated PGS encoder (composition state transitions,
  decoder model compliance, palette animation sequences)

## Release Builds

Pre-built binaries are distributed via GitHub Actions (`ffmpeg-release.yml`).

### Configuration

Minimal subtitle-focused FFmpeg build (`--disable-everything` + selective enables):
- **Subtitle decoders**: ASS, SSA, SRT, WebVTT, PGS, DVB, DVD, XSUB, MOV text, and others
- **Subtitle encoders**: pgssub (ours), ASS, SRT, WebVTT, MOV text, DVB
- **Container muxers/demuxers**: MKV, MP4, MOV, MPEG-TS, SUP, SRT, ASS, WebVTT, AVI (for copy)
- **Parsers**: H.264, HEVC, AV1, VP9, AAC, AC3 (bitstream framing for `-c copy`)
- **BSFs**: h264_mp4toannexb, hevc_mp4toannexb, aac_adtstoasc, extract_extradata
- **Filters**: subtitles (libass overlay), null, anull, copy, acopy
- **libass**: statically linked (from pinned `libass/` submodule on Windows; system package on Linux/macOS)

### Targets

| Target | Runner | Toolchain |
|--------|--------|-----------|
| linux-x86_64 | ubuntu-24.04 | native gcc |
| linux-arm64 | ubuntu-24.04-arm | native gcc |
| macos-x86_64 | macos-13 | native clang |
| macos-arm64 | macos-14 | native clang |
| windows-x86_64 | ubuntu-24.04 | mingw-w64 cross |
| windows-arm64 | ubuntu-24.04 | llvm-mingw cross |
