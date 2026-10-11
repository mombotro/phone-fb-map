# Old-phone framebuffer

This project draws pixels straight to the screen of old phones. It does not use the normal graphics stack.
It runs on a jailbroken iPhone 4 (iOS 7.1.2), an iPhone 5s (iOS 12.5.7), a Kindle Fire (Android 2.3), a Nexus 4 (LineageOS 14.1, Android 7.1.2), and an LG Intuition (Android 4.1.2).

On top of the display code, the project has multitouch input, a text console, an on-screen keyboard, a shell, a TIC-80 player with sound, and a music player.
One small platform layer hides the differences between the devices. On the iPhone, a small SpringBoard tweak lets a program own the screen for as long as it needs.

To see which phones work and which features work on each one, read [docs/PHONES.md](docs/PHONES.md).
For the details of each device, read [docs/DEVICE-MAP.md](docs/DEVICE-MAP.md). That file is written for LLM agents and for people who maintain the project.

## What you can run

| Program | What it does | Devices |
|---|---|---|
| `paint` | Finger painting with 10 colors, 3 brush sizes, an eraser, undo, and SAVE to a `.bmp` file. Every finger paints, so it also tests multitouch. | iPhone 4, Nexus 4, LG Intuition |
| `condemo` | A scrolling text console. | iPhone 4, Nexus 4 |
| `fbshell` | A shell with an on-screen keyboard. | iPhone 4, Nexus 4 |
| `calib` | Measures the touch axes. It writes the result to a file. | iPhone 4 |
| `tic80` | A TIC-80 player for `.tic` and `.lua` carts, with sound. | iPhone 4 |
| `musicplayer` | A touch music player for a folder of music. It plays MP3, M4A, AAC, WAV, AIFF, FLAC, and Ogg. On Android it plays through the system MediaPlayer. | iPhone 4, LG Intuition |
| `fblauncher` | An app picker with big touch tiles. It starts the other tools and returns to the picker when a tool ends. A home-screen icon, FB Tools, starts it. | iPhone 4 |
| `notes` | Notes and to-do lists. Each note is a `.txt` file. A line that starts with `[ ] ` is a tick-box. The TODO view lists all open items. | iPhone 4, Nexus 4, LG Intuition |
| `timer` | A stopwatch, a countdown timer, and an alarm clock. The timer and the alarm ring while the program runs. The alarm wakes a blank screen. | iPhone 4, Nexus 4, LG Intuition |
| `longhouse` | A chat client for the Longhouse family chat. It reads and sends text over the home Wi-Fi, and shows picture thumbnails. Full-size pictures and video are not possible. | iPhone 4, Intuition, Kindle Fire |
| `bible` | Reads the American Standard Version Bible by book and chapter, with three text sizes. The text is not in this repository. Run `tools/make_bible.py` on an ASV HTML package (the `asv` folder) before `tools/build_payload.sh`, then add the Bible app in Phone Files. | Kindle Fire |
| `reader` | Reads `.txt` and `.epub` books from a folder. It has a library list, three text sizes, page buttons, a progress bar you can tap, and it remembers your place in each book. PDF is not supported. | Kindle Fire |
| `pixel` | A pixel art editor: 16, 32, or 48 pixels square, 16 colors, pen, fill, color pick, and undo. It saves `.pix` files and `.bmp` pictures. | Kindle Fire |
| `fbreset` | Clears a picture that a killed program left on the screen. | iPhone 4 |
| `Phone Files` | A Mac app. It shows the files on a connected iPhone or Android phone. Drag files onto the window to copy them to the phone, and click a file to download it. | Mac to iPhone, Mac to Android |
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
- Android: `fbrun [-d] [-k] [-b] [seconds] [program]` (the picker by default; see `launcher/fbrun.c`), `common/run_paint.sh`, `common/run_shell.sh`, and `common/run_console.sh`

Install the SpringBoard tweak once on the iPhone. Without it, an iPhone program can own the screen for only about 110 seconds.
With it, a program can run as long as it needs. We played a 4-minute song and a 5-minute game.

