#!/usr/bin/env python3
"""
gpib_diag.py — bring-up helper for darwin-gpib.

Prints information the log/UI don't surface: whether the dext is installed,
which USB devices match a personality, the raw USB descriptor of a matched
adapter, and simple round-trips against libgpib.

Usage:
    tools/gpib_diag.py --status           # dext state + matched services
    tools/gpib_diag.py --enumerate        # walk IOKit for our personalities
    tools/gpib_diag.py --descriptor       # dump USB descriptors for matched adapters
    tools/gpib_diag.py --log [--seconds N]# tail dext log stream
    tools/gpib_diag.py --lines            # iblines() on board 0
    tools/gpib_diag.py --ifc              # SendIFC() on board 0
    tools/gpib_diag.py --idn PAD          # *IDN? round-trip against device at PAD
    tools/gpib_diag.py --all              # status + enumerate + descriptor
"""

import argparse
import ctypes
import ctypes.util
import os
import plistlib
import subprocess
import sys
import time
from pathlib import Path

BUNDLE_ID = "app.saturno.darwin-gpib.driver"

# VID/PID → (personality, description).
KNOWN_PERSONALITIES = {
    (0x3923, 0x709B): ("ni_usb",           "NI USB-HS GPIB"),
    (0x0957, 0x0107): ("agilent_82357a",   "Agilent 82357A"),
    (0x0957, 0x0718): ("agilent_82357b",   "Agilent 82357B"),
    (0x05E6, 0xEEEE): ("kusb_488b_loader", "Keithley KUSB-488B (loader)"),
    (0x05E6, 0x488B): ("kusb_488b",        "Keithley KUSB-488B"),
}

REPO_ROOT = Path(__file__).resolve().parent.parent


# -----------------------------------------------------------------------------
# Shell helpers
# -----------------------------------------------------------------------------

def _run(cmd, check=False, timeout=None):
    return subprocess.run(cmd, capture_output=True, text=True, check=check,
                          timeout=timeout)


# -----------------------------------------------------------------------------
# Dext state
# -----------------------------------------------------------------------------

def status():
    print("== dext registration ==")
    out = _run(["systemextensionsctl", "list"]).stdout
    hits = [line for line in out.splitlines() if BUNDLE_ID in line]
    if not hits:
        print(f"  {BUNDLE_ID}: NOT registered (run Open GPIB.app → "
              "'Reconnect all adaptors')")
    else:
        for line in hits:
            print(f"  {line.strip()}")
    print()


def _plist_from_ioreg(cls):
    """Return a list of dicts describing every IOKit entry of the given class."""
    r = _run(["ioreg", "-a", "-r", "-c", cls, "-l"])
    if not r.stdout.strip():
        return []
    try:
        return plistlib.loads(r.stdout.encode("utf-8"))
    except plistlib.InvalidFileException:
        return []


def enumerate_services():
    """Show what USB devices are currently plugged and whether we matched."""
    print("== USB adapters visible to IOKit ==")
    found = False
    for cls in ("IOUSBHostDevice", "IOUSBHostInterface"):
        for entry in _plist_from_ioreg(cls):
            vid = entry.get("idVendor")
            pid = entry.get("idProduct")
            if not (vid and pid):
                continue
            if (vid, pid) not in KNOWN_PERSONALITIES:
                continue
            found = True
            personality, desc = KNOWN_PERSONALITIES[(vid, pid)]
            iouser = entry.get("IOUserClass", "—")
            name   = entry.get("IORegistryEntryName", "?")
            print(f"  {desc}")
            print(f"    class      = {cls}")
            print(f"    VID:PID    = 0x{vid:04X}:0x{pid:04X}")
            print(f"    IOUserClass= {iouser}")
            print(f"    io name    = {name}")
            if iouser == personality:
                print(f"    STATUS     = matched to {personality} ✓")
            else:
                print(f"    STATUS     = present but NOT matched (expected "
                      f"IOUserClass={personality})")
            print()
    if not found:
        print("  (no adapters recognised)")
        print()

    print("== published GPIBUserClient services ==")
    ucs = _plist_from_ioreg("IOUserUserClient")
    hits = [e for e in ucs if e.get("IOUserClass") == "GPIBUserClient"]
    if not hits:
        print("  none published — no GPIB board is currently online")
    for e in hits:
        print(f"  IOClass={e.get('IOClass')} IOUserClass={e.get('IOUserClass')} "
              f"name={e.get('IORegistryEntryName')}")
    print()


