# NBA2K.AudioFix

`NBA2K.AudioFix` is an ASI plugin that brings higher-quality console audio to PC. Many sounds in the PC release use low-bitrate WMA at 22,050 Hz, while the PS3 banks use higher-bitrate ATRAC3 at 48,000 Hz, which sounds better than the PC audio. This issue was introduced with NBA 2K10 and continued through NBA 2K14.

The plugin decodes the PS3 audio to 16-bit PCM and sends it through the game's original XAudio2 system. For the few cues missing from the PS3 version, it uses the Xbox 360 xWMA/WMAv2 versions instead.

It currently supports NBA 2K11.

## Building

Requires:

- A 32-bit MinGW toolchain
- CMake 3.20 or newer
- A static i686 build of FFmpeg 8.1.2

Build:

```powershell
git submodule update --init
cmake -S . -B build -G "MinGW Makefiles" `
  -DFFMPEG_BUILD_ROOT="path/to/ffmpeg-build"
cmake --build build --config Release
```

## License

Licensed under the [GNU GPL version 3 or later](LICENSE). FFmpeg remains under its own license.
