EmulationStation
================

The PortareOS front-end. A fork of [ROCKNIX/emulationstation-next](https://github.com/ROCKNIX/emulationstation-next),
itself descended from batocera-emulationstation, for one device: the Retroid
Pocket Nova.

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
sudo apt-get install -y build-essential cmake pkg-config gettext \
  libsdl2-dev libsdl2-mixer-dev libfreeimage-dev libfreetype-dev \
  libcurl4-openssl-dev rapidjson-dev libasound2-dev libpulse-dev \
  libgl1-mesa-dev libgles-dev libvlc-dev libvlccore-dev \
  libpugixml-dev libudev-dev

cmake -S . -B build \
  -DPORTAREOS=1 -DDISABLE_KODI=1 -DENABLE_FILEMANAGER=0 \
  -DCEC=0 -DENABLE_PULSE=1 -DUSE_SYSTEM_PUGIXML=1 -DGLES3=1

cmake --build build -j"$(nproc)"
```

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