def descriptor():
    """Dump the raw configuration descriptor of every matched adapter."""
    print("== USB descriptors ==")
    for entry in _plist_from_ioreg("IOUSBHostDevice"):
        vid, pid = entry.get("idVendor"), entry.get("idProduct")
        if (vid, pid) not in KNOWN_PERSONALITIES:
            continue
        personality, desc = KNOWN_PERSONALITIES[(vid, pid)]
        print(f"{desc}  (VID=0x{vid:04X} PID=0x{pid:04X})")
        # Print the whole raw registry entry — endpoints etc. live under it.
        r = _run(["ioreg", "-r", "-l", "-w", "0",
                  "-n", entry.get("IORegistryEntryName", "")])
        for line in r.stdout.splitlines()[:80]:
            print(f"  {line}")
        print()


def tail_log(seconds=10):
    # Dext logs surface via kernelmanagerd → sender field ends up as
    # "kernel", not our bundle id. Filter by message-text for the strings
    # our drivers emit, plus catch the kernelmanagerd load notification.
    predicate = (
        'eventMessage CONTAINS "kusb_488b" OR '
        'eventMessage CONTAINS "agilent_82357" OR '
        'eventMessage CONTAINS "ni_usb:" OR '
        'eventMessage CONTAINS "NIUSBTransport" OR '
        'eventMessage CONTAINS "GPIBUserClient" OR '
        'eventMessage CONTAINS "app.saturno.darwin-gpib"'
    )
    print(f"== dext-related log stream (waiting {seconds}s) ==")
    p = subprocess.Popen(
        ["log", "stream",
         "--predicate", predicate,
         "--info", "--debug", "--style", "compact"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    start = time.time()
    try:
        while time.time() - start < seconds:
            line = p.stdout.readline()
            if not line:
                break
            print(line.rstrip())
    finally:
        p.terminate()
        try: p.wait(timeout=1)
        except subprocess.TimeoutExpired: p.kill()


# -----------------------------------------------------------------------------
# libgpib probes (ctypes)
# -----------------------------------------------------------------------------

def _find_libgpib():
    # Prefer the freshly-built dylib from DerivedData; fall back to the copy
    # embedded in the built app bundle.
    candidates = []
    dd = Path.home() / "Library/Developer/Xcode/DerivedData"
    for path in dd.glob("darwin-gpib-*/Build/Products/Debug/libgpib.dylib"):
        candidates.append(path)
    for path in dd.glob(
            "darwin-gpib-*/Build/Products/Debug/Open GPIB.app/Contents/Frameworks/libgpib.dylib"):
        candidates.append(path)
    # Repo-local fallback (if the user copied it).
    local = REPO_ROOT / "Products" / "libgpib.dylib"
    if local.exists(): candidates.append(local)
    if not candidates:
        raise SystemExit(
            "libgpib.dylib not found. Build the 'gpib' target in Xcode first.")
    return max(candidates, key=lambda p: p.stat().st_mtime)


def _load_libgpib():
    path = _find_libgpib()
    print(f"loading {path}")
    lib = ctypes.CDLL(str(path))

    # int ibdev(int board, int pad, int sad, int timo, int eoi, int eos)
    lib.ibdev.argtypes = [ctypes.c_int]*6
    lib.ibdev.restype  = ctypes.c_int
    lib.ibfind.argtypes = [ctypes.c_char_p];  lib.ibfind.restype  = ctypes.c_int
    lib.ibonl.argtypes  = [ctypes.c_int, ctypes.c_int]; lib.ibonl.restype = ctypes.c_int
    lib.ibsic.argtypes  = [ctypes.c_int];     lib.ibsic.restype   = ctypes.c_int
    lib.iblines.argtypes= [ctypes.c_int, ctypes.POINTER(ctypes.c_short)]
    lib.iblines.restype = ctypes.c_int
    lib.ibwrt.argtypes  = [ctypes.c_int, ctypes.c_char_p, ctypes.c_long]
    lib.ibwrt.restype   = ctypes.c_int
    lib.ibrd.argtypes   = [ctypes.c_int, ctypes.c_char_p, ctypes.c_long]
    lib.ibrd.restype    = ctypes.c_int
    lib.gpib_error_string.argtypes = [ctypes.c_int]
    lib.gpib_error_string.restype  = ctypes.c_char_p

    # Globals — access via in_dll.
    lib._sta = ctypes.c_int.in_dll(lib, "ibsta")
    lib._err = ctypes.c_int.in_dll(lib, "iberr")
    lib._cnt = ctypes.c_int.in_dll(lib, "ibcnt")
    return lib


def _decode_sta(v):
    bits = [("ERR",   0x8000), ("TIMO", 0x4000), ("END",  0x2000),
            ("SRQI",  0x1000), ("RQS",  0x0800), ("CMPL", 0x0100),
            ("REM",   0x0040), ("CIC",  0x0020), ("ATN",  0x0010),
            ("TACS",  0x0008), ("LACS", 0x0004)]
    return "|".join(name for name, mask in bits if v & mask) or "0"


def _post(lib, tag):
    print(f"  {tag}: ibsta={_decode_sta(lib._sta.value)} "
          f"iberr={lib._err.value} ({lib.gpib_error_string(lib._err.value).decode()}) "
          f"ibcnt={lib._cnt.value}")


def probe_lines():
    lib = _load_libgpib()
    board = lib.ibfind(b"gpib0")
    print(f"ibfind(\"gpib0\") = {board}")
    _post(lib, "after ibfind")
    if board < 0: return
    lines = ctypes.c_short(0)
    lib.iblines(board, ctypes.byref(lines))
    v = lines.value & 0xffff
    print(f"line status = 0x{v:04x}")
    for name, mask, bus in [
        ("DAV",  0x0100, "BusDAV"),
        ("NDAC", 0x0200, "BusNDAC"),
        ("NRFD", 0x0400, "BusNRFD"),
        ("IFC",  0x0800, "BusIFC"),
        ("REN",  0x1000, "BusREN"),
        ("SRQ",  0x2000, "BusSRQ"),
        ("ATN",  0x4000, "BusATN"),
        ("EOI",  0x8000, "BusEOI"),
    ]:
        state = "1 (idle)" if (v & mask) else "0 (asserted)"
        print(f"  {name}: {state}")
    _post(lib, "after iblines")
    lib.ibonl(board, 0)


def probe_ifc():
    lib = _load_libgpib()
    board = lib.ibfind(b"gpib0")
    if board < 0:
        print("ibfind failed")
        return
    lib.ibsic(board)
    _post(lib, "after SendIFC")
    lib.ibonl(board, 0)


def probe_idn(pad):
    lib = _load_libgpib()
    ud = lib.ibdev(0, pad, 0, 12, 1, 0)   # timo=T3s=12, eoi=1, eos=0
    print(f"ibdev(0, {pad}, 0, T3s, 1, 0) = {ud}")
    _post(lib, "after ibdev")
    if ud < 0: return
    msg = b"*IDN?\n"
    lib.ibwrt(ud, msg, len(msg))
    _post(lib, "after ibwrt")
    buf = ctypes.create_string_buffer(256)
    lib.ibrd(ud, buf, len(buf) - 1)
    _post(lib, "after ibrd")
    print(f"reply = {buf.value!r}")
    lib.ibonl(ud, 0)


# -----------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--enumerate", action="store_true")
    ap.add_argument("--descriptor", action="store_true")
    ap.add_argument("--log", action="store_true")
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--lines", action="store_true")
    ap.add_argument("--ifc", action="store_true")
    ap.add_argument("--idn", type=int, metavar="PAD",
                    help="PAD address (0..30) to send *IDN? to")
    ap.add_argument("--all", action="store_true",
                    help="status + enumerate + descriptor")
    args = ap.parse_args()

    if args.all:
        args.status = args.enumerate = args.descriptor = True

    if not any([args.status, args.enumerate, args.descriptor, args.log,
                args.lines, args.ifc, args.idn is not None]):
        ap.print_help()
        return 1

    if args.status:     status()
    if args.enumerate:  enumerate_services()
    if args.descriptor: descriptor()
    if args.log:        tail_log(args.seconds)
    if args.lines:      probe_lines()
    if args.ifc:        probe_ifc()
    if args.idn is not None: probe_idn(args.idn)
    return 0


if __name__ == "__main__":
    sys.exit(main())
