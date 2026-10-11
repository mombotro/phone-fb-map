# Phone directory

This page lists each phone we tried, and what works on it. The status of a feature is one of three words.

| Status | Meaning |
|---|---|
| Works | It was tested on the phone and it worked. Where only a script tested it, the note says so. |
| No | The owner tested it and it did not work, or it cannot work on this phone. |
| Unknown | Nobody tested it yet, or the result was not checked. |

We never mark a feature as Works from reading the code. If you test a feature on a phone, send a pull request that changes its status.
The last update was on 2026-10-10.

## Summary

| Phone | System | How it is opened | Overall |
|---|---|---|---|
| iPhone 4 (GSM, iPhone3,2) | iOS 7.1.2 | Pangu jailbreak (untethered) | Most features work. This unit cannot activate now. |
| iPhone 5s (iPhone6,1) | iOS 12.5.7 | Chimera jailbreak (semi-untethered) | The picker, Paint, Notes, and Timer work. Runs last about 2 minutes. |
| LG Intuition (VS950) | Android 4.1.2 | Root (SuperSU) | Most features work, including boot into FB Tools. |
| Nexus 4 | Android 7.1.2 (LineageOS 14.1) | Root (adb root) | Display, touch, shell, and Paint work. The rest is Unknown. |
| Moto Z2 Force | Android 8.0 | Not rooted (bootloader is unlocked) | No. It needs root first. |

## iPhone 4 (iOS 7.1.2)

| Feature | Status | Notes |
|---|---|---|
| Drawing to the screen | Works | Uses IOMobileFramebuffer. A SpringBoard tweak keeps the system screen quiet. |
| Multitouch | Works | Touch axes value 2. |
| Text console, keyboard, shell | Works | |
| Paint | Works | |
| Notes and to-do lists | Works | |
| Timer, stopwatch, alarm | Unknown | The screens work. The beep and the alarm wake were not checked. |
| Music player | Works | MP3, M4A, AAC, WAV, FLAC, Ogg. The Mac app "Send to Phone" copies files to it. |
| TIC-80 player with sound | Works | |
| App picker and home-screen icon | Works | |
| Boot straight into FB Tools (kiosk) | Works | No lock screen. The power button turns the screen off and on. |
| Long runs | Works | Tested for several minutes (a 5-minute game, a 4-minute song). The design has no time limit. Needs the FBMode tweak. |
| Phone Files (Mac app) | Unknown | Written for SSH. Not run on this phone yet. |
| FB Tools manager (install, repair, uninstall) | Unknown | Written, not run on this phone yet. |

The unit we tested on was restored to iOS 7.1.2 and now fails to activate with "Device Unknown". The phone is locked to a carrier, and a SIM from that carrier may be needed.

## iPhone 5s (iOS 12.5.7)

| Feature | Status | Notes |
|---|---|---|
| Drawing to the screen | Works | iOS 12 takes each rectangle as 4 doubles. Layer 1 shows on top. |
| Multitouch | Works | Needs the `com.apple.hid.multitouch.user-access` entitlement. Touch axes value 2. |
| Paint | Works | |
| App picker | Works | |
| Notes and to-do lists | Works | |
| Timer, stopwatch, alarm | Works | Only the screens were checked. Sound is Unknown. |
| Runs longer than about 2 minutes | No | iOS stops SpringBoard when we freeze it and touch the screen. A tweak is needed. |
| Boot straight into FB Tools | No | Chimera is semi-untethered. The phone starts without the jailbreak after every reboot. |
| Shell | Unknown | Built, not tested. |
| Music player, TIC-80 | No | Not built for 64-bit yet. |
| Home-screen icon | Unknown | The manager installs it (setuid stub, `uicache -a`). Nobody tapped it yet. |
| Phone Files (Mac app) | Works | Browse, upload, download, rename, delete, and the protection of system files. Tested with scripts through the app's own API. Not yet tried by hand in the window. |
| FB Tools manager | Works | Install, repair of a deleted and a corrupted file, and install over a broken state. Tested with scripts. Add and remove apps, mode changes, and uninstall: Unknown. |

You must sign programs on the phone with `ldid` and the entitlements in `common/ios12.entitlements.plist`. The `ldid` on the Mac is too old.

## LG Intuition (VS950, Android 4.1.2)

| Feature | Status | Notes |
|---|---|---|
| Drawing to the screen | Works | The display reads bytes as R, G, B, A. The code fixes the colors. |
| Multitouch | Works | |
| Paint | Works | |
| App picker | Works | |
| Notes and to-do lists | Works | `rename()` fails on Android 4.1 with this toolkit. The code calls the kernel directly. |
| Timer and stopwatch | Unknown | There is no sound on Android yet. The alarm cannot wake a blank screen. |
| Music: sound | Works | Android plays the files with MediaPlayer, through a small Java helper. |
| Music: player screen | Unknown | |
| Shell | Unknown | |
| Boot straight into FB Tools | Works | No Exit tile. The picker starts again if it stops. |
| Home-screen icon | Unknown | The icon app starts the picker when launched from `adb`. Nobody tapped it yet. |
| Phone Files (Mac app) | Works | Browse, upload, download, rename, delete, and the protection of system files. Tested with scripts through the app's own API. Not yet tried by hand in the window. |
| FB Tools manager | Works | Install, change start mode, add and remove apps, repair, and uninstall. Tested with scripts. Not yet tried by hand in the window. |

## Nexus 4 (LineageOS 14.1, Android 7.1.2)

| Feature | Status | Notes |
|---|---|---|
| Drawing to the screen | Works | Needs `adb root`. |
| Multitouch | Works | |
| Text console, keyboard, shell | Works | |
| Paint | Works | |
| App picker, Notes, Timer, Music | Unknown | The code is shared with the Intuition. Nobody ran it here. |
| Boot straight into FB Tools | Unknown | |
| Phone Files and the FB Tools manager | Unknown | |

## Moto Z2 Force (Android 8.0)

| Feature | Status | Notes |
|---|---|---|
| Everything | No | The phone has no root. The framebuffer device needs root. |

The bootloader is unlocked, so root is possible with a patched boot image. Newer phones often draw with a different display path, so the framebuffer may still show nothing. We did not try.

## Add a phone

1. Run the program for the feature on your phone, with the scripts in this repository.
2. Change its status in the table above, and add a short note.
3. Write the exact system version, and the way you opened the phone.
4. Send a pull request.
