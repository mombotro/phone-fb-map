"""FB Tools manager: install, repair, and remove the FB Tools programs on a phone, and choose how FB Tools starts.
Used by phonefiles_gui.py. The files to install come from tools/payload/ (made by tools/build_payload.sh).

Three kinds of phone:
  ios7     iPhone 4 and similar (iOS 7-9, 32-bit). Has the SpringBoard tweak, so it can boot straight into FB Tools.
  ios12    iPhone 5s (iOS 12, 64-bit). No tweak yet: an icon on the home screen only, and a run lasts about 2 minutes.
  android  a rooted Android phone. Boot job through install-recovery-2.sh, or a home-screen icon (a small app).

Start modes:  boot  FB Tools starts at boot and has no Exit   home  an FB Tools icon   none  nothing starts by itself
A manifest on the phone (.fbtools.json) lists what was installed, so uninstall removes only our own files. User data
(notes, music, carts) stays unless the user asks to remove it.
"""
import json, os, posixpath, subprocess, threading, time, uuid

HERE = os.path.dirname(os.path.abspath(__file__))
PAYLOAD = os.path.join(HERE, "payload")
MANIFEST = ".fbtools.json"
APK_PACKAGE = "com.fbtools.launcher"
ANDROID_HOOK = "/system/etc/install-recovery-2.sh"
ANDROID_HOOK_TEXT = "#!/system/bin/sh\n# FB Tools: start the app picker at boot (fbrun detaches itself)\n/data/local/tmp/fbrun -d -k -b 31536000 fblauncher\n"

APPS = [
    {"id": "paint", "name": "Paint", "desc": "Draw with your fingers.", "bin": "paint"},
    {"id": "shell", "name": "Shell", "desc": "A terminal with an on-screen keyboard.", "bin": "fbshell"},
    {"id": "notes", "name": "Notes", "desc": "Notes and to-do lists.", "bin": "notes", "data": ["data/notes"]},
    {"id": "timer", "name": "Timer", "desc": "Stopwatch, countdown timer, and alarm.", "bin": "timer", "data": ["data/timer"]},
    {"id": "music", "name": "Music", "desc": "Play your music.", "bin": "musicplayer", "data": ["music"], "android_extra": ["musichelper.jar"]},
    {"id": "tic80", "name": "TIC-80", "desc": "Play TIC-80 game carts.", "bin": "tic80", "data": ["carts"], "only": ["ios7"]},
    {"id": "calib", "name": "Calibrate", "desc": "Fix the touch axes.", "bin": "calib", "only": ["ios7", "ios12"]},
    {"id": "console", "name": "Console", "desc": "A text console demo.", "bin": "condemo", "only": ["android"]},
]
PLATFORMS = {
    "ios7": {"label": "iPhone (iOS 7)", "base": "/var/root", "modes": ["boot", "home", "none"]},
    "ios12": {"label": "iPhone (iOS 12)", "base": "/var/root", "modes": ["home", "none"]},
    "android": {"label": "Android", "base": "/data/local/tmp", "modes": ["boot", "home", "none"]},
}


class FBToolsError(Exception):
    pass


def platform_of(dev):
    if dev.kind == "android":
        return "android"
    try:
        major = int(dev.version.split(".")[0])
    except (ValueError, AttributeError):
        return None
    if major <= 9: return "ios7"
    if major == 12: return "ios12"
    return None


def pl(plat, rel):
    return os.path.join(PAYLOAD, plat, rel)


def apps_for(plat):
    return [a for a in APPS if plat in a.get("only", [plat])]


def app_available(plat, a):
    return os.path.exists(pl(plat, "bin/" + a["bin"]))


