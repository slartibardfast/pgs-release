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

## Execution Plan: Full Series Rebuild

The fixes are too interconnected for surgical edits to individual
patches. Rebuild the entire series from the working HEAD.

### New patch structure (16 patches)

```
Series A: PGS Encoder (standalone)
 1. lavc/pgssubenc: add HDMV PGS subtitle encoder

Series B: Quantization Infrastructure
 2. lavu: move OkLab palette utilities from libavfilter
 3. lavu: add color quantization API with NeuQuant
 4. lavu: extract palette mapping and dithering from vf_paletteuse
 5. lavfi/vf_paletteuse: use libavutil palette mapping
 6. lavu/quantize: add region-weighted palette generation
 7. lavu: move ELBG from libavcodec to libavutil
 8. lavu: add Median Cut quantizer algorithm
 9. lavu: add ELBG quantizer algorithm

Series C: Text-to-Bitmap (depends on B)
10. fftools: add text-to-bitmap subtitle conversion
11. fftools: add subtitle animation and event coalescing

Series D: GIF Encoder (depends on B)
12. lavc/gif: add RGBA input with built-in quantization

Series E: OCR Bitmap-to-Text
13. fftools: add bitmap-to-text subtitle conversion via OCR

Series F: Quantizer Selection (depends on B+C)
14. lavc/pgssubenc, fftools: add quantize_method option
```

### Fixes integrated into each patch

| Patch | Fixes integrated |
|-------|-----------------|
| 1 | Remove gif.c cosmetic, decoder model compliance built-in |
| 4 | FF-prefix structs, remove dead stack_node |
| 9 | Enum values appended (ABI-safe) |
| 10 | Renderer in fftools (not libavfilter), no public API, const-correct subtitle handling |
| 11 | Animation in fftools proper .c file (no #include .c), narrow detection heuristic |
| 12 | Sorted palette fix built-in |
| 13 | OCR in fftools (not libavfilter), |→I fixup in caller not library |

### New fftools files

```
fftools/ffmpeg_enc_sub.c   — subtitle_render + text-to-bitmap + animation
fftools/ffmpeg_enc_sub.h   — declarations for enc_sub functions
fftools/ffmpeg_dec_sub.c   — subtitle_ocr + bitmap-to-text + dedup
fftools/ffmpeg_dec_sub.h   — declarations for dec_sub functions
```

### Files removed from libavfilter

```
libavfilter/subtitle_render.{h,c}  — moved to fftools
libavfilter/subtitle_ocr.{h,c}    — moved to fftools
```

No libavfilter version bumps or APIchanges entries for render/OCR.

---

## Decisions

### D1: Move render/OCR code to fftools (option 3)

The render and OCR code moves from libavfilter into fftools. No
library API — the code compiles directly into the ffmpeg binary,
matching the sub2video precedent. fftools links libass and Tesseract
directly via configure changes.

This eliminates the `avfilter_*` vs `ff_*` naming debate, the API
stability commitment, and the shared-build visibility problem.

The `subtitle_render.{h,c}` and `subtitle_ocr.{h,c}` files move
from `libavfilter/` to `fftools/`. The APIchanges entries and
libavfilter version bumps for these functions are removed.

### D2: Animation stays in fftools

Animation classification and fade encoding remain in fftools,
consistent with the sub2video precedent and the D1 decision.
The encoder stays simple and format-agnostic.

### D3: Three-file split matching fftools convention

```
fftools/ffmpeg_enc_sub.c  — text-to-bitmap (~1200 lines)
fftools/ffmpeg_dec_sub.c  — bitmap-to-text (~500 lines)
fftools/ffmpeg_mux_sub.c  — gate logic, subtitle options (~50 lines)
```

Naming follows the existing pattern where the base concern comes
first and the specialization is the suffix:

```
ffmpeg_enc.c      →  ffmpeg_enc_sub.c
ffmpeg_dec.c      →  ffmpeg_dec_sub.c
ffmpeg_mux_init.c →  ffmpeg_mux_sub.c
```

The animation utilities (`ffmpeg_subtitle_animation.c`) fold into
`ffmpeg_enc_sub.c` as regular functions with proper declarations in
a header.
