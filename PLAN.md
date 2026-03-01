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
  ├─ ff_subtitle_render_text()               ← libavfilter (libass)
  │   └─ rasterize → composite → crop → RGBA buffer
  ├─ av_quantize_generate_palette()          ← libavutil
  ├─ av_palette_apply(dither=...)            ← libavutil
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
Phase 1:  [PATCH 1/1] PGS encoder + FATE test          ← DONE (6c96a661fd)
Phase 2a: [PATCH 1/2] OkLab move + Quantizer API        ← DONE (118a2b9b2c, 30dd94d8e0)
Phase 2b: [PATCH 1/2] Palette mapping extraction         ← DONE (246b8f30f7, 2d1cbaadd8)
Phase 3:  [PATCH 1/2] Text-to-bitmap conversion + tests  ← unlocks 72 pairs
Phase 4:  [PATCH 1/1] DVD subtitle consolidation         ← first consumer of shared API
Phase 5:  [PATCH 1/4] Median Cut + ELBG + GIF cleanup    ← complete unification
```

Total: 14 patches across 6 submissions. Each phase is independent.

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

I avoided libavcodec for the rendering because libavcodec
cannot depend on libavfilter, and libass naturally belongs
in libavfilter. I avoided building subtitle filter infrastructure
(buffersrc/sink, AVFrame subtitle support) as that overlaps
with the ongoing subtitle filtering discussion.

Code at [repo URL]. Tested with roundtrip encode/decode.
```

### Phase 1: PGS encoder — DONE

```
[PATCH 1/1] lavc/pgssubenc: add HDMV PGS subtitle encoder
```

Includes encoder, FATE test, reference CRC. Committed `6c96a661fd`.

### Phase 2a: Quantizer API + NeuQuant — DONE

```
[PATCH 1/2] lavu: move OkLab palette utilities from libavfilter  (118a2b9b2c)
[PATCH 2/2] lavu: add color quantization API with NeuQuant       (30dd94d8e0)
```

Patch 1 is a pure refactor (palette.{h,c} move, include updates, no functional change).
Patch 2 includes quantize.h, quantize.c, neuquant.{h,c}, tests/quantize.c,
version bump (MINOR 25→26), and APIchanges — one logical unit with its test.

### Phase 2b: Palette mapping extraction — DONE

```
[PATCH 1/2] lavu: extract palette mapping and dithering from vf_paletteuse  (246b8f30f7)
[PATCH 2/2] lavfi/vf_paletteuse: use libavutil palette mapping              (2d1cbaadd8)
```

Patch 1 creates palettemap.{h,c} with KD-tree colormap, hash cache, and 9 dithering
algorithms. Internal API only (ff_ prefix). Struct names kept verbatim from original;
FFColorInfo etc. noted as future work.
Patch 2 removes ~570 lines from vf_paletteuse.c, replacing with ff_palette_map_*() calls.
All 4 paletteuse FATE tests produce bit-for-bit identical output.

### Phase 3: Universal text-to-bitmap (after Phase 2 + RFC consensus)

Rendering in libavfilter, orchestration in fftools. Requires `--enable-libass`.

```
[PATCH 1/2] lavfi: add text subtitle rendering utility (via libass)
[PATCH 2/2] fftools: auto-convert text subtitles to bitmap for encoding
```

Patch 2 includes FATE roundtrip tests.

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

**DONE — committed `6c96a661fd`.**

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

### Architecture: render in libavfilter, quantize in libavutil, orchestrate in fftools

**Why not libavcodec?** Coza's 2022 series was rejected for putting libass
in libavcodec. libavcodec cannot depend on libavfilter (dependency violation).

**Why not libavfilter subtitle filter?** Infrastructure doesn't exist —
`fftools/ffmpeg_filter.c:1170` hard-rejects non-video/audio filters,
`buffersrc.c` returns `AVERROR_BUG` for subtitles, `AVFrame` has no
subtitle support. Building this overlaps with softworkz's 4-year-stuck work.

### Rendering utility (`libavfilter/subtitle_render.{h,c}`)

```c
int ff_subtitle_render_text(const char *header,
                            const char *text,
                            int64_t pts_ms,
                            int canvas_w, int canvas_h,
                            uint8_t **rgba,
                            int *x, int *y, int *w, int *h);
```

### Clip-box splitting (top/bottom subtitle support)

PGS supports up to 2 non-overlapping windows. The orchestrator in
fftools must split rendered RGBA into 1-2 non-overlapping rects
(analogous to PunkGraphicStream's `SubtitleEvent.walkClips()`).
The PGS encoder validates non-overlap via AABB intersection test.

### Usage (after Phase 3)

```bash
ffmpeg -i input.srt -c:s pgssub -canvas_size 1920x1080 output.sup
ffmpeg -i input.ass -c:s dvbsub output.ts
ffmpeg -i input.webvtt -c:s dvdsub output.sub
ffmpeg -i movie.mkv -map 0:s -c:s pgssub output.sup
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
| 4 | `libavcodec/dvdsubenc.c` | Use shared quantizer |
| 5 | `libavutil/{quantize.c,quantize.h}` | Add algorithms |
| 5 | `libavfilter/{vf_palettegen.c,vf_elbg.c}`, `libavcodec/gif.c` | Use shared API |

---

## Implementation Order

### Phase 1: DONE
Committed `6c96a661fd` in ffmpeg submodule.

### Phase 2a: DONE
Committed `118a2b9b2c` (palette move) and `30dd94d8e0` (quantizer API) in ffmpeg submodule.

### Phase 2b: Palette mapping extraction
6. Extract `palettemap.c` from `vf_paletteuse.c`
7. Refactor `vf_paletteuse.c` to call `av_palette_apply()`
8. Verify `make fate` (no behavior change)

### Phase 3: Text-to-bitmap
9. Create `libavfilter/subtitle_render.{h,c}`
10. Add orchestration in `fftools/ffmpeg_enc.c` + FATE tests
11. Verify `make fate`

### Phase 4: DVD subtitle consolidation
12. Replace dvdsubenc.c with shared API calls
13. Verify `make fate`

### Phase 5: Algorithm integration + GIF
14. Add Median Cut + ELBG algorithms, refactor palettegen + elbg + gif
15. Verify `make fate`

## Verification

```bash
# Phase 1 (done)
FATE_SAMPLES=/tmp/fate-samples make fate-sub-pgs

# Phase 2
make -j$(nproc) && make fate  # no behavior change

# Phase 3 (requires --enable-libass)
./configure --enable-libass --disable-doc && make -j$(nproc)
./ffmpeg -i test.srt -c:s pgssub -canvas_size 1920x1080 /tmp/t2b.sup
./ffprobe -v error -show_streams /tmp/t2b.sup | grep hdmv_pgs

# Phase 4-5
make -j$(nproc) && make fate  # all quantizers unified
```
