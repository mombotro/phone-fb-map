#!/usr/bin/env python3
"""Copy dropped music files to the iPhone and play the first one.

usage: send_to_phone.py FILE_OR_FOLDER [...]

Files go to /var/root/music/Dropped on the phone. A folder keeps its name and its audio files.
Then the script asks the music player to play the first file. It starts the player if it is not running,
and it installs the player first if the phone does not have it. The Mac app "Send to Phone" calls this script.
"""
import os, shlex, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ["PATH"] = "/opt/homebrew/bin:/usr/local/bin:" + os.environ.get("PATH", "")
import sync_music as sm            # reuses the ssh helpers and the list of audio extensions

PORT = 2222
ROOT = "/var/root/music"
DROP = ROOT + "/Dropped"
BUSY = ["tic80", "fbshell", "paint", "condemo", "calib"]          # programs that already own the phone screen
SSH = ["ssh", "-p", str(PORT)] + sm.SSH_OPTS + ["root@127.0.0.1"]
SCP = ["scp", "-P", str(PORT)] + sm.SSH_OPTS


def notify(text):
    print(text, flush=True)
    safe = text.replace("\\", "\\\\").replace('"', '\\"')          # AppleScript string: escape \ and "
    subprocess.run(["osascript", "-e", f'display notification "{safe}" with title "Send to Phone"'], capture_output=True)


def run(cmd, **kw):
    return sm.ssh(PORT, cmd, **kw)


def collect(paths):
    """Return a list of (local_file, remote_relative_path)."""
    out = []
    for p in paths:
        p = os.path.abspath(p)
        if os.path.isdir(p):
            base = os.path.basename(p.rstrip("/"))
            for root, dirs, names in os.walk(p):
                dirs[:] = sorted(d for d in dirs if not d.startswith("."))
                for n in sorted(names):
                    if not n.startswith(".") and os.path.splitext(n)[1].lower() in sm.AUDIO:
                        full = os.path.join(root, n)
                        out.append((full, base + "/" + os.path.relpath(full, p).replace(os.sep, "/")))
        elif os.path.splitext(p)[1].lower() in sm.AUDIO:
            out.append((p, os.path.basename(p)))
        else:
            print("skipping (not a supported audio file):", p)
    return out


def copy_file(local, remote_rel):
    remote = DROP + "/" + remote_rel
    rdir = remote.rsplit("/", 1)[0]
    cmd = f"mkdir -p {shlex.quote(rdir)} && cat > {shlex.quote(remote + '.part')} && mv {shlex.quote(remote + '.part')} {shlex.quote(remote)}"
    with open(local, "rb") as f:
        r = run(cmd, stdin=f, check=False)
    if r.returncode != 0:
        raise RuntimeError(f"copy failed for {local}: {r.stderr.strip()}")
    return remote


def player_running():
    return run("killall -0 musicplayer >/dev/null 2>&1 && echo yes || echo no", check=False).stdout.strip() == "yes"


def busy_program():
    for name in BUSY:
        if run(f"killall -0 {name} >/dev/null 2>&1 && echo yes || echo no", check=False).stdout.strip() == "yes":
            return name
    return None


def install_player():
    binary = os.path.join(HERE, "..", "build", "musicplayer_ios")
    script = os.path.join(HERE, "run_music_ios.sh")
    if not os.path.exists(binary):
        raise RuntimeError("build the player first: music/build.sh")
    subprocess.run(SCP + [binary, "root@127.0.0.1:/var/root/musicplayer"], check=True, capture_output=True)
    subprocess.run(SCP + [script, "root@127.0.0.1:/var/root/run_music_ios.sh"], check=True, capture_output=True)
    run("chmod +x /var/root/musicplayer")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    items = collect(sys.argv[1:])
    if not items:
        notify("No supported music files in the drop.")
        return 1
    try:
        sm.ensure_tunnel(PORT)
    except SystemExit as e:
        notify(str(e))
        return 1

    first = None
    for i, (local, rel) in enumerate(items, 1):
        print(f"[{i}/{len(items)}] {rel}", flush=True)
        remote = copy_file(local, rel)
        first = first or remote

    run(f"printf '%s\\n' {shlex.quote(first)} > {shlex.quote(ROOT + '/.play.tmp')} && mv {shlex.quote(ROOT + '/.play.tmp')} {shlex.quote(ROOT + '/.play')}")

    if player_running():
        notify(f"Playing {os.path.basename(first)}")
        return 0
    other = busy_program()
    if other:
        notify(f"Copied. The phone screen is busy with {other}. Close it, then start the music player.")
        return 0
    install_player()
    run("(sh /var/root/run_music_ios.sh /var/root/music 86400 </dev/null >/dev/null 2>&1 &)")
    notify(f"Starting the music player: {os.path.basename(first)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
