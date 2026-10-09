# Device map

This file tells an LLM agent how each device works. It lists only what we tested and know to work.
Read the shared design first. Then read the section for your device.

## What works

| Feature | iPhone 4 | Nexus 4 | LG Intuition |
|---|---|---|---|
| Draw to the framebuffer | yes | yes | yes |
| Text console | yes | yes | not tested |
| Touch | yes | yes | yes |
| On-screen keyboard and shell | yes | yes | not tested |
| TIC-80 player | yes | not tested | not tested |
| Music player | yes | not built | not built |
| Paint demo | yes | yes | yes |

## Shared design

The code has one small platform layer. The files are `common/plat.h`, `common/plat_ios.h`, and `common/plat_android.h`.
Programs call the same six functions on every device.

| Function | Job |
|---|---|
| `plat_init` | Open the display. Fill a `PlatInfo` with the size and the pixel format. |
| `plat_begin` | Return the back buffer and its stride in pixels. |
| `plat_flip` | Show the back buffer. Then rotate to the next buffer. |
| `plat_close` | Put the display back. |
| `plat_touch_init` | Open the touch source. |
| `plat_touch_poll` | Return the fingers that touch the screen now, in screen pixels. |

Rules that apply to all devices:

- Use three buffers. Two buffers cause tearing.
- Pace the loop with a 60 Hz timer. A vsync wait does not work on these devices.
- Write pixels with the channel shifts in `PlatInfo`. The shifts differ between devices.
- Redraw only when the screen changes. Draw the same picture into each of the three buffers.
- Stop the system UI before you draw. Always restart it when the program ends.

The higher layers work on every device:

| File | Job |
|---|---|
| `common/draw.h` | Rectangles, disks, lines, and text on a pixel surface. |
| `common/console.h` | A text grid with colors, scrolling, and an 8x16 font. |
| `common/term.h` | A small terminal emulator. It reads `\r`, `\n`, `\b`, and common ANSI sequences. |
| `common/kbd.h` | An on-screen QWERTY keyboard with shift, symbols, Ctrl, Tab, and backspace repeat. |
| `common/fbshell.c` | A shell on a pty. The terminal is on top and the keyboard is below. |
| `common/paint.c` | A multitouch paint program. It tests display and touch together. |

## Build

Use a Mac with Xcode command line tools, `ldid`, an iOS 9.3 SDK, and Android NDK r27.

1. Get the SDK: `git clone https://github.com/theos/sdks ~/sdks`.
2. Open `~/sdks/iPhoneOS9.3.sdk/usr/lib/libSystem.tbd`. It is a symlink, so replace it with a copy of its target.
3. Remove `/usr/lib/system/liblaunch.dylib, ` from the copy. The SDK does not include that library.
4. Run `./build.sh`. It writes every binary to `build/`.

Link iOS programs with `-Wl,-undefined,dynamic_lookup`. This flag resolves `memcpy` and the private symbols at run time.

Build Android programs as static binaries: `armv7a-linux-androideabi21-clang -O2 -static -fno-pie -fno-emulated-tls`.
The static build runs on Android 4.1. The NDK cannot target API 16 with dynamic linking.
The file `common/plat_android.h` has a 32-byte aligned thread-local variable. Without it, bionic rejects the TLS segment.

## iPhone 4

| Item | Value |
|---|---|
| Model | iPhone3,2 (A4, armv7) |
| System | iOS 7.1.2, jailbroken |
| Screen | 640x960 |
| Pixel format | `0xAARRGGBB` in a 32-bit word (BGRA bytes) |

### Access

1. Install OpenSSH from Cydia.
2. Run `iproxy 2222:22` on the Mac.
3. Connect with `ssh -p 2222 -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa root@127.0.0.1`.

The phone accepts only RSA keys. The default root password is `alpine`. Change it.
The phone has no `head`, `grep`, `wc`, `ps`, or `tr`. Filter output on the Mac.
`scp` can time out just after a respring. Retry it.

### Display

The layer default surface gives garbled pixels, so do not use it. Draw into your own surfaces.

1. Open `IOMobileFramebuffer.framework` and `IOSurface.framework` with `dlopen`.
2. Call `IOMobileFramebufferGetMainDisplay` to get the display.
3. Make three IOSurfaces with `IOSurfaceCreate`. Set width 640, height 960, bytes per element 4, bytes per row 2560.
4. Set the pixel format to `'BGRA'` (0x42475241). Set `IOSurfaceIsGlobal` to true. Set `IOSurfaceMemoryRegion` to `PurpleGFXMem`.
5. Lock a surface, write pixels, and unlock it.
6. Call `IOMobileFramebufferSwapBegin(display, &token)`.
7. Call `IOMobileFramebufferSwapSetLayer(display, 0, surface, rect, rect, 0)`. A rect is four floats: 0, 0, 640, 960.
8. Call `IOMobileFramebufferSwapEnd(display)`.

Do not call `IOMobileFramebufferSwapWait`. It blocks with these arguments. Use a 60 Hz timer.

### Keep SpringBoard quiet

