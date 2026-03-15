# v3 Expert Panel Review: Consolidated Findings

Five independent reviews covering architecture, API design, security,
subtitle domain expertise, and process/hygiene.

## Critical (must fix before any submission)

### R1. Missing clear Display Sets [subtitle domain]

The encoder emits subtitles but never emits the zero-rect PCS+END
Display Set at `PTS + duration` to clear the screen. Subtitles stick
on hardware players until the next event arrives. This is the single
biggest real-world bug.

**Fix:** Either the encoder or fftools must emit a clear DS at the
subtitle end time.

### R2. Duplicate libass wrappers [architecture]

`libavfilter/subtitle_render.c` wraps libass behind `ff_sub_render_*`.
`fftools/ffmpeg_enc_sub.c` wraps libass again with its own
`SubtitleRenderContext`. Two parallel implementations of the same
thing. Only the fftools version is actually used by the pipeline —
the libavfilter version exists solely for test harnesses.

**Fix:** Pick one. Either keep only the libavfilter version (and have
fftools call it), or keep only the fftools version (and fix the tests
to not need library-level access).

### R3. Integer overflow in DOB tracking [security]

`pgs_check_dob()` computes `w * h` as `int`. A 65535x65535 rect
overflows. `dob_used` accumulates as `int` across the stream.

**Fix:** Use `int64_t` for `needed` and `dob_used`.

### R4. NULL deref on palette data [security]

`pgs_determine_state()` and `pgs_update_cache()` dereference
`h->rects[0]->data[1]` without NULL check. A SUBTITLE_BITMAP rect
with NULL palette data crashes.

**Fix:** Validate `data[1] != NULL` in `pgssub_encode()`.

### R5. `ff_` symbols called across library boundaries [API]

`ff_palette_map_*` (libavutil) called from libavfilter and libavcodec.
`ff_mediancut_*` (libavutil) called from libavfilter. These are
hidden in shared builds.

**Fix:** Use `avpriv_` prefix for cross-library internal functions.

### R6. Patch ordering: PGS encoder must come before wiring [process]

Patch 14 (PGS encoder) is last but patch 13 (wiring) adds FATE test
infrastructure gated on `CONFIG_PGSSUB_ENCODER`. Breaks bisectability.

**Fix:** Move PGS encoder before the wiring patch.

### R7. Missing Changelog and libavcodec version bump [process]

No Changelog entry for new features. No libavcodec MINOR bump for
the PGS encoder.

**Fix:** Add both.

## High (reviewers would likely block)

### R8. Series too large for single submission [process]

14 patches / 10K lines. Recommended split:
1. Palette/quantization infrastructure (patches 1-9)
2. GIF RGBA (patch 11) — independent
3. PGS encoder (patch 14) — independent once (1) lands
4. Subtitle conversion pipeline (patches 10, 12, 13)

### R9. `ffmpeg_sub_util.h` with static function bodies [architecture]

221 lines of function implementations in a header. Not an FFmpeg
pattern for anything beyond trivial one-liners.

**Fix:** Put in a `.c` file and link properly.

### R10. Exposed internal structs in `palettemap.h` [API]

`FFColorInfo`, `FFColorNode`, `FFCachedColor`, `FFCacheNode`,
`FF_PALETTE_CACHE_SIZE` are implementation details in a cross-library
header. Only `FFPaletteMapContext` (opaque) should be visible.

**Fix:** Move internal structs to `palettemap_internal.h` or into
the `.c` file. The debug visualization function (`get_nodes`) can
use the internal header.

### R11. `AV_QUANTIZE_NB` in public header [API]

Sentinel `_NB` values are an anti-pattern FFmpeg is moving away from.

**Fix:** Remove from public header, use internal bounds check.

### R12. `quality` parameter is NeuQuant-specific [API]

Median Cut ignores it. ELBG maps it coarsely to 1-3 steps. The
generic API leaks algorithm-specific semantics.

**Fix:** Document as "hint" or make algorithm-specific configuration.

### R13. No documentation for CLI options and encoder [process]

Missing: `doc/encoders.texi` for PGS encoder, `doc/ffmpeg.texi` for
`-sub_ocr_lang`, `-sub_ocr_datapath`, `-sub_ocr_pageseg_mode`.

### R14. Memory leak on split error path [security]

In `convert_text_to_bitmap()`, if `fill_rect_bitmap(bot_rect)`
fails, the earlier successful `fill_rect_bitmap(rect)` allocations
are leaked.

### R15. PGS encoder: no dimension validation [subtitle domain]

No check that `avctx->width/height` and rect dimensions fit in
16 bits (PGS spec limit). Silent truncation on `bytestream_put_be16`.

### R16. Coalesced multi-event animation bug [subtitle domain]

Alpha animation path re-inits events and loses all but the first
text when re-rendering the peak frame for coalesced subtitles.

## Medium (would be flagged, not necessarily blocking)

### R17. Inconsistent Co-Authored-By format [process]

Two forms used: with and without `(1M context)`. Standardise.

### R18. Dithering enum duplication [API]

`enum FFDitheringMode` in `palettemap.h` mirrors `enum dithering_mode`
in `vf_paletteuse.c`. The filter casts between them. Should use one
enum directly.

### R19. Internal context types need FF prefix [API]

`NeuQuantContext`, `MedianCutContext`, `struct Lab` — should be
`FFNeuQuantContext`, `FFMedianCutContext`, `FFLabColor`.

### R20. `av_quantize_alloc()` returns NULL for both OOM and bad args [API]

Caller can't distinguish. Prefer int return with out-parameter.

### R21. Region API dual-mode overload [API]

`av_quantize_generate_palette` accepts either flat buffer or regions
via the same function. Two modes with different NULL semantics is
confusing.

### R22. `av_clip` vs `av_clip_uintp2` [process]

Several `av_clip(x, 0, 255)` should be `av_clip_uintp2(x, 8)`.
Currently accommodated in `tests/ref/fate/source` but idiomatic
FFmpeg prefers the specific variant.

### R23. DOB accounting on Acquisition Points [subtitle domain]

Acquisition Points add to `dob_used` without resetting, so it
accumulates indefinitely if no epoch start occurs.

### R24. Subtitle render/OCR unconditionally compiled into libavfilter [architecture]

Added to `OBJS` in the Makefile. Every build includes them even if
libass/tesseract are unavailable. The stubs compile but add dead code.

### R25. Pipe-to-I OCR heuristic in library function [architecture]

The `|`→`I` replacement is subtitle-specific. Should be in the
fftools caller, not the library.

## Low (may come up, not blocking)

- Region `MAX_REGIONS=16` undocumented in header
- `nb_pixels` is `int`, limits to ~536M pixels
- Commit dates non-monotonic (rebase artifact)
- RTL language list in OCR will rot — should be a parameter
- Region-sampling code duplicated across three algorithm branches
- APIchanges placeholder hashes (`xxxxxxxxxx`)

## Positive feedback

- Decoder model (CDB/DOB) tracking is unusual for FFmpeg and useful
- Animation classification is format-agnostic (no ASS tag parsing)
- Region-weighted quantization prevents color starvation in karaoke
- Transparent gap splitting for 2-object PGS Display Sets is clever
- Test suite is thorough
- Acquisition Point option helps with seeking
- Palette byte order, composition states, RLE encoding all correct
- Container compatibility (SUP, MKV, M2TS) verified