def core_files(plat, tweak=True):
    """(payload relative path, remote path, mode)"""
    b = PLATFORMS[plat]["base"]
    if plat == "ios7":
        return [("bin/fblauncher", b + "/fblauncher", 0o755), ("bin/fbreset", b + "/fbreset", 0o755),
                ("run_launcher_ios.sh", b + "/run_launcher_ios.sh", 0o755), ("run_kiosk_ios.sh", b + "/run_kiosk_ios.sh", 0o755),
                ("FBMode.dylib", "/Library/MobileSubstrate/DynamicLibraries/FBMode.dylib", 0o755),
                ("FBMode.plist", "/Library/MobileSubstrate/DynamicLibraries/FBMode.plist", 0o644)]
    if plat == "ios12":
        return [("bin/fblauncher", b + "/fblauncher", 0o755), ("bin/fbreset", b + "/fbreset", 0o755),
                ("run_ios12.sh", b + "/run_ios12.sh", 0o755), ("ios12.entitlements.plist", b + "/ios12.ent", 0o644)] + \
               ([("lib/FBMode12.dylib", "/usr/lib/FBMode12.dylib", 0o755), ("lib/TweakInject.dylib", "/usr/lib/TweakInject.dylib", 0o755)] if tweak else [])
    return [("bin/fblauncher", b + "/fblauncher", 0o755), ("bin/fbrun", b + "/fbrun", 0o755), ("bin/fbls", b + "/fbls", 0o755)]


def app_files(plat, a):
    b = PLATFORMS[plat]["base"]
    out = [("bin/" + a["bin"], b + "/" + a["bin"], 0o755)]
    if plat == "android":
        out += [(x, b + "/" + x, 0o644) for x in a.get("android_extra", [])]
    return out


def expected_files(plat, app_ids, tweak=True):
    files = list(core_files(plat, tweak))
    for a in apps_for(plat):
        if a["id"] in app_ids: files += app_files(plat, a)
    return files


class Job:
    def __init__(self, title):
        self.id = uuid.uuid4().hex[:10]
        self.title = title
        self.lines = []
        self.done = False
        self.ok = None

    def log(self, msg):
        self.lines.append(msg)


JOBS = {}
BUSY = {}                      # device id -> job id while a job runs


def start_job(dev, title, fn):
    if BUSY.get(dev.id) and not JOBS[BUSY[dev.id]].done:
        raise FBToolsError("Another job is running on this phone. Wait for it to end.")
    job = Job(title); JOBS[job.id] = job; BUSY[dev.id] = job.id

    def run():
        try:
            fn(job.log)
            job.ok = True
            job.log("Done.")
        except Exception as e:
            job.ok = False
            job.log("FAILED: %s" % e)
        job.done = True
    threading.Thread(target=run, daemon=True).start()
    return job


# ---------------------------------------------------------------- phone helpers

def read_manifest(dev, plat):
    t = dev.get_text(PLATFORMS[plat]["base"] + "/" + MANIFEST)
    if not t: return None
    try: return json.loads(t)
    except ValueError: return None


def write_manifest(dev, plat, m):
    dev.put_text(PLATFORMS[plat]["base"] + "/" + MANIFEST, json.dumps(m, indent=1))


def remote_sizes(dev, plat, paths):
    """Sizes of the given remote files, from one listing per folder. Missing files are not in the result."""
    out = {}
    for d in sorted({posixpath.dirname(p) for p in paths}):
        try:
            for e in dev.ls(d):
                out[d.rstrip("/") + "/" + e["name"]] = e["size"]
        except Exception:
            pass
    return out


def mtweak(m):
    return m.get("tweak", True)


def record_sizes(dev, plat, m):
    """Remember, in the manifest, the size of each installed file on the phone (after signing, which adds bytes on iOS 12)
    and the size of the copy in this app. Status and repair compare against these, so they notice damage and also
    notice when this app carries a newer build."""
    files = m.get("files", [])
    remote = remote_sizes(dev, plat, files)
    local = {}
    for rel, rem, _ in expected_files(plat, m.get("apps", []), mtweak(m)):
        src = pl(plat, rel)
        if os.path.exists(src): local[rem] = os.path.getsize(src)
    m["sizes"] = {f: remote[f] for f in files if f in remote}
    m["psizes"] = local