A program that owns the screen must stop SpringBoard from drawing, and must not let the system kill it.
Freezing SpringBoard does not work for long. iOS replaces a frozen SpringBoard after about two minutes.
Pulses of SIGCONT, a frozen `backboardd`, and an unloaded SpringBoard all failed. See the notes at the end of this section.

**Live mode** is the method that works. SpringBoard keeps running, and the program draws on top of it.

1. Build the tweak: `clang -arch armv7 -dynamiclib -x objective-c -isysroot SDK -Wl,-undefined,dynamic_lookup -o FBMode.dylib tweak/FBMode.m`, then `ldid -S FBMode.dylib`.
2. Copy `FBMode.dylib` and `tweak/FBMode.plist` to `/Library/MobileSubstrate/DynamicLibraries` on the phone.
3. Respring with `killall -9 SpringBoard`.

The tweak loads only into SpringBoard. It does nothing unless the file `/tmp/fbmode` exists and is under 5 seconds old.
While that flag is fresh, the tweak does these things:

- It drops touch events, so taps cannot reach the hidden home screen.
- It refuses the swipe gestures for Control Center and Notification Center.
- It hides every SpringBoard window and keeps new windows hidden. Banners and panels cannot show.
- It prevents idle sleep, so the screen stays on.

Each program creates the flag, refreshes it about once a second, and deletes it at exit. A program that dies leaves a stale flag,
so SpringBoard returns to normal within 5 seconds. The program draws on display layer 1, which sits above layer 0. Layer 2 does not exist.
Live mode has no time limit. We played a 4-minute song and the player stayed on screen and awake.

The launch scripts `common/run_shell_ios.sh`, `tic80/run_tic80_ios.sh`, and `music/run_music_ios.sh` use live mode when the tweak file is present.
Without the tweak they fall back to frozen mode, which holds a run to 110 seconds and waits 160 seconds after a respring.

Rules that came from failures:

- Keep the SBSettings tweak disabled. It crashes SpringBoard when SpringBoard wakes after a freeze.
- Do not freeze `backboardd`. Audio and the display need it.
- Do not unload SpringBoard. The phone rebooted when a program drew with no SpringBoard.
- A program killed with SIGKILL leaves its picture on layer 1. Run `fbreset` to clear it.
- Scope each watchdog timer to the process ID of its own run. An unscoped `killall` timer outlived its run and killed the next run.
- With two iOS devices on USB, `iproxy` tunnels to the first one. Start it with `iproxy -u UDID 2222:22`.

### Touch

The HID event system does not give touchscreen events to a second client. Do not use it.
Use the private framework `MultitouchSupport`.

1. Open `/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport` with `dlopen`.
2. Call `MTDeviceCreateDefault`.
3. Call `MTRegisterContactFrameCallback(device, callback)`.
4. Call `MTDeviceStart(device, 0)`.

The callback receives `(device, fingers, count, timestamp, frame)`. The fingers use the macOS `MTFinger` layout. One finger is 92 bytes.
The finger struct is in `common/plat_ios.h`. Its normalized position (0 to 1) is in two floats. State 4 means the finger touches the screen.

The panel axes do not match the display. Run the tool `calib_ios` once. It asks you to tap three corners.
It writes the transform number to `/var/root/mt_xform`. This phone gives 2, which means flip y and keep x.
Bit 0 flips x. Bit 1 flips y. Bit 2 swaps x and y.

### Sign and run

Sign each binary with `ldid -Scommon/hid.entitlements.plist`. Copy it to `/var/root`.
To clear a leftover picture on layer 1, run `/var/root/fbreset`.
Run the shell with `sh /var/root/run_shell_ios.sh 300 2`. Type `exit` to end it.

### Shell

`common/fbshell.c` uses `posix_openpt`, `grantpt`, `unlockpt`, and `ptsname`. It starts `/bin/bash` with `TERM=dumb`.

### TIC-80

The player is in `tic80/`. It runs TIC-80 with Lua only. It does not include the editor.

1. Run `tic80/fetch_source.sh`. It gets the TIC-80 source and five libraries.
2. Run `tic80/build_core.sh`. It builds the core as a static library.
3. Run `tic80/build_player.sh`. It writes `build/tic80_ios`.
4. Copy a cart to `/var/root/carts`.
5. Run `sh /var/root/run_tic80_ios.sh /var/root/carts/tetris.lua 90`.

The player loads `.tic` carts and `.lua` projects. It reads the `-- <TILES>` and similar sections of a `.lua` file with the core project parser.
The game runs at 60 ticks per second and about 57 frames per second.

The player has sound. It pushes the audio from `tic80_sound` into a ring buffer, and an `AudioQueue` plays it (`tic80/audio.h`).
The `music.lua` demo cart confirms it. The Tetris demo keeps its sound off until you choose "sound on" in its menu.

### Music player

The player is in `music/`. It plays MP3, M4A, AAC, WAV, AIFF, FLAC, and Ogg files from `/var/root/music`.

1. Run `music/fetch_libs.sh`. It gets two single-file decoders.
2. Run `music/build.sh`. It writes `build/musicplayer_ios`.
3. Copy music to the phone with `python3 music/sync_music.py FOLDER`. The script sends only new or changed files.
4. Run `sh /var/root/run_music_ios.sh`.

