# PPSA04341 — Avatar: The Last Airbender - Quest for Balance

## 2026-10-29 retest

The apparent immediate failure in the sweep was a wrong launch path. The outer
ROM folder contains a nested directory named `Avatar The Last Airbender - Quest
for Balance`; the executable and game data are inside that nested directory.
Launching the outer folder produces `Can't open ...\eboot.bin`. Launching the
nested directory loads the game.

With the current dirty build (`0844e669-dirty`):

- Reached the Nickelodeon opening, narration, a **Press Any Button** title
  screen, and then the **Story Mode / Settings / Help** main menu.
- `MainMenu` is verified. **In-game gameplay is not verified**.
- Rendering is visibly incomplete: opening scenes and the main menu occupy
  only part of the 1280×720 window, with the remaining area black. FPS varied
  substantially (about 14–56 in observed captures). The log reports a few
  Vulkan descriptor-set allocations returning `eErrorOutOfPoolMemory`, but no
  GPU-fence wait was observed. These are leads, not yet established as the
  cause of the partial image.

Evidence in `C:\Users\Gr33k\AppData\Local\Temp\opencode\`:

- `avatar_quest_20261029.png` — opening Nickelodeon logo.
- `avatar_quest_20261029_later.png` — narrated opening scene.
- `avatar_quest_20261029_blackstall.png` — title screen with “Press Any
  Button”.
- `avatar_quest_20261029_menucheck.png` — main menu, Story Mode highlighted.
- `avatar_quest_storytest_20261029.log` — crash after selecting Continue Story.
- `avatar_quest_freshprofile_menu.png` — separate emulator user still sees
  Continue Story; this did not provide a clean-save comparison.
- `avatar_quest_save_after_crash_20261029.dpx.sav` — backup of the save after
  the crash run; the original save file was not deleted.

The launcher status remains `MainMenu` in
`_Build/windows/install/compatibility_db.json`; its comment records the crash
stage. Selecting **Continue Story** reproduces a guest access violation on
`TaskGraphThreadHP 8`: `PC=0x900b20b40`, read from `0xffffffffffffffff`, with the
top caller at `0x900b206f0`. This is a guest-code fault, not an emulator process
that dies immediately at startup and not a GPU-fence timeout. Descriptor-pool
allocation failures also occur during loading but have not been shown to cause
the CPU fault. The user save was backed up after the crash; it was not cleared.

Next: identify the guest routine at the fault/call addresses and its bad
resource/list pointer. Do not clear or replace the saved game without the
user's permission. Once the crash is fixed, verify a level with visible,
controllable gameplay; the current evidence stops at the menu/crashing load.

## 2026-10-29 graphics follow-up

After starting from the refreshed launcher, the title did reach a tutorial
dialogue/in-level scene. Screenshot `avatar_graphics_after_go.png` in the
OpenCode temp folder shows UI/text drawing, but the 3D world is severely
overexposed/corrupt and occupies only the upper-left portion of the window;
most of the rest is black. This confirms the user's report: launch works, but
graphics are not usable, so this is not yet a playable result.

An RX 580 run logs repeated descriptor-set allocations returning
`eErrorOutOfPoolMemory`; however, it did not log `DescriptorHeap: no set` or
failed-pipeline messages in the sampled run, so these allocation retries are
not yet proven to explain the corrupted image. A diagnostic run on the second
adapter (NVIDIA GTX 970, `--gpu 0`) did not provide a clean comparison: Vulkan
lost the device and exited during queue submission. The normal launcher uses
the RX 580 (`--gpu 1`). A graphics-debug run recorded the opening/logo render
target at 3840×2160 with a full-target scissor, but did not reach the affected
in-game scene before the diagnostic overhead made it too slow. A save backup
was made before diagnostics; no save was cleared or restored.

Next diagnostics should capture the affected gameplay draw's viewport, render
target, and copy/present extents on the RX 580. Do not classify this title as
`InGame`/playable until the world renders correctly and control response is
visibly verified. The user would like the Maiden demo tested after Avatar is
fixed.