def problems_for(dev, plat, m):
    """(missing_or_changed, outdated) lists of (rel, remote, mode) for an installed phone."""
    exp = expected_files(plat, m.get("apps", []), mtweak(m))
    have = remote_sizes(dev, plat, [r for _, r, _ in exp])
    want, pwant = m.get("sizes", {}), m.get("psizes", {})
    bad, old = [], []
    for rel, remote, md in exp:
        src = pl(plat, rel)
        if not os.path.exists(src): continue
        h = have.get(remote)
        if remote not in set(m.get("files", [])):                      # a file this version needs, but the manifest never listed
            bad.append((rel, remote, md, "not recorded in the manifest")); continue
        if plat == "ios12" and not want:        # signing changes the size, and no size was recorded: nothing can be trusted
            bad.append((rel, remote, md, "not verified (no size record)")); continue
        if h is None: bad.append((rel, remote, md, "missing"))
        elif remote in want and h != want[remote]: bad.append((rel, remote, md, "changed (%d bytes, expected %d)" % (h, want[remote])))
        elif remote not in want and plat != "ios12" and h != os.path.getsize(src): bad.append((rel, remote, md, "changed"))
        elif remote in pwant and os.path.getsize(src) != pwant[remote]: old.append((rel, remote, md, "a newer build is in this app"))
    return bad, old


def android_up(dev, log):
    """Android's package manager only works while Android runs. The picker (kiosk) stops Android, so stop the picker first."""
    r = dev.sh("service check window", timeout=15).stdout.decode()
    if "not found" not in r:
        return
    log("Android is stopped by the FB Tools picker. Stopping the picker so Android starts again...")
    b = PLATFORMS["android"]["base"]
    dev.sh("echo off > %s/fbboot_off" % b, timeout=15)                      # the boot loop stops when this exists
    ps = dev.adb("shell", "ps", timeout=15).stdout.decode(errors="replace").replace("\r", "")
    pids = [ln.split()[1] for ln in ps.splitlines() if "fblauncher" in ln or "/tmp/paint" in ln]
    for pid in pids:
        dev.sh("kill %s" % pid, timeout=15)
    for _ in range(40):
        time.sleep(2)
        if "not found" not in dev.sh("service check window", timeout=15).stdout.decode():
            time.sleep(3); return
    raise FBToolsError("Android did not come back. Restart the phone and try again.")


def ios_respring(dev, log):
    log("Restarting SpringBoard (the home screen) so the changes load...")
    dev.sh("killall -9 SpringBoard", timeout=20)


def foreign_loader(dev, plat):
    """True if /usr/lib/TweakInject.dylib exists on an iOS 12 phone and is NOT ours. We never overwrite someone else's loader.
    Ours contains the text FBMode12.dylib. If we cannot tell, we treat the loader as someone else's (the safe answer)."""
    if plat != "ios12": return False
    r = dev.sh("if [ -f /usr/lib/TweakInject.dylib ]; then grep -c FBMode12.dylib /usr/lib/TweakInject.dylib; else echo MISSING; fi", timeout=20).stdout.decode().strip()
    if r == "MISSING": return False
    try: return int(r.split()[0]) == 0
    except (ValueError, IndexError): return True


# ---------------------------------------------------------------- start modes

