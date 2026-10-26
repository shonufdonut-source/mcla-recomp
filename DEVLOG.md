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
