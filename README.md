# Old-phone framebuffer

This project draws pixels straight to the screen of old phones. It does not use the normal graphics stack.
It runs on a jailbroken iPhone 4 (iOS 7.1.2), a Nexus 4 (LineageOS 14.1, Android 7.1.2), and an LG Intuition (Android 4.1.2).

On top of the display code, the project has multitouch input, a text console, an on-screen keyboard, a shell, a TIC-80 player with sound, and a music player.
One small platform layer hides the differences between the devices. On the iPhone, a small SpringBoard tweak lets a program own the screen for as long as it needs.

For the details of each device, read [docs/DEVICE-MAP.md](docs/DEVICE-MAP.md). That file is written for LLM agents and for people who maintain the project.

## What you can run

| Program | What it does | Devices |
|---|---|---|
| `paint` | Multitouch paint. It tests display and touch together. | iPhone 4, Nexus 4, LG Intuition |
| `condemo` | A scrolling text console. | iPhone 4, Nexus 4 |
| `fbshell` | A shell with an on-screen keyboard. | iPhone 4, Nexus 4 |
| `calib` | Measures the touch axes. It writes the result to a file. | iPhone 4 |
| `tic80` | A TIC-80 player for `.tic` and `.lua` carts, with sound. | iPhone 4 |
| `musicplayer` | A touch music player for a folder of music. It plays MP3, M4A, AAC, WAV, AIFF, FLAC, and Ogg. | iPhone 4 |
| `fblauncher` | An app picker with big touch tiles. It starts the other tools and returns to the picker when a tool ends. A home-screen icon, FB Tools, starts it. | iPhone 4 |
| `fbreset` | Clears a picture that a killed program left on the screen. | iPhone 4 |
| `Send to Phone` | A Mac app. Drop a music file on it and the iPhone plays the file. | Mac to iPhone 4 |

## Build

You need a Mac with these tools:

- Xcode command line tools
- `ldid` (`brew install ldid`)
- An iOS 9.3 SDK from `https://github.com/theos/sdks`
- Android NDK r27

The SDK needs one fix. See the Build section in the device map.

1. Run `./build.sh`. The binaries go to `build/`. The script stops with an error if any compile fails.
2. For TIC-80, run `tic80/fetch_source.sh`, then `tic80/build_core.sh`, then `tic80/build_player.sh`.
3. For the music player, run `music/fetch_libs.sh`, then `music/build.sh`.
4. For the iPhone tweak, build `tweak/FBMode.m` as shown in the device map.

## Run

Each iPhone program runs from a launch script, and each Android program runs from a script that stops the framework and restarts it.
Use the script. Do not run a program by hand while the system UI is stopped.

- iPhone 4: `common/run_shell_ios.sh`, `tic80/run_tic80_ios.sh`, and `music/run_music_ios.sh`
- Android: `common/run_paint.sh`, `common/run_shell.sh`, and `common/run_console.sh`

Install the SpringBoard tweak once on the iPhone. Without it, an iPhone program can own the screen for only about 110 seconds.
With it, a program can run as long as it needs. We played a 4-minute song and a 5-minute game.

To start the picker from the phone, run `launcher/install_icon_ios.sh` once. It adds the FB Tools icon to the home screen.
Tap the icon, then tap a tile.

For music, copy a folder from your Mac with `python3 music/sync_music.py FOLDER`. Or build the Mac app with `music/build_droplet.sh`, then drop music files on it.

The device map has the exact commands for each device.

## Layout

| Path | Content |
|---|---|
| `fb.h`, `fbdemo.c` | The iOS framebuffer library and a test program. |
| `android/fbdemo.c` | The Android framebuffer test program. |
| `common/` | The platform layer, drawing, console, keyboard, terminal, and the programs above. |
| `tic80/` | The TIC-80 player, its sound output, and the scripts that build it. |
| `music/` | The music player, the sync script, and the Mac drop app. |
| `launcher/` | The app picker and the installer for its home-screen icon. |
| `tweak/` | The SpringBoard tweak for long-running screen programs. |
| `docs/DEVICE-MAP.md` | How each device works. |
| `build.sh` | Builds every program. |

## License

This project uses the MIT license. See the file `LICENSE`.

## Third-party code

The repository does not contain the TIC-80 source. `tic80/fetch_source.sh` downloads it from `https://github.com/nesbox/TIC-80`.
TIC-80 has its own license in its repository.

The music player uses two single-file decoders: `dr_flac` (public domain or MIT-0) and `stb_vorbis` (public domain).
`music/fetch_libs.sh` downloads them.
