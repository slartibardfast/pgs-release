# Phase 13: PGS Encoder Features (v6)

## Status: WIP — 5 of 8 patches committed on `pgs6-wip`

Encoder features beyond v5's optimisations. Exploration revealed that 13a, 13b,
and 13c were already implemented in v5 — they needed FATE test coverage only.
The v6 scope expanded to include a comprehensive forced subtitle pipeline
(disposition bridging, DVB support, stream splitting) alongside the originally
planned features.

## 13a: Palette Reuse — DONE (v5, test added in v6)

The encoder already omits PDS when the palette hasn't changed. A position-only
Normal DS writes PCS+END only — no PDS, ODS, or WDS. The `palette_version`
field correctly references the previous palette.

**FATE test:** `api-pgs-palette-reuse` — Patch 1

## 13b: Multiple Composition Objects — DONE (v5, test added in v6)

The encoder handles 2 non-overlapping rects with correct PCS composition
descriptors, 2 ODS segments, 2 WDS window definitions, and shared PDS. Rect
splitting remains in fftools (`ff_sub_find_gap`) — the correct architectural
layer since splitting operates on pre-quantisation RGBA data.

**FATE test:** `api-pgs-multi-object` — Patch 2

## 13c: Acquisition Point Display Sets — DONE (v5, test added in v6)

The `ap_interval` AVOption (0–60000ms) promotes Normal DS to Acquisition Point
when the interval has elapsed. AP DS contains full PDS + ODS + WDS for
random-access seeking. Composition state 0x40 (not 0x80 as incorrectly stated
in the original plan — 0x80 is Epoch Start).

**FATE test:** `api-pgs-ap-interval` — Patch 3

## 13d: Forced Subtitles — DONE

Added `force_all` boolean AVOption. When set, all composition objects are marked
forced (0x40 in PCS composition descriptor) regardless of per-rect flags. The
existing per-rect `AV_SUBTITLE_FLAG_FORCED` behaviour is preserved when
`force_all` is not set.

**FATE test:** `api-pgs-forced` — Patch 4

## 13e: Rate Control — DONE

Added `max_cdb_usage` AVOption (0.0–1.0, default 0.0 = disabled). Before
encoding each Display Set, the encoder estimates the DS size (worst-case
uncompressed) and computes CDB headroom (accounting for refill at Rx = 2 MB/s
since the last encode). Events exceeding the threshold are dropped with a
warning.

**Design decision:** Drop-with-warning rather than deferral. The HDMV subtitle
encoding API (`avcodec_encode_subtitle`) is synchronous — there's no EAGAIN
mechanism. Full deferral would require an event re-queue in fftools, deferred
to v7 if needed. Drop-with-warning matches hardware authoring tool behaviour.

**FATE test:** `api-pgs-rate-control` — Patch 5

## 13f: Forced Subtitle Pipeline — IN PROGRESS

Three patches completing the forced subtitle pipeline end-to-end:

### Patch 6: Bidirectional Disposition Bridge — TODO

Bridge forced flags between stream-level and rect-level:
- **Input→rects:** `AV_DISPOSITION_FORCED` on input stream →
  `AV_SUBTITLE_FLAG_FORCED` on all decoded rects
- **Encoder→output:** `force_all=1` on encoder → `AV_DISPOSITION_FORCED`
  on output stream (so MKV FlagForced, MPEG-TS subtitling_type are set)

Location: `fftools/ffmpeg_enc_sub.c`

### Patch 7: MPEG-TS DVB Forced Subtitle Types — TODO

Standalone upstream fix. The MPEG-TS demuxer doesn't map DVB subtitling_type
0x30–0x35 (forced) to `AV_DISPOSITION_FORCED`. The muxer doesn't write 0x30
when the disposition is set. Both directions, mirroring the existing
hearing-impaired pattern.

Location: `libavformat/mpegts.c`, `libavformat/mpegtsenc.c`

### Patch 8: `-forced_subs_filter` CLI Option — TODO

fftools CLI option to split subtitle streams by forced flag:
- `forced` — encode only forced events
- `non_forced` — encode only non-forced events (unique — decoder has no equivalent)
- `all` — encode all events (default)

Only meaningful for bitmap→bitmap paths (PGS→PGS, DVD→PGS) where per-rect
forced flags are set by the decoder. For text→bitmap (ASS→PGS), use
`force_all=1` or `-disposition:s forced` instead.

Location: `fftools/ffmpeg_enc_sub.c`, `fftools/ffmpeg_opt.c`

## Forced Subtitle Roundtrip (after v6)

| Source → Target | Works? | How |
|----------------|--------|-----|
| PGS → PGS | Yes | rect→flags preserved (v5) |
| DVD → PGS | Yes | dvdsubdec sets rect→flags (v5) |
| MKV DVB → PGS | Yes | Patch 6 bridges disposition |
| MPEG-TS DVB → PGS | Yes | Patch 7 + Patch 6 |
| ASS/SRT → PGS | Yes | `force_all=1` (Patch 4) or `-disposition:s forced` (Patch 6) |
| PGS → MKV (force_all) | Yes | Patch 6 sets output disposition → matroskaenc FlagForced |
| PGS → MPEG-TS (force_all) | Yes | Patch 6 + Patch 7 muxer writes 0x30 |
| Mixed → forced-only | Yes | Patch 8 `-forced_subs_filter forced` |
| Mixed → non-forced-only | Yes | Patch 8 `-forced_subs_filter non_forced` |

## Known Limitations (upstream, not our series)

- **movenc.c** does not write `AV_DISPOSITION_FORCED` to MP4 track metadata
- **dvbsubdec.c** does not set `AV_SUBTITLE_FLAG_FORCED` per-rect (stream-level only)
- **dvbsubenc.c** does not read `AV_SUBTITLE_FLAG_FORCED`

## Patch Series Structure

Branch `pgs6-wip` off `pgs5-8.1`. Clean `pgs6` branch created when series is
complete, tagged `history/pgs-v6`.

| # | Commit | Description | Status |
|---|--------|-------------|--------|
| 1 | `1e6b5e3554` | FATE test: palette reuse | Done |
| 2 | `1cf8b21a4f` | FATE test: multi-object encoding | Done |
| 3 | `5dec6da839` | FATE test: AP interval | Done |
| 4 | `261ce70484` | `force_all` AVOption + test | Done |
| 5 | `aa9f64bdf9` | CDB rate control (`max_cdb_usage`) + test | Done |
| 6 | — | Bidirectional forced disposition bridge | TODO |
| 7 | — | MPEG-TS DVB forced types (demux + mux) | TODO |
| 8 | — | `-forced_subs_filter` CLI option | TODO |
