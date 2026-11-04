#!/usr/bin/env python3
"""Game server for WiiCompiled ports: disc files and save copies for the Apple TV.

Protocol (see runtime/include/remote_store.h), every request carries X-Wii-Token:
  GET  /g/<game>/manifest        "<size>\t<path>\n" per disc file under the game's files/ folder
  GET  /g/<game>/data/<path>     a disc file, honouring "Range: bytes=a-b"
  GET  /g/<game>/sys-manifest    "<size>	<path>
" per file of the disc's sys/ folder
  GET  /g/<game>/sys/<path>      a sys/ file (a device whose local copy tvOS deleted fetches these)
  GET  /g/<game>/save-index      "<size>\t<fnv1a64>\t<path>\n" per save file
  GET  /g/<game>/save/<path>     a save file
  PUT  /g/<game>/save/<path>     store a save file; the previous version moves to history/

Config: games.json next to this script's server folder maps a game code to its folders:
  {"RMCP": {"data": "/path/to/DATA/files", "saves": "/path/to/saves/RMCP"}}
"""
import hmac, json, os, shutil, sys, threading, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOME = os.path.expanduser("~/wiicompiled-tvos/server")
PORT = 8730
SAVE_SCOPE = "title/00010004/"
# Scratch files that are never saves: uploads in progress and the game's NAND write shadows.
SCRATCH_SUFFIXES = (".upload-tmp", ".remote-tmp", ".nandsafe.tmp")
HISTORY_KEEP = 50
MAX_SAVE_BYTES = 64 << 20

TOKEN = open(os.path.join(HOME, "token")).read().strip()
GAMES = json.load(open(os.path.join(HOME, "games.json")))
_hash_cache = {}
_save_lock = threading.Lock()


def fnv1a64(path):
    key = (path, os.path.getsize(path), os.path.getmtime(path))
    if key in _hash_cache:
        return _hash_cache[key]
    h = 0xcbf29ce484222325
    with open(path, "rb") as f:
        for byte in f.read():
            h = ((h ^ byte) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    _hash_cache[key] = "%016x" % h
    return _hash_cache[key]


def sys_dir(game):
    # The disc's sys/ folder (fst.bin, boot.bin, ...): "sys" in games.json, else beside files/.
    return game.get("sys") or os.path.join(os.path.dirname(game["data"].rstrip("/")), "sys")


def safe_join(root, rel):
    parts = rel.split("/")
    if not rel or rel.startswith("/") or "\\" in rel or any(p in ("", ".", "..") for p in parts):
        return None
    full = os.path.realpath(os.path.join(root, *parts))
    return full if full.startswith(os.path.realpath(root) + os.sep) else None


def walk(root, skip_dirs=()):
    for base, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if os.path.relpath(os.path.join(base, d), root) not in skip_dirs)
        for name in sorted(files):
            if name.endswith(SCRATCH_SUFFIXES):
                continue
            full = os.path.join(base, name)
            yield os.path.relpath(full, root).replace(os.sep, "/"), full