def apply_mode(dev, plat, mode, log):
    if mode not in PLATFORMS[plat]["modes"]:
        raise FBToolsError("This phone cannot use start mode '%s'." % mode)
    b = PLATFORMS[plat]["base"]
    if plat == "android":
        android_up(dev, log)
        hook = "No such file" not in dev.sh("ls %s" % ANDROID_HOOK, timeout=15).stdout.decode()
        if mode == "boot":
            log("Setting FB Tools to start at boot (no Exit tile)...")
            dev.put_text("/data/local/tmp/fbboot_hook.sh", ANDROID_HOOK_TEXT, 0o755)
            dev.sh("%s; mount -o remount,rw /system; cat /data/local/tmp/fbboot_hook.sh > %s; chmod 755 %s; mount -o remount,ro /system"
                   % (dev.rmf(b + "/fbboot_off"), ANDROID_HOOK, ANDROID_HOOK), timeout=60)
            if dev.get_text(ANDROID_HOOK) is None: raise FBToolsError("could not write the boot hook in /system")
            dev.adb("uninstall", APK_PACKAGE, timeout=60)
        else:
            if hook:
                log("Removing the boot hook...")
                dev.sh("mount -o remount,rw /system; rm %s; mount -o remount,ro /system" % ANDROID_HOOK, timeout=60)
            if mode == "home":
                log("Installing the FB Tools icon...")
                apk = pl("android", "FBTools.apk")
                r = dev.adb("install", "-r", apk, timeout=180)
                if b"Success" not in r.stdout: raise FBToolsError("icon install failed: " + (r.stdout + r.stderr).decode(errors="replace").strip()[-200:])
            else:
                log("Removing the FB Tools icon...")
                dev.adb("uninstall", APK_PACKAGE, timeout=60)
        return
    # iPhone
    icon_src = pl(plat, "FBTools.app")
    if mode in ("home", "boot"):
        log("Installing the FB Tools icon on the home screen...")
        dev.put_tree(icon_src, "/var/root")
        stub = "/Applications/FBTools.app/FBTools"
        sign = "ldid -S/var/root/ios12.ent %s; " % stub if plat == "ios12" else ""
        uic = "uicache -a" if plat == "ios12" else "su mobile -c uicache"
        r = dev.sh("rm -rf /Applications/FBTools.app; cp -R /var/root/FBTools.app /Applications/FBTools.app; rm -rf /var/root/FBTools.app; "
                   "chown -R root:wheel /Applications/FBTools.app; chmod 755 /Applications/FBTools.app; %schmod 4755 %s; %s" % (sign, stub, uic), timeout=90)
        if r.returncode: raise FBToolsError("icon install failed: " + r.stderr.decode(errors="replace").strip()[-200:])
    else:
        log("Removing the FB Tools icon...")
        dev.sh("rm -rf /Applications/FBTools.app; %s" % ("uicache -a" if plat == "ios12" else "su mobile -c uicache"), timeout=60)
    if plat == "ios7":
        if mode == "boot":
            log("Turning on kiosk mode (boots straight into FB Tools, no way out)...")
            dev.put_file(pl("ios7", "com.local.fbkiosk.plist"), "/Library/LaunchDaemons/com.local.fbkiosk.plist", 0o644)
            dev.sh("chown root:wheel /Library/LaunchDaemons/com.local.fbkiosk.plist; rm -f /var/root/kiosk_off; touch /var/mobile/fbkiosk_on; chmod 644 /var/mobile/fbkiosk_on", timeout=30)
        else:
            log("Turning off kiosk mode...")
            dev.sh("touch /var/root/kiosk_off; rm -f /var/mobile/fbkiosk_on /tmp/fbkiosk_wanted; launchctl unload /Library/LaunchDaemons/com.local.fbkiosk.plist 2>/dev/null; "
                   "rm -f /Library/LaunchDaemons/com.local.fbkiosk.plist", timeout=30)
        ios_respring(dev, log)


# ---------------------------------------------------------------- actions

SELF_HEAL = ("( zsh -c 'zmodload zsh/files; zmodload zsh/zselect; zselect -t 9000; rm -f /usr/lib/TweakInject.dylib /usr/lib/TweakInject.dylib.new; "
             ": > /var/root/.disable_fbmode' >/dev/null 2>&1 & )")


def copy_files(dev, plat, files, log):
    """Copy the files to the phone. On iOS 12 the programs and libraries are signed on the phone (the Mac ldid is too old).
    SAFETY: /usr/lib/TweakInject.dylib is loaded by Chimera into EVERY new process. A copy that is not signed yet would
    break every new process, SSH included. So a library is always written to a name that is never loaded (.new), signed
    there, and only then renamed into place. A shell timer that uses only built-in commands deletes the loader after
    90 seconds if something goes wrong, and it is cancelled when the copy has worked."""
    base = PLATFORMS[plat]["base"]
    for rel, remote, mode in files:
        if not os.path.exists(pl(plat, rel)):
            raise FBToolsError("missing in the payload: %s. Run tools/build_payload.sh and rebuild the app." % rel)
    is_lib = lambda rel: rel.startswith("lib/")
    risky = plat == "ios12" and any(is_lib(rel) for rel, _, _ in files)
    if risky:
        dev.sh(SELF_HEAL, timeout=20)
    try:
        for rel, remote, mode in files:
            src = pl(plat, rel)
            log("  copy %s" % remote)
            target = remote + ".new" if plat == "ios12" and is_lib(rel) else remote
            dev.put_file(src, target, mode)
            if plat == "ios12" and (rel.startswith("bin/") or is_lib(rel)):
                r = dev.sh("ldid -S%s/ios12.ent %s" % (base, target), timeout=30)
                if r.returncode: raise FBToolsError("signing %s failed: %s" % (remote, r.stderr.decode(errors="replace").strip()))
            if target != remote:
                dev.sh("mv %s %s" % (target, remote), timeout=20)
        if risky:
            ok = dev.sh("echo ok", timeout=20).stdout.decode().strip() == "ok"          # a new process still works
            if not ok: raise FBToolsError("the phone does not start new programs after the copy")
    finally:
        if risky:                                                                       # cancel the timer
            dev.sh("for p in $(ps -A -o pid,command | grep 'zmodload zsh/files' | grep -v grep | cut -c1-6); do kill $p; done", timeout=20)


