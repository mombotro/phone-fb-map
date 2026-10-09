#!/usr/bin/env python3
"""Pull and push the files of the phone apps (notes, books, pictures, ...) between the iPhone and the Mac.

usage: phonefiles.py status [APP ...]     show what would change (nothing is copied)
       phonefiles.py pull   [APP ...]     copy new and changed files from the phone to the Mac
       phonefiles.py push   [APP ...]     copy new and changed files from the Mac to the phone
       phonefiles.py sync   [APP ...]     pull and push
       phonefiles.py apps                 list the app folders on the phone

Each app keeps its files in /var/root/data/APP on the phone. On the Mac they are in ~/PhoneFiles/APP.
With no APP the tool uses every app folder it finds on either side.

The tool remembers the state of each file at the last sync (~/PhoneFiles/.phonefiles.json). That tells it who changed:
  changed on one side only  -> copy it to the other side
  changed on both sides     -> CONFLICT: nothing is overwritten, and the phone copy is saved next to the Mac copy as
                               "name (phone copy).ext". Use --prefer mac or --prefer phone to decide.
  deleted on one side       -> reported, and deleted on the other side only with --delete
Options: --dry-run, --delete, --prefer mac|phone, --mac-dir DIR, --port N

It needs the iproxy tunnel (it starts one if the port is closed). With two iOS devices on USB, set PHONE_UDID.
"""
import argparse, json, os, shlex, socket, subprocess, sys, time

