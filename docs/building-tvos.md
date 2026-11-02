# Building WiiCompiled for Apple TV (tvOS)

This port builds the **base game** (`WiiCompiled`, Mario Kart Wii) as a native Apple TV app.
The primary target is the **Apple TV HD** (A8, 2 GiB RAM). The same build also runs on every
Apple TV 4K, and you can build a faster 4K-only variant with `--cpu` (see below).

As on every other platform, there is no Nintendo code or data in this repository. The build runs
on your Mac against your own disc image and places your extracted game files inside the app. **The
resulting `.app` contains your copy of the game: never share or upload it.**

> [!WARNING]
> **Status: untested on hardware.** The port was written and statically checked without a Mac or an
> Apple TV. The tvOS-specific C++ was type-checked with a Clang front end against stand-in headers,
> the Dawn patch was applied to the pinned Dawn source, and every CMake file was parse-checked.
> It has **not** yet been compiled with Xcode's tvOS SDK or run on an Apple TV. Expect the first
> build to surface issues, and report them with the full build log.
>
> **Performance on the Apple TV HD is unknown and may be below full speed.** Its dual-core 1.5 GHz
> A8 is far below the desktop minimum in the README, and tvOS forces every guest memory access through
> the checked path (see [How the port works](#how-the-port-works)). Apple TV 4K models have
> much more headroom.

## What you need

| Requirement | Notes |
| --- | --- |
| A Mac with **Xcode** (not just the Command Line Tools) | Install the **tvOS** platform: Xcode > Settings > Components. Xcode 15 or later; Xcode 16+ recommended. |
| CMake ≥ 3.25, Ninja, .NET 8 SDK | `brew install cmake ninja` and `brew install --cask dotnet-sdk@8` |
| `python3` and `git` | Both ship with Xcode. The first configure downloads Dawn, SDL3 and Dawn's dependencies, so it needs network access. |
| [nodtool](https://github.com/encounter/nod/releases) | Extracts your disc image, exactly as in [the macOS guide](building-macos.md). |
| A clean **PAL `RMCP01`, NTSC-U `RMCE01` or NTSC-J `RMCJ01`** disc image you dumped yourself | Same hash-pinned releases as every other platform. |
| An Apple TV on **tvOS 15 or later**, paired with Xcode | Apple TV: Settings > Remotes and Devices > Remote App and Devices. Mac: Xcode > Window > Devices and Simulators > select the Apple TV > Pair. |
| An Apple ID in Xcode | A free Personal Team works, but its provisioning profiles expire after 7 days (re-sign and reinstall). A paid account lasts a year. |
| A **game controller** | MFi, Xbox Wireless or PlayStation (DualShock 4 / DualSense) controller, paired in the Apple TV's Bluetooth settings. The Siri Remote alone cannot play the game. |

About 20 GB of free disk space is needed during the build. The finished app is about as large as your
extracted `Assets/DATA` folder plus the game executable; the build warns if it exceeds tvOS's 4 GB
app size limit.

## 1. Get the port onto the Mac

The port is not part of the upstream repository. It lives in this working tree, and it is also
exported as two patches in `tvos-port-patches/`. Use either route:

**A. Copy this whole folder to the Mac.** Include the hidden `.git` folder and `aurora-main/`
(a git submodule that carries part of the port). Copy the files byte for byte: the shell scripts
need their LF line endings and break with Windows (CRLF) line endings.

**B. Apply the patches to a fresh clone** of the exact upstream revision they were made against:

```bash
git clone https://github.com/patchzyy/Wiicompiled.git
cd Wiicompiled
git checkout 0d5c402d0366640a294bf9c699068299591f7eff
git submodule update --init   # aurora-main at 33067e34a17ca680eb961d9e0f3d48ddb22a7d7f
git apply /path/to/tvos-port-patches/wiicompiled-tvos.patch
git -C aurora-main apply /path/to/tvos-port-patches/aurora-tvos.patch
```

## 2. Create a provisioning profile (device builds only)

The app is signed with a development profile that covers its bundle identifier and your Apple TV.
The simplest way to get one is to let Xcode create it once:

1. In Xcode: File > New > Project > **tvOS** > **App**. Any name works.
2. Set **Team** to your Apple ID's team and **Bundle Identifier** to the identifier you will build
   with, for example `com.yourname.wiicompiled`.
3. Pick your Apple TV as the run destination and press Run once. Xcode registers the device and
   writes the profile to `~/Library/Developer/Xcode/UserData/Provisioning Profiles`.

The build script finds that profile automatically. It also picks the certificate in your keychain
that the profile names, so you normally pass only `--bundle-id`.

## 3. Build, sign and install

Find your Apple TV's identifier:

```bash
xcrun devicectl list devices
```

Then build and install (run the script through `bash`; the executable bit may not survive a copy):

```bash
bash Launcher/tvos/build-tvos.command \
  --game /path/to/RMCP01.iso \
  --nodtool ./nodtool \
  --bundle-id com.yourname.wiicompiled \
  --output-dir build-tvos/Products \
  --install --device "<your Apple TV identifier>" --launch
```

The first run translates the game and builds Dawn from source, so it takes a while. Later runs reuse
the translation and the native build. `--game`/`--nodtool` can be dropped once `Assets/` holds a
verified extraction.

Useful options (`--help` lists all of them):

| Option | Effect |
| --- | --- |
| `--cpu apple-a10` / `apple-a12` / `apple-a15` | Tune for Apple TV 4K (1st / 2nd / 3rd generation). The result **does not run on an Apple TV HD**. The default, `apple-a8`, runs on every Apple TV. |
| `--simulator` | Build for the Apple TV Simulator on an Apple Silicon Mac. It is ad-hoc signed, and `--install --launch` targets the booted simulator. The simulator renders on the Mac's GPU, so it is no stand-in for A8 performance or GPU limits. |
| `--provisioning-profile PATH`, `--sign-identity ID` | Choose the profile or certificate yourself. |
| `--no-sign` | Leave the app unsigned, for example to sign it with your own tooling. |
| `--deployment-target 17.0` | Raise the minimum tvOS version (default 15.0). |

`xcrun devicectl` needs Xcode 15+ and an Apple TV on tvOS 17 or later. On older tvOS versions, install
`build-tvos/Products/WiiCompiled.app` from Xcode's Devices and Simulators window (the **+** under
Installed Apps).

## Building on GitHub instead (no Mac)

`.github/workflows/tvos.yml` runs the same build on a GitHub-hosted Mac and produces a
`WiiCompiled.ipa` for a sideloading tool such as [atvloadly](https://github.com/bitxeno/atvloadly),
which signs it with your Apple ID and installs it, so no Mac is needed at any point.

Your game data never enters the repository and never sits unencrypted on GitHub:

1. **Encrypt your extracted disc** (a `DATA` folder with `files/` and `sys/`) on your own computer.
   This creates a random key file the first time; keep it, since you need it to decrypt builds:
   ```bash
   bash Launcher/tvos/encrypt-game-data.sh /path/to/DATA ~/wiicompiled-tvos/upload ~/wiicompiled-tvos/key.txt
   ```
2. **Upload the encrypted parts** to a release tagged `game-data`, and store the key as the
   `MKW_DATA_KEY` repository secret:
   ```bash
   gh release create game-data ~/wiicompiled-tvos/upload/mkw-data.tar.enc.part-* --title "Encrypted game data" --notes "AES-256; key is the MKW_DATA_KEY secret"
   gh secret set MKW_DATA_KEY --body "$(tr -d '\r\n' < ~/wiicompiled-tvos/key.txt)"
   ```
3. **Run the workflow:** Actions > **tvOS build** > Run workflow (or `gh workflow run tvos.yml`).
   It decrypts the data on the build machine, builds an ad-hoc signed app, and publishes the `.ipa`,
   encrypted with the same key, as a release named `tvos-<run number>`.
4. **Download and decrypt the build:**
   ```bash
   bash Launcher/tvos/fetch-ipa.sh ~/wiicompiled-tvos/key.txt ~/wiicompiled-tvos
   ```
   Then install `~/wiicompiled-tvos/WiiCompiled.ipa` with atvloadly.

Things to know:
- On a **private** repository the GitHub Free plan includes roughly 200 macOS build minutes a month
  (macOS minutes count ten times). A full build may not fit; public repositories have no minute
  limit, and the encryption above is what keeps the game private there.
- GitHub's Macs are small (3 cores, 7 GB of RAM) and a job may run at most 6 hours, so the first
  build is slow and may need tuning.
- atvloadly's limits: a free Apple ID keeps at most 3 sideloaded apps active and needs 2FA for each
  login; you must re-pair after tvOS updates; it documents no maximum app size, and this app is about
  3 GB. Its Docker host needs `avahi-daemon`, and it has no login of its own, so keep it on a trusted
  network.

## Controls

- **Playing:** a controller is mapped to the GameCube controller, exactly as on desktop builds.
  Mapping, rumble and the other options live in the settings bar.
- **Settings bar** (the desktop's F10): press the controller's **View / Share / Create** button
  (the "Select" position; on MFi pads it is called Options) or **Play/Pause** on the Siri Remote.
  Move with the D-pad or the remote's arrows, confirm with **A** or **Select**, go back with **B**
  or **Menu**. The game's input is paused while the bar is open.
- **Siri Remote Menu** during play opens the exit prompt (the desktop's Escape). The **TV** button
  returns to the Home screen as usual.
- The Siri Remote never takes a player slot. Its arrows and Select reach the game through the
  keyboard bindings, which can be changed in the settings bar.
- **Wii Remotes are not supported:** tvOS only lets apps use controllers the system manages
  through the GameController framework.

## Where the app keeps its data

tvOS gives apps no persistent storage beyond a small key-value store, so everything the game writes
lives in the app container's `Library/Caches/WiiCompiled`:

| Path | Contents |
| --- | --- |
| `NAND/` | The emulated Wii NAND, including **your save data** |
| `Config.toml` | Settings (written by the settings bar; there is no way to edit it by hand on the device) |
| `Cache/` | Renderer pipeline cache |
| `Logs/` | Run logs and crash reports |

The data survives app updates. tvOS may **purge** it when the Apple TV runs very low on storage, and
deleting the app removes it. If Xcode's Devices and Simulators window offers **Download Container**
for the app, use it to back up your saves and to collect logs for bug reports.

The game files themselves are read from `WiiCompiled.app/DATA`. Setting `paths.dvd_root` in
`Config.toml` still overrides that location.

## Not supported on tvOS

- **Retro Rewind.** The Retro Rewind product builds, but its pack expects a writable folder (an app
  bundle is read-only) and the port has not been designed or tested for it.
- **Wii Remotes**, **Discord presence**, **Steam Input** and **MetalFX upscaling** (no Apple TV GPU
  older than Apple7 supports MetalFX).
- **Building on Windows or Linux.** Apple's tvOS SDK and code signing only exist on macOS.

## How the port works

The translator, the translated game code and almost all of the runtime are identical to the desktop
builds. The tvOS-specific pieces:

- **Guest memory.** Desktop builds reserve a fixed 4 GiB virtual range so that every guest address
  maps directly to a host address. A tvOS process cannot get that range: arm64 Darwin maps a 4 GiB
  `__PAGEZERO`, and a 2 GiB Apple TV HD leaves only a few GiB of user address space in total. On tvOS
  `GuestFlat::RequiresCheckedAccess()` is therefore a compile-time `true`. Every guest load and store,
  including the translator's `FlatWriteRam*` stores, goes through the existing checked page-table
  path (`Memory::*`), backed by per-region stores with the same MEM1/MEM2 mirroring as macOS
  (`runtime/src/platform/tvos/guest_flat_memory_tvos.cpp`). This needs no entitlement and costs
  some speed.
- **Start-up and stack.** UIKit has to own the process, so `main()` hands control to SDL
  (`SDL_RunApp`). The guest's main thread runs on the host main thread's stack, which tvOS limits to
  1 MiB (macOS and Linux give it 8 MiB). The runtime is therefore started on an 8 MiB stack, still on
  the main thread, using the same AArch64 context switch the guest fibers use
  (`runtime/src/platform/tvos/tvos_app.cpp`).
- **CPU target.** Desktop AArch64 builds use `-mcpu=native`, which on a Mac would describe the Mac's
  CPU. tvOS builds use `-mcpu=apple-a8` (`MKW_TVOS_CPU`).
- **Graphics.** aurora's Metal path runs unchanged. Dawn and SDL3 are built from aurora's pinned
  sources. A small patch (`aurora-main/cmake/patches/apply-dawn-tvos-gpu-family.cmake`) makes Dawn
  query the GPU family on tvOS: the A8 is Metal family Apple2 and lacks base-vertex/base-instance
  draws and sampler compare functions, which Dawn otherwise assumes every Apple TV has. Release
  builds skip Dawn's validation, so a second patch
  (`aurora-main/cmake/patches/apply-imgui-wgpu-no-base-vertex.cmake`) stops ImGui's WebGPU renderer
  from drawing with a base vertex: it binds each overlay window's vertices at an offset instead. Without
  it, Metal raises `APINotSupported` on the A8 as soon as a second overlay window appears.
- **Audio.** Music ducking uses `AVAudioSession.secondaryAudioShouldBeSilencedHint`
  (`external_audio_tvos.mm`) instead of macOS Core Audio process objects.
- **Paths.** User data lives in `Library/Caches` (`runtime/src/platform/host_platform.cpp`), and the
  DVD root defaults to the `DATA` folder inside the app bundle (`runtime/include/runtime_config.h`).

## Measuring performance

Two switches, both read from the launch environment, measure the game on the device without
Instruments. With [pymobiledevice3](https://github.com/doronz88/pymobiledevice3) and a running
`pymobiledevice3 remote tunneld`:

```sh
pymobiledevice3 developer dvt launch "<bundle id> --race-cpu" \
    --env MKW_PERF_STATS=1 --env MKW_SAMPLE_PROFILE=45:60:1000
```

- `--race-cpu` boots straight into a 12-racer 150cc VS race on Luigi Circuit with the CPU driving
  your kart, so every run measures the same scene with nobody holding a controller
  (`--race=<course id>` picks another course and lets you drive).
- `MKW_PERF_STATS=1` writes the game thread's busy milliseconds per frame (average, median, 95th
  percentile, worst) to the run's `console.log` every 240 frames.
- `MKW_SAMPLE_PROFILE=<delay>:<duration>:<Hz>` starts the built-in sampling profiler
  (`runtime/src/platform/tvos/sample_profiler_tvos.cpp`) after `<delay>` seconds. It writes
  `Logs/profile-<pid>.bin` (program counter and frame-pointer call chain of every thread that used
  CPU time, sampled `<Hz>` times a second for `<duration>` seconds) and `Logs/profile-<pid>.txt`
  (each thread's CPU share and priority, and the loaded images).
  `python3 Launcher/tvos/analyze-profile.py Logs/profile-<pid> WiiCompiled.app/WiiCompiled` turns
  them into per-thread self and inclusive time by function.

## Troubleshooting

- **"the appletvos SDK was not found"**: install the tvOS platform in Xcode and select Xcode with
  `sudo xcode-select -s /Applications/Xcode.app`.
- **"no unexpired tvOS provisioning profile covers …"**: redo step 2 with the same bundle identifier,
  or pass `--provisioning-profile`.
- **The app starts and closes immediately**: download the app container (see above) and read the newest
  file in `Library/Caches/WiiCompiled/Logs`, or watch the device console in Console.app while
  launching.
- **A configure error mentioning `apply-dawn-tvos-gpu-family.cmake`**: the pinned Dawn revision
  changed. Update the anchors in that patch to the new source.
