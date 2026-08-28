#!/usr/bin/env python3
"""
    LIBGPIB=/path/to/libgpib.dylib tools/pyvisa_idn.py [GPIB0::17::INSTR]
"""
import os
import sys

from gpib_ctypes import gpib
gpib._load_lib(os.environ.get("LIBGPIB", "libgpib.dylib"))

import pyvisa

rm = pyvisa.ResourceManager()
addr = sys.argv[1] if len(sys.argv) > 1 else next(
    (r for r in rm.list_resources() if r.startswith("GPIB")), None)
if addr is None:
    sys.exit("no GPIB instrument found")

inst = rm.open_resource(addr)
print(f"{addr} -> {inst.query('*IDN?').strip()}")
inst.close()
