#!/usr/bin/env python3
"""
Firmware downloader for the Keithley KUSB-488B, replicating
driver/devices/kusb_488b/KUSB488BLoader.cpp exactly, over raw pyusb/libusb.

The adapter has no on-board flash: on every power-up/replug it enumerates as
PID 0xEEEE ("loader") and needs the FPGA bitstream + 8051 firmware pushed to
it before it re-enumerates as the operational PID 0x488B. Normally the dext's
kusb_488b_loader service does this the moment it matches the loader PID. With
the dext deactivated (as required for tools/kusb_harness.py), nothing does --
so after a fresh boot/replug the device sits at the loader stage forever.
This script does the same download from userspace, parsing the byte arrays
directly out of kusb_488b_firmware_data.h so it can never drift from what the
dext itself would send.

Usage:
    tools/kusb_fw_download.py
"""

import re
import sys
import time
from pathlib import Path

import usb.core
import usb.backend.libusb1

REPO_ROOT = Path(__file__).resolve().parent.parent
FW_HEADER = REPO_ROOT / "driver/devices/kusb_488b/kusb_488b_firmware_data.h"

KUSB_VID = 0x05E6
PID_LOADER = 0xEEEE
PID_488B = 0x488B

REQ_RAM = 0xA0
REQ_EXTRAM = 0xA3
REQ_FPGA = 0xA4
CPUCS_ADDR = 0xE600
CHUNK = 0x1000
LEGACY_BCD_DEVICE = 0x0004


def parse_array(name: str, text: str) -> bytes:
    m = re.search(r"kKusb" + name + r"Data\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        sys.exit(f"could not find kKusb{name}Data in {FW_HEADER}")
    hexes = re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))
    return bytes(int(h, 16) for h in hexes)


def get_backend():
    backend = usb.backend.libusb1.get_backend(
        find_library=lambda x: "/opt/homebrew/lib/libusb-1.0.dylib"
    )
    return backend or usb.backend.libusb1.get_backend()


def vendor_write(dev, request, wValue, wIndex, data=b""):
    n = dev.ctrl_transfer(0x40, request, wValue, wIndex, data, timeout=5000)
    if n != len(data):
        raise RuntimeError(f"vendor write 0x{request:02x} short: sent {n} of {len(data)}")


def download_chunked(dev, request, base_addr, data):
    offset = 0
    while offset < len(data):
        chunk = data[offset:offset + CHUNK]
        vendor_write(dev, request, (base_addr + offset) & 0xFFFF, 0, chunk)
        offset += len(chunk)


def download_bitstream(dev, data):
    vendor_write(dev, REQ_FPGA, 0, 0, b"")   # begin
    offset = 0
    while offset < len(data):
        chunk = data[offset:offset + CHUNK]
        last = (offset + len(chunk)) >= len(data)
        vendor_write(dev, REQ_FPGA, 1, 1 if last else 0, chunk)
        offset += len(chunk)


def main():
    if not FW_HEADER.exists():
        sys.exit(f"{FW_HEADER} not found -- generate it with "
                  "tools/extract_kusb_firmware.py first")
    text = FW_HEADER.read_text()
    fpga = parse_array("FpgaBitstream", text)
    extram = parse_array("ExtRamBlob", text)
    main_fw = parse_array("MainFirmware", text)
    print(f"parsed firmware: fpga={len(fpga)}B extram={len(extram)}B main={len(main_fw)}B")

    backend = get_backend()
    dev = usb.core.find(idVendor=KUSB_VID, idProduct=PID_LOADER, backend=backend)
    if dev is None:
        already = usb.core.find(idVendor=KUSB_VID, idProduct=PID_488B, backend=backend)
        if already is not None:
            print("device is already at PID 0x488B -- nothing to do")
            return
        sys.exit("KUSB-488B loader (05e6:eeee) not found -- is it plugged in?")

    bcd = dev.bcdDevice
    print(f"found loader, bcdDevice=0x{bcd:04x}")
    if bcd < LEGACY_BCD_DEVICE:
        sys.exit(f"bcdDevice 0x{bcd:04x} needs the legacy EEPROM stage (request 0xA2) "
                  "-- destructive, not implemented here. Use the Windows driver once.")

    dev.set_configuration()
    usb.util.claim_interface(dev, 0) if False else None  # loader uses default pipe only

    print(f"downloading FPGA bitstream ({len(fpga)} bytes)...")
    download_bitstream(dev, fpga)

    print(f"downloading ext-RAM blob ({len(extram)} bytes)...")
    download_chunked(dev, REQ_EXTRAM, 0x4000, extram)

    print(f"downloading 8051 firmware ({len(main_fw)} bytes)...")
    vendor_write(dev, REQ_RAM, CPUCS_ADDR, 0, bytes([1]))   # hold CPU in reset
    download_chunked(dev, REQ_RAM, 0, main_fw)
    vendor_write(dev, REQ_RAM, CPUCS_ADDR, 0, bytes([0]))   # release

    print("download complete; waiting for re-enumeration as PID 0x488B...")
    for _ in range(50):
        time.sleep(0.2)
        d = usb.core.find(idVendor=KUSB_VID, idProduct=PID_488B, backend=backend)
        if d is not None:
            print("device re-enumerated as PID 0x488B -- ready")
            return
    sys.exit("device did not re-enumerate as PID 0x488B within 10s")


if __name__ == "__main__":
    import usb.util
    main()
