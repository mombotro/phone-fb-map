#!/usr/bin/env python3
"""Copy a music folder from this computer to the iPhone over the USB SSH tunnel.

usage: sync_music.py FOLDER [--dest /var/root/music] [--port 2222] [--delete] [--watch [SECONDS]]

It sends only the files that are new or whose size changed. It needs `iproxy` (libimobiledevice) and
the phone's OpenSSH. The phone has no rsync, so it streams a tar archive over ssh. Folder layout is kept,
and the player shows the same folders. --delete also removes phone files that are not in FOLDER.
"""
import argparse, os, re, shlex, socket, subprocess, sys, tempfile, time

AUDIO = {".mp3", ".m4a", ".aac", ".wav", ".aif", ".aiff", ".caf", ".flac", ".ogg", ".m4b", ".mp4"}
SSH_OPTS = ["-o", "HostKeyAlgorithms=+ssh-rsa", "-o", "PubkeyAcceptedAlgorithms=+ssh-rsa",
            "-o", "StrictHostKeyChecking=no", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"]


def ssh(port, command, stdin=None, check=True):
    cmd = ["ssh", "-p", str(port)] + SSH_OPTS + ["root@127.0.0.1", command]
    return subprocess.run(cmd, stdin=stdin, capture_output=True, text=True, check=check)


def tunnel_up(port):
    with socket.socket() as s:
        s.settimeout(1)
        return s.connect_ex(("127.0.0.1", port)) == 0


def ensure_tunnel(port):
    if tunnel_up(port):
        return
    print(f"starting iproxy {port}:22")
    subprocess.Popen(["iproxy", f"{port}:22"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(10):
        time.sleep(0.5)
        if tunnel_up(port):
            return
    sys.exit("cannot reach the phone: is it plugged in, and is iproxy installed?")


def local_files(folder):
    files = {}
    for root, dirs, names in os.walk(folder):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for n in names:
            if n.startswith(".") or os.path.splitext(n)[1].lower() not in AUDIO:
                continue
            full = os.path.join(root, n)
            files[os.path.relpath(full, folder).replace(os.sep, "/")] = os.path.getsize(full)
    return files


def remote_files(port, dest):
    out = ssh(port, f"find {shlex.quote(dest)} -type f -exec ls -ln {{}} \; 2>/dev/null", check=False).stdout
    files = {}
    for line in out.splitlines():
        parts = line.split(None, 8)
        if len(parts) == 9 and parts[4].isdigit() and parts[8].startswith(dest + "/"):
            files[parts[8][len(dest) + 1:]] = int(parts[4])
    return files


def sync_once(folder, dest, port, delete):
    ensure_tunnel(port)
    local, remote = local_files(folder), remote_files(port, dest)
    todo = sorted(p for p, size in local.items() if remote.get(p) != size)
    gone = sorted(p for p in remote if p not in local) if delete else []
    if todo:
        total = sum(local[p] for p in todo)
        print(f"sending {len(todo)} file(s), {total / 1e6:.1f} MB")
        with tempfile.NamedTemporaryFile("w", suffix=".lst", delete=False) as lst:
            lst.write("\n".join(todo) + "\n")
        env = dict(os.environ, COPYFILE_DISABLE="1")                 # no AppleDouble ._ files
        tar = subprocess.Popen(["tar", "-cf", "-", "-C", folder, "-T", lst.name], stdout=subprocess.PIPE, env=env)
        r = ssh(port, f"mkdir -p {shlex.quote(dest)} && tar -xf - -C {shlex.quote(dest)}", stdin=tar.stdout, check=False)
        tar.stdout.close()
        os.unlink(lst.name)
        if tar.wait() != 0 or r.returncode != 0:
            sys.exit(f"transfer failed: {r.stderr.strip()}")
        for p in todo:
            print("  +", p)
    for p in gone:
        ssh(port, f"rm -f {shlex.quote(dest + '/' + p)}", check=False)
        print("  -", p)
    if not todo and not gone:
        print("phone is up to date")
    return len(todo), len(gone)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("--dest", default="/var/root/music")
    ap.add_argument("--port", type=int, default=2222)
    ap.add_argument("--delete", action="store_true", help="remove phone files that are not in FOLDER")
    ap.add_argument("--watch", nargs="?", type=float, const=10.0, metavar="SECONDS",
                    help="keep running and sync again every SECONDS (default 10)")
    a = ap.parse_args()
    if not os.path.isdir(a.folder):
        sys.exit(f"not a folder: {a.folder}")
    folder = os.path.abspath(a.folder)
    if a.watch:
        print(f"watching {folder} (Ctrl-C to stop)")
        while True:
            try:
                sync_once(folder, a.dest, a.port, a.delete)
            except SystemExit as e:
                print(e)
            time.sleep(a.watch)
    else:
        sync_once(folder, a.dest, a.port, a.delete)


if __name__ == "__main__":
    main()
