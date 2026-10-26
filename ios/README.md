# MCLA iOS app: M1b device probe

This build contains **no game code and no game data**. It links the ReXGlue SDK core and MoltenVK and adds a
**Run probe** button that tests, on the iPad, the parts most likely to break on iOS: Vulkan through MoltenVK,
the SDK's guest memory layout, fault handling and fibers. The M1a features (device report, log, share, Files
import) are unchanged.

## Get the .ipa

1. GitHub repo, **Actions** tab, workflow **iOS app IPA**, newest green run.
2. Download the artifact **MCLA-probe-ipa** (a zip; unzip it to get `MCLA-unsigned.ipa`).
   You must be signed in to GitHub to download artifacts.

## Install with iloader (Windows)

1. Install iTunes from the Microsoft Store (iloader needs its USB driver) and iloader from
   <https://github.com/nab138/iloader/releases> or iloader.app (the only official sources).
2. Plug the iPad in, unlock it, tap **Trust**. Turn on **Developer Mode** (Settings, Privacy & Security).
3. Open iloader, sign in with your Apple ID, choose **Import IPA**, pick `MCLA-unsigned.ipa`.
   Installing over the earlier build keeps the app's Documents folder.
4. On the iPad: Settings, General, VPN & Device Management, trust your Apple ID profile if asked.
5. Free Apple ID limits: the app expires after 7 days and must be reinstalled or refreshed.

## What to test and what to send back

1. Close other apps, open MCLA, tap **Run probe**. It takes a few seconds and logs five numbered steps:
   `[1/5] Vulkan`, `[2/5] page allocation`, `[3/5] guest memory arena`, `[4/5] fault handler`, `[5/5] fibers`,
   then `probe finished: every step completed`.
2. Tap **Share log** and send the file(s), or scroll and screenshot the whole log.
3. **If the app closes during the probe**, that is a result, not a failure of the test: reopen the app and tap
   **Share log**. It sends `mcla.log` and `mcla-previous.log`; the previous one shows the step that stopped it.
   A crash report also helps: Settings, Privacy & Security, Analytics & Improvements, Analytics Data, newest
   `MCLA-...ips`.
