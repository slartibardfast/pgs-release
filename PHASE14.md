# Phase 14: Upstream Submission Restructuring (v7)

## Status: Complete — tagged `history/pgs-v7`

Restructured v5+v6 (32 patches on FFmpeg 8.1) into 5 independent upstream
submission series ordered by controversy level. Each series compiles
independently and passes FATE tests.

## Motivation

The v5+v6 stack coupled the PGS encoder (low controversy) with the
quantization API and text-to-bitmap pipeline (high controversy). Decoupling
lets the encoder merge independently.

## Series A: Standalone fix — `pgs7-a` (1 patch)

| # | Commit | Description |
|---|--------|-------------|
| 1 | `775a82ca1a` | lavf/mpegts: DVB forced types 0x30-0x35 |

No dependencies. Submit immediately.

## Series B: PGS encoder + bitmap features — `pgs7-b` (11 patches)

| # | Commit | Description |
|---|--------|-------------|
| 1 | `4162b7519f` | lavc/pgssubenc: add HDMV PGS subtitle encoder |
| 2 | `2d847dcb1b` | lavc/pgssubenc: add palette delta encoding for PDS |
| 3 | `9dfb7bc71d` | lavf/supenc: compute per-segment DTS for PGS timing model |
| 4 | `53140c4258` | fftools: set PGS packet DTS per HDMV decoder timing model |
| 5 | `6aa2169323` | tests: add FATE coverage for PGS encoder features |
| 6 | `8de64586a1` | tests: add FATE test for PGS palette reuse |
| 7 | `37db11ba6f` | tests: add FATE test for PGS multi-object encoding |
| 8 | `eeb354399a` | tests: add FATE test for PGS acquisition point interval |
| 9 | `2807cf6224` | lavc/pgssubenc: add force_all option for forced subtitles |
| 10 | `3119f9cc4c` | lavc/pgssubenc: add CDB rate control via max_cdb_usage |
| 11 | `199083db75` | fftools: add forced subtitle disposition bridge + filter |

No dependencies. The core deliverable — PGS encoder accepting SUBTITLE_BITMAP
input with 9 FATE tests. DTS, disposition bridge, and forced_subs_filter
placed in `fftools/ffmpeg_enc.c` (no `ffmpeg_enc_sub.c` at this stage).

## Series C: Quantization API — `pgs7-c` (10 patches)

| # | Commit | Description |
|---|--------|-------------|
| 1 | `e313b57287` | lavu: move OkLab palette utilities from libavfilter |
| 2 | `7622f59026` | lavu: add color quantization API with NeuQuant |
| 3 | `9115d0d6a2` | lavu: extract palette mapping and dithering from vf_paletteuse |
| 4 | `65954d7643` | lavfi/vf_paletteuse: use libavutil palette mapping |
| 5 | `7cf12137d9` | lavu/quantize: add region-weighted palette generation |
| 6 | `84b2886bff` | lavu: move ELBG from libavcodec to libavutil |
| 7 | `e78c8b09f6` | lavu: add Median Cut quantizer algorithm |
| 8 | `d29fa61053` | lavfi/vf_palettegen: use libavutil Median Cut API |
| 9 | `8abc6f3f29` | lavu: add ELBG quantizer algorithm |
| 10 | `8054179e0e` | lavc/gif: add RGBA input with built-in quantization |

Independent of Series B. Submit in parallel. Generic quantization API
with NeuQuant, Median Cut, ELBG. Also benefits GIF encoding.

Combined base `pgs7-bc` also available (B+C merged for Series D).

## Series D: Text-to-bitmap pipeline — `pgs7-d` (6 patches)

| # | Commit | Description |
|---|--------|-------------|
| 1 | `6968948034` | lavu: move subtitle bitmap utilities to libavutil |
| 2 | `87fa47ebf1` | lavfi: add text subtitle rendering utility via libass |
| 3 | `b6b6e8ce2e` | fftools: add text-to-bitmap subtitle conversion |
| 4 | `1f52d0593e` | fftools: wire subtitle conversion into encoding pipeline |
| 5 | `cc02b2ea5c` | fftools: add event lookahead window for overlapping subtitles |
| 6 | `bd31b3ba45` | lavc/pgssubenc, fftools: add forced_style option |

Depends on B + C. Creates `fftools/ffmpeg_enc_sub.c` with text-to-bitmap
pipeline, event lookahead, and forced_style ASS matching. D4 includes a
no-op `ffmpeg_dec_sub.h` stub (replaced by Series E's real implementation).

## Series E: OCR — `pgs7-e` (2 patches)

| # | Commit | Description |
|---|--------|-------------|
| 1 | `883f340a48` | lavfi: add bitmap subtitle OCR utility via Tesseract |
| 2 | `2d58ff3f15` | fftools: add bitmap-to-text subtitle conversion via OCR |

Depends on D. Optional. Replaces `ffmpeg_dec_sub.h` stub with real OCR.

## Submission Order

1. **Series A** — submit first (1 patch, quick win, builds reviewer rapport)
2. **Series B** — submit next (11 patches, the money shot — PGS encoder)
3. **Series C** — submit in parallel with B (10 patches, quantization API)
4. **Series D** — after B+C merge (6 patches, text-to-bitmap — controversial)
5. **Series E** — after D merge (2 patches, OCR — optional)

## Design Decisions

- **fftools/ffmpeg_enc.c for Series B**: DTS, disposition bridge, and
  forced_subs_filter placed in `do_subtitle_out()` rather than creating
  `ffmpeg_enc_sub.c`. Series D creates the file and migrates the logic.

- **quantize_method removed from Series B**: The AVOption references
  `AV_QUANTIZE_*` enums from Series C. For B standalone, the option is
  omitted. Series D adds it when the quantization API is available.

- **ffmpeg_dec_sub.h stub**: Series D's wiring patch includes OCR function
  stubs so it compiles without Series E. Series E replaces the stubs.

## Verification

All series compile independently (`git rebase --exec 'make -j$(nproc)'`).
All FATE tests pass on each series tip. Tagged `history/pgs-v7` on `pgs7-e`.
