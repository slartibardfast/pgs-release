# Phase 3: Text-to-Bitmap Subtitle Conversion

## Status: DONE (core), IN PROGRESS (rect splitting + fade animation)

Core text-to-bitmap committed as `0b803170ab` (renderer) + `9c953175c6`
(fftools orchestration). Rect splitting implemented but uncommitted.
Fade animation (Phase 3a) in progress — encoder composition states done in Phase 1.

## Context

FFmpeg has no way to convert text subtitles (SRT, ASS, WebVTT, etc.) to bitmap
format (PGS, DVB, DVD, XSUB). This blocks 72 subtitle conversion pairs
(18 text decoders x 4 bitmap encoders) and is tracked as ticket #3819.

Phases 1-2 built the infrastructure: PGS encoder, NeuQuant quantizer API,
OkLab palette utilities, and palette mapping/dithering extraction. Phase 3
connects them with a text rendering utility to close the text→bitmap gap.

## Prior Art and Approach Justification

Three prior attempts inform our design:

**1. Clement Boesch's AVFrame subtitle WIP (Nov 2016)**
Proposed putting subtitles into AVFrame via `extended_data` pointers to
`AVFrameSubtitleRectangle`. wm4 raised concerns about memory management,
ABI stability, and display time semantics. Never completed. Established the
idea but exposed the fundamental mismatch: subtitle events are sparse and
overlapping, unlike contiguous audio/video frames.

**2. Coza's text-to-bitmap series (May 2022, 12 patches, rejected)**
Put libass rendering directly in libavcodec. Paul B Mahol: "this needs to be
in libavfilter instead of libavcodec." Nicolas George raised process issues.
softworkz noted overlap with existing subtitle filtering work. The rejection
was clear: libavcodec is the wrong place for libass.
(patchwork.ffmpeg.org/project/ffmpeg/cover/20220503161328.842587-1-traian.coza@gmail.com/)

**3. softworkz's subtitle filtering (Sep 2021–Jun 2025, 25 patches, 9+ versions, unmerged)**
The most ambitious attempt: full subtitle filter infrastructure including
AVFrame subtitle fields, frame-based codec API, sbuffersrc/sbuffersink,
~15 subtitle filters, and sub2video replacement. Includes `text2graphicsub`
which does exactly what our Phase 3 does — but as a filter graph node.

Why it hasn't merged after 4+ years:
- **The timing problem**: Subtitle events are sparse (gaps) and non-exclusive
  (overlapping). The filter graph scheduler expects contiguous, non-overlapping
  frames. softworkz solved this with dual timing fields on AVFrame
  (`pts`/`duration` for scheduling, `subtitle_timing.start_pts`/`.duration`
  for the actual event). Hendrik Leppkes objected: "In 3 of your 4 cases, the
  two sets of fields seem to be close to identical with no good reason to be
  separate." Anton Khirnov and Lynne also expressed skepticism.
- **Series size**: 25 patches touching 89 files. Review burden is enormous.
- **$1,000 bounty**: softworkz offered $1,000 for anyone to demonstrate a
  working implementation without the extra timing fields, arguing it was
  structurally impossible. No taker as of Jun 2025.
- **Status**: Deadlocked. No consensus on the fundamental API design.

### Why we use a utility function, not subtitle filters

The subtitle filtering infrastructure requires: AVFrame subtitle fields,
frame-based subtitle codec API, sbuffersrc/sbuffersink, filter format
negotiation for subtitles, and a solution to the timing mismatch problem.
This is real, necessary work — but it's a multi-year project with unresolved
design disagreements among core developers.

Our approach sidesteps all of this:
- **No AVFrame changes.** We work with existing `AVSubtitle`/`AVSubtitleRect`.
- **No filter graph involvement.** The conversion is a function call in
  fftools, not a filter node. Same pattern as sub2video (which converts
  bitmap subtitles to video frames entirely in fftools, ~200 lines in
  `ffmpeg_filter.c`, without formal filter infrastructure).
- **No opinion on the timing debate.** We don't need dual timing fields
  because we're not scheduling subtitle frames through a filter graph.