def data_dirs(plat, app_ids):
    b = PLATFORMS[plat]["base"]
    return [b + "/" + d for a in apps_for(plat) if a["id"] in app_ids for d in a.get("data", [])]


def do_install(dev, plat, mode, app_ids, log):
    app_ids = [i for i in app_ids if any(a["id"] == i for a in apps_for(plat))]
    if plat == "android":
        if not dev.root: raise FBToolsError("This Android phone has no root. FB Tools needs root.")
        android_up(dev, log)
    tweak = True
    if foreign_loader(dev, plat):
        tweak = False
        log("Another tweak loader is installed on this phone. FB Tools will not replace it.")
        log("FB Tools still works, but each run lasts only about 2 minutes (SpringBoard is frozen).")
    log("Copying the FB Tools programs...")
    exp = expected_files(plat, app_ids, tweak)
    copy_files(dev, plat, exp, log)
    for d in data_dirs(plat, app_ids):
        dev.sh("mkdir -p %s; chmod 777 %s" % (d, d), timeout=20)
    files = {remote: None for _, remote, _ in exp}
    write_manifest(dev, plat, {"version": 1, "platform": plat, "apps": app_ids, "mode": "none", "files": list(files), "tweak": tweak})
    apply_mode(dev, plat, mode, log)
    m = read_manifest(dev, plat) or {}
    m.update({"version": 1, "platform": plat, "apps": app_ids, "mode": mode, "files": list(files), "tweak": tweak})
    record_sizes(dev, plat, m)
    write_manifest(dev, plat, m)
    if plat == "ios12" and tweak:
        log("Restarting SpringBoard (the home screen) so the FB Tools tweak loads...")
        dev.sh("rm -f /var/root/.disable_fbmode; killall -9 SpringBoard", timeout=30)
    if mode == "boot":
        log("FB Tools starts at the next boot. Use 'Start now' to start it without a restart.")
    elif mode == "home":
        log("Tap the FB Tools icon on the phone to start it.")


