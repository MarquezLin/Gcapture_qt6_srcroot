# FFmpeg Release Requirements

Customer release packages must use an LGPL shared FFmpeg build.

Required FFmpeg build properties:

- Shared libraries/DLLs are used.
- The FFmpeg configure line does not contain `--enable-gpl`.
- The FFmpeg configure line does not contain `--enable-nonfree`.
- GPL-only encoders are not used by the SDK recorder.

The DShow recorder prefers the FFmpeg Media Foundation encoders:

- `h264_mf` for 8-bit H.264/AVC output.
- `hevc_mf` for HEVC/H.265 output from 10-bit input formats when available.

Before shipping:

1. Run `third_party\ffmpeg\bin\ffmpeg.exe -version`.
2. Confirm the `configuration:` line does not include `--enable-gpl` or `--enable-nonfree`.
3. Include FFmpeg notices, LGPL license text, source availability, and the exact
   FFmpeg build/source information with the customer package.
4. Build or configure with `GCAP_ENFORCE_LGPL_FFMPEG=ON` when preparing a release.

`pack_qt6_viewer.bat` also checks the local FFmpeg build and stops packaging when
GPL or nonfree flags are detected.
