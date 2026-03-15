# Phase 8 Review: Upstream Suitability Audit

## Status: In Progress

End-to-end review of all 17 patches for FFmpeg upstream submission
readiness. Two independent review passes identified 16 blocking
issues and ~25 significant issues across the series.

## Patch Series (17 patches)

| # | Hash | Description | Series |
|---|------|-------------|--------|
| 1 | a6631a74f6 | PGS encoder + decoder model | A |
| 2 | a96d0dfb2f | OkLab move | B |
| 3 | f01af575cb | NeuQuant quantize API | B |
| 4 | af25d87a50 | Palette mapping extraction | B |
| 5 | fd1c1c7420 | vf_paletteuse refactor | B |
| 6 | e3d4e9604f | Region-weighted quantization | B |
| 7 | 904e64655f | Median Cut + palettegen refactor | B |
| 8 | a0c004ce0f | ELBG move + algo | B |
| 9 | 282fff42de | Subtitle renderer | C |
| 10 | b2344f3e3c | Renderer multi-timepoint API | C |
| 11 | 11a15c8bda | Core text-to-bitmap | D |
| 12 | 5564065730 | Animation classification | D |
| 13 | 57f01a5c28 | Fade animation | D |
| 14 | 4d96bd80fc | Event coalescing | D |
| 15 | efee54529c | GIF RGBA quantization | E |
| 16 | b5f3a76c2c | OCR library | F |
| 17 | 65e74aa15a | OCR fftools orchestration | F |

---

## Critical Issues (blocks acceptance)

### C1. `#include .c` file pattern [patches 12, 13]

`ffmpeg_subtitle_animation.c` is `#include`d directly into other `.c`
files. The commit comment calls this "standard FFmpeg .c inclusion
pattern for testability" — but this is not an FFmpeg pattern. FFmpeg
uses `.c` inclusion only for generated codec tables. For testable
internal functions, FFmpeg either makes them non-static with `ff_`
prefix or uses `tests/checkasm/`.

**Fix:** Extract `ffmpeg_subtitle_animation.c` into a proper `.c`/`.h`
pair. Compile as a separate object in `fftools/Makefile`. Test files
link against the object. Functions need `ff_` prefix if called from
test harnesses, or the test harness can include a thin wrapper.

**Discussion:** The reason for the `#include .c` pattern was to keep
animation utilities as static functions (avoiding symbol pollution)
while still being testable. The upstream-correct approach is to accept
the symbol visibility cost and use `ff_` prefix, which is consistent
with how `ffmpeg_filter.c` and `ffmpeg_mux.c` expose internal helpers.

### C2. Public API naming [patches 9, 10, 16]

`avfilter_subtitle_render_*` and `avfilter_subtitle_ocr_*` use the
`avfilter_` prefix, which in FFmpeg convention denotes the public
libavfilter API. This implies:

- ABI stability commitments
- Version bump + APIchanges entry
- Full Doxygen documentation
- Review scrutiny appropriate for permanent public API

The functions are only called from fftools. They don't need to be
public.

**Fix:** Change to `ff_subtitle_render_*` and `ff_subtitle_ocr_*`
(internal symbols). Remove APIchanges entries and version bumps for
these functions.

**Discussion:** The original reason for `avfilter_` prefix was that
`ff_` symbols are invisible to fftools in shared builds. In shared
builds, fftools is a separate binary that links against `libavfilter.so`,
and `ff_` symbols are not exported. This is why `sub2video` code lives
directly in fftools rather than calling filter internals.

Options:
1. **Keep public with `avfilter_` prefix** — requires full API review,
   version commitment, harder upstream path but correct for shared builds
2. **Use `ff_` prefix** — simpler review, but breaks shared builds where
   fftools can't see `ff_` symbols
3. **Move orchestration entirely into fftools** — same pattern as
   sub2video. The render/OCR code would be in fftools `.c` files that
   are compiled directly into the ffmpeg binary. libass and Tesseract
   would be linked directly to fftools, not through libavfilter.