class Handler(BaseHTTPRequestHandler):
    server_version = "WiiCompiledServer/1"

    def log_message(self, fmt, *args):
        # A malformed request line never sets self.path.
        if not getattr(self, "path", "").split("?")[0].split("/")[3:4] == ["data"]:  # disc reads are too chatty
            sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def reply(self, status, body=b"", headers=None):
        self.send_response(status)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def route(self):
        if not hmac.compare_digest(self.headers.get("X-Wii-Token", ""), TOKEN):
            self.reply(403, b"bad token\n")
            return None
        parts = urllib.parse.unquote(self.path.split("?")[0]).split("/", 4)
        if len(parts) < 4 or parts[1] != "g" or parts[2] not in GAMES:
            self.reply(404, b"unknown game\n")
            return None
        return GAMES[parts[2]], parts[3], parts[4] if len(parts) > 4 else ""

    def do_GET(self):
        routed = self.route()
        if not routed:
            return
        game, what, rel = routed
        if what == "manifest":
            lines = ["%d\t%s\n" % (os.path.getsize(full), rel) for rel, full in walk(game["data"])]
            self.reply(200, "".join(lines).encode())
        elif what == "data":
            self.send_data(game, rel)
        elif what == "sys-manifest":
            lines = ["%d\t%s\n" % (os.path.getsize(full), rel) for rel, full in walk(sys_dir(game))]
            self.reply(200, "".join(lines).encode())
        elif what == "sys":
            full = safe_join(sys_dir(game), rel)
            if not full or not os.path.isfile(full):
                self.reply(404, b"no such file\n")
            else:
                with open(full, "rb") as f:
                    self.reply(200, f.read())
        elif what == "save-index":
            os.makedirs(game["saves"], exist_ok=True)
            lines = ["%d\t%s\t%s\n" % (os.path.getsize(full), fnv1a64(full), rel)
                     for rel, full in walk(game["saves"], skip_dirs=("history",))]
            self.reply(200, "".join(lines).encode())
        elif what == "save":
            full = safe_join(game["saves"], rel)
            if not full or not rel.startswith(SAVE_SCOPE) or not os.path.isfile(full):
                self.reply(404, b"no such save\n")
            else:
                with open(full, "rb") as f:
                    self.reply(200, f.read())
        else:
            self.reply(404, b"unknown request\n")

    do_HEAD = do_GET

    def send_data(self, game, rel):
        full = safe_join(game["data"], rel)
        if not full or not os.path.isfile(full):
            self.reply(404, b"no such file\n")
            return
        size = os.path.getsize(full)
        start, end = 0, size - 1
        ranged = self.headers.get("Range", "")
        if ranged.startswith("bytes="):
            a, _, b = ranged[6:].partition("-")
            try:
                start = int(a) if a else max(0, size - int(b))
                end = min(int(b), size - 1) if a and b else size - 1
            except ValueError:
                self.reply(416, b"bad range\n")
                return
            if start > end or start >= size:
                self.reply(416, b"bad range\n", {"Content-Range": "bytes */%d" % size})
                return
        with open(full, "rb") as f:
            f.seek(start)
            body = f.read(end - start + 1)
        headers = {"Accept-Ranges": "bytes"}
        if ranged:
            headers["Content-Range"] = "bytes %d-%d/%d" % (start, end, size)
        self.reply(206 if ranged else 200, body, headers)

    def do_PUT(self):
        routed = self.route()
        if not routed:
            return
        game, what, rel = routed
        full = safe_join(game["saves"], rel) if what == "save" else None
        try:
            length = int(self.headers.get("Content-Length", "-1"))
        except ValueError:
            length = -1
        if (not full or not rel.startswith(SAVE_SCOPE) or rel.endswith(SCRATCH_SUFFIXES)
                or not 0 <= length <= MAX_SAVE_BYTES):
            self.reply(400, b"bad save upload\n")
            return
        data = self.rfile.read(length)
        if len(data) != length:
            self.reply(400, b"short upload\n")
            return
        with _save_lock:
            os.makedirs(os.path.dirname(full), exist_ok=True)
            if os.path.isfile(full):
                stamp = time.strftime("%Y-%m-%d_%H%M%S")
                keep = os.path.join(game["saves"], "history", stamp, *rel.split("/"))
                os.makedirs(os.path.dirname(keep), exist_ok=True)
                shutil.copy2(full, keep)
                history = os.path.join(game["saves"], "history")
                for old in sorted(os.listdir(history))[:-HISTORY_KEEP]:
                    shutil.rmtree(os.path.join(history, old), ignore_errors=True)
            tmp = full + ".upload-tmp"
            with open(tmp, "wb") as f:
                f.write(data)
            os.replace(tmp, full)
        self.reply(200, b"stored\n")


if __name__ == "__main__":
    ThreadingHTTPServer.daemon_threads = True
    print("WiiCompiled game server on port %d: %s" % (PORT, ", ".join(sorted(GAMES))), flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
