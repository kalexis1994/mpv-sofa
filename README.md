# mpv-sofa

HRTF binaural spatializer for headphone listening. Renders multichannel audio (stereo up to 7.1.4 + objects) as 3D binaural stereo using SOFA HRIR measurements.

Built on a patched [mpv](https://mpv.io/) / [FFmpeg](https://ffmpeg.org/) stack with a custom OpenGL GUI for real-time control.

## Features

- **HRTF spatialization** via SOFA files (libmysofa) with per-ear profile selection
- **Lossless spatial audio** decoding (16-channel extraction with bed + height + objects)
- **Lossy spatial audio** object reconstruction (QMF-domain JOC decoding from 5.1 downmix)
- **Speaker layouts**: 7.1.4, 7.1, 5.1, stereo
- **Room presets**: Studio, Home Theater, Cinema, Concert Hall
- **3D visualizer**: Interactive OpenGL speaker/object visualization with click-to-select
- **Video playback**: Integrated libmpv rendering with fullscreen mode
- **Transport controls**: Play/pause, seek, volume, bed/object mute toggles
- **Spatial object sidecar** (.aobj): Frame-accurate 3D object positions synced to playback PTS

## Architecture

```
src/
  app/          Application loop, GLFW window
  audio/        libmpv wrapper (MpvPlayer)
  core/         Shared state (C struct for cross-DLL IPC)
  renderer/     OpenGL renderer, camera, shaders, speaker primitives
  ui/           ImGui panels (ControlPanel, TransportBar)

mpv-patches/    Patches applied to mpv source
  af_hrtf.c        Audio filter: SOFA convolution + object reconstruction
  ad_losslesshd.c  Decoder wrapper for lossless spatial streams
  spatial_ext_coeff.h  Shared struct (coefficients, object metadata)
  objcoding_qmf.*  QMF filterbank for lossy object reconstruction

assets/
  hrtf/         SOFA profiles (.sofa files)
```

## Building

See [BUILD.md](BUILD.md) for full instructions.

### Quick summary

1. Install MSYS2 with UCRT64 toolchain + dependencies
2. Patch and build FFmpeg (with spatial decoding extensions)
3. Patch and build mpv (with `af_hrtf` filter + `libmysofa`)
4. Build the GUI app with CMake + Visual Studio or Ninja

```bash
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DMPV_INCLUDE_DIR=mpv-src/include \
    -DMPV_LIBRARY=mpv-src/build/libmpv.dll.a

cmake --build build
```

### CLI-only usage (no GUI)

```bash
mpv --af=hrtf=sofa=assets/hrtf/default.sofa movie.mkv
```

## Dependencies

| Component | Source |
|-----------|--------|
| GLFW | submodule (`external/glfw`) |
| Dear ImGui | submodule, docking branch (`external/imgui`) |
| GLM | submodule (`external/glm`) |
| GLAD | vendored (`external/glad`) |
| PFFFT | vendored (`external/pffft`) |
| libmpv | built from patched mpv-src |
| libmysofa | system (MSYS2 package) |
| FFmpeg | built from patched ffmpeg-src |

## License

This project contains patches for mpv (GPL-2.0+) and FFmpeg (LGPL-2.1+). The host application and custom audio filters follow the same license terms as their respective upstream projects.