- **Minimal scope.** 2 patches, ~500 lines. Independently useful regardless
  of whether subtitle filters eventually land.
- **Rendering in libavfilter** where libass already lives (vf_subtitles.c).
  This directly addresses Coza's rejection reason.
- **Compatible with future subtitle filters.** If softworkz's work (or a
  successor) eventually merges, our rendering utility can be called from
  a `text2graphicsub` filter. The utility function becomes a building block,
  not an obstacle.

### Anticipated reviewer questions and prepared answers

| Question | Answer |
|----------|--------|
| "Why not wait for subtitle filters?" | Deadlocked 4+ years. 72 broken pairs today. Our utility is compatible with future filter work. |
| "Why not put this in libavcodec?" | Coza 2022 rejection: "needs to be in libavfilter." libavcodec→libass is a dependency violation. |
| "Why public API in libavfilter?" | `ff_` symbols are invisible to fftools in shared builds. fftools must call the renderer. |
| "Doesn't this overlap with softworkz's text2graphicsub?" | Same goal, different mechanism. Our utility can be reused by a future filter. We don't build filter infrastructure. |
| "Why not use the existing subtitles filter?" | It burns text onto video (overlay). We need standalone bitmaps for subtitle encoders. Different output. |
| "What about the sub2video approach?" | sub2video converts bitmap→video for filter graphs. We convert text→bitmap for encoding. Complementary, same organizational pattern. |

## Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Library for rendering | **libavfilter** | libass already lives here (vf_subtitles.c). libavcodec→libass was rejected in 2022. |
| API visibility | **Public** (`avfilter_subtitle_render_*`) | `ff_` symbols invisible to fftools in shared builds. fftools needs to call the renderer. Version bump + APIchanges required. |
| Stub for no-libass builds | **Return `AVERROR(ENOSYS)`** | Header always installed; functions return error when CONFIG_LIBASS unset. fftools checks at runtime. |
| Quantization | **Caller (fftools) calls `av_quantize_*`** | Rendering returns RGBA; caller quantizes. Keeps renderer focused on one job. |
| Canvas size | **Required parameter** | Auto-detection from video is a nice-to-have but adds complexity. Required for correct ASS layout. |
| Rect splitting | **Single rect initially** | PGS spec allows up to 2 composition objects but doesn't require it. Multi-rect splitting is a follow-up optimization. |
| Font attachments | **Supported in API from day 1** | MKV/ASS files embed fonts. Reviewers will ask. libass handles via `ass_add_font()`. |
| FATE testing | **Structural roundtrip test** | Font rendering is platform-dependent (FreeType version, fontconfig). Can't do pixel-exact. Test: text→PGS→decode, verify valid output structure. |

## Patch Structure (2 patches)

### Patch 1/2: `lavfi: add text subtitle rendering utility (via libass)`

New public API in libavfilter for rendering text/ASS subtitle events to
cropped RGBA bitmaps. CONFIG_LIBASS gated. FATE trivially passes (no
existing code changed).

### Patch 2/2: `fftools: auto-convert text subtitles to bitmap for encoding`

Orchestration in `fftools/ffmpeg_enc.c`. Relaxes the text-to-bitmap
rejection in `fftools/ffmpeg_mux_init.c`. Includes FATE roundtrip test.

## Patch 1 Detail

### New: `libavfilter/subtitle_render.h` (public — added to HEADERS)

