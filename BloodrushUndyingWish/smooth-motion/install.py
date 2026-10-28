"""Smooth Motion for Bloodrush: Undying Wish.

Makes movement render at your monitor's refresh rate instead of the 60 Hz physics rate:
  1. swaps BloodRush.exe for the official Godot 4.4.1 engine (downloaded from GitHub),
  2. sets override.cfg: ultrawide support, unlocked fps and physics interpolation,
  3. updates the script class list inside BloodRush.pck so the game's scripts load on Godot 4.4.

Run it again after a game update. To undo, see the Uninstall steps in README.md.
"""
import hashlib
import http.client
import os
import re
import shutil
import socket
import struct
import sys
import urllib.error
import urllib.request
import zipfile

MOD_DIR = os.path.dirname(os.path.abspath(__file__))
ENGINE_CACHE = os.path.join(MOD_DIR, "cache", "godot-4.4.1-windows-release.exe")

ENGINE_URL = "https://github.com/godotengine/godot/releases/download/4.4.1-stable/Godot_v4.4.1-stable_export_templates.tpz"
ENGINE_MEMBER = "templates/windows_release_x86_64.exe"
ENGINE_SHA256 = "b493c1d53fb915f7805f360cfe42d1c3126e720dfc234a4b0f2feb99200f9a4b"

CLASS_CACHE_PATH = "res://.godot/global_script_class_cache.cfg"


class InstallError(Exception):
    pass


class DownloadError(Exception):
    pass


# ---------------------------------------------------------------- pck access

def read_pck_index(f):
    """Return (file_base, {res_path: (record_offset, data_offset, size)}, (major, minor))."""
    try:
        magic, fmt, major, minor, _patch, _flags, base = struct.unpack("<4s5IQ", f.read(32))
        if magic != b"GDPC" or fmt != 2:
            raise InstallError("BloodRush.pck has an unexpected format; this game version is not supported.")
        f.read(16 * 4)
        (count,) = struct.unpack("<I", f.read(4))
        entries = {}
        for _ in range(count):
            (n,) = struct.unpack("<I", f.read(4))
            name = f.read(n).rstrip(b"\0").decode("utf-8")
            record = f.tell()
            offset, size = struct.unpack("<QQ", f.read(16))
            f.read(16 + 4)  # md5, flags
            entries[name] = (record, base + offset, size)
    except (struct.error, UnicodeDecodeError):
        raise InstallError("BloodRush.pck looks damaged. Verify the game files in Steam, then try again.")
    return base, entries, (major, minor)


def read_pck_file(f, entries, name):
    if name not in entries:
        return None
    _, offset, size = entries[name]
    f.seek(offset)
    return f.read(size)


def write_pck_file(path, base, entries, name, data):
    """Append new contents to the end of the pck and repoint the file's index entry."""
    with open(path, "r+b") as f:
        f.seek(0, 2)
        offset = f.tell()
        f.write(data)
        f.seek(entries[name][0])
        f.write(struct.pack("<QQ", offset - base, len(data)) + hashlib.md5(data).digest())


# ---------------------------------------------------------------- patches

def patch_class_cache(text, f, entries):
    """Godot 4.4 ignores class entries without is_abstract/is_tool, which breaks every class_name."""
    if '"is_tool":' in text:
        return None

    def add_keys(m):
        script = read_pck_file(f, entries, m.group(1)) or b""
        # @tool has to come before extends/class_name, so the first lines are enough
        head = [l.strip() for l in script.decode("utf-8", "replace").splitlines()
                if l.strip() and not l.strip().startswith("#")][:5]
        is_tool = any(l.startswith("@tool") for l in head)
        return '"is_abstract": false,\n"is_tool": %s,\n"path": "%s"\n}' % (str(is_tool).lower(), m.group(1))

    new, count = re.subn(r'"path": "(res://[^"]+)"\n\}', add_keys, text)
    if count == 0 or count != text.count('"class": &"'):
        raise InstallError("The script class list has an unexpected layout in this game version.")
    return new


