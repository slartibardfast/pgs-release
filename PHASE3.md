# Phase 3: Text-to-Bitmap Subtitle Conversion

## Status: DONE (core + rect splitting + animation)

All committed on `pgs-series` branch, reorganized into 4 independent
submission series. Encoder composition states done in Phase 1.

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

/**
 * Set up a subtitle event for multi-timepoint rendering.
 * Clears previous events and adds the given text. After calling this,
 * use avfilter_subtitle_render_sample() at one or more timestamps.
 */
int avfilter_subtitle_render_init_event(AVSubtitleRenderContext *ctx,
                                         const char *text,
                                         int64_t start_ms,
                                         int64_t duration_ms);

/**
 * Render a snapshot of the current event at a specific time.
 * Must be called after init_event(). Can be called multiple times
 * at different timestamps to sample animation frames.
 * @param detect_change  0=identical, 1=changed, 2=unknown (first call)
 */
int avfilter_subtitle_render_sample(AVSubtitleRenderContext *ctx,
                                     int64_t render_time_ms,
                                     uint8_t **rgba, int *linesize,
                                     int *x, int *y, int *w, int *h,
                                     int *detect_change);
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

**`avfilter_subtitle_render_init_event()`:**
1. `ass_flush_events(track)` (clear previous events)
2. `ass_process_chunk(track, text, strlen(text), start_ms, duration_ms)`

**`avfilter_subtitle_render_sample()`:**
1. `ass_render_frame(renderer, track, render_time_ms, &detect_change)`
2. Walk `ASS_Image` linked list → compute bounding box
3. Allocate RGBA canvas (bounding box size, transparent)
4. Composite each ASS_Image span:
   ```c
   for (ASS_Image *img = images; img; img = img->next) {
       // img->bitmap is an alpha mask
       // img->color is 0xRRGGBBAA (AA=0 means opaque in ASS)
       // Alpha-composite mask * color onto RGBA canvas
   }
   ```
5. Set output x, y, w, h (bounding box position/size)
6. Return 0

**`avfilter_subtitle_render_frame()`:**
Thin wrapper — calls `init_event()` then `sample()` at start_ms.
Convenience function for static (non-animated) subtitle rendering.

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

## Rect Splitting

### Problem

When a rendered bitmap contains text at both top and bottom of the screen
(e.g., dialogue at bottom + song lyrics at top, or an ASS event with
`\an8` and `\an2` positioning), the single RGBA bitmap includes a large
transparent gap in the middle. Encoding this as one object wastes
bandwidth on transparent pixels and RLE-encodes empty rows.

PGS supports up to 2 non-overlapping composition objects per display set.
Splitting the bitmap at a transparent gap produces two smaller objects,
each with its own window and position.

### Algorithm

In `convert_text_to_bitmap()` and `flush_coalesced_subtitles()`:

1. After rendering RGBA, quantize the full image to palette + indices
2. Scan rows for a fully-transparent horizontal gap (threshold: 32 rows)
3. If gap found, split the index buffer into top and bottom halves
4. Both halves share one palette via `fill_rect_bitmap()` helper

**Critical constraint**: PGS allows only one PDS (palette) per Display Set.
The encoder writes `rects[0]->data[1]` as the single PDS. Independent
quantization per half would produce incorrect colors for the second rect —
its palette indices would reference the first rect's palette. The fix is
to quantize once, then distribute indices.

### Implementation

Functions in `fftools/ffmpeg_enc.c` and `fftools/ffmpeg_subtitle_animation.c`:
- `find_transparent_gap()` — scan for horizontal gap in RGBA data
- `fill_rect_bitmap()` — fill AVSubtitleRect from pre-quantized indices + shared palette
- `quantize_rgba_to_rect()` — single-rect helper (quantize + fill)
- Both `convert_text_to_bitmap()` and `flush_coalesced_subtitles()` use
  quantize-first-then-split when a gap is found

## Animation Support

### Problem

Text subtitle effects (fades, motion, transforms) produce a single static
bitmap per event in the basic pipeline. The PGS spec supports animation
through:

- **Palette animation**: One bitmap (ODS) + chain of palette updates (PDS)
  with modified alpha values (NORMAL composition, palette_update_flag)
- **Position animation**: One bitmap (ODS) + chain of PCS coordinate
  updates (NORMAL composition, no ODS retransmission)

These techniques are specified in:
- US20090185789A1 (Panasonic) — composition states, palette_update_flag
- US8638861B2 (Sony) — segment timing, PDS versioning
- US7620297B2 (Panasonic) — decoder model, object buffer persistence

### Approach: Format-Agnostic Multi-Timepoint Rendering

Rather than parsing format-specific tags, render each event at every
frame interval and observe what changes in the RGBA output. Classify
changes to determine the optimal PGS encoding:

- **Alpha-only changes** (fades) → palette-only Normal Display Sets
- **Position-only changes** (motion) → position-only Normal Display Sets
- **Content changes** (transforms, karaoke) → new Epoch Start per frame