```c
// Copyright 2026 David Connolly

typedef struct AVSubtitleRenderContext AVSubtitleRenderContext;

/**
 * Allocate a subtitle rendering context.
 * @param canvas_w  output canvas width (e.g. 1920)
 * @param canvas_h  output canvas height (e.g. 1080)
 * @return context or NULL on failure (ENOMEM or no libass)
 */
AVSubtitleRenderContext *avfilter_subtitle_render_alloc(int canvas_w,
                                                        int canvas_h);
void avfilter_subtitle_render_freep(AVSubtitleRenderContext **ctx);

/**
 * Set ASS script header (styles, script info).
 * From AVCodecContext->subtitle_header after decoding.
 */
int avfilter_subtitle_render_set_header(AVSubtitleRenderContext *ctx,
                                         const char *header);

/**
 * Add an embedded font (e.g. MKV attachment).
 * @param name  font family name (from attachment metadata)
 * @param data  raw font file data
 * @param size  font data size in bytes
 */
int avfilter_subtitle_render_add_font(AVSubtitleRenderContext *ctx,
                                       const char *name,
                                       const uint8_t *data, int size);

/**
 * Render one subtitle event to a cropped RGBA bitmap.
 * @param[in]  text         ASS dialogue line (Dialogue: 0,...)
 * @param[in]  start_ms     event start time in milliseconds
 * @param[in]  duration_ms  event duration in milliseconds
 * @param[out] rgba         allocated RGBA buffer (caller frees with av_free)
 * @param[out] linesize     RGBA row stride
 * @param[out] x,y          crop position on canvas
 * @param[out] w,h          crop dimensions
 * @return 0 on success, negative AVERROR on failure
 */
int avfilter_subtitle_render_frame(AVSubtitleRenderContext *ctx,
                                    const char *text,
                                    int64_t start_ms, int64_t duration_ms,
                                    uint8_t **rgba, int *linesize,
                                    int *x, int *y, int *w, int *h);
```

### New: `libavfilter/subtitle_render.c`

**Context struct** (definition in .c, opaque to consumers):
```c
typedef struct AVSubtitleRenderContext {
    ASS_Library  *library;
    ASS_Renderer *renderer;
    ASS_Track    *track;
    int canvas_w, canvas_h;
} AVSubtitleRenderContext;
```

**`avfilter_subtitle_render_alloc()`:**
1. `av_mallocz()` context
2. `ass_library_init()`, `ass_renderer_init()`
3. `ass_set_frame_size(renderer, canvas_w, canvas_h)`
4. `ass_set_fonts(renderer, NULL, NULL, 1, NULL, 1)` (system font fallback)
5. `ass_new_track(library)` (empty track, header added later)

Follows vf_subtitles.c `init()` pattern (lines 135-161).

**`avfilter_subtitle_render_set_header()`:**
- `ass_process_codec_private(track, header, strlen(header))`

**`avfilter_subtitle_render_add_font()`:**
- `ass_add_font(library, name, data, size)`

**`avfilter_subtitle_render_frame()`:**
1. `ass_flush_events(track)` (clear previous events)
2. `ass_process_chunk(track, text, strlen(text), start_ms, duration_ms)`
3. `ass_render_frame(renderer, track, start_ms, &detect_change)`
4. Walk `ASS_Image` linked list → compute bounding box
5. Allocate RGBA canvas (bounding box size, transparent)
6. Composite each ASS_Image span:
   ```c
   for (ASS_Image *img = images; img; img = img->next) {
       // img->bitmap is an alpha mask
       // img->color is 0xRRGGBBAA (AA=0 means opaque in ASS)
       // Alpha-composite mask * color onto RGBA canvas
   }
   ```
7. Set output x, y, w, h (bounding box position/size)
8. Return 0

Key difference from vf_subtitles.c `overlay_ass_image()` (line 218):
that function blends onto an existing video frame via `ff_blend_mask()`.
We composite onto a fresh transparent RGBA canvas — simpler alpha math,
no `FFDrawContext` dependency.

**No-libass stub** (compiled when CONFIG_LIBASS is unset):
```c
AVSubtitleRenderContext *avfilter_subtitle_render_alloc(int w, int h)
{ return NULL; }
// All other functions return AVERROR(ENOSYS)
```

### Modified: `libavfilter/Makefile`

```makefile
HEADERS += subtitle_render.h
OBJS += subtitle_render.o    # always compiled (public API symbols must exist)
```

The .o is always compiled. Inside `subtitle_render.c`, `#if CONFIG_LIBASS`
selects full implementation vs stubs (alloc returns NULL, others return
AVERROR(ENOSYS)). This ensures the shared library always exports the
symbols regardless of build configuration.

### Modified: `libavfilter/version.h`

Bump MINOR (since new public API added).

### Modified: `doc/APIchanges`

Add entry for `avfilter_subtitle_render_*` functions.

## Patch 2 Detail

### Modified: `fftools/ffmpeg_mux_init.c`

