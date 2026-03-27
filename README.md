# MoonlightRecorder

MoonlightRecorder is an experimental recorder-focused fork of [moonlight-stream/moonlight-qt](https://github.com/moonlight-stream/moonlight-qt).

Instead of decoding and playing the stream locally, this fork negotiates stream capabilities with the host, receives the reordered bitstream from Moonlight's networking pipeline, and records that stream to a local container file.

## Status

This repository is currently an `alpha` / `experimental` build.

Phase 1 is focused on:

- GUI-based recording workflow
- Source-exact bitstream capture after Moonlight reordering
- Local recording to selectable containers
- AV1 / HEVC / H.264 negotiation using the client-side recording profile
- Audio capture alongside video recording

Planned later work includes live restream / push-stream output.

## AI Disclosure

This fork is a vibe-coded experiment.

To be explicit: the recorder-specific glue and integration code in this fork was produced through AI-assisted development rather than being hand-written line-by-line by a human. The upstream Moonlight codebase remains the original upstream project; this disclosure applies to the fork-specific recording work layered on top of it.

If you prefer to avoid AI-generated or AI-heavy projects, this repository is probably not a good fit for you, and this note is here so you do not waste your time.

## Upstream And License

This project is a fork of:

- [moonlight-stream/moonlight-qt](https://github.com/moonlight-stream/moonlight-qt)
- [moonlight-stream/moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c)

The repository keeps the upstream `GPL-3.0` licensing model. See [LICENSE](LICENSE).

Retaining upstream names, notices, and attributions inside the source tree is intentional. This project is not the official Moonlight release.

## What This Fork Changes

MoonlightRecorder changes the behavior of Moonlight Qt from "stream and play" into "stream and record".

In practical terms, the current fork:

- bypasses local decode-and-playback for the recorder workflow
- keeps Moonlight's packet recovery / reordering path
- writes reordered source bitstreams into a recording pipeline
- exposes recorder-centric settings in the GUI
- builds Windows installer and portable packages under the `MoonlightRecorder` product name

## Windows Build Notes

The current tested toolchain is:

- Qt `6.8.3` (`MSVC 2022 64-bit`)
- Visual Studio 2022 Build Tools
- Windows 10/11 SDK

Build scripts are configured so that build artifacts go to:

- `C:\Users\Administrator\Desktop\MoonlightRecorder-build`

instead of polluting the repository root with build outputs.

### Windows Build Requirements

- Qt 6.7 SDK or later
- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/)
- Select `MSVC` during Qt installation
- [7-Zip](https://www.7-zip.org/) only if you want to customize packaging beyond the current scripts
- Graphics Tools only if running debug builds

### Windows Build Steps

1. Open a Visual Studio Developer Command Prompt or otherwise ensure the MSVC toolchain is available.
2. Run:

```bat
qmake moonlight-qt.pro
jom release
```

3. For packaged Windows builds, from the repository root run:

```bat
scripts\build-arch.bat release
scripts\generate-bundle.bat release
```

The current scripts emit artifacts under the external desktop build root by default.

## Current Windows Artifacts

The packaging flow currently produces:

- `MoonlightRecorderSetup-<version>.exe`
- `MoonlightRecorder.msi`
- `MoonlightRecorderPortable-x64-<version>.zip`

## Credits

All credit for the original streaming client architecture belongs to the Moonlight upstream maintainers and contributors.

Additional thanks to:

- [Sunshine](https://github.com/LizardByte/Sunshine) for the host-side streaming stack used for testing
- the original Moonlight Qt and Moonlight Common C contributors

## Contributing

If you want to contribute, please treat this repository as an experimental fork rather than the official upstream project.

For official Moonlight development, issues, and upstream contributions, see:

- [moonlight-stream/moonlight-qt](https://github.com/moonlight-stream/moonlight-qt)
