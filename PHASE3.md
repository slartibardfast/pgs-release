# Phase 3: Text-to-Bitmap Subtitle Conversion

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

## Dependencies

Our existing work consumed by this phase:
- `av_quantize_*` (Phase 2a) — RGBA → palette + indices
- `ff_palette_map_*` (Phase 2b) — available but not needed directly
  (quantizer's `av_quantize_apply()` handles the index mapping)
- PGS encoder (Phase 1) — primary test target for the pipeline
