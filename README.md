# Old-phone framebuffer

This project draws pixels straight to the screen of old phones. It does not use the normal graphics stack.
It runs on a jailbroken iPhone 4 (iOS 7.1.2), a Nexus 4 (LineageOS 14.1, Android 7.1.2), and an LG Intuition (Android 4.1.2).

On top of the display code, the project has multitouch input, a text console, an on-screen keyboard, a shell, and a TIC-80 player.
One small platform layer hides the differences between the devices.

For the details of each device, read [docs/DEVICE-MAP.md](docs/DEVICE-MAP.md). That file is written for LLM agents and for people who maintain the project.

## What you can run

| Program | What it does | Devices |
|---|---|---|
| `paint` | Multitouch paint. It tests display and touch together. | iPhone 4, Nexus 4, LG Intuition |
| `condemo` | A scrolling text console. | iPhone 4, Nexus 4 |
| `fbshell` | A shell with an on-screen keyboard. | iPhone 4, Nexus 4 |
| `calib` | Measures the touch axes. It writes the result to a file. | iPhone 4 |
| `tic80` | A TIC-80 player for `.tic` and `.lua` carts. Sound does not work yet. | iPhone 4 |

## Build

You need a Mac with these tools:

- Xcode command line tools
- `ldid` (`brew install ldid`)
- An iOS 9.3 SDK from `https://github.com/theos/sdks`
- Android NDK r27

The SDK needs one fix. See the Build section in the device map.

1. Run `./build.sh`. The binaries go to `build/`.
2. For TIC-80, run `tic80/fetch_source.sh`, then `tic80/build_core.sh`, then `tic80/build_player.sh`.

## Run

Each device has a launch script that stops the system UI, runs the program, and restores the UI.
Use the script. Do not run a program by hand while the system UI is stopped.

- iPhone 4: `common/run_shell_ios.sh` and `tic80/run_tic80_ios.sh`
- Android: `common/run_paint.sh`, `common/run_shell.sh`, and `common/run_console.sh`

The device map has the exact commands for each device.

## Layout

| Path | Content |
|---|---|
| `fb.h`, `fbdemo.c` | The iOS framebuffer library and a test program. |
| `android/fbdemo.c` | The Android framebuffer test program. |
| `common/` | The platform layer, drawing, console, keyboard, terminal, and the programs above. |
| `tic80/` | The TIC-80 player and the scripts that build it. |
| `docs/DEVICE-MAP.md` | How each device works. |
| `build.sh` | Builds every program. |

## License

This project uses the MIT license. See the file `LICENSE`.

## Third-party code

The repository does not contain the TIC-80 source. `tic80/fetch_source.sh` downloads it from `https://github.com/nesbox/TIC-80`.
TIC-80 has its own license in its repository.