CFG_SETTINGS = {
    "display": {
        # fill wider screens (ultrawide) instead of showing black bars
        "window/stretch/aspect": '"expand"',
        # vsync off and no frame cap: unlocked fps
        "window/vsync/vsync_mode": "0",
    },
    "application": {
        "run/max_fps": "0",
    },
    "physics": {
        "common/physics_interpolation": "true",
        "common/physics_jitter_fix": "0.0",
    },
}


def patch_override_cfg(text):
    """Set CFG_SETTINGS in their sections, keeping everything else as is."""
    newline = "\r\n" if "\r\n" in text else "\n"
    # a BOM from some editors would hide the first section header
    lines = text.lstrip("﻿").replace("\r\n", "\n").split("\n")
    for section, keys in CFG_SETTINGS.items():
        header = "[%s]" % section
        start = next((i + 1 for i, l in enumerate(lines) if l.strip() == header), None)
        if start is None:
            while lines and not lines[-1].strip():
                lines.pop()
            lines += ([""] if lines else []) + [header]
            start = len(lines)
        end = next((i for i in range(start, len(lines)) if lines[i].lstrip().startswith("[")), len(lines))
        rest = [l for l in lines[start:end] if l.split("=", 1)[0].strip() not in keys]
        lines[start:end] = ["%s=%s" % kv for kv in keys.items()] + rest
    while lines and not lines[-1].strip():
        lines.pop()
    return newline.join(lines) + newline


# ---------------------------------------------------------------- engine download

def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class RemoteFile:
    """Seekable read-only view of a URL using HTTP range requests, so only the needed part is downloaded."""

    def __init__(self, url):
        resp = urllib.request.urlopen(urllib.request.Request(url, method="HEAD"), timeout=60)
        self.url = resp.url
        if not resp.headers.get("Content-Length"):
            raise DownloadError("the server did not report the file size")
        self.size = int(resp.headers["Content-Length"])
        self.pos = 0

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, offset, whence=0):
        self.pos = [offset, self.pos + offset, self.size + offset][whence]
        return self.pos

    def read(self, n=-1):
        if n < 0:
            n = self.size - self.pos
        if n == 0 or self.pos >= self.size:
            return b""
        end = min(self.pos + n, self.size) - 1
        req = urllib.request.Request(self.url, headers={"Range": "bytes=%d-%d" % (self.pos, end)})
        with urllib.request.urlopen(req, timeout=60) as resp:
            # 200 would mean the whole 1 GB archive is coming instead of the requested part
            if resp.status != 206:
                raise DownloadError("the server does not support partial downloads (HTTP %d)" % resp.status)
            data = resp.read()
        if len(data) != end - self.pos + 1:
            raise DownloadError("the server sent an incomplete response")
        self.pos += len(data)
        return data


