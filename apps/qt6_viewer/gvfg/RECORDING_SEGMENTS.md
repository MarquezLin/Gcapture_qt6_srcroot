# GVFG recording capacity protection

- GVFG opts into a 2 GiB target (`cfg.segment_bytes`); DirectShow leaves it zero.
- The selected filename is the first segment; later segments append `_002`, `_003`, etc.
- Switch containers on a video key packet, without resetting the video/AAC encoders.
- Timestamps in each segment are rebased to the new video keyframe; negative AAC
  preroll timestamps are possible at the boundary.
- Begin looking for a keyframe 64 MiB before the target. If no keyframe arrives
  before the safety threshold, stop rather than exceed the target.
- Check available space at start and at most once per second during writes.
  Reserve 256 MiB for queued packets and finalization. This cannot prevent an
  unrelated application from exhausting the disk between checks.
- Never overwrite an existing subsequent segment. Report write, trailer, and
  file-close failures; do not claim a failed segment is playable.

## Manual verification (not yet performed)

1. Build in Qt Creator. For a short test, temporarily set GVFG's
   `cfg.segment_bytes` to `8ULL * 1024 * 1024`, record several segments with
   audio, then restore `2ULL * 1024 * 1024 * 1024` before normal use.
2. Check every segment with `ffprobe` and play each independently. Check the
   first video frame, the end of the old segment, AAC timestamps, and audible
   A/V alignment across boundaries. Encoder key flags must produce independently
   decodable segments on the actual installed encoder.
3. Pre-create a `_002` file: recording must stop with an error and preserve it.
4. Use a disposable test volume with less than 256 MiB free: recording must
   refuse to start. With a nearly full test volume, verify clean stop while recording.
   Do not fill the system disk for this test.
5. Perform a real long recording on FAT32; verify files stay below its limit.
6. Verify cancellation, ordinary Stop, and capture format changes still stop
   cleanly. DirectShow should retain its existing single-file behavior.

No hardware, runtime, build, or recovery guarantee follows from source checks.
