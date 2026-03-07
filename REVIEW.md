# Upstream Review Findings

Systematic review of all 19 commits on `pgs-series` for FFmpeg upstream
submission readiness. Findings organized by severity.

## Status Key

- [ ] Not started
- [x] Fixed

---

## A. CRITICAL (would block merge)

### A1. Non-ASCII characters in source files -- DONE

Fixed via rebase with exec. All em-dashes, Unicode arrows, and
box-drawing characters replaced with ASCII equivalents across 12 files.
Author names in copyright headers preserved verbatim.

### A2. Non-ASCII in commit messages -- DONE

Fixed via rebase with exec. Em-dashes replaced with `--` in 2 commits.

### A3. Missing Signed-off-by on 7 commits -- DONE

Added `Signed-off-by: David Connolly <david@connol.ly>` to all 7.

### A4. Inconsistent Signed-off-by email -- DONE

Standardized to `david@connol.ly` across all 19 commits.
Also fixed trailer ordering (Signed-off-by before Co-Authored-By)
on 5 commits.

---

## B. BUGS (functional defects)

### B1. Memory leak: `data[1]` alloc failure in `quantize_rgba_to_rect` -- DONE

Fixed: `av_freep(&rect->data[0])` before returning ENOMEM.

### B2. Memory leak: `data[1]` alloc failure in `fill_rect_bitmap` -- DONE

Fixed: `av_freep(&rect->data[0])` before returning ENOMEM.

### B3. Partial realloc inconsistency in `sub_coalesce_append` -- DONE

False positive: if `texts` realloc succeeds but `durations` fails,
`cap` is not updated, so the next call re-enters the realloc block
and retries both. Added comment documenting this invariant.

### B4. Dangling `sub->rects` in `convert_text_to_bitmap` -- DONE

Fixed: realloc first, assign `sub->rects = new_rects` immediately,
then alloc `bot_rect` in a separate check.

### B5. ELBG codebook values not clamped below zero -- DONE

Fixed: replaced `FFMIN(cb[x], 255)` with `av_clip(cb[x], 0, 255)`.

### B6. Median Cut palette count discarded -- DONE

Fixed: return `ret` (actual count from `ff_mediancut_learn`)
instead of `ctx->max_colors`.

---

## C. SECURITY (integer overflow)

### C1. `gif.c:479` -- `int nb_pixels = w * h`

GIF allows up to 65535x65535. Product overflows `int` (max ~2.1B).
Fix: `if ((int64_t)w * h > INT_MAX) return AVERROR(EINVAL);`

### C2. `pgssubenc.c:354` -- `rect->w * rect->h * 4`

No guard against overflow in RLE allocation size.
Fix: `int64_t` intermediate with `INT_MAX / 4` check.

### C3. `ffmpeg_enc.c` -- `rw * rh` in 4 locations

Lines 418, 464, 626, 1366: all use `int nb_pixels = rw * rh` without
overflow check.
Fix: use `(int)FFMIN((int64_t)rw * rh, INT_MAX)` pattern (already used
at line 1394).

### C4. `subtitle_render.c:206-208` -- `stride * bh` allocation

`stride = bw * 4` then `av_mallocz(stride * bh)`. Both multiplications
are `int * int` with no overflow guard. Also no upper bound on canvas
dimensions in `avfilter_subtitle_render_alloc`.

Fix: add `if ((int64_t)canvas_w * canvas_h > INT_MAX / 4)` in alloc,
and use `(size_t)stride * bh` for the malloc call.

### C5. `neuquant.c` / `mediancut.c` -- missing `nb_pixels` overflow guard

Called from `av_quantize_generate_palette` without `nb_pixels > INT_MAX / 4`
check when no regions are used.
Fix: add the check in `av_quantize_generate_palette` before dispatching.

### C6. `palettemap.c:192-193` -- `y_start * linesize` overflow

`int * int` multiplication with no protection.
Fix: cast to `size_t` or `ptrdiff_t`.

---

## D. API BOUNDARY (missing validation)

### D1. `palettemap.c:555` -- no dither enum range check

`ff_palette_map_apply` does not validate `dither < 0 || dither >= FF_NB_DITHERING`.
Fix: add `if (dither < 0 || dither >= FF_NB_DITHERING) return AVERROR(EINVAL);`

### D2. `palettemap.c:555` -- no w/h/x_start/y_start validation

Negative values or values exceeding buffer size cause out-of-bounds writes.
Fix: validate non-negative and within reasonable bounds.

### D3. `palettemap.c:525` -- no NULL check on `palette` param

`ff_palette_map_init` segfaults on NULL palette.
Fix: `if (!palette) return NULL;`

### D4. `palettemap.c:588,593` -- no NULL check in get_palette/get_nodes

Other functions check for NULL ctx; these two don't.
Fix: add `if (!ctx) return NULL;`

### D5. `subtitle_render.c:47` -- no upper bound on canvas dimensions

Only checks `<= 0`, not overflow-inducing large values.
Fix: add upper bound check (e.g., 32768x32768 or INT_MAX/4 check).

---

## E. STYLE -- Lines over 80 characters

### E1. Our new code (must fix)

- [ ] `pgssubenc.c:553-558` -- AVOption lines (5 lines)
- [ ] `quantize.h:52` -- enum comment
- [ ] `palettemap.h:142` -- function declaration
- [ ] `palettemap.c:106,109,114` -- function calls
- [ ] `subtitle_render.h:72,168` -- Doxygen lines
- [ ] `gif.c:558` -- error message
- [ ] `ffmpeg_enc.c:938,1061,1131,1174` -- function calls

### E2. Verbatim upstream copies (judgment call)

