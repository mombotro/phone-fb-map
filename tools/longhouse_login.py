#!/usr/bin/env python3
"""Sign in to Longhouse from the Mac and put the session on a phone, for the Longhouse chat app in FB Tools.

usage: longhouse_login.py [--invite FILE] [--home http://HOME-NODE-IP:8787] [--device ID_PART] [--no-push]

What it does:
  1. Reads a person invite link from a file (default: research/longhouse_invite.txt, which git ignores).
  2. Signs in at the signaling server (HTTPS) and receives a session token that lasts 24 hours, plus a refresh secret.
  3. Fetches the roster (person names) from the signaling server. A person seat may read /roster, not /directory.
  4. Writes two files, session.json and directory.json, to <phone>/data/longhouse/, using the same phone code as Phone Files.
The home-node address comes from --home, then $LONGHOUSE_HOME, then the file research/longhouse_home.txt (saved after the first
time, so you type it once). The phone app talks to the home-node over plain HTTP on your home network, so it never needs HTTPS.
If the token expires, or the phone says so, run this again (it signs in again with the invite).
The invite and the session are secrets. They stay in research/ (ignored by git) and on the phone. Nothing here prints them.
"""
import argparse, json, os, sys, tempfile, time, urllib.error, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SIGNALING = "https://longhouse.tech/longhouse"
PRIVATE = os.path.join(ROOT, "research")


def post_json(url, body, headers=None):
    h = {"content-type": "application/json"}
    h.update(headers or {})
    req = urllib.request.Request(url, data=json.dumps(body).encode(), headers=h, method="POST")
    return json.load(urllib.request.urlopen(req, timeout=25))


def get_json(url, token):
    req = urllib.request.Request(url, headers={"authorization": "Bearer " + token})
    return json.load(urllib.request.urlopen(req, timeout=25))


def invite_key(text):
    """The 64-hex key from an invite link (or the bare key)."""
    text = text.strip()
    import re
    if re.fullmatch(r"[0-9a-f]{64}", text): return text
    key = urllib.parse.parse_qs(urllib.parse.urlparse(text).query).get("key", [None])[0]
    if not key or not re.fullmatch(r"[0-9a-f]{64}", key):
        raise ValueError("that is not a Longhouse invite link (it needs a key= part)")
    return key


def sign_in(invite_text, home):
    """Sign in with an invite. Returns (session dict, names dict). Raises ValueError with a plain message on failure."""
    key = invite_key(invite_text)
    if not home.startswith("http://"): raise ValueError("the home-node address must look like http://192.168.1.20:8787")
    try:
        body = post_json(SIGNALING + "/join", {"key": key}, {"x-longhouse-refresh": "1"})
    except urllib.error.HTTPError as e:
        raise ValueError("Longhouse refused the invite (%d). Was it revoked or replaced?" % e.code)
    except (urllib.error.URLError, OSError) as e:
        raise ValueError("cannot reach Longhouse: %s" % e)
    seat = body.get("seat", {})
    if seat.get("role") != "person": raise ValueError("the invite is not a person seat")
    try:
        roster = get_json(SIGNALING + "/roster", body["token"])
    except (urllib.error.URLError, OSError):
        roster = {"persons": []}
    names = {p["id"]: p.get("displayName") or p["id"][:8] for p in roster.get("persons", []) if "id" in p}
    import socket
    try: sig_ip = socket.gethostbyname(urllib.parse.urlparse(SIGNALING).hostname)      # the phone cannot look names up, so it gets the number
    except OSError: sig_ip = ""
    session = {"home": home, "signaling": SIGNALING, "signalingIp": sig_ip, "token": body["token"], "refreshSecret": body.get("refreshSecret", ""),
               "personId": seat.get("personId"), "homeId": seat.get("homeId"), "displayName": seat.get("displayName"),
               "signedInAt": int(time.time()), "validSeconds": 86400}
    return session, names


def device_folder(dev):
    return ("/var/root" if dev.kind == "ios" else "/data/local/tmp") + "/data/longhouse"


def push_to_device(dev, session, names):
    """Put session.json (readable by root only) and directory.json on a phone, through temporary files that are deleted at once."""
    folder = device_folder(dev)
    dev.mkdir(folder)
    for name, data in (("session.json", session), ("directory.json", {"names": names})):
        fd, tmp = tempfile.mkstemp(suffix=".json"); os.chmod(tmp, 0o600)
        try:
            with os.fdopen(fd, "w") as f: json.dump(data, f)
            dev.put_file(tmp, folder + "/" + name, 0o600)
        finally:
            try: os.unlink(tmp)
            except OSError: pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--invite", default=os.path.join(PRIVATE, "longhouse_invite.txt"))
    ap.add_argument("--home", default="", help="the home-node address, for example http://192.168.1.20:8787")
    ap.add_argument("--device", default="", help="part of the device id (see Phone Files); default: every connected phone")
    ap.add_argument("--no-push", action="store_true", help="only write the files in research/")
    a = ap.parse_args()
    home_file = os.path.join(PRIVATE, "longhouse_home.txt")
    if not a.home: a.home = os.environ.get("LONGHOUSE_HOME", "")
    if not a.home and os.path.exists(home_file): a.home = open(home_file).read().strip()
    os.makedirs(PRIVATE, exist_ok=True)
    try:
        session, names = sign_in(open(a.invite).read(), a.home)
    except (ValueError, OSError) as e:
        sys.exit("sign-in failed: %s" % e)
    with open(home_file, "w") as f: f.write(a.home + "\n")
    for name, data in (("session.json", session), ("directory.json", {"names": names})):
        fd = os.open(os.path.join(PRIVATE, "longhouse_" + name), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f: json.dump(data, f)
    print("signed in as %s. %d people in the directory. Files written in research/." % (session["displayName"], len(names)))
    if a.no_push: return
    sys.path.insert(0, HERE)
    import phonefiles_gui as pf
    devices = [d for d in pf.refresh_devices() if a.device in d.id]
    if not devices: sys.exit("no phone found. Plug one in, or use --no-push.")
    for d in devices:
        try:
            push_to_device(d, session, names)
            print("copied to %s (%s): %s" % (d.name, d.id.split(":")[0], device_folder(d)))
        except Exception as e:
            print("failed on %s: %s" % (d.name, e))


if __name__ == "__main__":
    main()
