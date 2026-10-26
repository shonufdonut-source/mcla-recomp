# DEVLOG: MCLA iPad port

Legend: **[device]** verified on the iPad, **[pc]** verified on the Windows PC, **[read]** read from source or docs, **[assumed]** not verified.

## 2026-10-25: Study (M0)

- [pc] Windows build of upstream `holdmysocks/mcla-recomp` installed to `C:\MCLA`; plays at 720p/30 FPS
  after moving it from a hard drive to the SSD. Heat is a likely cause of late-session stutter on that laptop;
  not proven.
- [read] ReXGlue SDK v0.10.0 already has a macOS MoltenVK path and `linux-arm64` builds. No iOS target.
- [read] Vulkan renderer feature list recorded in `NOTES.md`; the A12X feature check is still open.
- [read] Blocking issue: a CI-built `.ipa` needs generated code derived from the game (NOTES.md section 4).
- Nothing built for iOS. Nothing run on the iPad.

## Not working / unknown

- Everything iOS. No iOS target exists yet.

## 2026-10-25: M1a skeleton app and CI

- [read] Decision: private-repo build for the playable app (game-derived code stays in the private repo's runner
  and artifact). Public fork holds code only.
- [CI] `ios/` skeleton builds on a macos-15 runner and produces `MCLA-unsigned.ipa` with an arm64 Mach-O
  executable (157 KB). First run reported success but had a compile error hidden by a pipe; fixed with
  `pipefail` and an executable check, second run is genuine.
- [assumed] App launches, log viewer, folder picker and device report work. **Not run on the iPad yet.**
- Next: iPad test of M1a (see `ios/README.md`), then M1b: private repo, link the SDK and recompiled code,
  reach the entry point.

## 2026-10-25: M1a on the iPad

- [device] Skeleton installs with iloader and runs on iPad8,1 (iPad Pro 11" 2018), iPadOS 26.7.1, free Apple ID.
- [device] physical memory 3786 MB. `os_proc_available_memory` 2862 MB at launch. Better than the 1.5 to 2 GB guess.
- [device] Metal: Apple A12X GPU, family apple4/apple5 true, apple6/7 false. Unified memory. Max buffer 2048 MB.
  Recommended working set 2730 MB. Read-write texture tier 2. Raster order groups true.
- [device] **Argument buffers tier 0**, **BC texture compression false**, **32-bit float filtering false**.
- Bug seen in the screenshot: every log line was shown twice in the viewer (the file log was not checked).
  Fixed by rebuilding the viewer text from the log; fix not yet re-tested on device.
- Not yet tested on device: Import game folder, Share log.

## 2026-10-25: M1a import and share verified on device

- [device] Viewer shows each log line once (duplicate fix confirmed).
- [device] **Share log works**: `mcla.log` exported from the iPad and read on the PC.
- [device] **App Documents folder works**: after putting `default.xex` into On My iPad > MCLA (the app's own
  Documents folder, via LocalSend + Files), the log reported `1 files, 8 MB, default.xex present: true`.
- [device] Files-app folder picker: first attempt had every item greyed out (files are greyed because the picker
  only accepts folders; one test stopped at the Downloads folder, which is in iCloud with an upload error).
  The folder bookmark path (`saved game folder: ...`) is **not verified**. The Documents folder route is the
  one to use for now.
- Decision: game files live in the app's Documents folder (On My iPad > MCLA). The PC prep script (M3) will
  arrange files for that layout.
- Privacy note: do not stage game files in iCloud Drive. Use On My iPad.