To start the picker from the phone, run `launcher/install_icon_ios.sh` once. It adds the FB Tools icon to the home screen.
Tap the icon, then tap a tile. The picker is called momPKT (mom pocket) and has a Palm-style look: a dark title bar with the clock, a battery indicator (tap it for the exact percentage), and outlined tiles. The Shut down tile turns the phone off after you confirm. The Exit tile shows only when you open the picker from its icon, not when it starts at boot. To get the old dark look, put the word `dark` in the file `theme`. On Android, the power button turns the screen off and on. Tap the name momPKT to open the settings drop-down: Wi-Fi and Bluetooth switches, a sound level that the music player and the alarm use, a brightness slider, portrait or landscape, and a button for Android's own Settings. Wi-Fi works on the Kindle Fire. The Bluetooth switch needs a Bluetooth rfkill entry, and the Kindle Fire has none.

The iPhone 5s (iOS 12, jailbroken with Chimera) runs the same programs as arm64 builds. Run `tools/build_ios12.sh`, then `tools/deploy_ios12.sh`.
The deploy script signs the programs on the phone, because the Mac `ldid` is too old for iOS 12. Run a program with `run_ios12.sh PROGRAM` on the phone.
SpringBoard stays frozen while the program runs, so a run lasts at most 100 seconds. There is no tweak or kiosk mode for the 5s yet.

For the Longhouse chat, run `python3 tools/longhouse_login.py` on the Mac. It signs in with a person invite (saved in `research/longhouse_invite.txt`, which git ignores), and copies a 24-hour session to the phone.
The phone talks to the home-node over plain HTTP, so the phone must be on the same Wi-Fi. Picture thumbnails come through the signaling server over HTTPS.
The Longhouse app needs two libraries that are not in this repository. `./build.sh` fetches them: BearSSL (`common/fetch_bearssl.sh`) and `stb_image.h` (`common/fetch_stb_image.sh`).

To copy files to and from a phone, build the Mac app with `tools/build_phonefiles_app.sh`, then open `tools/PhoneFiles.app`.
It uses SSH for iPhones and `adb` for Android phones.
It only deletes, renames, or overwrites your own files: notes, music, carts, and app data. System files and the programs on the phone are locked. A lock icon marks them.

The FB Tools tab in Phone Files installs FB Tools on a phone, and removes it again. It also does these jobs:
- Choose how FB Tools starts: boot straight into it (no Restart in the menu, but the icon stays as a backup), an icon on the home screen only, or nothing.
- Add or remove one app at a time. Notes, music, and carts stay when you remove an app.
- Repair a broken install. It checks each file against the copy in the app, and restores any that are missing or changed.

Build the files for the app with `tools/build_payload.sh`, then `tools/build_phonefiles_app.sh`.
To start an Android phone on the picker at boot without the app, run `launcher/android_boot.sh on`. `adb` still works. Run `launcher/android_boot.sh off` to undo it. The phone needs root.

Kiosk mode makes the phone boot straight into the picker with no way out to the home screen. See `launcher/kiosk.sh` and the device map.

For music, copy a folder from your Mac with `python3 music/sync_music.py FOLDER`. Or build the Mac app with `music/build_droplet.sh`, then drop music files on it.

To copy app files between the Mac and the phone, run `python3 tools/phonefiles.py status`, then `pull`, `push`, or `sync`.
The files go to `~/PhoneFiles/<app>` on the Mac. If both sides changed a file, the tool keeps both copies and does not overwrite anything.

The device map has the exact commands for each device.

## Layout

| Path | Content |
|---|---|
| `fb.h`, `fbdemo.c` | The iOS framebuffer library and a test program. |
| `android/fbdemo.c` | The Android framebuffer test program. |
| `common/` | The platform layer, drawing, console, keyboard, terminal, the touch UI kit `ui.h`, and the programs above. |
| `tic80/` | The TIC-80 player, its sound output, and the scripts that build it. |
| `music/` | The music player (iOS and Android), the sync scripts, and the Mac drop app. |
| `tools/` | `phonefiles.py`, the file sync tool, and the build and deploy scripts for the iPhone 5s. |
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
