# FFmpeg Release Requirements

Customer release packages must use an LGPL shared FFmpeg build.

Required FFmpeg build properties:

- Shared libraries/DLLs are used.
- The FFmpeg configure line does not contain `--enable-gpl`.
- The FFmpeg configure line does not contain `--enable-nonfree`.
- GPL-only encoders such as `libx264` and `libx265` are not required by the SDK.

The DShow recorder prefers the FFmpeg Media Foundation encoders:

- `h264_mf` for 8-bit H.264/AVC output.
- `hevc_mf` for HEVC/H.265 output from 10-bit input formats when available.

For internal testing only, `GCAP_FFMPEG_ALLOW_GPL_ENCODERS=1` allows the recorder
to fall back to GPL encoders such as `libx264` or `libx265`. Do not set this in a
customer release environment.

Before shipping:

1. Run `third_party\ffmpeg\bin\ffmpeg.exe -version`.
2. Confirm the `configuration:` line does not include `--enable-gpl` or `--enable-nonfree`.
3. Include FFmpeg notices, LGPL license text, source availability, and the exact
   FFmpeg build/source information with the customer package.
4. Build or configure with `GCAP_ENFORCE_LGPL_FFMPEG=ON` when preparing a release.

`pack_qt6_viewer.bat` also checks the local FFmpeg build and stops packaging when
GPL or nonfree flags are detected.