Apple's `ExtAudioFile` decodes MP3, M4A, WAV, and AIFF. The library `dr_flac` decodes FLAC. The library `stb_vorbis` decodes Ogg.
Output goes to an `AudioQueue`. Audio output works from a root tool with no audio session.
In live mode the player runs with no time limit.

The Mac app `Send to Phone` copies a dropped file to the phone and asks the player to play it. Build the app with `music/build_droplet.sh`.

## Nexus 4

| Item | Value |
|---|---|
| Model | Nexus 4 (mako), Qualcomm APQ8064 |
| System | LineageOS 14.1, Android 7.1.2 |
| Screen | 768x1280, MIPI DSI video panel |
| Framebuffer | `/dev/graphics/fb0`, driver `msmfb44_80601` |
| Pixel format | 32 bits per pixel. Red shift 24, green 16, blue 8, alpha 0 |

### Access

1. Turn on USB debugging in Developer options.
2. Set Developer options, Root access, to "ADB only".
3. Run `adb root`.

Without step 2, the shell cannot open `/dev/graphics/fb0`.

### Display

1. Open `/dev/graphics/fb0`.
2. Read `FBIOGET_VSCREENINFO` and `FBIOGET_FSCREENINFO`.
3. Map `smem_len` bytes with `mmap`.
4. The virtual height is 3840. This gives three pages of 1280 rows.
5. Draw into a page. Then call `FBIOPAN_DISPLAY` with `yoffset` set to the page times 1280.

The `FBIO_WAITFORVSYNC` ioctl fails on this kernel. The pan call blocks, so the display runs at about 27 frames per second.
The kernel log shows the failed ioctl. This is normal.

### Stop the system UI

Run `stop` to stop the framework. Run `start` to restart it.

CAUTION: Never run `stop` while Android is still booting. The display stays black or shows an old frame.
Before you stop the framework, wait until both of these are true:

- `service check window` says the service is found.
- `getprop init.svc.bootanim` prints `stopped`.

Then wait three more seconds. The scripts `common/run_paint.sh`, `common/run_shell.sh`, and `common/run_console.sh` do this check.

If the display gets stuck, reset it: `echo 4 > /sys/class/graphics/fb0/blank`, wait one second, then `echo 0 > /sys/class/graphics/fb0/blank`.
The program does this reset at start. The scripts do it again before `start`.

The backlight can be 0 after a screen timeout. `plat_init` sets `/sys/class/leds/lcd-backlight/brightness` to 128 when it reads less than 40.

### Touch

The touchscreen is `/dev/input/event2` (`touch_dev`). Its range is 0 to 1535 in x and 0 to 2559 in y.
Read `struct input_event` records. The code in `common/plat_android.h` handles multitouch protocols A and B.
It scales the values to screen pixels.

### Run

Use these commands from the Mac:

1. `adb push build/paint_android /data/local/tmp/paint`
2. `adb push common/run_paint.sh /data/local/tmp/run_paint.sh`
3. `adb shell 'nohup sh /data/local/tmp/run_paint.sh 60 >/dev/null 2>&1 &'`

Run the script in the background on the phone. Then an interrupted `adb` session cannot leave Android stopped.
The shell program is `fbshell_android` with `run_shell.sh`. It starts `/system/bin/sh` on a pty.

## LG Intuition

| Item | Value |
|---|---|
| Model | LG VS950 (Verizon) |
| System | Android 4.1.2, API 16 |
| Screen | 768x1024 |
| Framebuffer | `/dev/graphics/fb0`, three pages (virtual height 3072) |
| Pixel format | 32 bits per pixel. Red shift 24, green 16, blue 8, alpha 0 |

### Access

The phone has a setuid `su` at `/system/xbin/su`. Turn on USB debugging.
The `adb shell` user is in the group `graphics`, so it can open `fb0` without root.
Use `su` only to run `stop` and `start`. A Superuser prompt can appear on the phone. Tap Allow.

### Display and touch

The display works the same way as on the Nexus 4. The pan ioctl gives about 59 frames per second.
The touchscreen is `/dev/input/event5` (`synaptics_ts`). Its range is 0 to 1535 in x and 0 to 2047 in y.

### Run

We ran the paint demo with this sequence as root: `stop`, then `/data/local/tmp/paint 60`, then `start`.
Start it with `adb shell 'su -c "sh /data/local/tmp/run_paint.sh 60 >/dev/null 2>&1 &"'`.
The Android 4.1 `rm` does not know the `-f` option. This only prints a message.

## Rules for agents

1. Run risky programs in the background on the device. Use a script with a `trap` that restores the UI.
2. Tell the user to watch the screen before you start a run. Output from a command reaches the user only after the command ends.
3. Never interrupt a run that has stopped the UI. If you do, restart the UI at once.
4. Do not start a run just after a respring or a restart. Wait for the UI to finish its start.
5. Dump `fb0` to a file to see what is in the buffers. Convert it to PNG with Python and PIL. This shows if the problem is in the picture or in the display path.
6. Write calibration results to a file. Do not guess touch axes.
