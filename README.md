# KytyPS5 — Experimental Compatibility & Diagnostics Fork

> **Fork note:** this is an experimental fork of [KytyPS5/KytyPS5](https://github.com/KytyPS5/KytyPS5)
> (`upstream`). It regularly merges upstream `main` while adding local
> compatibility testing, diagnostics, and launcher improvements. This fork's
> goal is to make more titles boot and progress reliably, record reproducible
> per-title results, and turn rendering or stability regressions into actionable
> fixes. Compatibility is still experimental; a title reaching gameplay does
> not mean it is stable or fully playable. Results below apply only to this fork
> and the test platform described under [Test Platform](#test-platform).

<img width="1920" height="1040" alt="image" src="https://github.com/user-attachments/assets/5dd8eb48-817c-40fd-a378-6b5f7568a94c" />
<img width="1920" height="1040" alt="image" src="https://github.com/user-attachments/assets/5837dd1b-4d1f-400f-9abe-d78c959f4213" />


## Changes in This Fork

Relative to upstream `main`:

- **Skip gracefully instead of aborting.** Where upstream hits an
  unrecoverable path, this fork keeps the game running where it is safe to do
  so: draws/dispatches with unmaterializable shader resources, failed pipeline
  creations, unresolvable texture overlaps (equal-address images), and
  unrepresentable stencil state are skipped with a log message.
- **Per-game status and notes in the launcher.** Statuses and comments can be
  set per title ID and are merged on save, so local notes persist across
  remote compatibility-database refreshes instead of being overwritten.
- **File-driven compute-shader skip list.** A `skip_cs_hashes.txt` file next
  to the executable lists shader hashes (one per line, or separated by
  spaces/commas) whose dispatches are skipped. Used to work around
  known-hanging compute kernels during triage.
- **Identifiable builds.** The window title and logs include the short git
  hash of the build, so test results can be tied to an exact commit.
- **AMD / RX580-class compatibility.** Enables primitive-topology list restart
  via `VK_EXT_primitive_topology_list_restart` where supported, and falls back
  to `General` image layout when a depth feedback loop is unsupported.
  Strips/fans are expanded for barycentric draws on AMD.
- **Shader decoder additions.** Implements VOPC `0xbb` (`V_CMPX_LE_U16`) and
  accepts clamp/output-modifier on `V_MIN3`/`V_MAX3`/`V_MED3_F16`, with
  regression tests.
- **Triage logging.** Per-dispatch shader hashes, indirect-dispatch counts,
  subgroup caps, VMA heap budgets, and GPU-assisted validation options to
  diagnose hangs and device-memory pressure.

Upstream merges are pulled in regularly; local commits sit on top.

### Fork goals

- Keep the fork close to upstream while carrying focused, reviewable fixes.
- Test a broad local game set and publish the exact observed state, including
  crashes, missing graphics, visual artifacts, and performance limitations.
- Improve compatibility and diagnostics without implying that booting or
  reaching gameplay equals a finished or fully playable experience.

## About KytyPS5

KytyPS5 is a free and open-source PlayStation 5 emulator written in C++ for
Windows and Linux, with experimental macOS support. It is based on a heavily
modified version of [Kyty](https://github.com/InoriRus/Kyty). The project is in
active development, and behavior can change significantly between builds.

> [!IMPORTANT]
> KytyPS5 is not affiliated with Sony Interactive Entertainment or PlayStation.
> The project does not distribute games or copyrighted system software. Use
> only game files that you have obtained legally.

KytyPS5 can boot 2D games and a selection of 3D games, including titles built
with Unreal Engine 4/5, Unity, and custom engines. External low-level emulation
modules are neither required nor planned. Windows and Linux are the primary
platforms and receive the most testing.

Community test results for upstream builds are in the
[KytyPS5 Compatibility List](https://kytyps5.github.io/). Results measured with
this fork on the test platform below follow in the next section.

## Test Platform

All results in the compatibility table were measured on:

- OS: Windows 10/11 x64
- CPU: Intel Core i7-5820K
- GPU: Radeon RX 580 4GB (amdvlk driver)
- RAM: 16GB

Note: the RX 580 does not support mesh shaders, so titles that require
mesh-shader pipelines are hardware-blocked on this platform regardless of
emulator progress.

## Compatibility (Fork Retest, 2026-10-29)

Results below reflect the 2026-10-29 retest on Windows 11 x64, Intel Core i7-5820K, Radeon RX 580 4GB. `InGame` means visible gameplay was reached; it does not imply reliable controls or full playability. The full evidence log is in [the dated compatibility notes](docs/notes/game-compatibility-check-2026-10-29.md).

| Game | Title ID | Status | Latest observed result |
| --- | --- | --- | --- |
| A Plague Tale - Innocence | PPSA02388 | Logo | Extended launcher run rendered a scene and “PRESS” prompt, then Cross opened “Select a Slot”; all three slots displayed “Synchronization conflict.” Did not confirm a slot, reach the main menu, or verify gameplay. Existing settings/save data was unchanged. |
| Alan Wake Remastered | PPSA01925 | InGame | Verified the main menu and Easy difficulty screen. Starting a new game and pressing Circle to skip the opening previously hit `unsupported render-target mip range: current=1 levels=1`. After changing color-target discovery to materialize the selected mip when it exceeds MAX_MIP, Circle skip progressed into the playable road scene. Forward (W) moved Alan into the light and prompted “Look up to the light”; the mapped right-stick control changed the camera view. Control response verified, but performance was poor (roughly 3–11 FPS during the scene). |
| Alone in the Dark | — | DoesntBoot | Did not launch: `eboot.bin` missing from folder. |
| Avatar - The Last Airbender (USA) | — | DoesntBoot | Did not launch: `eboot.bin` missing from folder. |
| Avatar The Last Airbender - Quest for Balance | PPSA04341 | MainMenu | Nested game directory reaches the main menu; selecting Continue Story crashes with a guest access violation. |
| Away - The Survival Series | PPSA04555 | Logo | Booted to title card/logo. |
| Back 4 Blood | PPSA01695 | Logo | Booted to title card/logo. |
| Beyond a Steel Sky | PPSA03979 | Logo | Booted to title card/logo. |
| Borderlands 3 | PPSA01462 | Logo | Booted to title card/logo. |
| Call of the Sea | PPSA01546 | Logo | Booted to title card/logo. |
| Chernobylite | PPSA04747 | Logo | Booted to title card/logo. |
| Resident Evil 8 - Maiden Demo | PPSA01859 | DoesntBoot | Focused relaunch still rendered black after a Cross input; no menu or gameplay/control response was visible. Earlier log records mesh draws being skipped because the host lacks mesh-shader support, followed by a GPU fence stall. Both installed GPUs (GTX 970 and RX 580) lack `VK_EXT_mesh_shader`; a capable GPU or a substantial mesh-shader fallback is needed. |
| Control Ultimate Edition | PPSA01949 | Logo | Booted to title card/logo. |
| DIRT5 | PPSA01552 | DoesntBoot | Emulator process ran, but captured output was black; no title/menu/gameplay visible. |
| Fishing: North Atlantic Enhanced Edition | PPSA02985 | Logo | Found by its full title (including colon) after the initial search missed it. Correct `eboot.bin` launches; after the opening graphic, screen stays black. Cross produced no visible change. No fatal error or GPU-fence stall in this run; no menu/gameplay verified. |
| Five Nights at Freddy's: Security Breach | PPSA04677 | DoesntBoot | Focused relaunch remained black and stopped advancing at frame 1519 (0 FPS). Cross produced no visible change. No menu/gameplay verified; log shows GPU scheduler progress stalled without a fatal error. |
| Five Nights at Freddy's: Help Wanted 2 | PPSA18887 | DoesntBoot | Retest exits during boot with GPU flip submission failure (`result=-2144796661`, `handle=-1`, `index=-1`, `mode=2`); no menu/gameplay. |
| Ghost of Tsushima | PPSA03208 | DoesntBoot | Emulator identified the title during launch, then exited before its game window could be captured. No menu/gameplay verified; no explicit fatal-error block in the run log. |
| Ghostrunner | PPSA03682 | DoesntBoot | Emulator identified the game and reached frame 1915, then stalled on a GPU fence at tick 3633 and exited with code 321 after `vkQueueSubmit` returned `ErrorDeviceLost (-4)`. No menu/gameplay verified. |
| Grand Theft Auto III - The Definitive Edition | PPSA03527 | InGame | Reached the main menu and started a new game. Opening cinematic led into the playable street scene with HUD. Mapped Triangle (C) entered the Kuruma and triggered the “Press R2 to accelerate” prompt, confirming a gameplay input response. Gameplay rendered at roughly 6–12 FPS with very dark/low-contrast scenes; full driving control not tested. |
| Grand Theft Auto: San Andreas - The Definitive Edition | PPSA03524 | MainMenu | Reached the main menu after the Rockstar credits. Gameplay not verified: keyboard input without explicit mappings did not activate Start; a mapped-input retry exited before the menu. Existing database/GitHub report says in-game, but that was not reproduced in this test. |
| Grand Theft Auto: Vice City - The Definitive Edition | PPSA03530 | MainMenu | Reached the main menu and started a new game. Opening cinematic rendered correctly, then transitioned to a very dark exterior scene at roughly 4–8 FPS with no HUD or clearly visible player. Mapped forward input caused no discernible change; controllable gameplay not verified. Earlier menu-only result reported a crash when starting. |
| Ghostwire: Tokyo | PPSA01337 | MainMenu | Reached the main menu (Continue, New Game, Load, Options, Credits). Rendering is severely corrupted, mostly black with wireframe fragments. Emulator exited before navigation/control testing; no gameplay verified. |
| Hot Wheels Unleashed | PPSA02325 | Logo | Reached the title splash, then showed a loading spinner at 0 FPS and exited. The available log is stale (2026-10-02), so its `ErrorOutOfDeviceMemory` report is not evidence for this run. Current-run exit cause, menu, and gameplay are unverified. |
| Indiana Jones and The Great Circle | PPSA26786 | Logo | Not retested: the game folder contains only `.7z`/`.rar` packages, with no `eboot.bin` or unpacked game directory. Previous 2026-10-05 result was boot logo followed by crash before menu. |
| It Takes Two | PPSA02343 | MainMenu | Reached the title prompt and main menu; mapped Cross confirmed through the prompt and opened Play Local > New Game/player selection. Menu/background have severe multicolored corruption though UI text remains readable. Could not proceed past player selection; no gameplay verified. |
| THE KING OF FIGHTERS XV | PPSA02213 | MainMenu | Reached the Press Any Button prompt, main menu, and VS mode selection. Text/UI are readable, but most backgrounds/artwork are black. No match/gameplay verified; classify this run as menu only. |
| Jumanji: The Video Game | PPSA03973 | InGame | Reached title prompt, main menu, local-game setup, character selection, and a gameplay-level scene with player and HUD visible. W was held, but the scene changed during that interval and there was no controlled before/after baseline; movement response is not confirmed. Portions are very dark; performance roughly 9–12 FPS. This improves on the prior logo-only result; cause not isolated between upstream and local dirty changes. |
| LEGO Star Wars: The Skywalker Saga | PPSA01865 | Logo | Reached language selection, seizure warning, and accessibility-options prompt. Emulator exited after input at that prompt, before a game menu was verified. Existing log is stale (2026-10-21), so this run's exit cause is undetermined. |
| MADiSON | PPSA07370 | MainMenu | Reached title prompt, main menu, Story/New Game, and difficulty screen. Rendering is severely black/crushed; menu text is visible but most artwork is missing. Save-slot screen displayed an overwrite warning even for the selected slot; did not confirm or modify save data. No gameplay verified. |
| Maneater | PPSA01862 | DoesntBoot | Prior 2026-10-02 result reached the menu then crashed. In the 2026-10-29 retest, the window remained black and the process exited before a menu was visible; the existing run log did not refresh. No gameplay verified. |
| Martha Is Dead | PPSA02006 | Logo | Reached a black title screen with “Use X to start” prompt at roughly 7–8 FPS. Mapped Cross and Circle inputs did not dismiss the prompt; no menu/gameplay verified. |
| The Medium | PPSA03717 | MainMenu | Improved from prior no-boot result: reached the title prompt and main menu; New Game, Settings, and Credits were readable against an otherwise black background. Selecting New Game stopped rendering at 0 FPS while the window still responded. No gameplay verified. Existing run log is stale. |
| MXGP 2020 - The Official Motocross Videogame | PPSA01646 | Logo | Prior result reached menu with graphical items missing and eventually crashed. This retest showed the legal/title splash, then the process exited before a menu; existing log is stale and does not establish this run's cause. |
| Observer: System Redux | PPSA02118 | DoesntBoot | Direct launch rendered a black screen at roughly 0 FPS and exited before any logo/menu was visible. Existing log is stale (2026-10-21), so no current-run error cause is established. |
| Planet Coaster (EU) | PPSA01735 | DoesntBoot | Launch displayed a PlayStation error dialog over a black screen (error code `0x80553407`) before any game menu. No gameplay verified. |
| Planet Coaster (US) | PPSA01736 | DoesntBoot | Reached screen setup but was blocked by a PlayStation error dialog (code `0x80553407`). Confirm input did not dismiss it. No main menu/gameplay verified; previous report of main-menu status was not reproduced. |
| Port Royale 4 | PPSA02815 | MainMenu | Reached a correctly rendered main menu with Campaign and Free Game options. This run did not successfully navigate/confirm a game mode; the “Load saved game” screen confirmed there are no saves. Prior 2026-10-02 database entry reports in-game/playable, but gameplay/control was not reproduced in this retest. |
| The Matrix Awakens Demo | PPSA05754 | DoesntBoot | 2026-10-29 retest showed a black screen with loading dots, then the process exited before title/menu. Existing log is stale (2026-10-21) and reports unsupported `VK_EXT_mesh_shader`; that historical error is not confirmed as the cause of this run. |
| Sackboy: A Big Adventure | PPSA01288 | DoesntBoot | 2026-10-29 retest remained black at 0 FPS, then the emulator process exited on its own about one minute later; I did not kill it. No menu/gameplay verified. The available log is stale (2026-10-21); its runtimeLinker access violation belongs to that older run and does not establish this exit's cause. |
| Sifu | PPSA03001 | DoesntBoot | Window rendered a nearly black scene, then stopped advancing at frame 5437 / 0 FPS. Mapped Cross input produced no visible change. No menu/gameplay verified; the stalled process was closed manually. |
| STAR WARS Jedi: Fallen Order | PPSA02198 | Logo | Reached the Respawn logo, then stopped advancing at 0 FPS before a menu. Mapped Cross input produced no visible change; the process exited afterward. Existing log is stale (2026-10-21), so the current exit cause is unknown. |
| Terminator: Resistance Enhanced | PPSA02474 | DoesntBoot | Prior result showed logo briefly then crashed. This retest displayed a black screen with a save icon at 0 FPS; the process exited before menu. Existing log is stale (2026-10-21), so it does not establish this run's cause. |
| The Riftbreaker | — | DoesntBoot | Direct launch process exited within 45 seconds before a game window/title was captured. No fresh log or title ID was available in the game folder; cause and menu/gameplay are unverified. |
| Tony Hawk's Pro Skater 1 + 2 | PPSA02176 | Logo | Opening credits/video rendered at 60 FPS, then the image went black and stopped advancing at frame 12212 / 0 FPS for at least 60 seconds. Mapped Cross had no effect; I closed the stalled process manually. Prior 2026-10-05 report reached menu, but no current menu/gameplay verified. |
| Train Sim World 2: Rush Hour | PPSA03944 | Logo | Rendered the opening train sequence, then reached the "Press Any Button to Advance" title prompt. The mapped Cross and Enter presses did not advance it during this test. No main menu or gameplay verified. Prior 2026-10-05 report claimed menu then crash loading into game; existing log is stale (2026-10-05). |
| Tropico 6 | PPSA05682 | Logo | Reached the Tropico 6 "Press X to Start" title prompt. After mapped Cross, the image turned black and remained black for 60 seconds while frames continued advancing. No main menu or gameplay verified; process closed manually. Existing log is stale (2026-10-21). |
| Uncharted: Legacy of Thieves Collection | PPSA05389 | DoesntBoot | 2026-10-29 retest rendered a black screen at 0 FPS; the process exited before any title/menu was visible. Existing log is stale (2026-10-21), so current exit cause is unknown. |
| Werewolf: The Apocalypse - Earthblood | PPSA02113 | MainMenu | Reached the main menu and save-slot screen. Chose an empty New Game slot (leaving the existing save untouched), reached the Tarker's Mill loading screen and narrative cutscene, then the image turned black and stopped advancing at 0 FPS. No gameplay verified; stalled process closed manually. Existing log is stale (2026-10-05). |
| WWE 2K22 | PPSA02460 | MainMenu | Reached the main menu after the Press Any Button prompt and a server-data warning. Menu labels and navigation are visible, but the entire background/artwork is black. Previous 2026-10-05 entry said InGame; that gameplay result was not reproduced, so this retest supports MainMenu only. Rendering regression/limitation observed; cause not isolated. Process closed manually. |
| The Ascent | PPSA02593 | MainMenu | Improved from prior audio/no-video result: reached the title prompt, main menu, Single Player > New Game, and profile selection. Main scene is severely red-corrupted. Selecting the first blank profile caused the process to exit before gameplay; available log is stale (2026-10-21), so cause is unknown. |
| Red Dead Redemption - PS5 | PPSA99078 | Unknown | Not launchable from the current folder: it contains a 9.7 GB `.pkg` and metadata, but no `eboot.bin` or unpacked game directory. Package was not installed/extracted. |
| Resident Evil Village | PPSA01556 | Unknown | Not launchable from the current folder: only `.pkg` packages are present; no `eboot.bin` or unpacked game directory. Package was not installed/extracted. |
| RIDE 4 | PPSA01599 | Logo | Prior result reached menu then crashed during menu loading. This retest displayed the copyright/legal screen, then the process exited before a menu; current log freshness was not established. |
| Teenage Mutant Ninja Turtles: The Cowabunga Collection | PPSA04489 | InGame | Reached title screen, main menu/game selection, selected the arcade TMNT game, and reached a gameplay screen with 1UP HUD, character, and active scene. D was pressed, but no controlled before/after comparison established movement response. The arcade viewport rendered clearly at 60 FPS; some game-selection screens were corrupted. |

<table align="center">
  <tr>
    <td align="center">
      <strong>Cyberpunk 2077</strong><br>
      <img src="docs/screenshots/ps5-07.png" width="300" alt="Cyberpunk 2077 running in KytyPS5">
    </td>
    <td align="center">
      <strong>MLB The Show 23</strong><br>
      <img src="docs/screenshots/ps5-08.png" width="300" alt="MLB The Show 23 running in KytyPS5">
    </td>
  </tr>
  <tr>
    <td align="center">
      <strong>Astro Bot</strong><br>
      <img src="docs/screenshots/ps5-01.png" width="300" alt="Astro Bot running in KytyPS5">
    </td>
    <td align="center">
      <strong>Saros</strong><br>
      <img src="docs/screenshots/ps5-03.png" width="300" alt="Saros running in KytyPS5">
    </td>
  </tr>
  <tr>
    <td align="center">
      <strong>Neptunia ReVerse</strong><br>
      <img src="docs/screenshots/ps5-04.png" width="300" alt="Neptunia ReVerse running in KytyPS5">
    </td>
    <td align="center">
      <strong>SILENT HILL: The Short Message</strong><br>
      <img src="docs/screenshots/ps5-05.png" width="300" alt="SILENT HILL: The Short Message running in KytyPS5">
    </td>
  </tr>
  <tr>
    <td align="center">
      <strong>Demon's Souls</strong><br>
      <img src="docs/screenshots/ps5-02.png" width="300" alt="Demon's Souls running in KytyPS5">
    </td>
    <td align="center">
      <strong>UFC 6</strong><br>
      <img src="docs/screenshots/ps5-06.jpg" width="300" alt="UFC 6 running in KytyPS5">
    </td>
  </tr>
</table>


- Windows 10 version 1803, a current Linux distribution, or macOS on Apple
  Silicon
- A 64-bit x86 processor (on macOS, Apple Silicon with Rosetta 2)
- A Vulkan 1.3-capable GPU with current drivers (on macOS, Vulkan is provided
  by the bundled MoltenVK)

## Building

### Windows

Requirements: Git, CMake 3.22.1+, Ninja, Visual Studio 2022 or Build Tools
2022 with the **Desktop development with C++** workload and **C++ Clang tools
for Windows**, Qt 6 for MSVC 2022 64-bit (Concurrent, Network, Widgets), and
`glslangValidator` on `PATH`. The MSVC compiler (`cl.exe`) is not supported;
use `clang-cl`.


### Build requirements (Windows)

- Git
- CMake 3.22.1 or newer
- Ninja
- Visual Studio 2022 or Build Tools 2022 with the **Desktop development with C++** workload and
  **C++ Clang tools for Windows** component
- Qt 6 for MSVC 2022 64-bit, including Concurrent, Network, and Widgets
- [glslang](https://github.com/KhronosGroup/glslang/releases) (`glslang` or `glslangValidator`) on `PATH`
- Python 3 on `PATH`; the bundled SPIRV-Tools runs it at configure time

The Microsoft C++ compiler (`cl.exe`) is not supported; use `clang-cl`.

Open an **x64 Native Tools Command Prompt for Visual Studio 2022** (or the equivalent Developer
PowerShell), change to the repository root, and initialize the dependencies:


```powershell
git submodule update --init --recursive
cmake -S . -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64"
cmake --build _Build/windows --target launcher
cmake --install _Build/windows --prefix _Build/windows/install
```

The runnable application lands in `_Build/windows/install`.

### Linux


Install the toolchain and the libraries the bundled SDL3 needs. Without the audio, Wayland and
udev development packages SDL3 quietly configures itself without those backends, and the resulting
build has no working sound and no gamepad hotplug:

```bash
sudo apt-get install --no-install-recommends \
  clang lld ninja-build cmake git glslang-tools python3 pkg-config \
  libgl1-mesa-dev libx11-dev libxcursor-dev libxext-dev libxfixes-dev \
  libxi-dev libxrandr-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libwayland-dev wayland-protocols
```

Qt 6 (Concurrent, Network, Widgets) is required for the launcher — either the distribution packages
(`qt6-base-dev`) or an official Qt installation.


```bash
git submodule update --init --recursive
cmake -S . -B _Build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH="$Qt6_DIR"
cmake --build _Build/linux --target launcher --parallel
cmake --install _Build/linux --prefix _Build/linux/install
```

For `kyty_emulator`/`kyty_tests` without Qt, configure a separate directory
with `-DKYTY_BUILD_LAUNCHER=OFF`.

### macOS

Experimental: x86-64 builds run under Rosetta 2 on Apple Silicon (Xcode CLT,
`cmake`, `ninja`, `glslang`, universal Qt 6). Configure with
`-DCMAKE_OSX_ARCHITECTURES=x86_64`, build and install as on Linux; see
upstream docs for the MoltenVK `libMoltenVK.dylib` signing steps.


cmake -S . -B _Build/linux-no-qt -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DKYTY_BUILD_LAUNCHER=OFF

cmake --build _Build/linux-no-qt --target kyty_emulator kyty_tests --parallel
```

As on Windows, the MSVC compiler is not used; Clang is required. `cl.exe` is rejected at configure
time.

The CMake source root is the repository root.

### Building on NixOS

A development shell provides Clang, CMake, Ninja, Qt 6, the Vulkan headers, and the SDL3 backend
libraries. Enter it and configure exactly as on other Linux distributions; the shell exports
`CMAKE_PREFIX_PATH` and `QT_PLUGIN_PATH`, so the `-DCMAKE_PREFIX_PATH="$Qt6_DIR"` argument is not
needed:

```bash
nix-shell # or: nix develop
git submodule update --init --recursive

cmake -S . -B _Build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++

cmake --build _Build/linux --target launcher --parallel
cmake --install _Build/linux --prefix _Build/linux/install
```

The configure step downloads the FFmpeg prebuilts and the `xbyak`, `zydis`, `zstd`, and ZArchive
sources, so it needs network access; a fully sandboxed `nix build` would require vendoring those
inputs. A Vulkan 1.3 driver must be available at runtime (on NixOS,
`hardware.graphics.enable = true`).

### Building on Fedora and Fedora Atomic (Bazzite, Bluefin, Silverblue)

On regular Fedora, install dependencies on the host. On Fedora Atomic variants such as Bazzite,
Bluefin and Silverblue, use [Distrobox](https://distrobox.it/) and run the install and build steps
inside the container. Keep the checkout in your home directory so it is shared with the container.
Add `--nvidia` to the create command for hosts using the proprietary NVIDIA driver:

```bash
distrobox create --name kyty-build --image registry.fedoraproject.org/fedora:latest
distrobox enter kyty-build
```

Install the toolchain, Qt 6 and SDL3 dependencies:

```bash
sudo dnf install -y \
  clang lld ninja-build cmake git glslang python3 pkgconf-pkg-config \
  mesa-libGL-devel libX11-devel libXcursor-devel libXext-devel libXfixes-devel \
  libXi-devel libXrandr-devel libXScrnSaver-devel libXtst-devel libxkbcommon-devel \
  alsa-lib-devel pulseaudio-libs-devel systemd-devel dbus-devel \
  wayland-devel wayland-protocols-devel qt6-qtbase-devel
```

From the repository root, follow the launcher configure, build and install steps in
[Building on Linux](#building-on-linux), omitting `-DCMAKE_PREFIX_PATH="$Qt6_DIR"`; CMake finds
Fedora's packaged Qt 6 automatically.

Run `./_Build/linux/install/kyty_launcher` on regular Fedora or inside the container. To launch a
container build from the host, leave the container with `exit`, then run from the repository root:

```bash
distrobox enter kyty-build -- ./_Build/linux/install/kyty_launcher
```

Qt libraries and plugins are bundled. You can also launch directly on a Fedora Atomic host
if its other system libraries are compatible with the container build.

### Building on macOS

macOS builds target x86-64 and run under Rosetta 2 on Apple Silicon, so the PS5's x86-64 game
code executes through the same translation layer as the emulator itself. Prebuilt archives are
attached to releases; the steps below are for building from source.

Requirements:

- An Apple Silicon Mac with Rosetta 2 installed (`softwareupdate --install-rosetta`)
- Xcode (or the Command Line Tools)
- Homebrew packages: `brew install cmake ninja glslang python`
- Qt 6 (Concurrent, Network, Widgets) with x86-64 support. The official Qt installation is
  universal and works; Homebrew's Qt is arm64-only and will not link

```bash
git submodule update --init --recursive

cmake -S . -B _Build/macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH="$Qt6_DIR"

cmake --build _Build/macos --target launcher --parallel
cmake --install _Build/macos --prefix _Build/macos/install
```

The build re-signs `kyty_emulator` with the JIT entitlements it needs to execute translated
guest code; no manual signing step is required. When the launcher is built, the install
also produces `_Build/macos/install/KytyPS5.app` — double-click to launch the GUI.
A flat `kyty_emulator` is kept for CLI usage.

Vulkan comes from MoltenVK. Download `MoltenVK-macos.tar` from the
[MoltenVK releases](https://github.com/KhronosGroup/MoltenVK/releases), then copy
`MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib` next to the flat `kyty_emulator`
(and, for the bundle, into `KytyPS5.app/Contents/Frameworks/`) and ad-hoc sign it:

```bash
codesign --force --sign - _Build/macos/install/libMoltenVK.dylib
# For the bundle (if present):
codesign --force --sign - _Build/macos/install/KytyPS5.app/Contents/Frameworks/libMoltenVK.dylib
codesign --force --sign - _Build/macos/install/KytyPS5.app
```

Release archives already include a signed `libMoltenVK.dylib` (both flat and inside the bundle).

### Regression tests

Build every regression executable and run the registered tests with:


```powershell
cmake --build _Build/windows --target kyty_tests
ctest --test-dir _Build/windows --output-on-failure
```

Use `_Build/linux` instead of `_Build/windows` for a Linux build.

### Visual Studio Code

A ready-made Visual Studio Code setup is included in [`.vscode`](.vscode). It configures CMake
Tools to build the project with Ninja and `clang-cl` and provides launch profiles for both
`kyty_launcher.exe` and `kyty_emulator.exe`. It is Windows-only: VS Code settings cannot select a
compiler per platform, so on Linux configure from the command line as shown above.

Before using it:

1. Install the **CMake Tools** and **C/C++** extensions in Visual Studio Code.
2. Update `CMAKE_PREFIX_PATH` in [`.vscode/settings.json`](.vscode/settings.json) to point to your
   Qt 6 MSVC installation.
3. Update the `--game` path in [`.vscode/launch.json`](.vscode/launch.json) for the
   **Debug kyty_emulator** profile.
4. Open the repository in an x64 Visual Studio developer environment, configure the CMake project,
   and select a launch profile from **Run and Debug**.

## Running

Update the graphics driver before reporting rendering problems. Launch the GUI:

```powershell
.\_Build\windows\install\kyty_launcher.exe
```

```bash
./_Build/linux/install/kyty_launcher
```

On first launch, add game folders in the global settings. The launcher searches
them recursively for game directories containing `eboot.bin` and read-only
ZArchive (`.zar`) dumps, which are streamed without extraction. Or start the
emulator directly:

```powershell
.\_Build\windows\install\kyty_emulator.exe --game "D:\Games\ExampleGame"
```

```bash
./_Build/linux/install/kyty_emulator --game "/games/ExampleGame"
```

Run `kyty_emulator --help` for graphics, logging, validation, profiling, and
debugging options, including the GPU-assisted validation option added in this
fork.

## Contributing

Testing games and submitting detailed bug reports are useful contributions.
Search existing issues first, then use the **Game Emulation Status Report**
template and attach the complete log file. Include the short-hash build label
from the window title or log when reporting against this fork.

Code contributions should be focused, build on the platforms they touch, and
include tests where practical. Set up the clang-format pre-commit hook after
cloning: `python -m pip install pre-commit`, then
`python -m pre_commit install --install-hooks`.

## License

KytyPS5 is licensed under the [GNU General Public License version
2](LICENSE) (`GPL-2.0-only`). It is based on [Kyty](https://github.com/InoriRus/Kyty)
(MIT); the original notice is preserved in [`LICENSES/Kyty-MIT.txt`](LICENSES/Kyty-MIT.txt).
Third-party components keep their own licenses.

## Special Thanks

- [InoriRus/Kyty](https://github.com/InoriRus/Kyty) — the project KytyPS5 is based on.
- [shadps4-emu/shadPS4](https://github.com/shadps4-emu/shadPS4) — reference for PS4
  memory behavior, GPU resource aliasing/cache coherency, and AVPlayer.