def do_uninstall(dev, plat, remove_data, log):
    if plat == "android":
        android_up(dev, log)
    m = read_manifest(dev, plat) or {}
    b = PLATFORMS[plat]["base"]
    if plat == "ios7":
        log("Turning off kiosk mode and removing the icon...")
        dev.sh("touch /var/root/kiosk_off; rm -f /var/mobile/fbkiosk_on; launchctl unload /Library/LaunchDaemons/com.local.fbkiosk.plist 2>/dev/null; "
               "rm -f /Library/LaunchDaemons/com.local.fbkiosk.plist; for n in fblauncher paint tic80 musicplayer fbshell calib condemo notes timer; do killall $n >/dev/null 2>&1; done; sleep 2; "
               "/var/root/fbreset 2>/dev/null; rm -rf /Applications/FBTools.app; su mobile -c uicache", timeout=90)
    elif plat == "ios12":
        log("Removing the icon...")
        dev.sh("for n in fblauncher paint musicplayer fbshell calib notes timer; do killall $n >/dev/null 2>&1; done; rm -rf /Applications/FBTools.app; uicache -a", timeout=90)
        if m.get("tweak", True):
            log("Removing the FB Tools tweak and its loader, then restarting SpringBoard...")
    else:
        log("Removing the boot hook and the icon...")
        dev.sh("echo off > %s/fbboot_off; mount -o remount,rw /system; rm %s; mount -o remount,ro /system" % (b, ANDROID_HOOK), timeout=60)
        dev.adb("uninstall", APK_PACKAGE, timeout=60)
    files = m.get("files") or [r for _, r, _ in expected_files(plat, [a["id"] for a in apps_for(plat)])]
    log("Removing %d program files..." % len(files))
    for f in files:
        dev.sh(dev.rmf(f), timeout=20)
    for f in (".fbtools.json", "kiosk.log", "kiosk.boot.log", "kiosk.pid", "fbboot_off", "fbboot_hook.sh", "boot.log", "fblauncher.out", "run_launcher.log", "fbrun.pid"):
        dev.sh(dev.rmf(b + "/" + f), timeout=20)
    if remove_data:
        log("Removing your data (notes, music, carts)...")
        for d in data_dirs(plat, [a["id"] for a in apps_for(plat)]):
            dev.sh("rm -rf %s" % d, timeout=120)
    else:
        log("Your notes, music, and carts were kept.")
    if plat == "ios7":
        ios_respring(dev, log)
    elif plat == "ios12" and m.get("tweak", True):
        dev.sh("rm -f /var/root/.disable_fbmode; killall -9 SpringBoard", timeout=30)


def do_repair(dev, plat, log):
    m = read_manifest(dev, plat)
    if not m: raise FBToolsError("FB Tools is not installed on this phone. Use Install.")
    if plat == "android":
        android_up(dev, log)
    app_ids = m.get("apps", []); mode = m.get("mode", "none")
    exp = expected_files(plat, app_ids, mtweak(m))
    log("Checking %d files..." % len(exp))
    bad, old = problems_for(dev, plat, m)
    new_files = [r for _, r, _ in exp]
    stale = [f for f in m.get("files", []) if f not in new_files]            # files an older version installed, which this one does not use
    if stale:
        log("Removing %d file%s from an older version:" % (len(stale), "" if len(stale) == 1 else "s"))
        for f in stale:
            log("  remove %s" % f)
            dev.sh(dev.rmf(f), timeout=20)
        m["files"] = new_files
        record_sizes(dev, plat, m)
        write_manifest(dev, plat, m)
    if bad or old:
        for rel, remote, md, why in bad + old: log("  %s: %s" % (remote, why))
        log("Restoring %d file%s..." % (len(bad) + len(old), "" if len(bad) + len(old) == 1 else "s"))
        copy_files(dev, plat, [(r, rem, md) for r, rem, md, _ in bad + old], log)
        if plat == "ios12" and any(r.startswith("lib/") for r, _, _, _ in bad + old):
            log("Restarting SpringBoard so the restored tweak loads...")
            dev.sh("killall -9 SpringBoard", timeout=30)
        m["apps"] = app_ids
        m["files"] = [r for _, r, _ in exp]                            # the manifest lists every file we now own
        record_sizes(dev, plat, m)
        write_manifest(dev, plat, m)
    else:
        log("All program files are in place.")
    b = PLATFORMS[plat]["base"]
    log("Resetting permissions, data folders, and stuck switches...")
    for d in data_dirs(plat, app_ids):
        dev.sh("mkdir -p %s; chmod 777 %s" % (d, d), timeout=20)
    for _, remote, md in exp:
        dev.sh("chmod %o %s" % (md, remote), timeout=15)
    if plat == "android":
        dev.sh(dev.rmf(b + "/.pf_stage") + "; " + dev.rmf(b + "/.pf_up"), timeout=20)
        if mode == "boot": dev.sh(dev.rmf(b + "/fbboot_off"), timeout=15)         # a crash-loop shut-off must not stay on
    if plat == "ios7" and mode == "boot":
        dev.sh("rm -f /var/root/kiosk_off /var/root/kiosk.pid", timeout=15)
    apply_mode(dev, plat, mode, log)
    log("Repair finished. If something is still wrong, use Uninstall and then Install.")


