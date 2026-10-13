#!/usr/bin/env python3
"""Phone Files: a window on the Mac to move files to and from the phones. iPhones go over SSH, Android phones over adb.

usage: phonefiles_gui.py [--app] [--port N]
  --app   open the window (Chrome app window if Chrome is installed, else the default browser) and quit when it closes.
Needs: python3, iproxy + idevice_id (libimobiledevice) for iPhones, adb for Android. Nothing else to install.

The page and its API listen on 127.0.0.1 only. Every API call needs a random token that only the page knows, and the
Host header must match, so other web pages on this Mac cannot use it.
"""
import argparse, http.server, io, json, os, re, secrets, shutil, socket, socketserver, subprocess, sys, tarfile
import posixpath, tempfile, threading, time, urllib.parse, webbrowser, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fbtools_manager as fbm
import longhouse_login as lhl
TOKEN = secrets.token_urlsafe(18)
LAST_PING = time.time()
FBLS_CANDIDATES = [os.path.join(HERE, "fbls_android"), os.path.join(HERE, "..", "build", "fbls_android")]
ANDROID_TMP = "/data/local/tmp"


def q(s):
    """Quote for a POSIX shell."""
    return "'" + s.replace("'", "'\\''") + "'"


def run(cmd, **kw):
    kw.setdefault("stdout", subprocess.PIPE)
    kw.setdefault("stderr", subprocess.PIPE)
    return subprocess.run(cmd, **kw)


def parse_listing(text):
    out = []
    for line in text.splitlines():
        p = line.rstrip("\r").split("\t", 3)
        if len(p) != 4:
            continue
        try:
            out.append({"type": p[0], "size": int(float(p[1])), "mtime": int(float(p[2])), "name": p[3]})
        except ValueError:
            pass
    out.sort(key=lambda e: (e["type"] != "d", e["name"].lower()))
    return out


def safe_to_delete(path):
    parts = [x for x in path.split("/") if x]
    return len(parts) >= 2 and ".." not in parts


# Protection: this app only changes the user's own files. Delete, rename and overwrite work only INSIDE these folders
# (not on the folders themselves). System files, the programs and settings stay locked. Download and browse always work.
# Set PHONEFILES_EXPERT=1 in the environment (start the app from a terminal) to lift the lock.
EXPERT = os.environ.get("PHONEFILES_EXPERT") == "1"
PROTECTED_MSG = "Protected: this is a system or app file. Phone Files only changes your own files (notes, music, carts and app data)."


def in_zone(zones, path):
    if EXPERT:
        return True
    if ".." in path.split("/"):
        return False
    p = posixpath.normpath("/" + path.lstrip("/"))      # a leading "//" would survive normpath
    return any(p.startswith(z.rstrip("/") + "/") for z in zones)


class PhoneError(Exception):
    pass


# ---------------------------------------------------------------- iPhone over SSH

