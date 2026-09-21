emulationstation-sdl3
=====================

The PortareOS front-end. A fork of [ROCKNIX/emulationstation-next](https://github.com/ROCKNIX/emulationstation-next),
itself descended from batocera-emulationstation, for one device: the Retroid
Pocket Nova.

The name says what the fork is for: it runs on **SDL3**, and it talks to
**PipeWire** directly. Neither is a version bump.

SDL3 inverted the return convention of most of the API — `SDL_Init` and its
neighbours return true on success where SDL2 returned zero — so every call site
had to be read rather than recompiled. A missed one is not a compile error. It
is a black screen on a device that still answers SSH, which is how the first
port shipped.

PipeWire replaced `VolumeControl`, which carried ALSA and PulseAudio side by
side inside `__APPLE__` and `WIN32` branches for platforms this will never run
on. It is now one native libpipewire implementation that binds the default sink
through the registry and sets `channelVolumes` directly.

Video playback moved from libVLC to libmpv's software render API along the way,
and the gettext translation layer is gone.

That is the whole scope. There is no Windows build, no Raspberry Pi support and
no other architecture. Anything here that still mentions them is a leftover
being removed, not a platform we support.

Building
========

The distribution builds this. See
[`projects/PortareOS/packages/ui/emulationstation`](https://github.com/portare-ch/distribution)
for the recipe and the flags it passes, which are the ones that matter.

CI builds it standalone on aarch64 for every pull request, which is the same
thing the recipe does minus the cross-compile. To do it by hand on an arm64
Linux machine:

```bash
sudo apt-get install -y build-essential cmake pkg-config \
  libsdl3-dev libsdl3-mixer-dev libmpv-dev libpipewire-0.3-dev \
  libfreeimage-dev libfreetype-dev libcurl4-openssl-dev rapidjson-dev \
  libgl1-mesa-dev libgles-dev libpugixml-dev libudev-dev libboost-all-dev

cmake -S . -B build \
  -DPORTAREOS=1 -DDISABLE_KODI=1 -DENABLE_FILEMANAGER=0 \
  -DCEC=0 -DUSE_SYSTEM_PUGIXML=1 -DGLES3=1

cmake --build build -j"$(nproc)"
```

`libsdl3-dev` and `libsdl3-mixer-dev` are only in recent distributions. On
anything older, build SDL3 and SDL3_mixer from source first — `CMakeLists.txt`
asks for them through their own CMake config packages
(`find_package(SDL3 CONFIG REQUIRED)`), so a stray SDL2 on the system will not
be picked up by mistake.

The binary lands at the repository root, not under `build/`: `CMakeLists.txt`
forces `EXECUTABLE_OUTPUT_PATH` to the source directory on non-Windows.

Keep those flags in step with the distribution's `package.mk`. If they drift,
CI stops compiling what actually ships.

Documentation
=============

- [GAMELISTS.md](GAMELISTS.md), the gamelist.xml format
- [THEMES.md](THEMES.md) and [THEMES_BINDINGS.md](THEMES_BINDINGS.md), theming
- [SYSTEMS.md](SYSTEMS.md), system definitions
- [DEVNOTES.md](DEVNOTES.md), developer notes
- [CREDITS.md](CREDITS.md)

Issues
======

Here, for the front-end. Everything else belongs on
[portare-ch/distribution](https://github.com/portare-ch/distribution/issues).

Please do not take PortareOS problems to the ROCKNIX or Batocera maintainers.
They are not theirs to fix.