Option 3 avoids the API naming debate entirely but requires build
system changes (fftools linking against libass/tesseract directly).
Option 2 only works for static builds. Option 1 is the current
approach and arguably correct.

**Recommendation:** Keep `avfilter_` for now with strong justification
in the cover letter. The API surface is small (5 render functions,
5 OCR functions) and follows the precedent of `avfilter_graph_*` being
public. If reviewers push back, fall back to option 3.

### C3. ABI break: enum reordering [patch 8]

`AVQuantizeAlgorithm` enum values were reordered alphabetically:

```c
// Before (patches 3-7):
AV_QUANTIZE_NEUQUANT = 0,
AV_QUANTIZE_MEDIAN_CUT = 1,

// After (patch 8):
AV_QUANTIZE_ELBG = 0,        // INSERTED — shifts all others
AV_QUANTIZE_MEDIAN_CUT = 1,
AV_QUANTIZE_NEUQUANT = 2,    // WAS 0, NOW 2
```

Code compiled against the old enum and linked against the new library
silently uses the wrong algorithm.

**Fix:** Append `AV_QUANTIZE_ELBG` at the end:

```c
AV_QUANTIZE_NEUQUANT = 0,
AV_QUANTIZE_MEDIAN_CUT = 1,
AV_QUANTIZE_ELBG = 2,
```

Also add `AV_QUANTIZE_NB` sentinel and use it for AVOption max values
instead of hardcoding the last enum value.

**Discussion:** This is a hard rule in FFmpeg — new enum values are
always appended, never inserted. The alphabetical ordering was
cosmetic and not worth the ABI break. The fix is mechanical.

### C4. ELBG move mixed into Median Cut patch [patch 7]

Patch 7 does two unrelated things:

1. Moves `elbg.{c,h}` from `libavcodec/` to `libavutil/` (updates 5
   consumers: a64multienc, cinepakenc, msvideo1enc, roqvideoenc, vf_elbg)
2. Adds Median Cut quantizer algorithm with palettegen refactor

These must be separate patches. The ELBG move is a significant
cross-library change with 5 consumer updates. The Median Cut addition
is a new algorithm. Mixing them makes review impossible.

**Fix:** Split into two patches:
- `lavu: move ELBG algorithm from libavcodec to libavutil`
- `lavu: add Median Cut quantizer algorithm`

**Discussion:** These were originally separate patches (the series had
21 commits) and were squashed during the consolidation pass. The
squash went too far — these two are logically independent and should
have stayed separate. This brings us back to 18 patches, which is
fine.

### C5. fftools including private headers [patch 17]

```c
#include "libavcodec/ass.h"       // ff_ass_subtitle_header_full
#include "libavformat/avlanguage.h" // ff_convert_lang_to
```

These are `ff_`-prefixed internal APIs. fftools should not call them.

**Fix options:**
1. Make the needed functions public (`av_` prefix, APIchanges entry)
2. Duplicate the minimal needed code in fftools
3. Use public alternatives that already exist

For `ass.h`: the function `ff_ass_subtitle_header_full` constructs an
ASS header string. This could be replaced with a simple string
template in fftools — it's ~10 lines.

For `avlanguage.h`: `ff_convert_lang_to` converts between ISO 639
language code formats. This is genuinely useful as a public API and
could be proposed for promotion to `av_convert_lang_to`.

**Recommendation:** Inline the ASS header construction in fftools
(trivial). Propose `av_convert_lang_to` as a public API in a separate
preparatory patch.

### C6. Unprefixed struct names in header [patch 4]

`palettemap.h` exposes these in the global namespace:

```c
struct color_info
struct color_node
struct cached_color
struct cache_node
```

These will collide with user code.

**Fix:** Prefix with `FF`:

```c
struct FFColorInfo
struct FFColorNode
struct FFCachedColor
struct FFCacheNode
```

**Discussion:** The commit message acknowledges this as future work.
Upstream won't accept "future work" — it must be done now. The rename
is mechanical but touches many lines in `palettemap.c` and
`vf_paletteuse.c`.