class IOS:
    kind = "ios"
    places = [("Home", "/var/root"), ("Notes", "/var/root/data/notes"), ("Music", "/var/root/music"),
              ("Books", "/var/root/data/books"), ("Paint pictures", "/var/root/data/paint"), ("Pixel art", "/var/root/data/pixel"),
              ("Carts", "/var/root/carts"), ("All app data", "/var/root/data")]
    zones = ["/var/root/data", "/var/root/music", "/var/root/carts"]

    def can_change(self, path):
        return in_zone(self.zones, path)

    def guard(self, path):
        if not self.can_change(path): raise PhoneError(PROTECTED_MSG)

    def exists(self, path):
        return self.sh("[ -e %s ] && echo y || echo n" % q(path), timeout=15).stdout.decode().strip() == "y"

    def __init__(self, udid, port):
        self.udid, self.port = udid, port
        self.id = "ios:" + udid
        self.proc = None
        self.name = self.detail = self.version = ""

    def info(self):
        def get(k):
            r = run(["ideviceinfo", "-u", self.udid, "-k", k], timeout=8)
            return r.stdout.decode().strip() if r.returncode == 0 else ""
        self.name = get("DeviceName") or "iPhone"
        self.version = get("ProductVersion")
        self.detail = " ".join(x for x in (get("ProductType"), "iOS " + self.version) if x.strip() != "iOS")

    def tunnel(self):
        try:
            socket.create_connection(("127.0.0.1", self.port), 0.5).close()
            return
        except OSError:
            pass
        self.proc = subprocess.Popen(["iproxy", "-u", self.udid, "%d:22" % self.port], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(30):
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.3).close()
                return
            except OSError:
                time.sleep(0.1)

    def ssh_cmd(self, cmd):
        return ["ssh", "-p", str(self.port), "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", "-o", "LogLevel=ERROR",
                "-o", "HostKeyAlgorithms=+ssh-rsa", "-o", "PubkeyAcceptedAlgorithms=+ssh-rsa", "root@127.0.0.1", cmd]

    def sh(self, cmd, **kw):
        self.tunnel()
        r = run(self.ssh_cmd(cmd), **kw)
        return r

    def status(self):
        try:
            return "ready" if self.sh("true", timeout=10).returncode == 0 else "no SSH (jailbreak and OpenSSH needed)"
        except Exception:
            return "no SSH (jailbreak and OpenSSH needed)"

    def ls(self, path):
        r = self.sh("find -L %s -mindepth 1 -maxdepth 1 -printf '%%y\\t%%s\\t%%T@\\t%%f\\n'" % q(path), timeout=30)
        if r.returncode != 0:
            raise PhoneError(r.stderr.decode(errors="replace").strip() or "cannot list this folder")
        return parse_listing(r.stdout.decode(errors="replace"))

    def stat_type(self, path):
        r = self.sh("[ -d %s ] && echo d || echo f" % q(path), timeout=15)
        return r.stdout.decode().strip()

    def read_stream(self, path):
        """Return (iterator of chunks, filename, size or None)."""
        self.tunnel()
        if self.stat_type(path) == "d":
            parent, name = os.path.split(path.rstrip("/"))
            p = subprocess.Popen(self.ssh_cmd("tar -C %s -cf - %s" % (q(parent or "/"), q(name))), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            fname = name + ".tar"
        else:
            p = subprocess.Popen(self.ssh_cmd("cat %s" % q(path)), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            fname = os.path.basename(path)

        def gen():
            try:
                while True:
                    b = p.stdout.read(65536)
                    if not b:
                        break
                    yield b
            finally:
                p.stdout.close(); p.wait()
        return gen(), fname, None

    def write(self, path, reader, length):
        if not self.can_change(path) and self.exists(path): raise PhoneError(PROTECTED_MSG)   # no overwriting protected files
        self.tunnel()
        parent = os.path.dirname(path)
        p = subprocess.Popen(self.ssh_cmd("mkdir -p %s && cat > %s" % (q(parent), q(path))), stdin=subprocess.PIPE, stderr=subprocess.PIPE)
        left = length
        try:
            while left > 0:
                b = reader.read(min(65536, left))
                if not b:
                    break
                p.stdin.write(b); left -= len(b)
            p.stdin.close()
        except BrokenPipeError:
            pass
        err = p.stderr.read().decode(errors="replace"); p.wait()
        if p.returncode != 0 or left > 0:
            raise PhoneError(err.strip() or "upload failed")

    def mkdir(self, path):
        r = self.sh("mkdir -p %s" % q(path), timeout=20)
        if r.returncode: raise PhoneError(r.stderr.decode(errors="replace"))

    # --- used by the FB Tools manager (not limited by the protection rules: it only touches its own files)
    def rmf(self, path):
        return "rm -rf %s" % q(path)

    def put_file(self, local, remote, mode=0o755):
        self.tunnel()
        with open(local, "rb") as f:
            r = subprocess.run(self.ssh_cmd("mkdir -p %s && cat > %s && chmod %o %s" % (q(posixpath.dirname(remote)), q(remote), mode, q(remote))),
                               stdin=f, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600)
        if r.returncode: raise PhoneError("copy to %s failed: %s" % (remote, r.stderr.decode(errors="replace").strip()))

    def put_text(self, remote, text, mode=0o644):
        tmp = tempfile.NamedTemporaryFile(delete=False); tmp.write(text.encode()); tmp.close()
        try: self.put_file(tmp.name, remote, mode)
        finally: os.unlink(tmp.name)

    def get_text(self, remote):
        r = self.sh("cat %s" % q(remote), timeout=20)
        return r.stdout.decode(errors="replace") if r.returncode == 0 else None

    def put_tree(self, local_dir, remote_parent):
        """Copy a folder (such as FBTools.app) to remote_parent/<folder name>, with tar over SSH."""
        self.tunnel()
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as t:
            t.add(local_dir, arcname=os.path.basename(local_dir.rstrip("/")), filter=lambda ti: (setattr(ti, "uid", 0), setattr(ti, "gid", 0), ti)[2])
        r = subprocess.run(self.ssh_cmd("mkdir -p %s && tar -C %s -xf -" % (q(remote_parent), q(remote_parent))), input=buf.getvalue(),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
        if r.returncode: raise PhoneError("copy of %s failed: %s" % (local_dir, r.stderr.decode(errors="replace").strip()))

    def rm(self, path):
        if not safe_to_delete(path): raise PhoneError("refusing to delete this path")
        self.guard(path)
        r = self.sh("rm -rf %s" % q(path), timeout=60)
        if r.returncode: raise PhoneError(r.stderr.decode(errors="replace"))

    def mv(self, a, b):
        self.guard(a); self.guard(b)
        # a move never replaces something: the phone's own mv would do it without a word
        r = self.sh("if [ -e %s ]; then echo 'A file or folder with that name is already there.' >&2; exit 3; fi; mv %s %s" % (q(b), q(a), q(b)), timeout=30)
        if r.returncode: raise PhoneError(r.stderr.decode(errors="replace").strip() or "move failed")


# ---------------------------------------------------------------- Android over adb

class Android:
    kind = "android"
    places = [("App files", ANDROID_TMP), ("Notes", ANDROID_TMP + "/data/notes"), ("Music", ANDROID_TMP + "/music"),
              ("Books", ANDROID_TMP + "/data/books"), ("Paint pictures", ANDROID_TMP + "/data/paint"), ("Pixel art", ANDROID_TMP + "/data/pixel"),
              ("Carts", ANDROID_TMP + "/carts"), ("Internal storage", "/sdcard")]
    zones = [ANDROID_TMP + "/data", ANDROID_TMP + "/music", ANDROID_TMP + "/carts", "/sdcard"]

    def can_change(self, path):
        return in_zone(self.zones, path)

    def guard(self, path):
        if not self.can_change(path): raise PhoneError(PROTECTED_MSG)

    def exists(self, path):
        return self.sh("ls %s >/dev/null 2>&1 && echo y || echo n" % q(path), timeout=15).stdout.decode().strip().endswith("y")

    def __init__(self, serial):
        self.serial = serial
        self.id = "android:" + serial
        self.name = self.detail = self.version = ""
        self.root = None
        self.adbroot = False
        self.fbls_ok = False

    def adb(self, *args, **kw):
        return run(["adb", "-s", self.serial] + list(args), **kw)

    def info(self):
        def prop(k):
            r = self.adb("shell", "getprop " + k, timeout=8)
            return r.stdout.decode().strip()
        self.version = prop("ro.build.version.release")
        self.name = prop("ro.product.model") or "Android"
        self.detail = "Android " + prop("ro.build.version.release")
        self.adbroot = False                                          # adbd itself runs as root (a userdebug ROM such as LineageOS): no su is needed
        suroot = "uid=0" in self.adb("shell", "su -c id", timeout=10).stdout.decode()
        if not suroot:
            if "uid=0" in self.adb("shell", "id", timeout=10).stdout.decode(): self.adbroot = True
            elif prop("ro.debuggable") == "1":                         # ask adbd to restart as root, then wait for the phone to come back
                self.adb("root", timeout=15)
                run(["adb", "-s", self.serial, "wait-for-device"], timeout=30)
                time.sleep(1.5)
                self.adbroot = "uid=0" in self.adb("shell", "id", timeout=10).stdout.decode()
        self.root = suroot or self.adbroot

    def ensure_root(self):
        """A phone that is rooted only through `adb root` goes back to a normal adb shell after every restart. Ask for root again."""
        if not self.adbroot: return
        if "uid=0" in self.adb("shell", "id", timeout=10).stdout.decode(): return
        self.adb("root", timeout=15)
        run(["adb", "-s", self.serial, "wait-for-device"], timeout=30)
        time.sleep(1.5)

    def sh(self, cmd, **kw):
        wrapped = "su -c " + q(cmd) if (self.root and not self.adbroot) else cmd
        return self.adb("shell", wrapped, **kw)

    def status(self):
        return "ready" if self.root else "no root: only /sdcard and the app folder work"

    def ensure_fbls(self):
        if self.fbls_ok:
            return
        if b"fbls" not in self.adb("shell", "ls %s/fbls" % ANDROID_TMP, timeout=10).stdout:
            src = next((p for p in FBLS_CANDIDATES if os.path.exists(p)), None)
            if not src: raise PhoneError("fbls_android not found (build it first)")
            self.adb("push", src, ANDROID_TMP + "/fbls", timeout=30)
            self.adb("shell", "chmod 755 %s/fbls" % ANDROID_TMP, timeout=10)
        self.fbls_ok = True

    def ls(self, path):
        self.ensure_fbls()
        r = self.sh("%s/fbls %s" % (ANDROID_TMP, q(path)), timeout=30)
        text = r.stdout.decode(errors="replace")
        if r.returncode != 0 and not text.strip():
            raise PhoneError("cannot list this folder (does it exist?)")
        return parse_listing(text)

    def is_dir(self, path):
        r = self.sh("cd %s && echo d || echo f" % q(path), timeout=15)      # "cd" works in every shell; Android 2.3 has no "[" or "test"
        return r.stdout.decode().strip().endswith("d")

    def pull_file(self, path, local):
        stage = ANDROID_TMP + "/.pf_stage"
        r = self.sh("cat %s > %s; chmod 666 %s" % (q(path), stage, stage), timeout=300)
        r = self.adb("pull", stage, local, timeout=1800)
        if r.returncode != 0 or not os.path.exists(local):
            raise PhoneError(r.stderr.decode(errors="replace") or "download failed")
        self.sh("rm %s" % stage, timeout=15)

    def walk(self, path, base=""):
        for e in self.ls(path):
            rel = base + e["name"]
            if e["type"] == "d":
                yield from self.walk(path.rstrip("/") + "/" + e["name"], rel + "/")
            else:
                yield rel, path.rstrip("/") + "/" + e["name"]

    def read_stream(self, path):
        tmpdir = tempfile.mkdtemp(prefix="pf_")
        if self.is_dir(path):
            name = os.path.basename(path.rstrip("/")) or "phone"
            zpath = os.path.join(tmpdir, name + ".zip")
            n = 0
            with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
                for rel, remote in self.walk(path):
                    n += 1
                    if n > 2000: raise PhoneError("folder has too many files")
                    local = os.path.join(tmpdir, "f%d" % n)
                    self.pull_file(remote, local)
                    z.write(local, name + "/" + rel); os.remove(local)
            fname, local = name + ".zip", zpath
        else:
            fname = os.path.basename(path)
            local = os.path.join(tmpdir, "f")
            self.pull_file(path, local)
        size = os.path.getsize(local)

        def gen():
            try:
                with open(local, "rb") as f:
                    while True:
                        b = f.read(65536)
                        if not b: break
                        yield b
            finally:
                shutil.rmtree(tmpdir, ignore_errors=True)
        return gen(), fname, size

    # --- used by the FB Tools manager (not limited by the protection rules: it only touches its own files)
    def rmf(self, path):
        return "rm -r %s" % q(path)                                 # an error for a missing file is harmless; the old shell has no "[" or "rm -f"

    def put_file(self, local, remote, mode=0o755):
        stage = ANDROID_TMP + "/.pf_up"
        r = self.adb("push", local, stage, timeout=900)
        if r.returncode: raise PhoneError("push of %s failed: %s" % (local, r.stderr.decode(errors="replace").strip()))
        self.mkdir(posixpath.dirname(remote))
        self.sh("cat %s > %s; chmod %o %s; rm %s" % (stage, q(remote), mode, q(remote), stage), timeout=120)

    def put_text(self, remote, text, mode=0o644):
        tmp = tempfile.NamedTemporaryFile(delete=False); tmp.write(text.encode()); tmp.close()
        try: self.put_file(tmp.name, remote, mode)
        finally: os.unlink(tmp.name)

    def get_text(self, remote):
        r = self.sh("cat %s" % q(remote), timeout=20)
        t = r.stdout.decode(errors="replace").replace("\r\n", "\n")
        return t if r.returncode == 0 and "No such file" not in t else None

    def mkdir(self, path):
        parts = [x for x in path.split("/") if x]
        cur, cmds = "", []
        for p in parts:
            cur += "/" + p
            cmds.append("mkdir %s" % q(cur))                         # errors for folders that exist are harmless
        self.sh("; ".join(cmds), timeout=30)
        if not self.is_dir(path): raise PhoneError("cannot create the folder")

    def write(self, path, reader, length):
        if not self.can_change(path) and self.exists(path): raise PhoneError(PROTECTED_MSG)   # no overwriting protected files
        tmpdir = tempfile.mkdtemp(prefix="pf_")
        try:
            local = os.path.join(tmpdir, "up")
            left = length
            with open(local, "wb") as f:
                while left > 0:
                    b = reader.read(min(65536, left))
                    if not b: break
                    f.write(b); left -= len(b)
            if left > 0: raise PhoneError("upload was cut short")
            self.mkdir(os.path.dirname(path))
            stage = ANDROID_TMP + "/.pf_up"
            r = self.adb("push", local, stage, timeout=1800)
            if r.returncode != 0: raise PhoneError(r.stderr.decode(errors="replace") or "push failed")
            self.sh("cat %s > %s; chmod 666 %s; rm %s" % (stage, q(path), q(path), stage), timeout=300)
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

    def rm(self, path):
        if not safe_to_delete(path): raise PhoneError("refusing to delete this path")
        self.guard(path)
        flag = "-r " if self.is_dir(path) else ""
        self.sh("rm %s%s" % (flag, q(path)), timeout=60)

    def mv(self, a, b):
        self.guard(a); self.guard(b)
        # a move never replaces something: the phone's own mv would do it without a word
        r = self.sh("if [ -e %s ]; then echo 'A file or folder with that name is already there.' >&2; exit 3; fi; mv %s %s" % (q(b), q(a), q(b)), timeout=30)
        if r.returncode: raise PhoneError(r.stderr.decode(errors="replace").strip() or "move failed")


# ---------------------------------------------------------------- device list

DEVICES = {}
DEV_LOCK = threading.Lock()


def refresh_devices():
    found = {}
    try:
        for udid in run(["idevice_id", "-l"], timeout=10).stdout.decode().split():
            key = "ios:" + udid
            d = DEVICES.get(key) or IOS(udid, 2250 + len(DEVICES) + len(found))
            if not d.name: d.info()
            found[key] = d
    except Exception:
        pass
    try:
        for line in run(["adb", "devices"], timeout=10).stdout.decode().splitlines()[1:]:
            p = line.split()
            if len(p) >= 2 and p[1] == "device":
                key = "android:" + p[0]
                d = DEVICES.get(key) or Android(p[0])
                if not d.name: d.info()
                found[key] = d
    except Exception:
        pass
    with DEV_LOCK:
        DEVICES.clear(); DEVICES.update(found)
    return list(found.values())


APP_SUPPORT = os.path.expanduser("~/Library/Application Support/PhoneFiles")


def lh_saved_home():
    try:
        return json.load(open(os.path.join(APP_SUPPORT, "longhouse.json"))).get("home", "")
    except (OSError, ValueError):
        return ""


def lh_save_home(home):
    os.makedirs(APP_SUPPORT, mode=0o700, exist_ok=True)
    with open(os.path.join(APP_SUPPORT, "longhouse.json"), "w") as f: json.dump({"home": home}, f)


def longhouse_status(dev):
    """What the phone has: never returns the token or the refresh secret."""
    text = dev.get_text(lhl.device_folder(dev) + "/session.json")
    out = {"signedIn": False, "savedHome": lh_saved_home()}
    try:
        j = json.loads(text) if text else None
    except ValueError:
        j = None
    if j and j.get("token"):
        out.update({"signedIn": True, "displayName": j.get("displayName", ""), "home": j.get("home", ""), "canRenew": bool(j.get("refreshSecret"))})
    return out


def get_device(dev_id):
    d = DEVICES.get(dev_id)
    if not d:
        refresh_devices(); d = DEVICES.get(dev_id)
    if not d: raise PhoneError("device not found. Is it plugged in?")
    return d


# ---------------------------------------------------------------- web server

class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "PhoneFiles"
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def reply(self, code, body, ctype="application/json"):
        if not isinstance(body, bytes):
            body = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def authorized(self, qs):
        host = self.headers.get("Host", "")
        return host.startswith("127.0.0.1:") and (qs.get("t", [""])[0] == TOKEN or self.headers.get("X-Token") == TOKEN)

    def do_GET(self):
        global LAST_PING
        u = urllib.parse.urlparse(self.path); qs = urllib.parse.parse_qs(u.query)
        if u.path == "/favicon.ico":
            self.send_response(204); self.send_header("Content-Length", "0"); self.end_headers(); return
        if u.path == "/":
            if not self.authorized(qs): return self.reply(403, b"forbidden", "text/plain")
            return self.reply(200, PAGE.replace("__TOKEN__", TOKEN).encode(), "text/html; charset=utf-8")
        if not u.path.startswith("/api/") or not self.authorized(qs):
            return self.reply(403, {"error": "forbidden"})
        try:
            if u.path == "/api/ping":
                LAST_PING = time.time(); return self.reply(200, {"ok": True})
            if u.path == "/api/devices":
                devs = refresh_devices()
                out = [None] * len(devs)
                def st(i, d): out[i] = {"id": d.id, "kind": d.kind, "name": d.name, "detail": d.detail, "status": d.status(),
                                        "places": [{"name": n, "path": p} for n, p in d.places]}
                ts = [threading.Thread(target=st, args=(i, d)) for i, d in enumerate(devs)]
                [t.start() for t in ts]; [t.join() for t in ts]
                return self.reply(200, out)
            if u.path == "/api/job":
                j = fbm.JOBS.get(qs.get("id", [""])[0])
                if not j: return self.reply(404, {"error": "unknown job"})
                return self.reply(200, {"title": j.title, "lines": j.lines, "done": j.done, "ok": j.ok})
            d = get_device(qs.get("dev", [""])[0])
            path = qs.get("path", ["/"])[0]
            if u.path == "/api/longhouse/status":
                return self.reply(200, longhouse_status(d))
            if u.path == "/api/fbtools/status":
                return self.reply(200, fbm.status(d))
            if u.path == "/api/ls":
                ents = d.ls(path)
                for e in ents:
                    e["protected"] = not d.can_change(path.rstrip("/") + "/" + e["name"])
                return self.reply(200, {"path": path, "entries": ents})
            if u.path == "/api/download":
                gen, fname, size = d.read_stream(path)
                self.send_response(200)
                kinds = {".bmp": "image/bmp", ".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".gif": "image/gif"}
                ctype = kinds.get(os.path.splitext(fname)[1].lower()) if qs.get("inline", [""])[0] else None       # inline: show a picture in the page
                self.send_header("Content-Type", ctype or "application/octet-stream")
                self.send_header("Content-Disposition", ("inline" if ctype else "attachment") + "; filename*=UTF-8''" + urllib.parse.quote(fname))
                if size is not None: self.send_header("Content-Length", str(size))
                self.end_headers()
                for b in gen: self.wfile.write(b)
                return
            self.reply(404, {"error": "unknown call"})
        except PhoneError as e:
            self.reply(400, {"error": str(e)})
        except subprocess.TimeoutExpired:
            self.reply(504, {"error": "the phone did not answer in time"})
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:
            self.reply(500, {"error": "%s: %s" % (type(e).__name__, e)})

    def do_POST(self):
        u = urllib.parse.urlparse(self.path); qs = urllib.parse.parse_qs(u.query)
        if not u.path.startswith("/api/") or not self.authorized(qs):
            return self.reply(403, {"error": "forbidden"})
        try:
            d = get_device(qs.get("dev", [""])[0])
            length = int(self.headers.get("Content-Length", "0"))
            if u.path == "/api/upload":
                dest = qs.get("path", ["/"])[0].rstrip("/") + "/" + qs.get("name", [""])[0].lstrip("/")
                if ".." in dest.split("/"): raise PhoneError("bad name")
                d.write(dest, self.rfile, length)
                return self.reply(200, {"ok": True})
            body = json.loads(self.rfile.read(length) or b"{}")
            if u.path == "/api/longhouse/signin":
                try:
                    session, names = lhl.sign_in(str(body.get("invite", "")), str(body.get("home", "")).strip())
                    lhl.push_to_device(d, session, names)
                except ValueError as e:
                    raise PhoneError(str(e))
                lh_save_home(session["home"])
                return self.reply(200, {"ok": True, "displayName": session.get("displayName"), "people": len(names)})
            if u.path == "/api/longhouse/signout":
                folder = lhl.device_folder(d)
                d.sh(d.rmf(folder + "/session.json") + "; " + d.rmf(folder + "/directory.json"), timeout=20)
                return self.reply(200, {"ok": True})
            if u.path.startswith("/api/fbtools/"):
                return self.reply(200, {"job": self.fbtools_action(d, u.path.rsplit("/", 1)[1], body)})
            if u.path == "/api/mkdir": d.mkdir(body["path"])
            elif u.path == "/api/delete": d.rm(body["path"])
            elif u.path == "/api/rename": d.mv(body["from"], body["to"])
            else: return self.reply(404, {"error": "unknown call"})
            self.reply(200, {"ok": True})
        except (PhoneError, fbm.FBToolsError) as e:
            self.reply(400, {"error": str(e)})
        except subprocess.TimeoutExpired:
            self.reply(504, {"error": "the phone did not answer in time"})
        except Exception as e:
            self.reply(500, {"error": "%s: %s" % (type(e).__name__, e)})


def fbtools_start(dev, action, body):
    """Start an FB Tools job on the phone and return its id."""
    plat = fbm.platform_of(dev)
    if not plat: raise PhoneError("This phone type is not supported by FB Tools.")
    if action == "install":
        mode, apps = body.get("mode", "home"), body.get("apps", [])
        return fbm.start_job(dev, "Install FB Tools", lambda log: fbm.do_install(dev, plat, mode, apps, log)).id
    if action == "uninstall":
        rd = bool(body.get("remove_data"))
        return fbm.start_job(dev, "Uninstall FB Tools", lambda log: fbm.do_uninstall(dev, plat, rd, log)).id
    if action == "repair":
        force = bool(body.get("force"))
        return fbm.start_job(dev, "Repair FB Tools", lambda log: fbm.do_repair(dev, plat, log, force)).id
    if action == "mode":
        mode = body.get("mode")
        return fbm.start_job(dev, "Change start mode", lambda log: fbm.do_set_mode(dev, plat, mode, log)).id
    if action == "app":
        aid, act = body.get("id"), body.get("action")
        if act not in ("add", "remove"): raise PhoneError("bad action")
        return fbm.start_job(dev, "%s app" % act.capitalize(), lambda log: fbm.do_app(dev, plat, aid, act, log)).id
    if action == "start":
        return fbm.start_job(dev, "Start FB Tools", lambda log: fbm.do_start(dev, plat, log)).id
    if action == "android":
        return fbm.start_job(dev, "Back to Android", lambda log: fbm.do_to_android(dev, plat, log)).id
    if action == "clock":
        return fbm.start_job(dev, "Set the clock", lambda log: fbm.do_set_clock(dev, plat, log)).id
    raise PhoneError("unknown FB Tools action")


Handler.fbtools_action = lambda self, d, action, body: fbtools_start(d, action, body)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


PAGE = open(os.path.join(HERE, "phonefiles_gui.html"), encoding="utf-8").read() if os.path.exists(os.path.join(HERE, "phonefiles_gui.html")) else "page missing"


def open_window(url):
    chrome = "/Applications/Google Chrome.app"
    if os.path.exists(chrome):
        subprocess.Popen(["open", "-na", "Google Chrome", "--args", "--app=" + url, "--window-size=1100,720"])
    else:
        webbrowser.open(url)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", action="store_true", help="open a window and quit when it closes")
    ap.add_argument("--port", type=int, default=0)
    a = ap.parse_args()
    srv = Server(("127.0.0.1", a.port), Handler)
    port = srv.server_address[1]
    url = "http://127.0.0.1:%d/?t=%s" % (port, TOKEN)
    print("Phone Files: " + url, flush=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    if a.app:
        open_window(url)
        t0 = time.time()
        while True:                                   # quit when the window has been closed for 40 s
            time.sleep(2)
            if time.time() - LAST_PING > 40 and time.time() - t0 > 40:
                break
        for d in list(DEVICES.values()):
            if getattr(d, "proc", None): d.proc.terminate()
    else:
        try:
            while True: time.sleep(3600)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
