# nemu
WIP famicom/NES emulator

## features
- 6502 cpu, 2C02 ppu and 2A03 apu, NTSC, PAL and Dendy timing
- mappers including MMC1, MMC3, VRC6, VRC7, N163 and the Famicom Disk System (bring your own BIOS)
- NSF and NSFe playback with expansion audio (FDS, N163, VRC6, VRC7), a favorites playlist and track timing
- per channel oscilloscopes, a piano roll and register views
- mixer, pitch shift, effects (limiter, chorus, echo, reverb, widening, EQ) and replacement waveforms, including .wav samples
- video export of the scopes, register log and piano roll through FFmpeg
- palette presets and adjustments, blargg's NTSC filter as a GPU shader
- PPU debug window, zipped ROMs, Japanese file names
- remappable keyboard and gamepad controls for two players

## compiling
nemu uses [raylib](https://www.raylib.com/) and builds with [meson](https://mesonbuild.com/) and ninja:

```
meson setup builddir
ninja -C builddir
```

On Windows this works from an MSYS2 UCRT64 shell with `mingw-w64-ucrt-x86_64-raylib`, `meson` and `ninja` installed; the raylib, glfw and winpthread DLLs are copied next to `nemu.exe` so the build folder runs on its own.

Video export needs `ffmpeg` on the PATH, next to `nemu.exe`, or set in the settings window.

The `Makefile` and `config.mk` are from the older SDL3 version and don't build the current sources.

## credits / libraries

- raylib: https://www.raylib.com/

- Nuklear: https://github.com/Immediate-Mode-UI/Nuklear and raylib-nuklear: https://github.com/RobLoach/raylib-nuklear

- cJSON: https://github.com/DaveGamble/cJSON

- blargg's NTSC filter, ported from the libretro glsl-shaders collection: https://github.com/libretro/glsl-shaders

- Nesdev wiki: https://www.nesdev.org/wiki/
