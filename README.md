# Alfred

A free and open source cross-platform **audio synthesizer**, **sequencer** and **composition editor** program.

It comprises a software multi-timbral synthesizer using additive and wavetable methods, and an interface to compose and play music.

It is same instrument + pitch monophonic, otherwise it supports up to twelve voices simultaneously.

The synthesized sounds are meant to be *suggestions* and *hints*, rather than realistic emulations.

I built this program for myself, as a reason to try out the art of music composition. Of course, MuseScore already exists, but isn't it nice to make your own tooling?

Read the manual [here](MANUAL.md).

![Alfred](gfx/mascot/alfred.png)

Alfred is split into two main parts:

- Three synthesizer libraries worrying about synthesis and audio-related things almost exclusively
    - `synthesis` Core library handling the synthesis, math and generic synthesizer things
    - `audio` Audio device handling library also implementing a real time synthesizer
    - `instruments` Built-in instruments/presets library
- A sequencer and composition editor GUI application which enables playing notes and editing music, and which loosely models some sheet music concepts

Currently, Alfred is in its infancy, so its current instruments are not well-made, and it is still missing some important features, which will probably be addressed with time:

- Plugins which dynamically add compiled instruments/presets
- ~~A graphical way to create, store and load runtime instruments/presets~~
- Support for MIDI keyboard controllers
- ~~Presenting errors nicely instead of just silently logging them~~

Alfred works with 16-bit samples, one channel, and frequency of 44100 Hz.

![Screenshot](gfx/screenshots/1.png)

## Building, Installing and Packaging

Alfred was at least built and tested on:

- Fedora Linux 43, GNOME 49, Wayland, PipeWire, GCC 15.2, CMake 3.31
- Ubuntu 25.04, GNOME 48, Wayland (Xwayland), PipeWire, GCC 14.2, CMake 3.31
- Windows 11 25H2, WASAPI, MSVC 19.50, CMake 4.3

Additionally, it was at least tested on:

- Linux Mint 22.3, Cinnamon 6.6, X11, PipeWire

Alfred is solely 64-bit software.

Build in distribution/release mode for the best audio experience! Compiled without optimizations, due to low performance, the audio output can sometimes sound glitchy and bad!

*Distribution* mode means compiling without assertions, without additional debug code and generally for system integration. It differs from release mode, which merely means compiling with optimization flags enabled.

### Requirements

- Git
- CMake
- GCC or MSVC with C++23 support
- Python
- NSIS (Windows only)
- SDL dependency packages - video, audio etc. libraries (Linux only)

### Download

```txt
git clone https://github.com/SimonMaracine/alfred
git submodule update --init
```

### Build

See the available presets:

```txt
cmake --list-presets
cmake --list-presets=build
```

#### Linux

```txt
cmake --preset conf-dev-nix
cmake --build --preset build-dev-nix

cmake --preset conf-dist-nix
cmake --build --preset build-dist-nix
```

#### Windows

```txt
cmake --preset conf-dev-win
cmake --build --preset build-dev-win

cmake --preset conf-dist-win
cmake --build --preset build-dist-win
```

### Install

#### Linux

```txt
cmake --preset conf-dist-nix
cmake --build --preset build-dist-nix --target install
```

#### Windows

```txt
cmake --preset conf-dist-win
cmake --build --preset build-dist-win --target install
```

### Package

#### Linux

```txt
cmake --preset conf-dist-nix
cmake --build --preset build-dist-nix --target package_source

cmake --preset conf-dist-nix-rpm
cmake --build --preset build-dist-nix --target package

cmake --preset conf-dist-nix-deb
cmake --build --preset build-dist-nix --target package
```

#### Windows

```txt
cmake --preset conf-dist-win
cmake --build --preset build-dist-win --target package_source

cmake --preset conf-dist-win
cmake --build --preset build-dist-win --target package
```

## Dependencies

- [SDL](https://github.com/libsdl-org/SDL)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [Cereal](https://github.com/USCiLab/cereal)
- [FFTW](https://www.fftw.org/)

The licenses can be found in their respective directories.

## Contributing

The `main` branch is used for development, containing the newest bug fixes and features, while the `release` branch always points to a commit corresponding to a release.

## Initial Material and Acknowledgements

- PadSynth algorithm by Paul Nașca
- Sound Synthesis and Sampling Second Edition by Martin Russ
- THE COMPLETE SYNTHESIZER: A Comprehensive Guide by David Crombie
- Code-It-Yourself! Sound Synthesizer by javidx9

I give my thanks to my friend Tudor for sharing his knowledge of signal processing with me.

## Credits

One of the test compositions is the first section only from [I Secretly Love You](https://www.youtube.com/watch?v=7gYG95NG1YA) by Yuang Chen (Seycara Orchestral).

---

This software, even though it's free and open source, is not meant to be consumed for LLM training!