def do_set_mode(dev, plat, mode, log):
    m = read_manifest(dev, plat)
    if not m: raise FBToolsError("FB Tools is not installed on this phone. Use Install.")
    apply_mode(dev, plat, mode, log)
    m["mode"] = mode
    write_manifest(dev, plat, m)


def do_app(dev, plat, app_id, action, log):
    m = read_manifest(dev, plat)
    if not m: raise FBToolsError("FB Tools is not installed on this phone. Use Install.")
    a = next((x for x in apps_for(plat) if x["id"] == app_id), None)
    if not a: raise FBToolsError("This app does not exist for this phone.")
    ids = list(m.get("apps", []))
    if action == "add":
        if not app_available(plat, a): raise FBToolsError("%s is not in the payload. Run tools/build_payload.sh." % a["name"])
        if plat == "android": android_up(dev, log)
        log("Adding %s..." % a["name"])
        copy_files(dev, plat, app_files(plat, a), log)
        for d in data_dirs(plat, [app_id]): dev.sh("mkdir -p %s; chmod 777 %s" % (d, d), timeout=20)
        if app_id not in ids: ids.append(app_id)
        files = set(m.get("files", [])) | {r for _, r, _ in app_files(plat, a)}
    else:
        log("Removing %s (your data stays)..." % a["name"])
        for _, remote, _ in app_files(plat, a): dev.sh(dev.rmf(remote), timeout=20)
        ids = [i for i in ids if i != app_id]
        files = set(m.get("files", [])) - {r for _, r, _ in app_files(plat, a)}
    m["apps"] = ids; m["files"] = sorted(files)
    record_sizes(dev, plat, m)
    write_manifest(dev, plat, m)
    log("The picker shows only the apps that are installed. It looks again each time it starts.")


def do_start(dev, plat, log):
    b = PLATFORMS[plat]["base"]
    m = read_manifest(dev, plat) or {}
    if plat == "android":
        android_up_needed = "not found" not in dev.sh("service check window", timeout=15).stdout.decode()
        log("Starting FB Tools on the phone (Android stops while it runs)...")
        dev.sh(dev.rmf(b + "/fbboot_off") + "; " + b + "/fbrun -d 3600 fblauncher", timeout=20)
    elif plat == "ios7":
        log("Starting FB Tools...")
        dev.sh("(sh /var/root/run_kiosk_ios.sh 3 </dev/null >/dev/null 2>&1 &) ; true" if m.get("mode") == "boot"
               else "(sh /var/root/run_launcher_ios.sh </dev/null >/dev/null 2>&1 &) ; true", timeout=20)
    else:
        log("Starting FB Tools (the home screen freezes while it runs)...")
        dev.sh("(sh /var/root/run_ios12.sh fblauncher 3000 </dev/null >/dev/null 2>&1 &) ; true", timeout=20)


# ---------------------------------------------------------------- status

def status(dev):
    plat = platform_of(dev)
    if not plat:
        return {"supported": False, "message": "This phone type is not supported by FB Tools (iOS 7 to 9, iOS 12, or rooted Android)."}
    if dev.kind == "android" and not dev.root:
        return {"supported": False, "message": "This Android phone has no root. FB Tools needs root."}
    info = PLATFORMS[plat]
    m = read_manifest(dev, plat)
    apps = []
    for a in apps_for(plat):
        apps.append({"id": a["id"], "name": a["name"], "desc": a["desc"], "available": app_available(plat, a),
                     "installed": bool(m and a["id"] in m.get("apps", []))})
    out = {"supported": True, "platform": plat, "label": info["label"], "modes": info["modes"], "installed": bool(m),
           "mode": m.get("mode") if m else None, "apps": apps, "payload_ok": os.path.exists(pl(plat, "bin/fblauncher")), "problems": []}
    if m:
        bad, old = problems_for(dev, plat, m)
        out["problems"] = ["%s: %s" % (why, remote) for _, remote, _, why in bad]
        out["updates"] = [remote for _, remote, _, _ in old]
    if plat == "ios12":
        out["note"] = ("With the FB Tools tweak (installed with FB Tools), a run has no time limit. This iPhone cannot start FB Tools at boot, "
                       "because the jailbreak must be started again after every restart.")
    return out