def get_engine():
    if os.path.exists(ENGINE_CACHE) and sha256_of(ENGINE_CACHE) == ENGINE_SHA256:
        print("  Using the engine downloaded earlier.")
        return ENGINE_CACHE
    os.makedirs(os.path.dirname(ENGINE_CACHE), exist_ok=True)
    tmp = ENGINE_CACHE + ".part"
    print("  Downloading the Godot 4.4.1 engine from GitHub (about 98 MB)...")
    try:
        with zipfile.ZipFile(RemoteFile(ENGINE_URL)) as z:
            info = z.getinfo(ENGINE_MEMBER)
            done = 0
            with z.open(info) as src, open(tmp, "wb") as dst:
                while True:
                    chunk = src.read(4 << 20)
                    if not chunk:
                        break
                    dst.write(chunk)
                    done += len(chunk)
                    print("\r  %3d%%" % (done * 100 // info.file_size), end="", flush=True)
    except (DownloadError, urllib.error.URLError, http.client.HTTPException, socket.timeout,
            ConnectionError, zipfile.BadZipFile, KeyError) as e:
        raise InstallError("\nCould not download the engine (%s).\n"
                           "Check your internet connection and try again. Nothing was changed." % e)
    print()
    if sha256_of(tmp) != ENGINE_SHA256:
        os.remove(tmp)
        raise InstallError("The downloaded engine did not match the expected checksum. Nothing was changed.")
    os.replace(tmp, ENGINE_CACHE)
    return ENGINE_CACHE


# ---------------------------------------------------------------- main

def is_game_dir(path):
    return all(os.path.isfile(os.path.join(path, n)) for n in ("BloodRush.exe", "BloodRush.pck"))


def find_game_dir():
    print("Tip: in Steam, right-click the game > Manage > Browse local files, then copy the folder path.\n")
    while True:
        path = input("Enter the game folder path (or leave empty to cancel): ").strip().strip('"')
        if not path:
            return None
        if is_game_dir(path):
            return os.path.abspath(path)
        print("BloodRush.exe and BloodRush.pck are not in that folder. Please try again.\n")


def check_game_closed(exe, pck):
    try:
        with open(exe, "r+b"), open(pck, "r+b"):
            pass
    except PermissionError:
        raise InstallError("Please close the game first, then run this again.")


def install():
    print("Smooth Motion for Bloodrush: Undying Wish\n")
    game_dir = find_game_dir()
    if game_dir is None:
        print("Cancelled. Nothing was changed.")
        return
    exe = os.path.join(game_dir, "BloodRush.exe")
    pck = os.path.join(game_dir, "BloodRush.pck")
    cfg = os.path.join(game_dir, "override.cfg")
    check_game_closed(exe, pck)

    # work out every change before touching any file
    with open(pck, "rb") as f:
        base, entries, (major, minor) = read_pck_index(f)
        cache = (read_pck_file(f, entries, CLASS_CACHE_PATH) or b"").decode("utf-8")
        if not cache:
            raise InstallError("This game version is not supported (expected files are missing from BloodRush.pck).")
        if (major, minor) >= (4, 4):
            raise InstallError("This game version already uses Godot %d.%d; this mod is for older builds." % (major, minor))
        new_cache = patch_class_cache(cache, f, entries)

    cfg_old = ""
    if os.path.exists(cfg):
        with open(cfg, encoding="utf-8", newline="") as f:
            cfg_old = f.read()
    cfg_new = patch_override_cfg(cfg_old)
    exe_done = sha256_of(exe) == ENGINE_SHA256

    if exe_done and new_cache is None and cfg_new == cfg_old:
        print("Smooth Motion is already installed. Nothing to do.")
        return

    print("This will:")
    if not exe_done:
        print("  - replace BloodRush.exe with the official Godot 4.4.1 engine")
    if cfg_new != cfg_old:
        print("  - set override.cfg: ultrawide support, unlocked fps, physics interpolation")
    if new_cache is not None:
        print("  - update the script class list in BloodRush.pck")
    print("\nTo undo later, see the Uninstall steps in README.md.")
    if input("\nContinue? [Y/n] ").strip().lower() not in ("", "y", "yes"):
        print("Cancelled. Nothing was changed.")
        return

    print()
    # download first, swap the engine last: the patched class list also works on the old engine,
    # so a failure part-way never leaves the new engine with an unpatched game
    engine = None if exe_done else get_engine()
    if new_cache is not None:
        write_pck_file(pck, base, entries, CLASS_CACHE_PATH, new_cache.encode("utf-8"))
        print("  Game data patched.")
    if cfg_new != cfg_old:
        with open(cfg, "w", encoding="utf-8", newline="") as f:
            f.write(cfg_new)
        print("  override.cfg updated.")
    if engine:
        shutil.copyfile(engine, exe)
        print("  Engine replaced.")
    print("\nDone! Start the game from Steam as usual.")


def main():
    try:
        install()
    except InstallError as e:
        print("\n" + str(e))
    except KeyboardInterrupt:
        print("\nCancelled.")
    except PermissionError as e:
        print("\nWindows denied access to %s." % (e.filename or "a game file"))
        print("Make sure the game is closed and that you can write to the game folder, then try again.")
    except Exception as e:
        print("\nSomething went wrong: %s: %s" % (type(e).__name__, e))
    if "--no-pause" not in sys.argv:
        try:
            input("\nPress Enter to close this window.")
        except (EOFError, KeyboardInterrupt):
            pass


if __name__ == "__main__":
    main()
