# Plan: NeuQuant Quantizer + Text-to-Bitmap for FFmpeg (Upstream Focus)

## Current Work

_Scratch buffer — what we're doing right now._

v8 development on `pgs8-wip` (master base, off `pgs7`).
1 patch so far: rect bounds validation.

## Outstanding Items

### Quick fixes

- [x] **CI FATE workflow** — added 5 API tests (15 total). `sub-pgs`,
  `sub-pgs-overlap`, `sub-ocr-roundtrip` intentionally excluded (need
  fate-suite samples or pixel-matched system libs).
- [x] **quantizers/index.html** — updated pgs5 → pgs7
- [x] **color-distance/index.html** — checked, no stale links
- [x] **Co-Authored-By** — inconsistent across patches ("Claude Opus 4.6" vs
  "Claude Opus 4.6 (1M context)"). Standardise on next rebase.

### Encoder improvements (deferred from Phase 8)

- [x] **Object version tracking** — already implemented in v5. Reset on
  Epoch Start (line 723), passed to ODS (742), incremented after write (745).
  PHASE8.md said deferred but it was done.
- [ ] **Window bounds validation** — added `rect->x + rect->w <= avctx->width`
  check (on pgs7-8.1, uncommitted). No minimum size in PGS spec, but
  PunkGraphicStream has `minimumSize = 503` pixels for quantizer quality.
  Better approach: pad tiny bitmaps with transparent pixels before
  quantization so NeuQuant gets enough samples, then map the original
  small image to the resulting palette. Goes in fftools or av_quantize_*.
- [ ] **SUPer reference validation** — never compared output against
  cubicibo's hardware-validated reference encoder. Would catch spec
  interpretation differences.

### Rate control (deferred from Phase 13)

- [ ] **CDB event deferral** — current `max_cdb_usage` drops events. Full
  deferral would re-queue events in the fftools event buffer and retry when
  CDB has refilled. Requires changes to `ffmpeg_enc_sub.c` event loop.
  Deferred because `avcodec_encode_subtitle` is synchronous — no EAGAIN.

### Upstream fixes (not our code, but we could submit)

- [ ] **movenc.c** — doesn't write `AV_DISPOSITION_FORCED` to MP4 track
  metadata. The read side (isom.c) handles it. One-line fix.
- [ ] **dvbsubdec.c** — doesn't set `AV_SUBTITLE_FLAG_FORCED` per-rect.
  DVB forced is stream-level only. Bridge via disposition (our Patch 6)
  works around this.
- [ ] **dvbsubenc.c** — doesn't read `AV_SUBTITLE_FLAG_FORCED`. PGS→DVB
  transcoding loses forced flag at the content level.

### Features (discussed, not started)

- [ ] **Subtitle stream merging** — merge forced + non-forced input streams
  into one PGS output. Discussed as "Approach E" (priority queue in fftools
  accepting events from multiple decoders). Significant fftools work.
- [ ] **Over-broad animation detection** — `strchr(rect->ass, '{')` triggers
  multi-timepoint scanning for any ASS override tag, including non-animated
  ones like `{\b1}`. Check for animation-specific tags (`\fad`, `\move`,
  `\t`, `\fade`) instead. Review finding S4 from PHASE8-REVIEW.md.

### Submission

- [ ] **Upstream submission to ffmpeg-devel** — patches ready as 4 independent
  series (see PHASE14.md). Series A (mpegts fix) can go immediately. Series B
  (encoder) is the core value. Need RFC email first (template in PLAN.md §Phase 0).
- [ ] **Rebase onto latest upstream** before submission — both master and 8.1
  may have advanced. Use `scripts/resolve-version-conflicts.sh`.

---

## Context

