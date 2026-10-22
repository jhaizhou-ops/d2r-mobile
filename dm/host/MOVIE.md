# Optional macOS VP8 playback

中文: [MOVIE.zh-CN.md](MOVIE.zh-CN.md)

`MOVIE_FFMPEG=1` replaces the host's duration-only `movie::` backend with VP8 decoding through FFmpeg. The default build continues to use the existing backend. No game content or FFmpeg binaries are included. On Mac, install FFmpeg development libraries. On iOS, build the static libraries with the script below and link them into the app.

The adapter implements the existing extractor/decoder vtables: the guest supplies a random-access reader, owns its input/output events, schedules frames using their PTS, and handles skip and looping. Frames are packed NV12. Audio and subtitles remain on their existing guest paths; FFmpeg does not play audio. The adapter shares the reader between extractor and decoder and keeps acquired frames until release, with eight output slots and input backpressure.

## Build

Install `pkg-config` and FFmpeg development libraries providing `libavformat`, `libavcodec`, `libavutil`, and `libswscale`. The synthetic fixture also needs the `ffmpeg` executable with `libvpx`, `libopus`, and lavfi. Development was checked against FFmpeg 9.0.2 (library versions 63.1.102 / 63.1.102 / 61.1.102 / 10.1.102).

After preparing your own game files with the normal setup, force a rebuild when changing backends:

```sh
make -C dm/host -B dmhost MOVIE_FFMPEG=1
```

Omit the flag, or use `MOVIE_FFMPEG=0`, for the existing backend. The same option applies to `dmhost_m3`, but installed FFmpeg libraries may require a newer macOS than that target. Check dependency deployment versions separately. This change does not bundle libraries into an app or change the iOS project.

## iOS

Build minimal arm64 iOS static libraries containing only the Matroska/WebM, VP8 decoder, and NV12 conversion modules:

```sh
dm/ios/tools/build_ffmpeg_ios.sh
DM_MOVIE_FFMPEG=1 dm/ios/run.sh build
```

The script writes to `$DM_ROOT/build/cache/dm/ffmpeg-ios/out` by default. `DM_MOVIE_FFMPEG_PREFIX` may point to another compatible prebuilt directory. `run.sh` sets both `MOVIE_FFMPEG=1` and `MOVIE_PREFIX`, links the four static libraries into the app, and keeps the original Cinematics entry available. Without `DM_MOVIE_FFMPEG`, the build keeps the duration-only backend.

The standard 18 resource chunks contain the global story videos. Japanese and Traditional Chinese localized openings live in the corresponding language chunks and must be added separately to the ordinary RomFS outside the app installer.

Cinematics use the game's original `VoiceLanguage` selection. The Custom page adds an independent voice-over radio: Chinese text can use English voice, or switch back to Chinese voice. Tap Apply, then close and reopen the app for the change to take effect. This changes voice-over only, not the UI language.

## Synthetic tests, no game files

```sh
make -C dm/host/tests movie-real PY=python3
make -C dm/host/tests movie-real PY=python3 B=/tmp/dm-movie-sanitized \
  MOVIE_TEST_FLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'
make -C dm/host/tests build/t_movie
./dm/host/tests/build/t_movie
```

The test generates two 60-frame VP8 WebM files in a temporary directory, one with an Opus stream, and compares every decoded NV12 frame with FFmpeg's raw output. It also checks metadata, monotonic PTS, EOS, seek/restart, the empty packet used after loop seek, output capacity, acquired-frame ownership, backpressure, stop/skip, and shared-reader teardown. Fixtures are removed after the test. Guest event delivery is represented by a test callback; no guest executable, Metal presentation, speaker output, or on-device behavior is tested.

No rewrite sites, startup cuts, touch layouts, product revision, or movie-selection UI are changed. Only movies already reached by the current guest build use this backend. Enabling initial intro sequences is a separate change and is not claimed here.

API references: the installed FFmpeg headers document custom `AVIOContext` ownership and the send/receive decoder state machine; see also the official [custom-IO example](https://ffmpeg.org/doxygen/trunk/avio_read_callback_8c-example.html) and [decoding API](https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html). Installed headers take precedence over the moving documentation version.