### C7. Mutating const subtitle [patch 11]

`do_subtitle_out` signature changed from `const AVSubtitle *` to
`AVSubtitle *` so that `convert_text_to_bitmap` can rewrite the
subtitle rects in-place. This is a behavioral change to pre-existing
code.

**Fix:** Work on a copy. Allocate a local `AVSubtitle`, copy the
header fields, allocate new rects for the bitmap data, and free after
encoding. The original subtitle is not modified.

**Discussion:** The in-place mutation was a shortcut to avoid
allocation overhead. The cost of copying a subtitle header + rect
pointers is negligible compared to the render + quantize + encode
pipeline. The correctness benefit outweighs the minor performance
cost.

### C8. Unrelated gif.c change [patch 1]

The PGS encoder patch reformats an `av_log` line in `gif.c`. This has
nothing to do with PGS and will be immediately called out.

**Fix:** Remove the gif.c hunk from the PGS encoder patch.

---

## Significant Issues (likely reviewer flags)

### S1. Domain-specific OCR post-processing [patch 16]

The `|`-to-`I` replacement in `avfilter_subtitle_ocr_recognize` is
a subtitle-specific heuristic. A generic OCR API should return raw
Tesseract output. Post-processing belongs in the caller (fftools).

### S2. 460-line animated encoding function [patch 13]

`do_subtitle_out_animated()` has three nearly-identical code paths
for ALPHA, POSITION, and CONTENT change types. Each branch repeats
`local_sub` setup, `check_recording_time`, and `encode_subtitle_packet`
calls. Extract the common pattern into a helper.

### S3. ffmpeg_enc.c growth [patches 11-14]

The subtitle conversion code adds ~1500 lines to `ffmpeg_enc.c`.
Upstream will want a separate file (`ffmpeg_subtitle.c` or similar).

### S4. Over-broad animation detection [patch 13]

`strchr(rect->ass, '{')` triggers multi-timepoint scanning for any
ASS event with override tags, including simple `{\b1}Bold` that has
no animation. Check for animation-specific tags (`\fad`, `\move`,
`\t`, `\fade`) instead.

### S5. Dead struct in refactored filter [patch 5]

`struct stack_node` is left behind in `vf_paletteuse.c` after the
extraction. Remove it.

### S6. Palette index bounds [patch 17]

In `bitmap_to_grayscale`, pixel indices index into the palette array
but are not bounds-checked against `nb_colors`. Since indices are
`uint8_t` (0-255) and palette is 256 entries, this is safe in practice,
but entries beyond `nb_colors` may be uninitialized.

### S7. APIchanges date ordering [patch 3]

The quantize API entry is dated `2026-02-xx` but has a higher version
number than entries dated later. APIchanges must be chronologically
ordered.

### S8. `tests/ref/fate/source` — av_clip vs av_clip_uintp2 [patch 3]

Several `av_clip(x, 0, 255)` calls could use `av_clip_uintp2(x, 8)`.
Reviewers (especially Michael Niedermayer) will flag this.

### S9. GIF palette byte order and transparency [patch 15]

The transparency slot is hardcoded at index 255, but the quantizer
is asked for max_colors=255. If quantization uses all 255 slots,
there's no collision check. And the palette byte order between
`av_quantize_generate_palette` and `ff_palette_map_apply` relies on
both using `0xAARRGGBB` — document this contract.

### S10. Alloc-as-probe for libass availability [patch 11]

The code probes for libass by allocating a 1x1 render context and
checking for NULL. A simple `avfilter_subtitle_render_available()`
boolean would be cleaner.

---

## Low Priority (may come up, not blocking)

- No `av_log` context in render API (functions log to NULL)
- ELBG `av_quantize_apply` is O(n*k) brute force for pixel mapping
- `frame_rate` AVOption allows invalid intermediate values (0x50)
- Deterministic ELBG seed hardcoded to 1
- GIF per-frame palette with no temporal coherence
- Region-weighted `SAMPLES_PER_REGION` cap at 8192 is a magic number
- `bpp` parameter in OCR API is always 1 — unnecessary parameter
- `TessBaseAPIRect()` may be deprecated in newer Tesseract versions
- Incomplete RTL language detection in multi-language strings

