# Updating MCLA on the iPad with SideStore (no PC needed)

The game files (On My iPad > MCLA) stay on the iPad; only the app (about 25 MB) is replaced.

## Once, at home with the PC

1. Install **SideStore** on the iPad. iloader can install it, and it can also create the
   *pairing file* SideStore needs. Follow SideStore's own instructions at docs.sidestore.io
   for the current steps.
2. Install the VPN app SideStore uses (LocalDevVPN or StosVPN, whichever docs.sidestore.io names
   now) and connect it once.
3. Test once at home: install the current MCLA build with SideStore (steps below) and check that
   the game still starts with your game files.

## Every new build, from anywhere

### 1. Download the build in Safari

1. Open **Safari** and go to **github.com**. Sign in with the `shonufdonut-source` account (the
   build repository is private, so you must be signed in).
2. Open the build link from the chat
   (`https://github.com/shonufdonut-source/mcla-ios-private/actions/runs/...`),
   or: the `mcla-ios-private` repository > **Actions** > the newest run.
3. Scroll to the bottom, to **Artifacts**, and tap **MCLA-game-ipa**. Safari saves
   `MCLA-game-ipa.zip` (about 25 MB) to **Files > Downloads**.
   - No *Artifacts* section? Tap **aA** in the address bar > **Request Desktop Website** and
     look again.
   - Artifacts are kept for 3 days.

### 2. Unzip it

1. Open the **Files** app > **Downloads**.
2. Tap **MCLA-game-ipa.zip** once. Files unzips it; inside is **MCLA-game-unsigned.ipa**.

### 3. Install it with SideStore

1. Open the VPN app and make sure it is **connected**.
2. Open **SideStore** > **My Apps** > **+** (top left).
3. Choose **MCLA-game-unsigned.ipa** in Downloads. Sign in with your Apple ID if asked and wait
   until it finishes.

### 4. Check

- **One MCLA icon**: it updated in place and the game files are still there.
- **Two MCLA icons**: do not delete either one (the old one holds the game files); say so in
  the chat.

## Limits of a free Apple ID

- 3 sideloaded apps at a time (SideStore is one of them).
- Apps expire after 7 days. SideStore can refresh them on the iPad while the VPN is connected.