FFmpeg has no PGS subtitle encoder (ticket #6843) and no way to convert
text subtitles to bitmap format (ticket #3819). This blocks 72 subtitle
conversion pairs (18 text decoders × 4 bitmap encoders).

We build seven things in phases:
1. PGS encoder (self-contained, no dependencies on the rest)
2. Generic color quantization API (pure utility in libavutil)
3. Text-to-bitmap conversion (rendering in libavfilter, orchestrated by fftools)
4. DVD subtitle encoder consolidation (first consumer of shared API)
5. Median Cut + ELBG algorithm integration + GIF cleanup (complete unification)
6. GIF encoder RGBA quantization (direct RGBA-to-GIF without filter pipeline)
7. OCR bitmap-to-text conversion (reverse of Phase 3, via Tesseract)

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
Phase 4:  [PATCH 1/2] Region-weighted quantization          ← DONE (fd72cd4d83, b4ed0c4e82)
Phase 5:  [PATCH 1/5] Median Cut + ELBG algorithm integration  ← DONE
Phase 6:  [PATCH 1/1] GIF encoder RGBA quantization          ← DONE (d215fe732d)
Phase 8:  [PATCH 1/1] PGS decoder model compliance            ← PARTIAL (DTS+palette done in v5; buffer model+AP → Phase 13)
```

Total: ~20 patches across 9 submissions. Each phase is independent
(Phase 8 depends on Phase 1 only).

### Phase dependency for animation

Animation support spans Phase 1 (encoder state machine, now done) and
Phase 3a (animation-aware conversion). Phase 3a calls the encoder with
palette-only Normal Display Sets to produce fade effects.

```
Phase 1 (encoder + composition states) ← DONE
                        │
Phase 3 (text-to-bitmap)  ──→ Phase 3a (universal animation pipeline)
```

Phases 2a, 2b, 5 are unaffected by animation work. Phase 4 (region-weighted
quantization) improves karaoke quality specifically during animation.

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

### Phase 4: Region-weighted quantization (after Phase 3)

```
[PATCH 1/1] lavu/quantize: add region-weighted palette generation
```

When overlapping subtitle events have radically different color profiles
(karaoke + dialogue), NeuQuant allocates palette entries by pixel count,
starving the smaller event. Region-weighted sampling ensures each event
gets fair palette representation. Quality validation uses HyAB distance
(Abasi et al. 2020) — Euclidean OkLab gives misleading results for
sparse per-region palette coverage. See PHASE4.md for full design.

### Phase 5: Algorithm integration (after Phase 2)

```
[PATCH 1/5] libavutil: add Median Cut quantizer algorithm
[PATCH 2/5] lavfi/vf_palettegen: use libavutil Median Cut API
[PATCH 3/5] libavutil: move ELBG from libavcodec to libavutil
[PATCH 4/5] libavutil: add ELBG quantizer algorithm
[PATCH 5/5] lavc/pgssubenc, fftools: add quantize_method option
```

### Phase 6: GIF encoder RGBA quantization (after Phase 5)

```
[PATCH 1/2] lavc/gif: add RGBA input with built-in quantization
[PATCH 2/2] lavc/gif: add dithering support via palette mapping
```

Accepts `AV_PIX_FMT_RGB32` input directly, using `av_quantize_*` for
palette generation and `ff_palette_map_apply()` for dithered mapping.
Simplifies GIF encoding from a complex filter pipeline to a single
encoder call. The `palettegen`+`paletteuse` pipeline remains available
for power users who need cross-frame palette analysis or specific
dithering tuning.

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

## Phase 4 Detail: Region-Weighted Quantization

Extends `av_quantize_*` API with `av_quantize_add_region()` for
multi-event palette generation. When overlapping events contribute to a
single PGS Display Set, equal-weight sampling ensures each event's
colors get fair palette representation regardless of pixel count.

HyAB distance metric was considered but dropped — SSE proved sufficient
to demonstrate the 76% karaoke quality improvement unambiguously.

See [PHASE4.md](PHASE4.md) for full design and implementation notes.

---

## Phase 5 Detail: Algorithm Integration

- Wrap Median Cut from `vf_palettegen.c` as `AV_QUANTIZE_MEDIAN_CUT`
- Wrap ELBG from `libavcodec/elbg.{h,c}` as `AV_QUANTIZE_ELBG`
- Refactor `vf_palettegen.c` to use shared quantizer API

## Phase 6 Detail: GIF Encoder RGBA Quantization

Add `AV_PIX_FMT_RGB32` support to the GIF encoder so users can encode
GIF directly from RGBA without the `palettegen`+`paletteuse` filter
pipeline:

```bash
# Before (complex filter chain)
ffmpeg -i input.mp4 -vf "split[a][b];[a]palettegen[p];[b][p]paletteuse" out.gif

# After (direct encoding)
ffmpeg -i input.mp4 -c:v gif out.gif
```

**Single patch** (originally planned as two, but `ff_palette_map_apply()`
does both mapping and dithering in one call -- splitting would create
a non-dithered intermediate with terrible quality):
- Add `AV_PIX_FMT_RGBA` to accepted pixel formats
- When input is RGBA, use `av_quantize_generate_palette()` for palette
- Use `ff_palette_map_apply()` for dithered index mapping
- Add `quantize_method` option (elbg/mediancut/neuquant, default mediancut)
- Add `dither` option (9 modes from palettemap, default floyd_steinberg)
- Per-frame palette, transparency via reserved slot 255

**Trade-offs vs filter pipeline:**
- Per-frame palette (no cross-frame optimization) — acceptable for most use cases
- No `stats_mode=diff_frames` equivalent — animated GIFs with changing content may use more bandwidth
- Dithering quality equivalent to paletteuse when using same mode
- Global palette option: cache first frame's palette for subsequent frames (optional enhancement)

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
| `libavutil/quantize.{h,c}` | single-buffer quantization only | `av_quantize_add_region()` for multi-event | 4 |
| `vf_palettegen.c:319-392` | get_palette_frame (median cut) | `AV_QUANTIZE_MEDIAN_CUT` | 5 |
| `vf_elbg.c` + `elbg.{h,c}` | ELBG vector quantizer | `AV_QUANTIZE_ELBG` | 5 |
| `gif.c` | PAL8 only, no built-in quantization | RGBA via `av_quantize_*` + `ff_palette_map_apply()` | 6 |

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
| 7 | `libavfilter/subtitle_ocr.{h,c}` | libavfilter |

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
| 4 | `libavutil/quantize.{h,c}` | Add `av_quantize_add_region()` |
| 4 | `fftools/ffmpeg_enc.c` | Use `add_region()` in coalescing path |
| 5 | `libavutil/{quantize.c,quantize.h}` | Add algorithms |
| 5 | `libavfilter/{vf_palettegen.c,vf_elbg.c}`, `libavcodec/gif.c` | Use shared API |
| 7 | `libavfilter/{Makefile,version.h}`, `doc/APIchanges` | OCR API additions |
| 7 | `fftools/ffmpeg_enc.c` | convert_bitmap_to_text + dedup |
| 7 | `fftools/ffmpeg_mux_init.c` | Lift bitmap-to-text gate |
| 7 | `tests/api/Makefile` | OCR test target |

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

### Phase 4: Region-weighted quantization ← DONE
8. Add `av_quantize_add_region()` to quantizer API ← DONE
9. Use `add_region()` in coalescing path for multi-event frames ← DONE
10. ~~Add HyAB distance for quality validation~~ — dropped, SSE sufficient (76% improvement)
11. Verify `make fate` ← DONE

### Phase 5: Algorithm integration
10. Extract Median Cut from vf_palettegen as `AV_QUANTIZE_MEDIAN_CUT`
11. Refactor vf_palettegen to use shared quantizer API
12. Wrap ELBG as `AV_QUANTIZE_ELBG`
13. Verify `make fate`

### Phase 6: GIF encoder RGBA quantization ← DONE
14. Add RGBA input with built-in quantization + dithering to GIF encoder ← DONE (d215fe732d)
15. Verify `make fate` ← DONE

### Phase 7: OCR bitmap-to-text subtitle conversion ← DONE
Reverse of Phase 3: bitmap subtitles (PGS, DVB, DVD, XSUB) to text
(ASS, SRT, WebVTT, MOV text) via Tesseract OCR. Unlocks 24 conversion
pairs (4 bitmap decoders x 6 text encoders).

**Design decisions:**
- Symmetric to Phase 3: library in libavfilter, orchestration in fftools
- `subtitle_ocr.{h,c}` mirrors `subtitle_render.{h,c}` API pattern
- Gated on `CONFIG_LIBTESSERACT` (stubs when unavailable)
- Buffered bitmap dedup: palette-only changes (PGS fades) skip OCR
- Min-duration filtering (200ms) discards stray fade frames
- Position mapping: bitmap (x,y) to `\an`/`\pos`/`\move` tags

**Two-patch structure (matching Phase 3):**
1. `lavfi: add bitmap subtitle OCR utility` — subtitle_ocr.{h,c}, API test
2. `fftools: auto-convert bitmap subtitles to text via OCR` — dedup, positioning

**Status:**
- [x] Patch 1: Library API (subtitle_ocr.h/c, Makefile, version, APIchanges)
- [x] Patch 1: API unit test (api-subtitle-ocr-test.c)
- [x] Patch 2: convert_bitmap_to_text() with bitmap dedup
- [x] Patch 2: Buffered dedup with EOS flush
- [x] Patch 2: Bitmap-to-text gate lifted in ffmpeg_mux_init.c
- [x] Clean build (no warnings)
- [x] FATE roundtrip test (sub-ocr-roundtrip, gated on CONFIG_LIBTESSERACT)
- [x] Commits on pgs-series and pgs-series-8.0.1
- [x] Language coverage: 105/114 pass (92%), documented in PHASE7.md
- [x] Release builds with Tesseract (CI, `-eng` variant with tessdata)

### Phase 8: PGS decoder model compliance ← PARTIAL
16. ~~Compute DTS/PTS per HDMV timing formulas~~ ← DONE (v5, Phase 10b)
17. Validate coded data buffer (1 MB leaky bucket) ← deferred to Phase 13e
18. Track decoded object buffer (4 MB) ← deferred to Phase 13e
19. Insert Acquisition Points at configurable interval ← deferred to Phase 13c
20. ~~Optimize PDS to write only active palette entries~~ ← DONE (v5, Phase 10a)
21. Track object version numbers ← deferred to Phase 13
22. FATE tests for timing, buffer model, palette size ← partially done (DTS test in v5)
23. Verify against SUPer reference output ← deferred

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

# Phase 4 (region-weighted quantization)
make -j$(nproc) && make fate
# Karaoke test: overlapping events with different color profiles
./ffmpeg -i karaoke_test.ass -c:s pgssub -s 1920x1080 /tmp/karaoke.sup

# Phase 5 (algorithm integration)
make -j$(nproc) && make fate  # all quantizers unified

# Phase 6 (GIF RGBA quantization)
make -j$(nproc) && make fate
./ffmpeg -i input.mp4 -c:v gif -quantizer mediancut -dither floyd_steinberg /tmp/test.gif
```

## Detailed Phase Documentation

| Phase | Document | Status |
|-------|----------|--------|
| Phase 1 + 1a | [PHASE1.md](PHASE1.md) | Retrospective + amendment plan |
| Phase 2a + 2b | [PHASE2.md](PHASE2.md) | Retrospective (complete) |
| Phase 3 + 3a | [PHASE3.md](PHASE3.md) | Retrospective + animation plan |
| Phase 4 | [PHASE4.md](PHASE4.md) | Region-weighted quantization |
| Phase 5 | [PHASE5.md](PHASE5.md) | Algorithm integration |
| Phase 6 | [PHASE6.md](PHASE6.md) | GIF encoder RGBA quantization |
| Phase 7 | [PHASE7.md](PHASE7.md) | OCR bitmap-to-text conversion |
| Phase 8 | [PHASE8.md](PHASE8.md) | PGS decoder model compliance |

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
