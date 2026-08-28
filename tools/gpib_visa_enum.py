#!/usr/bin/env python3
"""
Enumerate GPIB instruments through PyVISA on macOS.

pyvisa-py's GPIB backend talks to linux-gpib through gpib-ctypes, which will
load any library you point it at. darwin-gpib's libgpib.dylib is API-compatible
with linux-gpib, so pyvisa drives our stack with no patches to either project.

This works from an ordinary, unsigned interpreter because libgpib reaches the
driver through the gpibd broker. Before the broker existed, no amount of Python
could get past DriverKit's user-client entitlement check.

Setup:
    gpibd/build.sh && gpibd/install.sh
    pip install pyvisa pyvisa-py gpib-ctypes

Usage:
    tools/gpib_visa_enum.py [path/to/libgpib.dylib]
"""

import glob
import os
import sys


def find_libgpib() -> str:
    if len(sys.argv) > 1:
        return sys.argv[1]
    if os.environ.get("LIBGPIB"):
        return os.environ["LIBGPIB"]
    patterns = [
        os.path.expanduser(
            "~/Library/Developer/Xcode/DerivedData/darwin-gpib-*/Build/Products/Debug/libgpib.dylib"),
        os.path.expanduser(
            "~/Library/Developer/Xcode/DerivedData/darwin-gpib-*/Build/Products/Debug/"
            "Open GPIB.app/Contents/Frameworks/libgpib.dylib"),
    ]
    hits = [p for pat in patterns for p in glob.glob(pat)]
    if not hits:
        sys.exit("libgpib.dylib not found — build the 'gpib' target, or pass a path")
    return max(hits, key=os.path.getmtime)


def main() -> None:
    lib = find_libgpib()
    print(f"libgpib: {lib}")

    # Must happen before pyvisa_py.gpib is imported: that module binds
    # gpib_ctypes' library handle at import time, and would otherwise latch
    # onto the "not found" mock.
    from gpib_ctypes import gpib
    if not gpib._load_lib(lib):
        sys.exit(f"gpib_ctypes could not load {lib}")
    print("gpib_ctypes loaded it")

    import pyvisa
    rm = pyvisa.ResourceManager("@py")
    print(f"pyvisa {pyvisa.__version__}, backend {rm.visalib}")

    resources = rm.list_resources()
    gpib_resources = [r for r in resources if r.startswith("GPIB")]

    print(f"\nresources: {len(resources)} total, {len(gpib_resources)} GPIB")
    for r in gpib_resources:
        print(f"  {r}")

    # IEEE-488 allows at most 15 devices. More than that means listener
    # detection is broken, not that the bench is busy — currently the case
    # here, see reverse/notes/02-usb-protocol.md on register 0x107.
    if len(gpib_resources) > 15:
        print(f"\n!! {len(gpib_resources)} listeners is physically impossible on one bus.")
        print("   Known issue: the adapter's bus-line register reads 0x00, so")
        print("   GPIBBoard::listenerPresent sees a listener at every address.")
        return

    for r in gpib_resources:
        try:
            inst = rm.open_resource(r)
            inst.timeout = 3000
            print(f"\n{r}\n  *IDN? -> {inst.query('*IDN?').strip()}")
            inst.close()
        except Exception as exc:                     # noqa: BLE001 - report and continue
            print(f"\n{r}\n  failed: {type(exc).__name__}: {exc}")


if __name__ == "__main__":
    main()