The animation-aware conversion runs in `fftools/ffmpeg_enc.c` via
`do_subtitle_out_animated()`, gated by format hint (only SUBTITLE_ASS
with override tags triggers the scan). Static subtitles (SRT, ASS
without `{`) use the fast single-render path with zero overhead.

### Encoder state machine (Phase 1)

The PGS encoder composition state machine provides the foundation:
- Automatic state detection (Epoch Start, Normal, palette_update)
- palette_update_flag for PDS-only Display Sets
- palette_version tracking and incrementing
- Object reuse across Normal Display Sets

### Decoder model budgeting

Per the HDMV spec, a palette-only Display Set (PCS + PDS + END) is
small (~1300 bytes for 256-entry palette) and transfers quickly. At
Rx = 2 MB/s, a full palette update takes ~0.65 ms to arrive. This
allows high-frequency palette animation (60+ steps/second) without
exceeding buffer constraints.

Position animation requires PCS + WDS + END (~30-50 bytes), even smaller.

## Phase 3a: Universal Subtitle Animation

### Status: DONE

### Key Insight

Rather than parsing format-specific tags (`\fad`, `\move`, `\t`), render
the subtitle event at multiple timepoints and observe what changed in the
RGBA output. Frame-to-frame comparison classifies changes for optimal PGS
encoding. This handles ALL animation types in ALL text subtitle formats
without parsing any tags.

The PGS encoder's composition state machine (Phase 1) already supports
three Display Set types for changed frames:

| Change type | PGS encoding | Segments |
|-------------|-------------|----------|
| New/full change | Epoch Start (0x80) | PCS + WDS + PDS + ODS + END |
| Palette only | Normal + palette_update | PCS + PDS + END |
| Position only | Normal | PCS + END |

### Render API Extension

Split `avfilter_subtitle_render_frame()` into two functions for
multi-timepoint rendering:

- `init_event()` — flush events, add text to track (call once per event)
- `sample()` — render at a specific timestamp (call N times)

`render_frame()` becomes a thin wrapper calling both. Both have
`#if !CONFIG_LIBASS` stubs matching the existing pattern.

### Animation Detection: Every-Frame Scan + Format Hint

Scan every frame interval within the event duration. Gate by format:

1. **SUBTITLE_TEXT** (SRT, WebVTT): animation structurally impossible.
   Render once, quantize, single encode. Zero overhead.
2. **SUBTITLE_ASS without `{`**: no override tags, no animation.
   Same fast path as SUBTITLE_TEXT.
3. **SUBTITLE_ASS with `{...}`**: every-frame scan via `init_event`/`sample`.

Frame interval: `frame_ms = 1000 * framerate.den / framerate.num`,
fallback to 42ms (~24fps) if framerate unset.

Cost: `ass_render_frame()` with cached glyphs: ~0.03ms per call.
A 3-second event at 24fps: 72 calls = ~2ms. Static frames
(detect_change=0) are skipped instantly. Acceptable for offline
PGS encoding.

### Change Classification (`fftools/ffmpeg_subtitle_animation.c`)

Given two RGBA frames with bounding boxes, classify the change:

```c
enum SubtitleChangeType {
    SUB_CHANGE_NONE,       /* Identical frames */
    SUB_CHANGE_POSITION,   /* Same content, different x/y (motion) */
    SUB_CHANGE_ALPHA,      /* Same RGB, different alpha (fades) */
    SUB_CHANGE_CONTENT,    /* Different bitmap (complex animation) */
};
```

Algorithm:
1. Dimensions differ → **CONTENT**
2. Pixel data identical, position differs → **POSITION**
3. Pixel data identical, position same → **NONE**
4. For all pixels where alpha>0 in either frame: if RGB matches
   but alpha differs → **ALPHA**
5. Otherwise → **CONTENT**

Combined changes (alpha + position) → **CONTENT** (PGS can't combine
palette_update and position update in one Normal DS).

### Utility Functions

Extracted to `fftools/ffmpeg_subtitle_animation.c` for testability via
FFmpeg's `.c` inclusion pattern. All functions are `static` — private in
production, accessible to tests via `#include`.

```c
// Compute sum of alpha values across RGBA buffer
static int64_t rgba_alpha_sum(const uint8_t *rgba, int w, int h, int linesize);

// Classify change between two RGBA frames
static enum SubtitleChangeType classify_subtitle_change(
    const uint8_t *rgba0, int x0, int y0, int w0, int h0, int ls0,
    const uint8_t *rgba1, int x1, int y1, int w1, int h1, int ls1);

// Scale palette alpha by percentage (0-100)
static void scale_palette_alpha(const uint32_t *src, uint32_t *dst,
                                int nb_colors, int alpha_pct);
```

### Animation Loop (`do_subtitle_out_animated()`)

Two-pass architecture for the animation path:

**Pass 1 (scan):** Render every frame, classify changes, find peak alpha
frame, record animated timestamps. Per-event classification uses worst-case
change type across ALL frames — if ANY frame is CONTENT, the whole event
uses the CONTENT path.

