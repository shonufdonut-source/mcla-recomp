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