---

## Action Plan

### Phase 1: Structural fixes (change patch count/structure)

1. Split patch 7 back into ELBG move + Median Cut → 18 patches
2. Extract `ffmpeg_subtitle_animation.c` to proper `.c`/`.h` pair
3. Split subtitle code out of `ffmpeg_enc.c` into `ffmpeg_subtitle.c`

### Phase 2: API fixes (change interfaces)

4. Fix enum ordering — append `AV_QUANTIZE_ELBG`, add `AV_QUANTIZE_NB`
5. Prefix palettemap structs with `FF`
6. Resolve `avfilter_` vs `ff_` naming (decision needed)
7. Remove `ff_` header includes from fftools (inline ASS header,
   propose `av_convert_lang_to`)
8. Move `|`-to-`I` OCR post-processing to fftools

### Phase 3: Code quality fixes

9. Remove unrelated gif.c change from PGS encoder patch
10. Remove dead `struct stack_node`
11. Fix `do_subtitle_out` const-correctness (work on copy)
12. Refactor 460-line animated encoding function
13. Narrow animation detection heuristic
14. Add integer overflow checks (`nb_pixels`, `dob_used`)
15. Fix APIchanges date ordering
16. Convert `av_clip(x, 0, 255)` to `av_clip_uintp2(x, 8)`

### Phase 4: Polish

17. Add `av_log` context to render/OCR APIs
18. Document palette byte order contract
19. Add `avfilter_subtitle_render_available()` probe function
20. Add bounds check in `bitmap_to_grayscale`
21. Review GIF transparency slot handling

---

## Decision Points

### D1: Public vs internal API naming

The render and OCR APIs must be callable from fftools. In shared
builds, `ff_` symbols are not exported from `.so` files. Three paths:

| Option | Pros | Cons |
|--------|------|------|
| Keep `avfilter_*` (public) | Works in all builds, correct API boundary | Requires API stability commitment, harder review |
| Use `ff_*` (internal) | Simpler review, no stability commitment | Breaks shared/dynamic builds |
| Move to fftools directly | No library API at all, matches sub2video | Requires fftools to link libass/tesseract directly |

**Recommended:** Keep `avfilter_*` for the initial submission with
strong cover letter justification. The API is small, stable, and
follows precedent. If reviewers insist on `ff_`, propose the fftools
integration path as a fallback.

### D2: Subtitle animation in fftools vs library

The animation classification and fade encoding code is currently in
fftools. An alternative is to put it in the encoder itself (the PGS
encoder receives `AVSubtitle` and handles animation internally).

| Option | Pros | Cons |
|--------|------|------|
| Keep in fftools | Encoder stays simple, format-agnostic | fftools grows, logic duplicated if other tools need it |
| Move to encoder | Self-contained encoder, simpler fftools | Encoder grows significantly, harder to review |

**Recommended:** Keep in fftools for the initial submission. The
sub2video precedent shows FFmpeg is comfortable with conversion logic
in fftools. Move to encoder in a follow-up if there's demand.

### D3: File split for fftools subtitle code

Options for the ~1500 lines of subtitle conversion code:

| Option | Files |
|--------|-------|
| Single file | `fftools/ffmpeg_subtitle.c` (all t2b + b2t + animation) |
| Split by direction | `fftools/ffmpeg_sub_bitmap.c` (t2b) + `fftools/ffmpeg_sub_ocr.c` (b2t) |
| Split by function | `fftools/ffmpeg_sub_convert.c` + `fftools/ffmpeg_sub_animate.c` |

**Recommended:** Single `fftools/ffmpeg_subtitle.c` file. The code is
tightly coupled (animation calls conversion, coalescing calls both).
Splitting would create circular dependencies or awkward interfaces.