**Pass 2 (encode):** Based on event-level classification:

- **ALPHA (fade):** Re-render peak frame, quantize as reference palette.
  Epoch Start with SCALED palette (first frame's alpha ratio vs peak).
  Subsequent frames get palette-only Normal Display Sets.
- **POSITION (motion):** Quantize first frame (all frames share same
  bitmap). Epoch Start at first position, then position-only Normal DS.
- **CONTENT (complex animation):** Each frame quantized independently.
  Epoch Start per frame.

Reference frame selection for ALPHA: the peak-opacity frame gives best
palette quality (quantizing at low alpha wastes precision). The Epoch Start
uses reference INDICES (peak bitmap shape) but SCALED PALETTE (first
frame's alpha), preventing a flash on fade-in.

### Fade Timing Model

```
\fad(300,500) on event 1.0s → 4.0s (24fps):

Pass 1 (scan ~2ms):
  Render 72 frames. detect_change=1 for ~7 fade-in + ~12 fade-out.
  Peak found around 1.333s (alpha_sum is maximum).
  Event classified as ALPHA (all changes are alpha-only).

Pass 2 (encode):
  Quantize peak frame → ref_palette + ref_indices
  1.000s  Epoch Start: ref indices + scaled palette (alpha≈0%)
  1.042s  Normal PDS: alpha≈14%
  ...
  1.292s  Normal PDS: alpha≈100%   ← matches peak palette
  (static frames skipped)
  3.500s  Normal PDS: alpha≈80%
  ...
  3.958s  Normal PDS: alpha≈8%
  4.000s  Clear (num_rects=0)      ← existing pipeline handles this
```

For `\move(100,200,300,400)`:
```
Pass 1: all frames differ in position, classified POSITION
Pass 2:
  1.000s  Epoch Start at (100,200)
  1.042s  Normal: position (103,203)
  ...
  4.000s  Clear
```

### palette_version Safety

Alpha-only steps increment palette_version (byte, 0-255). If steps
exceed 254 within an epoch, emit an Epoch Start to reset the counter.
`palette_version` is a byte (0-255) — the "8 palettes per epoch"
spec limit refers to palette **ID slots** (0-7), not version
increments. We use a single palette ID (0), so 255 versions are
available. This matches SUPer's approach.

### Rect Splitting Interaction

Skip rect splitting when animation is detected. Two independently-quantized
rects have separate palettes — a single PDS can't update both.
Animated events use single-rect encoding.

### Animation Coverage

All animation is handled by observing renderer output — no tag parsing:

| Animation type | Examples | Classification | PGS path |
|---------------|---------|---------------|----------|
| Uniform alpha fade | `\fad(in,out)`, `\fade(...)`, `\t(\alpha...)` | ALPHA | Palette-only Normal DS |
| Linear motion | `\move(x1,y1,x2,y2)` | POSITION | Position-only Normal DS |
| Bitmap transform | `\t(\fscx)`, `\t(\frz)`, `\t(\bord)` | CONTENT | Epoch Start per frame |
| Color change | `\t(\1c&H...)`, `\t(\3c&H...)` | CONTENT | Epoch Start per frame |
| Karaoke | `\k`, `\K`, `\ko` | CONTENT | Epoch Start per frame |
| Fade + move | `\fad(300,0)\move(...)` | CONTENT | Epoch Start per frame |
| No animation | SRT, `{\pos(...)}`, `{\b1}` | NONE | Single Epoch Start |

### Encoder Bug Fix (`libavcodec/pgssubenc.c`)

Fixed palette_version ordering: version must be incremented BEFORE writing
segments, not after. Without this fix, the first palette update would
repeat version 0, confusing hardware decoders.

### FATE Tests

1. **Encoder state machine** (`tests/api/api-pgs-fade-test.c`, 6 tests):
   Construct AVSubtitle structs, call encoder with different palettes,
   positions, and clears. Verifies composition_state, palette_update_flag,
   palette_version, and num_composition_objects in output segments.
2. **Animation utilities** (`tests/api/api-pgs-animation-util-test.c`):
   Unit tests for `classify_subtitle_change()` (identical, position,
   alpha, content, null, transparent pixels), `compute_alpha_ratio()`,
   and `scale_palette_alpha()` via `.c` inclusion.

### Files

| File | Change |
|------|--------|
| `libavfilter/subtitle_render.h` | Add `init_event()` + `sample()` declarations |
| `libavfilter/subtitle_render.c` | Split `render_frame()` into init + sample |
| `fftools/ffmpeg_subtitle_animation.c` | NEW — classify + alpha utilities |
| `fftools/ffmpeg_enc.c` | Animation orchestration, dispatch, render context helper |
| `libavcodec/pgssubenc.c` | palette_version ordering fix |
| `tests/api/api-pgs-fade-test.c` | NEW — encoder state machine test (6 tests) |
| `tests/api/api-pgs-animation-util-test.c` | NEW — animation utility tests |
| `tests/api/Makefile` | Add test programs |
| `tests/fate/api.mak` | Add FATE targets |

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
