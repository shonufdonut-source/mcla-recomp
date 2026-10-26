# MCLA iOS skeleton (M1a)

This build contains **no game code and no game data**. It only proves the toolchain: app launch,
device/memory report, in-app log with share, and picking a game folder in the Files app.

## Get the .ipa

1. GitHub repo, **Actions** tab, workflow **iOS skeleton IPA**, newest green run.
2. Download the artifact **MCLA-skeleton-ipa** (a zip; unzip it to get `MCLA-unsigned.ipa`).
   You must be signed in to GitHub to download artifacts.

## Install with iloader (Windows)

1. Install iTunes from the Microsoft Store (iloader needs its USB driver) and iloader from
   <https://github.com/nab138/iloader/releases> or iloader.app (the only official sources).
2. Plug the iPad in, unlock it, tap **Trust**. Turn on **Developer Mode** (Settings, Privacy & Security).
3. Open iloader, sign in with your Apple ID, choose **Import IPA**, pick `MCLA-unsigned.ipa`.
4. On the iPad: Settings, General, VPN & Device Management, trust your Apple ID profile if asked.
5. Free Apple ID limits: the app expires after 7 days and must be reinstalled or refreshed.

## What to test and what to send back

1. Open the app. It should show a log. **Send a screenshot of the whole log.** The important lines are
   `physical memory`, `os_proc_available_memory`, the `Metal device` / `supportsFamily` lines and
   `BC texture compression`.
2. Tap **Import game folder**, pick the folder with your extracted game files, and note what the log says
   about files and size (nothing is copied or uploaded).
3. Tap **Share log** and send the `mcla.log` file, or screenshot the share sheet's file.
4. If the app crashes at launch, send the crash report: Settings, Privacy & Security, Analytics & Improvements,
   Analytics Data, newest `MCLA-...ips`.