Relax the text-to-bitmap rejection gate (lines 891-896):

```c
if (input_props && output_props && input_props != output_props) {
    if ((input_props & AV_CODEC_PROP_TEXT_SUB) &&
        (output_props & AV_CODEC_PROP_BITMAP_SUB)) {
        // Text-to-bitmap handled by subtitle renderer in ffmpeg_enc.c
        AVSubtitleRenderContext *test = avfilter_subtitle_render_alloc(1, 1);
        if (!test) {
            av_log(ost, AV_LOG_ERROR,
                   "Text to bitmap subtitle conversion requires libass "
                   "(configure with --enable-libass)\n");
            return AVERROR(ENOSYS);
        }
        avfilter_subtitle_render_freep(&test);
    } else {
        av_log(ost, AV_LOG_ERROR,
               "Subtitle encoding currently only possible from text to text "
               "or bitmap to bitmap\n");
        return AVERROR(EINVAL);
    }
}
```

Probe-and-free pattern avoids `#if CONFIG_LIBASS` preprocessor in fftools
(which doesn't include config_components.h). Runtime check is cleaner.

### Modified: `fftools/ffmpeg_enc.c`

**Add to `OutputStream` or separate context**: a `AVSubtitleRenderContext *sub_render`
field, initialized lazily on first text-to-bitmap encode.

**Modify `do_subtitle_out()`** (line 379):
Before `avcodec_encode_subtitle()`, detect text→bitmap mismatch and convert:

```c
static int convert_text_to_bitmap(OutputStream *ost, AVSubtitle *sub)
{
    // 1. Lazy-init: create AVSubtitleRenderContext if not yet created
    //    - canvas_size from ost->enc_ctx (or AVOption)
    //    - set header from ost->enc_ctx->subtitle_header
    //    - add fonts from input attachments

    // 2. For each rect with type == SUBTITLE_ASS or SUBTITLE_TEXT:
    //    a. Wrap plain text in ASS dialogue format if needed
    //    b. Call avfilter_subtitle_render_frame() → RGBA + position
    //    c. Call av_quantize_alloc() + av_quantize_generate_palette()
    //    d. Call av_quantize_apply() → palette indices
    //    e. Rewrite rect:
    //       - type = SUBTITLE_BITMAP
    //       - data[0] = palette indices
    //       - data[1] = ARGB palette (uint32_t[256])
    //       - x, y, w, h from render output
    //       - nb_colors from quantizer

    // 3. Free RGBA intermediate buffers
    return 0;
}
```

Insert before the `avcodec_encode_subtitle()` call:
```c
if (needs_text_to_bitmap_conversion(ost, sub)) {
    ret = convert_text_to_bitmap(ost, sub);
    if (ret < 0)
        return ret;
}
```

**Canvas size**: Accept from encoder's `-canvas_size` option. PGS encoder
already has `canvas_size` in its AVOptions. For other bitmap encoders,
may need to add it or derive from video stream dimensions.

**Font attachment forwarding**: Iterate input file's attachment streams,
call `avfilter_subtitle_render_add_font()` for each font.

**Cleanup**: Free `sub_render` context in stream cleanup path.

### FATE Test

Add a roundtrip test (requires `--enable-libass` and FATE samples):

```bash
# Test: SRT → PGS encode → PGS decode → verify structure
ffmpeg -i input.srt -c:s pgssub -canvas_size 1920x1080 output.sup
ffprobe -show_packets output.sup
```

The test verifies:
- Pipeline doesn't crash
- Output contains valid PGS packets
- Packet count matches input subtitle count
- Timing is preserved (PTS values)

**NOT pixel-exact** — font rendering varies by platform (FreeType version,
fontconfig, available fonts). Reference is packet count + timing, not CRC.

Gate the test with `CONFIG_LIBASS` so it's skipped on builds without libass.

## Risks

| Risk | Severity | Mitigation |
|------|----------|------------|
| "Why public API?" pushback | HIGH | `ff_` symbols invisible in shared builds. fftools must call renderer. No alternative without subtitle filter infra. |
| "Put it in libavcodec" pushback | HIGH | Coza 2022 precedent. libavcodec→libass rejected. Prepared answer. |
| "Build subtitle filters instead" | MEDIUM | Punt: no AVFrame subtitle support, no subtitle buffersrc/sink. Too large a project. Our utility is minimal. |
| Platform-dependent FATE | MEDIUM | Structural test (packet count + timing), not pixel CRC. |
| ASS rendering edge cases | MEDIUM | Defer complex ASS features (animations, karaoke). Basic dialogue positioning covers 95% of use cases. |
| Canvas size required | LOW | Document clearly. Auto-detection from video stream is follow-up work. |
| Memory management in fftools | LOW | Follow existing `do_subtitle_out()` patterns. Careful free on error paths. |

## Verification

```bash
cd /home/david/pgs/ffmpeg
./configure --enable-libass --disable-doc && make -j$(nproc)

# Existing tests still pass:
make fate-quantize
FATE_SAMPLES=/tmp/fate-samples make fate-sub-pgs
FATE_SAMPLES=/tmp/fate-samples make fate-filter-paletteuse-nodither
FATE_SAMPLES=/tmp/fate-samples make fate-filter-paletteuse-bayer

# Manual roundtrip test:
echo "1\n00:00:01,000 --> 00:00:03,000\nHello World" > /tmp/test.srt
./ffmpeg -i /tmp/test.srt -c:s pgssub -canvas_size 1920x1080 /tmp/test.sup
./ffprobe -v error -show_packets /tmp/test.sup

# ASS with styles:
./ffmpeg -i test.ass -c:s pgssub -canvas_size 1920x1080 /tmp/test_ass.sup
./ffprobe -v error -show_streams /tmp/test_ass.sup

# Verify no libass regression:
FATE_SAMPLES=/tmp/fate-samples make fate-filter-sub2video
```

## Critical Files

| File | Role |
|------|------|
| `libavfilter/subtitle_render.{h,c}` | **NEW** — rendering utility |
| `libavfilter/Makefile` | Add HEADERS + OBJS |
| `libavfilter/version.h` | Version bump |
| `doc/APIchanges` | New public API entry |
| `fftools/ffmpeg_enc.c` | Orchestration (convert_text_to_bitmap) |
| `fftools/ffmpeg_mux_init.c` | Relax text→bitmap gate |
| `libavfilter/vf_subtitles.c` | **Reference** — libass init/render patterns |
| `libavcodec/avcodec.h` | **Reference** — AVSubtitleRect struct |
| `fftools/ffmpeg_filter.c` | **Reference** — sub2video_copy_rect pattern |

## Rect Splitting (in progress)

### Problem

When a rendered bitmap contains text at both top and bottom of the screen
(e.g., dialogue at bottom + song lyrics at top, or an ASS event with
`\an8` and `\an2` positioning), the single RGBA bitmap includes a large
transparent gap in the middle. Encoding this as one object wastes
bandwidth on transparent pixels and RLE-encodes empty rows.

PGS supports up to 2 non-overlapping composition objects per display set.
Splitting the bitmap at a transparent gap produces two smaller objects,
each with its own window, position, and independently quantized palette.

### Algorithm

In `convert_text_to_bitmap()` (fftools/ffmpeg_enc.c):

1. After rendering RGBA, scan rows for a fully-transparent horizontal gap
2. If gap exceeds threshold (32 rows default), split into top and bottom
3. Each half is independently quantized (separate `av_quantize_*` calls)
4. The original rect is rewritten as top; a second rect is allocated for bottom
5. The PGS encoder handles 2 rects natively (already supported)

### Implementation

Three functions added to `fftools/ffmpeg_enc.c`:
- `quantize_rgba_to_rect()` — extracted helper for RGBA → palette + indices
- `find_transparent_gap()` — scan for horizontal gap in RGBA data
- Updated `convert_text_to_bitmap()` — split logic when gap found

Status: implemented, builds, uncommitted. Needs FATE test coverage.

## Animation Support (requires Phase 1 amendment)

### Problem

ASS subtitle effects like `\fad` (fade), `\t(\alpha)` (alpha transition),
and `\move` (position animation) currently produce a single static bitmap
per event. The PGS spec supports animation effects through:

- **Palette animation**: One bitmap (ODS) + chain of palette updates (PDS)
  with modified alpha/color values (NORMAL composition, palette_update_flag)
- **Position animation**: One bitmap (ODS) + chain of PCS coordinate
  updates (NORMAL composition, no ODS retransmission)

These techniques are specified in:
- US20090185789A1 (Panasonic) — composition states, palette_update_flag
- US8638861B2 (Sony) — segment timing, PDS versioning
- US7620297B2 (Panasonic) — decoder model, object buffer persistence

### Approach

Animation is a two-layer problem:

**Layer 1: Encoder state machine (Phase 1 amendment)**
The PGS encoder must support composition states beyond Epoch Start:
- NORMAL Display Sets with palette_update_flag for PDS-only updates
- NORMAL Display Sets referencing previously-decoded objects
- Acquisition Point Display Sets for seek support
- Decoder model timing (DTS computation from buffer model)
- See PHASE1.md "Amendment Plan" section for full details.

**Layer 2: Animation-aware conversion (Phase 3 extension)**
The text-to-bitmap layer detects animation in ASS events and produces
multiple Display Sets per event:

1. **Fade detection**: Parse `\fad(in,out)` from ASS override tags
2. **Render base frame**: Full RGBA at peak visibility
3. **Generate palette variants**: Same bitmap indices, different alpha
   values in palette entries for each fade step
4. **Emit Display Sets**: Epoch Start (full ODS + PDS), then NORMAL
   (PDS-only) at each animation step

The animation-aware conversion runs in `fftools/ffmpeg_enc.c` and calls
the PGS encoder multiple times per ASS event (once for the base frame
as Epoch Start, then once per animation step as Normal).

### Decoder model budgeting

Per the HDMV spec, a palette-only Display Set (PCS + PDS + END) is
small (~1300 bytes for 256-entry palette) and transfers quickly. At
Rx = 2 MB/s, a full palette update takes ~0.65 ms to arrive. This
allows high-frequency palette animation (60+ steps/second) without
exceeding buffer constraints.

Position animation requires PCS + WDS + END (~30-50 bytes), even smaller.

The main constraint is the initial ODS transfer. For a 1920×200 subtitle
bitmap (~380 KB decoded, ~20-50 KB RLE), the object decode time is:
```
⌈200 × 1920 × 90000 / 16000000⌉ ≈ 2160 ticks ≈ 24 ms
```

This must complete before the first PCS PTS, setting the minimum lead
time for the Epoch Start Display Set.

### Dependencies on Phase 1

Animation requires these Phase 1 encoder changes (detailed in PHASE1.md):
- Composition state field (currently hardcoded to 0x80)
- palette_update_flag support
- palette_version incrementing
- Object version tracking and reuse
- DTS computation from decoder model
- Acquisition Point generation

## Phase 3a: Fade Animation Implementation

### Status: IN PROGRESS

### Approach

Strip `\fad(in,out)` from ASS text before rendering so libass produces a
full-opacity bitmap. Quantize once to get a base palette + indices. For each
fade step, scale palette alpha and call the encoder. The encoder's composition
state machine (Phase 1) automatically emits Epoch Start for the first call and
Normal+palette_update for subsequent calls — no encoder changes needed.

### Utility Functions (`fftools/ffmpeg_subtitle_fade.c`)

Extracted to a separate file for testability via FFmpeg's `.c` inclusion
pattern (same as `libavutil/tests/parseutils.c` including `parseutils.c`).
All functions are `static` — private in production, accessible to tests
via `#include`.

```c
// Strip \fad(in,out) from ASS text, return durations
static void parse_and_strip_fad(char *ass_text,
                                 int *fade_in_ms, int *fade_out_ms);

// Scale palette alpha by percentage (0-100)
static void scale_palette_alpha(const uint32_t *src, uint32_t *dst,
                                int nb_colors, int alpha_pct);

// Frame-rate-aware step count computation
static int compute_fade_steps(int fade_in_ms, int fade_out_ms,
                               int duration_ms, AVRational framerate,
                               int *in_steps, int *out_steps);
```

### Step Count: Frame-Rate Aware, No Artificial Cap

Step interval derived from `enc_ctx->framerate`, NOT hardcoded:
- `frame_ms = 1000 * framerate.den / framerate.num`
- Falls back to 24fps if framerate unset
- One palette update per video frame for smooth animation
- Minimum 2 steps per fade region

No artificial cap on step count. `palette_version` is a byte (0-255) — the
"8 palettes per epoch" spec limit refers to palette **ID slots** (0-7), not
version increments. We use a single palette ID (0), so 255 versions are
available. This matches SUPer's approach.

| Rate | Frame interval | 300ms fade | 1000ms fade |
|------|---------------|------------|-------------|
| 23.976 | 41.7ms | 7 steps | 24 steps |
| 25.000 | 40.0ms | 7 steps | 25 steps |
| 29.970 | 33.4ms | 9 steps | 30 steps |
| 59.940 | 16.7ms | 18 steps | 60 steps |

Safety clamp at 254 total steps (Epoch Start uses version 0). Only triggers
for extreme cases like 10+ second fades at 60fps.

### Fade Timing Model

```
\fad(300,500) on event 1.0s → 4.0s (24fps):

1.000s  Epoch Start: alpha=0%     ← loads ODS invisibly
1.042s  Normal PDS:  alpha=14%
1.083s  Normal PDS:  alpha=29%
  ...
1.292s  Normal PDS:  alpha=100%   ← peak
        ... peak period, no packets ...
3.500s  Normal PDS:  alpha=83%
  ...
3.958s  Normal PDS:  alpha=8%
4.000s  Clear (num_rects=0)       ← removes from decoder
```

- **Fade-in** starts at alpha=0% (Epoch Start must load ODS before palette updates)
- **Fade-out** ends at the fade boundary, NOT at alpha=0%. Clear packet handles removal.
- **Clear packet** already emitted by existing subtitle pipeline. No change needed.

### Rect Splitting Interaction

Skip rect splitting when fade detected. Two independently-quantized rects have
separate palettes — a single PDS update can't fade both. Animated events are
always single-rect.

### Scope

- `\fad(in,out)` only (2-argument). Covers vast majority of ASS files.
- No `\fade(...)` (7-argument), `\move`, `\t(\alpha)`. Future work.
- No encoder changes. No subtitle_render API changes.

### FATE Tests

1. **Encoder state machine** (`tests/api/api-pgs-fade-test.c`): construct
   AVSubtitle structs, call encoder with different palettes/positions/clears,
   verify composition_state, palette_update_flag, palette_version in output.
2. **`\fad` parsing** (`tests/fate/pgs-fade-test.c`): edge cases via `.c`
   inclusion of `ffmpeg_subtitle_fade.c`.
3. **Step computation** (same test file): frame rate variants, clamping, minimums.

### Files

| File | Change |
|------|--------|
| `fftools/ffmpeg_subtitle_fade.c` | NEW — utility functions |
| `fftools/ffmpeg_enc.c` | `#include` fade utils, animation loop, wiring |
| `tests/api/api-pgs-fade-test.c` | NEW — encoder state machine test |
| `tests/fate/pgs-fade-test.c` | NEW — fade utility unit tests |
| `tests/api/Makefile` | Add api-pgs-fade-test |
| `tests/fate/subtitles.mak` | Add FATE targets |

## Dependencies

Our existing work consumed by this phase:
- `av_quantize_*` (Phase 2a) — RGBA → palette + indices
- `ff_palette_map_*` (Phase 2b) — available but not needed directly
  (quantizer's `av_quantize_apply()` handles the index mapping)
- PGS encoder (Phase 1) — primary test target for the pipeline

## References

- US20090185789A1 (Panasonic) — Composition states, palette_update_flag
- US8638861B2 (Sony) — PDS versioning, segment timing constraints
- US7620297B2 (Panasonic) — Decoder model, object buffer persistence
- `docs/pgs-specification.md` — Compiled spec with decoder model constants
- SUPer encoder — Hardware-validated reference for palette animation
  sequences and decoder model compliance (cited for spec interpretation,
  not code patterns)
