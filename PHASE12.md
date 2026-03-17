# Phase 12: FATE CI + Website Test Visibility

## Goal

1. GitHub Actions workflow that builds FFmpeg (non-static, system libs) and runs
   FATE on every push/PR. Validates patches stay functional. No published assets.
2. Website section showing what we test, linking to CI results.

## 12a: FFmpeg FATE CI Workflow — PLANNED

- Single job on `ubuntu-24.04`
- System packages: `libass-dev`, `libtesseract-dev`, `tesseract-ocr-eng`, `nasm`
- Default FFmpeg build with `--disable-doc --enable-libass --enable-libtesseract`
- Run 12 self-contained FATE tests (no external samples needed)
- `fate-sub-pgs` excluded (needs fate-suite samples); covered by other tests

## 12b: Website Test Section — PLANNED

- Add "Tests" section to main page (index.html) linking to development page
- Development page already has the FATE list — add CI badge and note any
  known failures with upstream/WIP attribution
- Link to OCR languages page rather than duplicating

## Known Pre-existing Failures

- `sws-unscaled` — fails on clean n8.1 (not our patches)
