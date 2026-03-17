# FFmpeg Subtitle Tools

Pre-built FFmpeg with full subtitle format conversion. Text to bitmap, bitmap to text, and everything between.

**Website:** [slartibardfast.github.io/pgs-release](https://slartibardfast.github.io/pgs-release/)

## Quick start

```bash
# SRT/ASS/WebVTT to Blu-ray PGS
ffmpeg -i subtitles.srt -s 1920x1080 output.sup

# PGS/DVD/DVB bitmap to SRT via OCR
ffmpeg -i input.mkv -c:s srt output.srt

# RGBA to GIF without filter pipeline
ffmpeg -i input.mp4 -c:v gif out.gif
```

## Downloads

Static binaries for 6 platforms on the [releases page](https://github.com/slartibardfast/pgs-release/releases). No dependencies. LGPL 2.1.

## Development

23 patches on FFmpeg, currently in its sixth iteration. See the [development page](https://slartibardfast.github.io/pgs-release/development.html) for patch structure, series history, and data.

Grew out of [PunkGraphicStream](https://github.com/slartibardfast/punkgraphicstream). Developed with assistance from [Claude](https://claude.ai) (Anthropic).