REMOTE_ROOT = "/var/root/data"
SSH_OPTS = ["-o", "HostKeyAlgorithms=+ssh-rsa", "-o", "PubkeyAcceptedAlgorithms=+ssh-rsa",
            "-o", "StrictHostKeyChecking=no", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"]


# ---------------------------------------------------------------- connection

def ssh_cmd(port, command):
    return ["ssh", "-p", str(port)] + SSH_OPTS + ["root@127.0.0.1", command]


def ssh(port, command, stdin=None, **kw):
    return subprocess.run(ssh_cmd(port, command), stdin=stdin, capture_output=True, **kw)


def tunnel_up(port):
    with socket.socket() as s:
        s.settimeout(1)
        return s.connect_ex(("127.0.0.1", port)) == 0


def ensure_tunnel(port):
    os.environ["PATH"] = "/opt/homebrew/bin:/usr/local/bin:" + os.environ.get("PATH", "")
    if tunnel_up(port):
        return
    cmd = ["iproxy"] + (["-u", os.environ["PHONE_UDID"]] if os.environ.get("PHONE_UDID") else []) + [f"{port}:22"]
    print("starting", " ".join(cmd))
    subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(10):
        time.sleep(0.5)
        if tunnel_up(port):
            return
    sys.exit("cannot reach the phone: is it plugged in, and is iproxy installed?")


# ---------------------------------------------------------------- listing

def remote_apps(port):
    r = ssh(port, f"ls -1 {REMOTE_ROOT} 2>/dev/null", text=True)
    return sorted(a for a in r.stdout.split() if a and not a.startswith("."))


def remote_files(port, app):
    """{relpath: (size, mtime)} for one app on the phone."""
    cmd = f"find {shlex.quote(REMOTE_ROOT + '/' + app)} -type f -printf '%s %T@ %P\\n' 2>/dev/null"
    out = ssh(port, cmd, text=True).stdout
    files = {}
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[0].isdigit():
            files[parts[2]] = (int(parts[0]), int(float(parts[1])))
    return files


def local_files(mac_dir, app):
    files = {}
    base = os.path.join(mac_dir, app)
    for root, dirs, names in os.walk(base):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for n in names:
            if n.startswith(".") or n.endswith("~") or " (phone copy)" in n:      # conflict copies stay on the Mac
                continue
            full = os.path.join(root, n)
            st = os.stat(full)
            files[os.path.relpath(full, base).replace(os.sep, "/")] = (st.st_size, int(st.st_mtime))
    return files


def same(a, b):
    return a is not None and b is not None and a[0] == b[0] and abs(a[1] - b[1]) <= 2


# ---------------------------------------------------------------- planning

def plan(app, L, R, base):
    """Return a list of (relpath, action). Actions: push, pull, conflict, del-remote, del-local, same."""
    out = []
    for rel in sorted(set(L) | set(R)):
        l, r, b = L.get(rel), R.get(rel), base.get(rel)
        if same(l, r):
            out.append((rel, "same")); continue
        if b is None:                                   # never synced
            if l and not r: out.append((rel, "push"))
            elif r and not l: out.append((rel, "pull"))
            else: out.append((rel, "conflict"))
            continue
        bl, br = tuple(b["l"]), tuple(b["r"])
        l_chg = l is None or not same(l, bl)
        r_chg = r is None or not same(r, br)
        if l and r:
            out.append((rel, "conflict" if (l_chg and r_chg) else "push" if l_chg else "pull"))
        elif l and not r:                               # missing on the phone
            out.append((rel, "push" if l_chg else "del-local"))
        else:                                           # missing on the Mac
            out.append((rel, "pull" if r_chg else "del-remote"))
    return out


# ---------------------------------------------------------------- transfers

def push_files(port, mac_dir, app, rels):
    if not rels:
        return
    lst = "\n".join(rels) + "\n"
    env = dict(os.environ, COPYFILE_DISABLE="1")
    tar = subprocess.Popen(["tar", "-C", os.path.join(mac_dir, app), "-cf", "-", "-T", "-"], stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, env=env)
    rd = shlex.quote(REMOTE_ROOT + "/" + app)
    ssh_p = subprocess.Popen(ssh_cmd(port, f"mkdir -p {rd} && tar -C {rd} -xf -"), stdin=tar.stdout,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    tar.stdout.close()
    tar.stdin.write(lst.encode()); tar.stdin.close()
    _, err = ssh_p.communicate()
    if tar.wait() != 0 or ssh_p.returncode != 0:
        sys.exit(f"push failed: {err.decode().strip()}")


def pull_files(port, mac_dir, app, rels):
    if not rels:
        return
    dest = os.path.join(mac_dir, app)
    os.makedirs(dest, exist_ok=True)
    rd = shlex.quote(REMOTE_ROOT + "/" + app)
    ssh_p = subprocess.Popen(ssh_cmd(port, f"tar -C {rd} -cf - -T -"), stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    tar = subprocess.Popen(["tar", "-C", dest, "-xf", "-"], stdin=ssh_p.stdout)
    ssh_p.stdout.close()
    ssh_p.stdin.write(("\n".join(rels) + "\n").encode()); ssh_p.stdin.close()
    tar.wait()
    err = ssh_p.stderr.read()
    ssh_p.wait()
    if tar.returncode != 0 or ssh_p.returncode != 0:
        sys.exit(f"pull failed: {err.decode().strip()}")


def pull_as_copy(port, mac_dir, app, rel):
    """Conflict: save the phone's version beside the Mac's, named "name (phone copy).ext"."""
    stem, ext = os.path.splitext(rel)
    copy_rel = f"{stem} (phone copy){ext}"
    r = ssh(port, f"cat {shlex.quote(REMOTE_ROOT + '/' + app + '/' + rel)}")
    if r.returncode != 0:
        sys.exit(f"cannot read {rel} on the phone")
    path = os.path.join(mac_dir, app, copy_rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(r.stdout)
    return copy_rel


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["status", "pull", "push", "sync", "apps"])
    ap.add_argument("apps", nargs="*")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--delete", action="store_true", help="also propagate deletions")
    ap.add_argument("--prefer", choices=["mac", "phone"], help="how to resolve a conflict")
    ap.add_argument("--mac-dir", default=os.path.expanduser("~/PhoneFiles"))
    ap.add_argument("--port", type=int, default=2222)
    a = ap.parse_args()

    ensure_tunnel(a.port)
    if a.command == "apps":
        print("on the phone:", " ".join(remote_apps(a.port)) or "(none)")
        return
    os.makedirs(a.mac_dir, exist_ok=True)
    mpath = os.path.join(a.mac_dir, ".phonefiles.json")
    manifest = json.load(open(mpath)) if os.path.exists(mpath) else {}

    local_apps = sorted(d for d in os.listdir(a.mac_dir) if os.path.isdir(os.path.join(a.mac_dir, d)) and not d.startswith("."))
    apps = a.apps or sorted(set(remote_apps(a.port)) | set(local_apps))
    if not apps:
        print("no app folders yet. Open an app on the phone and create something, or make ~/PhoneFiles/APP.")
        return

    dry = a.dry_run or a.command == "status"
    do_pull = a.command in ("pull", "sync")
    do_push = a.command in ("push", "sync")
    totals = {"push": 0, "pull": 0, "conflict": 0, "del": 0}

    for app in apps:
        L, R = local_files(a.mac_dir, app), remote_files(a.port, app)
        base = manifest.setdefault(app, {})
        actions = plan(app, L, R, base)
        pushes = [r for r, act in actions if act == "push" and (do_push or a.command == "status")]
        pulls = [r for r, act in actions if act == "pull" and (do_pull or a.command == "status")]
        conflicts = [r for r, act in actions if act == "conflict"]
        dels_remote = [r for r, act in actions if act == "del-remote" and (do_push or a.command == "status")]
        dels_local = [r for r, act in actions if act == "del-local" and (do_pull or a.command == "status")]
        if not (pushes or pulls or conflicts or dels_remote or dels_local):
            print(f"{app}: up to date ({len(L)} on the Mac, {len(R)} on the phone)")
            continue
        print(f"{app}:")
        for r in pulls: print(f"  phone -> Mac   {r}")
        for r in pushes: print(f"  Mac -> phone   {r}")
        for r in conflicts: print(f"  CONFLICT       {r}  (changed on both sides)")
        for r in dels_remote: print(f"  deleted on the Mac, still on the phone: {r}" + ("" if a.delete else "  (use --delete)"))
        for r in dels_local: print(f"  deleted on the phone, still on the Mac:  {r}" + ("" if a.delete else "  (use --delete)"))
        if dry:
            continue

        if a.prefer:                                    # resolve conflicts by taking one side
            (pushes if a.prefer == "mac" else pulls).extend(conflicts)
            conflicts = []
        if do_pull and pulls: pull_files(a.port, a.mac_dir, app, pulls)
        if do_push and pushes: push_files(a.port, a.mac_dir, app, pushes)
        if a.delete:
            for r in dels_remote:
                ssh(a.port, f"rm -f {shlex.quote(REMOTE_ROOT + '/' + app + '/' + r)}")
            for r in dels_local:
                try: os.remove(os.path.join(a.mac_dir, app, r))
                except OSError: pass
        for r in conflicts:
            if do_pull:
                print(f"  saved the phone version as: {pull_as_copy(a.port, a.mac_dir, app, r)}")

        # record the new state of every file that is now identical on both sides
        L, R = local_files(a.mac_dir, app), remote_files(a.port, app)
        for rel in set(L) | set(R):
            if same(L.get(rel), R.get(rel)):
                base[rel] = {"l": list(L[rel]), "r": list(R[rel])}
            elif rel in base and rel not in L and rel not in R:
                base.pop(rel, None)
        for rel in [k for k in base if k not in L and k not in R]:
            base.pop(rel, None)
        totals["push"] += len(pushes); totals["pull"] += len(pulls); totals["conflict"] += len(conflicts)
        totals["del"] += (len(dels_remote) + len(dels_local)) if a.delete else 0

    if not dry:
        json.dump(manifest, open(mpath, "w"), indent=1, sort_keys=True)
        print(f"done: {totals['pull']} pulled, {totals['push']} pushed, {totals['conflict']} conflict(s)"
              + (f", {totals['del']} deleted" if totals["del"] else ""))


if __name__ == "__main__":
    main()