Lines in `palettemap.c` (dithering code, 95-110 chars) and `palette.c`
(lookup tables, OkLab conversion) are verbatim copies from
`vf_paletteuse.c`. Reformatting would make the extraction diff
unverifiable. Note in commit message that these are kept verbatim.

### E3. Test files (should fix)

Multiple lines >80 in all 5 API test files and `quantize.c` test.

---

## F. STYLE -- Other

### F1. Hardcoded 256 instead of AVPALETTE_COUNT

**pgssubenc.c:** Lines 59, 195, 232, 313 use `256` where
`AVPALETTE_COUNT` should be used.

### F2. Missing spaces in operators

**palettemap.c:74:** `{.srgb=srgb}` -- use `.srgb = srgb`
**palettemap.c:134:** `color>>24` -- use `color >> 24`

### F3. Include order

**palettemap.c:32:** Own header `palettemap.h` should be first include,
not last.

### F4. Internal structs exposed in header

**palettemap.h:45-67:** `color_info`, `color_node`, `cached_color`,
`cache_node` are implementation details leaked into the header solely
for `ff_palette_map_get_nodes()`. Consider making return type opaque.

### F5. Unused include

**palettemap.h:30:** `#include "pixfmt.h"` is not used in the header.

### F6. Doxygen `[in]/[out]` inconsistency

**palettemap.h:** `ff_palette_map_init` uses plain `@param`,
`ff_palette_map_get_palette` uses `@param[in]`. Pick one style.

### F7. Const-casts without comment

**subtitle_render.c:104,117,136,149:** Cast away `const` for libass API
calls. Add brief comment: `/* libass API takes non-const but does not modify */`

### F8. Missing libass log callback

**subtitle_render.c:54:** `ass_library_init()` without
`ass_set_message_cb()`. libass messages bypass `av_log`. Compare
`vf_subtitles.c` which sets a callback.

### F9. Alphabetical ordering in Makefile

**libavutil/Makefile:169:** `mediancut.o` should appear before `mem.o`,
not after `murmur3.o`.

### F10. `av_assert0` in library code

**mediancut.c:184-185:** `av_assert0(box->len >= 1)` will abort the
process. Library code should return errors, not assert.

### F11. Missing `const` qualifier

**neuquant.c:140:** `int32_t *n = ctx->network[i]` in `contest()` --
never modified, should be `const int32_t *n`.

### F12. Missing Doxygen on internal functions

**neuquant.h:** No Doxygen on `ff_neuquant_*` functions. Contrast
`mediancut.h` which documents them.

---

## G. COMMIT MESSAGE ISSUES

### G1. Inconsistent trailer ordering

Commits 936eea48 and 260e5d61 have `Co-Authored-By` before
`Signed-off-by`. Convention is `Signed-off-by` first.

### G2. Missing ticket references

Commits 8-11 (fftools animation/coalescing) relate to #3819 but have
no `Ref:` trailer. Commits 6-7 correctly reference it.

### G3. Body lines slightly over 72 characters

Commits 71d11a909b and 5bf6e8c8db have 1-2 lines at 73-74 chars.

### G4. Misleading "Extract" wording

Commit 7af2968f says "Extract the Median Cut quantization algorithm"
but it creates a new implementation; the extraction happens in the
next commit (ee118f62). Reword to "Add" or "Implement".

---

## H. DESIGN CONCERNS (may be raised by reviewers)

### H1. `quality` parameter ignored for Median Cut

`av_quantize_generate_palette` accepts `quality` (1-30) but Median Cut
ignores it entirely. Document this or use quality to control box-split
iterations.

### H2. Region-sampling code duplication

**quantize.c:255-342:** The region-sampling logic (find max_px, compute
per_region, call `build_region_samples`) is copy-pasted across all 3
algorithm branches. Extract to a helper.

### H3. Over-broad animation detection

**ffmpeg_enc.c:1318:** `strchr(text, '{')` treats any `{` as animation.
Common static ASS tags like `{\an8}` trigger the expensive multi-pass
render path. Consider checking for animation-specific tags only.

### H4. Magic numbers in fftools

Repeated values without named constants:
- `42` (frame_ms fallback, line 808)
- `32` (transparent gap threshold, animation.c:187)
- `256 * 4` (palette buffer size, 6+ occurrences)
- `1024 * 1024` (subtitle_out_max_size, defined in 2 places)
- `10` (quality argument, 3+ places)
- `254` (palette version limit, line 1002)

### H5. Stale `elbg_filter_deps` in configure

**configure:4112:** `elbg_filter_deps="avcodec"` but ELBG code has
moved to libavutil. The dependency is now incorrect (though harmless
since avcodec is almost always enabled).

### H6. APIchanges split inconsistency

**doc/APIchanges:14-23:** All 8 subtitle_render functions listed under
one version (lavfi 11.13.100), but they were added across 2 commits.
Either squash the commits or add separate version bumps.

### H7. `quantize_method` field unused within PGS encoder

**pgssubenc.c:47:** The AVOption stores the value but the encoder never
reads it -- it exists solely for external query via `av_opt_get_int`.
Add a comment explaining this pattern.

---

## Fix Priority Order

1. **A1-A4:** Non-ASCII + Signed-off-by (automated sweep, then rebase)
2. **B1-B4:** Memory leaks and dangling pointer (security-adjacent)
3. **C1-C6:** Integer overflow guards
4. **D1-D5:** API boundary validation
5. **B5-B6:** ELBG clamp + Median Cut count
6. **E1,E3:** Line length in our code
7. **F1-F12:** Style nits
8. **G1-G4:** Commit message cleanup (during final rebase)
9. **H1-H7:** Design items (address or document in commit messages)
