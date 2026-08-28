#!/usr/bin/env python3
"""
Userspace protocol harness for the Keithley KUSB-488B, over raw USB
(pyusb/libusb) rather than the DriverKit dext.

Purpose: a fast, disposable oracle for "is this a GPIB/hardware problem or a
DriverKit-specific one" — same reverse-engineered protocol as
driver/devices/kusb_488b/KUSB488BTransport.cpp, same register/firmware split,
but edit-and-rerun instead of rebuild-reinstall-reactivate-replug. It exists
to cross-check driver behavior, not to replace the dext — the dext is still
the real, shipping target.

Requires the dext NOT be holding the device (it claims it exclusively while
matched/activated): `systemextensionsctl list` should show
"app.saturno.darwin-gpib.driver ... [activated disabled]" or the extension
uninstalled outright before running this.

Usage:
    tools/kusb_harness.py idn <pad> [--pipeline registers|firmware] [-v]
    tools/kusb_harness.py write <pad> <message> [--pipeline ...] [-v]
    tools/kusb_harness.py read  <pad> [<nbytes>] [--pipeline ...] [-v]

Every protocol constant and byte sequence below is transcribed directly from
kusb_488b_protocol.h and KUSB488BTransport.cpp — see those files (and
reverse/notes/02-usb-protocol.md) for the reverse-engineering provenance.
Keep this file's constants in sync with them by hand; there is no automatic
sharing between the C++ driver and this script.
"""

import argparse
import struct
import sys
import time

import usb.core
import usb.util
import usb.backend.libusb1

# ---------------------------------------------------------------------------
# Identity
# ---------------------------------------------------------------------------

KUSB_VID = 0x05E6
KUSB_PID_488B = 0x488B

# Endpoint addresses, resolved by hand from this unit's descriptor dump
# (matches KUSB_PIPE_DATA_OUT/CMD_OUT/DATA_IN's *positions* in
# kusb_488b_protocol.h — index 2/3/4 among this interface's endpoints).
EP_DATA_OUT = 0x02
EP_CMD_OUT = 0x04
EP_DATA_IN = 0x86

# ---------------------------------------------------------------------------
# Operational vendor requests (PID 0x488B)
# ---------------------------------------------------------------------------

REQ_STATUS = 0xB0
REQ_CMDBLOCK = 0xB1
REQ_BEGIN_CMD = 0xB3
REQ_PULSE_IFC = 0xB4
REQ_BEGIN_WRITE = 0xB9
REQ_BEGIN_READ = 0xBA
REQ_ABORT = 0xBB
REQ_REG = 0xBD  # bidirectional: IN reads a register, OUT writes one

CMD_SYSTEM_CONTROLLER = 1
READ_WVALUE = 0x0100
WRITE_EOI_SHIFT = 8

XFER_HDR_LEN = 8
STATUS_LEN_CMD = 7
STATUS_LEN_WRITE = 6
STATUS_LEN_READ = 0x0B
STATUS_MIN_READ = 7  # what the device actually returns for a read status
STATUS_LEN_IFC = 1
COUNT_WIDTH_CMD = 2
COUNT_WIDTH_DATA = 4

STATE_BUSY = 1
STATE_DONE = 2
ERR_ABORTED = 5

MAX_CMD_BYTES = 0x200
MAX_DATA_CHUNK = 0x10000

CONTROL_TIMEOUT_MS = 1000
STATUS_POLL_LIMIT = 100000

# ---------------------------------------------------------------------------
# NEC7210-style command blocks (request 0xB1: stream of "<reg> <value>" pairs)
# ---------------------------------------------------------------------------

CMD_CDOR = 0x00
CMD_DIR = 0x00
CMD_ISR1 = 0x01
CMD_ISR2 = 0x02
CMD_ADMR = 0x04
CMD_ADSR = 0x04
CMD_ADR = 0x06
CMD_AUXMR = 0x05

ADMR_ADM0 = 0x01
ADMR_TRM = 0x30
ADMR_LON = 0x40
ADMR_TON = 0x80

ISR1_DI = 0x01
ISR1_DO = 0x02
ISR1_END = 0x10
ISR2_CO = 0x08

ADSR_TA = 0x02
ADSR_LA = 0x04
ADSR_NATN = 0x40
ADSR_CIC = 0x80

AUX_PON = 0x00
AUX_CHIP_RESET = 0x02
AUX_FH = 0x03
AUX_SEOI = 0x06
AUX_GTS = 0x10
AUX_TCA = 0x11
AUX_TCS = 0x12
AUX_CIFC = 0x16
AUX_SET_REN = 0x1F
AUX_CLEAR_REN = 0x17
AUX_SIFC = 0x1E
AUX_ONLINE = 0x20

ADR_ARS = 0x80
ADR_DISABLE_TL = 0x60

REG_AUXMR = 0x0005
REG_BUS_LINES = 0x0107

# ---------------------------------------------------------------------------
# GPIB bus commands (driver/gpib/gpib_user.h)
# ---------------------------------------------------------------------------

SDC = 0x04
LAD = 0x20
UNL = 0x3F
TAD = 0x40
UNT = 0x5F
SAD = 0x60

BOARD_PAD = 0  # our own controller address; matches GPIBBoard's default

# EOS flags (subset of gpib_user.h's ibeos bits)
REOS = 0x0400


def MLA(pad):
    return (pad & 0x1F) | LAD


def MTA(pad):
    return (pad & 0x1F) | TAD


class GPIBError(Exception):
    pass


class Timeout(GPIBError):
    pass


class NoListener(GPIBError):
    pass


def ms_from_usec(timeout_us):
    if timeout_us == 0xFFFFFFFF:
        return 0
    ms = timeout_us // 1000
    if timeout_us != 0 and ms == 0:
        ms = 1
    return ms


def poll_iterations_for(timeout_us):
    if timeout_us == 0xFFFFFFFF:
        return 30000
    ms = timeout_us // 1000
    if ms < 100:
        ms = 100
    if ms > 30000:
        ms = 30000
    return ms


class KusbTransport:
    def __init__(self, verbose=False):
        self.verbose = verbose
        self.dev = None
        self.rfd_holdoff_pending = False
        self.ren_asserted = False

    def log(self, msg):
        if self.verbose:
            print(f"  [harness] {msg}", file=sys.stderr)

    # -- USB open ------------------------------------------------------

    def open(self):
        backend = usb.backend.libusb1.get_backend(
            find_library=lambda x: "/opt/homebrew/lib/libusb-1.0.dylib"
        )
        if backend is None:
            backend = usb.backend.libusb1.get_backend()
        dev = usb.core.find(idVendor=KUSB_VID, idProduct=KUSB_PID_488B, backend=backend)
        if dev is None:
            raise GPIBError(
                "KUSB-488B (05e6:488b) not found. Is it plugged in, and is "
                "the dext deactivated (it claims the device exclusively)?"
            )
        try:
            dev.set_configuration()
        except usb.core.USBError as e:
            self.log(f"set_configuration: {e} (continuing; often already set)")

        try:
            usb.util.claim_interface(dev, 0)
        except usb.core.USBError as e:
            raise GPIBError(
                f"Could not claim interface 0: {e}. The dext (or another "
                f"process) likely still holds this device."
            )

        self.dev = dev
        self.log("device opened, interface 0 claimed")

    def close(self):
        if self.dev is not None:
            try:
                usb.util.release_interface(self.dev, 0)
            except usb.core.USBError:
                pass
            usb.util.dispose_resources(self.dev)
            self.dev = None

    # -- Control transfers -----------------------------------------------

    def control_out(self, request, wValue, wIndex, data=b""):
        n = self.dev.ctrl_transfer(
            0x40, request, wValue, wIndex, data, timeout=CONTROL_TIMEOUT_MS
        )
        if n != len(data):
            raise GPIBError(
                f"controlOut req=0x{request:02x} short: sent {n} of {len(data)}"
            )

    def control_in(self, request, wValue, wIndex, length):
        data = self.dev.ctrl_transfer(
            0xC0, request, wValue, wIndex, length, timeout=CONTROL_TIMEOUT_MS
        )
        return bytes(data)

    # -- Bulk transfers ----------------------------------------------------

    def bulk_out(self, ep, data, timeout_us):
        timeout_ms = ms_from_usec(timeout_us) or CONTROL_TIMEOUT_MS
        sent = self.dev.write(ep, data, timeout=timeout_ms)
        if sent != len(data):
            raise GPIBError(f"bulkOut to ep 0x{ep:02x} short: sent {sent} of {len(data)}")

    def bulk_in(self, ep, capacity, timeout_us):
        timeout_ms = ms_from_usec(timeout_us) or CONTROL_TIMEOUT_MS
        data = self.dev.read(ep, capacity, timeout=timeout_ms)
        return bytes(data)

    # -- Register / command-block access ------------------------------------

    def write_command_block(self, block):
        assert 0 < len(block) <= 64
        self.control_out(REQ_CMDBLOCK, 0, 0, bytes(block))

    def write_raw_register(self, reg, value):
        self.write_command_block(bytes([reg & 0xFF, value & 0xFF]))

    def read_raw_register(self, reg):
        data = self.control_in(REQ_REG, reg, 0, 1)
        if len(data) < 1:
            raise GPIBError(f"readRawRegister(0x{reg:04x}): short read")
        return data[0]

    def write_aux_via_reg(self, value):
        # writeAux(): 0xBD OUT, wValue = register selector (AUXMR), not the
        # 0xB1 command-block path. Used only for REN, matching the driver.
        self.control_out(REQ_REG, REG_AUXMR, 0, bytes([value & 0xFF]))

    def read_bus_lines(self):
        v = self.read_raw_register(REG_BUS_LINES)
        return v

    # -- Bring-up ------------------------------------------------------------

    def run_bringup(self):
        self.log("bring-up: init blocks A/B/C")
        self.write_command_block(bytes([0x05, 0x02, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00]))
        self.write_command_block(bytes([0x05, 0x01, 0x02]))
        self.write_command_block(bytes([
            0x05, 0x70, 0x05, 0x82, 0x05, 0xC0, 0x05, 0xA8,
            0x05, 0xC0, 0x05, 0xD0, 0x05, 0x49, 0x05, 0xE1,
        ]))
        self.log("bring-up: go online")
        self.write_command_block(bytes([CMD_AUXMR, AUX_ONLINE]))
        self.log("bring-up: program address (ADR1 disable T/L, ADR0 addr 0)")
        self.write_raw_register(CMD_ADR, ADR_ARS | ADR_DISABLE_TL)
        self.write_raw_register(CMD_ADR, 0x00)
        # ADMR = addressing mode 1 + TRM. Without this ADMR stays 0, which is
        # not a valid NEC7210 addressing mode, so the core never asserts
        # TA/LA from its own MTA/MLA. This is what removed every per-transfer
        # AUX_TCA / ADMR=TON / ADMR=LON poke. Mirrors runBringUpSequence().
        self.write_raw_register(CMD_ADMR, ADMR_ADM0 | ADMR_TRM)
        self.log("bring-up: AUX_PON (release power-on hold)")
        self.write_raw_register(CMD_AUXMR, AUX_PON)
        self.rfd_holdoff_pending = False
        self.log("bring-up complete")

    def recover(self):
        """Abort any armed transfer and clear stalled bulk endpoints.

        A firmware transfer that fails part-way leaves two pieces of state
        behind that nothing else clears: an armed-but-never-serviced transfer
        (so the next status poll reports a stale count -- observed as
        "firmware reported 12 of 6", the previous attempt's bytes still being
        counted), and a HALTED bulk endpoint (every following bulk_out raises
        "Pipe error"/STALL). Neither `run_bringup()` (which only resets the
        7210 core) nor a USB port reset via `dev.reset()` clears them --
        both were measured NOT to help.

        The vendor driver does exactly this on its own failure path: when a
        transfer ends with the wrong state or a short count,
        `FUN_1400036e0`/`FUN_140002e1c` call `0xBB` (abort) and then
        `FUN_1400021d0(handle, pipe)`, the pipe reset. So this is the
        vendor-sanctioned recovery, not a workaround.

        Cheap and side-effect-free when nothing is wrong -- call it before
        any fresh sequence.
        """
        try:
            self.abort_transfer()
        except Exception:
            pass
        for ep in (EP_DATA_OUT, EP_CMD_OUT, EP_DATA_IN):
            try:
                self.dev.clear_halt(ep)
            except Exception:
                pass

    def write_reg_bd(self, reg, value):
        """Write ONE register the way the vendor driver does: request 0xBD
        OUT, wValue = register index, one data byte.

        This is what `FUN_140002ddc` in KUSB488B_X64.sys does, and it is the
        only register-write path the vendor uses during a transfer. Our
        `write_raw_register()` instead goes through the 0xB1 command-block
        request, which the firmware **STALLs while a transfer is armed** --
        that, not a dead device, is what the "everything returns Pipe error"
        wedge actually is (0xBD keeps working right through it).

        Measured: switching the per-transfer AUXMR/ADMR pokes to this made no
        difference to the still-broken firmware read, but it is the
        vendor-accurate path and it does not stall, so prefer it for anything
        issued while a transfer may be outstanding.
        """
        self.dev.ctrl_transfer(0x40, REQ_REG, reg, 0, bytes([value & 0xFF]),
                               timeout=CONTROL_TIMEOUT_MS)

    def pulse_interface_clear(self):
        self.write_raw_register(CMD_AUXMR, AUX_SIFC)
        time.sleep(0.001)
        self.write_raw_register(CMD_AUXMR, AUX_CIFC)

    def set_remote_enable(self, enable):
        self.write_aux_via_reg(AUX_SET_REN if enable else AUX_CLEAR_REN)
        self.ren_asserted = enable

    def release_atn(self):
        self.write_raw_register(CMD_AUXMR, AUX_GTS)

    def release_rfd_holdoff_if_pending(self):
        if not self.rfd_holdoff_pending:
            return
        self.log("releasing pending RFD holdoff (AUX_FH)")
        self.write_raw_register(CMD_AUXMR, AUX_FH)
        self.rfd_holdoff_pending = False

    # -- Register-path GPIB primitives ---------------------------------------

    def send_command_bytes_via_registers(self, cmds):
        if not cmds:
            return
        self.release_rfd_holdoff_if_pending()

        self.write_raw_register(CMD_AUXMR, AUX_TCA)
        atn_asserted = False
        for _ in range(200):
            adsr = self.read_raw_register(CMD_ADSR)
            if (adsr & ADSR_NATN) == 0:
                atn_asserted = True
                break
            time.sleep(0.001)
        if not atn_asserted:
            raise Timeout("take-control (AUX_TCA) never asserted ATN")

        self.write_raw_register(CMD_ADMR, ADMR_TRM)

        for i, b in enumerate(cmds):
            self.write_raw_register(CMD_CDOR, b)
            accepted = False
            for _ in range(200):
                isr2 = self.read_raw_register(CMD_ISR2)
                if isr2 & ISR2_CO:
                    accepted = True
                    break
                time.sleep(0.001)
            if not accepted:
                raise NoListener(
                    f"command byte 0x{b:02x} never got CO (i={i}/{len(cmds)})"
                )

    def write_data_via_registers(self, buf, send_eoi, timeout_us):
        if not buf:
            return 0
        max_spins = poll_iterations_for(timeout_us)

        self.write_raw_register(CMD_ADMR, ADMR_TON | ADMR_TRM)
        self.write_raw_register(CMD_AUXMR, AUX_GTS)

        atn_released = False
        for _ in range(500):
            adsr = self.read_raw_register(CMD_ADSR)
            if adsr & ADSR_NATN:
                atn_released = True
                break
            time.sleep(0.001)
        if not atn_released:
            self.write_raw_register(CMD_ADMR, ADMR_TRM)
            raise GPIBError("writeData: GTS never released ATN")

        n = len(buf)
        for i, b in enumerate(buf):
            if send_eoi and i == n - 1:
                self.write_raw_register(CMD_AUXMR, AUX_SEOI)
            self.write_raw_register(CMD_CDOR, b)

            sent = False
            for _ in range(max_spins):
                isr1 = self.read_raw_register(CMD_ISR1)
                if isr1 & ISR1_DO:
                    sent = True
                    break
                time.sleep(0.001)
            if not sent:
                self.write_raw_register(CMD_ADMR, ADMR_TRM)
                raise NoListener(f"writeData: byte 0x{b:02x} never got DO (i={i}/{n})")
        return n

    def read_data_via_registers(self, request_count, eos_char, eos_flags, timeout_us):
        max_spins = poll_iterations_for(timeout_us)
        self.release_rfd_holdoff_if_pending()

        self.write_raw_register(CMD_ADMR, ADMR_LON | ADMR_TRM)
        self.write_raw_register(CMD_AUXMR, AUX_GTS)

        atn_released = False
        for _ in range(500):
            adsr = self.read_raw_register(CMD_ADSR)
            if adsr & ADSR_NATN:
                atn_released = True
                break
            time.sleep(0.001)
        if not atn_released:
            self.write_raw_register(CMD_ADMR, ADMR_TRM)
            raise GPIBError("readData: GTS never released ATN")

        isr1_start = self.read_raw_register(CMD_ISR1)
        end_unusable = (isr1_start & ISR1_END) != 0
        if end_unusable:
            self.log("ISR1 END already latched at start of read -- ignoring END this cycle")

        idle_spins = 250
        out = bytearray()
        saw_end = False
        timed_out = False

        while len(out) < request_count:
            wait_spins = max_spins if len(out) == 0 else idle_spins
            ready = False
            isr1 = 0
            for _ in range(wait_spins):
                isr1 = self.read_raw_register(CMD_ISR1)
                if isr1 & ISR1_DI:
                    ready = True
                    break
                time.sleep(0.001)
            if not ready:
                self.write_raw_register(CMD_ADMR, ADMR_TRM)
                if len(out) == 0:
                    timed_out = True
                break

            byte = self.read_raw_register(CMD_DIR)
            out.append(byte)

            if not end_unusable and (isr1 & ISR1_END):
                saw_end = True
                break
            if (eos_flags & REOS) and byte == eos_char:
                break

        if saw_end or len(out) > 0:
            self.rfd_holdoff_pending = True

        if timed_out and len(out) == 0:
            raise Timeout("readData: no bytes received")
        return bytes(out), saw_end

    # -- Firmware-path primitives ---------------------------------------------

    def begin_transfer(self, request, wValue, wIndex, length, timeout_us):
        ms = ms_from_usec(timeout_us)
        hdr = struct.pack("<I", ms)[:3] + b"\x00" + struct.pack("<I", length)
        assert len(hdr) == XFER_HDR_LEN
        self.control_out(request, wValue, wIndex, hdr)

    def poll_status(self, status_len, count_width):
        # BUG FOUND (2026-08-27): the device sends a SHORT packet (one byte
        # less than status_len) whenever state==BUSY, and only the full
        # status_len once state leaves BUSY. An earlier version of this
        # function treated any short read as a transient USB glitch and gave
        # up after 5 retries of 2ms each (~10ms total) -- nowhere near a real
        # GPIB turnaround -- which made every write/read through this method
        # fail with "short status block" even when the transfer was still
        # perfectly healthy and simply not done yet. A short glitch tolerance
        # is still worth keeping for spin 0 specifically (the very first poll
        # right after a bulk transfer can transiently fail at the USB level
        # before state is even readable) -- but once byte 0 (state) is
        # actually available, trust it regardless of overall packet length.
        glitch_retries = 0
        for spin in range(STATUS_POLL_LIMIT):
            try:
                buf = self.control_in(REQ_STATUS, 0, 0, status_len)
            except usb.core.USBError:
                if glitch_retries < 5:
                    glitch_retries += 1
                    time.sleep(0.005)
                    continue
                raise
            if len(buf) < 1:
                if glitch_retries < 5:
                    glitch_retries += 1
                    time.sleep(0.005)
                    continue
                raise GPIBError("pollStatus: empty status block")
            glitch_retries = 0

            if buf[0] == STATE_BUSY:
                # No sleep: pollStatus() in the driver spins, and each
                # control_in is itself a USB round trip (~0.1 ms), so it is
                # self-rate-limiting. A sleep here added ~5 ms to EVERY
                # firmware write and made harness timings useless for
                # performance work.
                continue

            if len(buf) < status_len:
                # Done/error but the packet was still short -- now this
                # really is unexpected; give it a few retries before failing.
                if glitch_retries < 5:
                    glitch_retries += 1
                    time.sleep(0.005)
                    continue
                raise GPIBError(
                    f"pollStatus: short status block after state left BUSY "
                    f"(got {len(buf)} of {status_len}, state=0x{buf[0]:02x})"
                )

            count = buf[2] | (buf[3] << 8)
            if count_width == 4 and status_len >= 6:
                count |= (buf[4] << 16) | (buf[5] << 24)
            end = buf[6] if status_len > 6 else None
            if buf[0] != STATE_DONE:
                if buf[1] == ERR_ABORTED:
                    raise Timeout(f"pollStatus: aborted, count={count}")
                raise GPIBError(f"pollStatus: error state={buf[0]} err={buf[1]}")
            return count, end
        self.abort_transfer()
        raise Timeout("pollStatus: never left BUSY")

    def abort_transfer(self):
        self.control_out(REQ_ABORT, 0, 0)

    def send_command_bytes_via_firmware(self, cmds):
        if not cmds:
            return
        self.release_rfd_holdoff_if_pending()

        # NO recover() and NO AUX_TCA poke here -- both were removed from
        # sendCommandBytesViaFirmware(). recover() before read addressing
        # tears the engine down (ENOL on every read); AUX_TCA is unnecessary
        # now that bring-up programs ADMR. Mirrors the driver exactly.
        timeout_us = 3_000_000

        done = 0
        n = len(cmds)
        while done < n:
            chunk = min(n - done, MAX_CMD_BYTES)
            self.begin_transfer(REQ_BEGIN_CMD, CMD_SYSTEM_CONTROLLER, 0, chunk, timeout_us)
            try:
                self.bulk_out(EP_CMD_OUT, cmds[done:done + chunk], timeout_us)
            except Exception:
                self.abort_transfer()
                raise
            count, _ = self.poll_status(STATUS_LEN_CMD, COUNT_WIDTH_CMD)
            if count != chunk:
                raise NoListener(f"sendCommandBytes: firmware reported {count} of {chunk}")
            done += chunk

    def write_data_via_firmware(self, buf, send_eoi, timeout_us):
        if not buf:
            return 0
        self.release_rfd_holdoff_if_pending()

        # No ADMR=TON / AUX_GTS poke, and no 0xB7 (go-to-standby): all
        # removed from writeDataViaFirmware() once bring-up programmed ADMR.
        done = 0
        n = len(buf)
        while done < n:
            chunk = min(n - done, MAX_DATA_CHUNK)
            last = (done + chunk == n)
            wValue = ((1 if (send_eoi and last) else 0) << WRITE_EOI_SHIFT)
            self.begin_transfer(REQ_BEGIN_WRITE, wValue, 0, chunk, timeout_us)
            try:
                self.bulk_out(EP_DATA_OUT, buf[done:done + chunk], timeout_us)
            except Exception:
                self.abort_transfer()
                raise
            count, _ = self.poll_status(STATUS_LEN_WRITE, COUNT_WIDTH_DATA)
            if count != chunk:
                raise NoListener(f"writeData: firmware reported {count} of {chunk}")
            done += chunk
        return done

    def read_data_via_firmware(self, request_count, eos_char, eos_flags, timeout_us):
        # No ADMR=LON / AUX_GTS poke -- see writeDataViaFirmware() above.
        #
        # wIndex carries the EOS byte UNCONDITIONALLY (the vendor passes its
        # persisted ctx+0xC1 on every read). With wIndex = 0 the firmware
        # never terminates: err=5, count=0, no data. This is the single fix
        # that made the firmware read path work.
        wIndex = eos_char if (eos_flags & REOS) else ord('\n')
        self.begin_transfer(REQ_BEGIN_READ, READ_WVALUE, wIndex, request_count, timeout_us)

        done = 0
        out = bytearray(request_count)
        glitch_retries = 0
        while True:
            if done < request_count:
                try:
                    got = self.bulk_in(EP_DATA_IN, request_count - done, timeout_us)
                except Exception:
                    self.abort_transfer()
                    raise
                out[done:done + len(got)] = got
                done += len(got)

            try:
                status = self.control_in(REQ_STATUS, 0, 0, STATUS_LEN_READ)
            except usb.core.USBError:
                status = b""
            if len(status) < STATUS_MIN_READ:
                if glitch_retries < 5:
                    glitch_retries += 1
                    time.sleep(0.002)
                    continue
                self.abort_transfer()
                raise GPIBError("readData: short/failed status poll")
            glitch_retries = 0

            if status[0] == STATE_DONE:
                break
            if status[0] != STATE_BUSY:
                self.abort_transfer()
                if status[1] == ERR_ABORTED:
                    raise Timeout("readData: aborted")
                raise GPIBError(f"readData: error state={status[0]} err={status[1]}")
            if done >= request_count:
                self.abort_transfer()
                break

        count = status[2] | (status[3] << 8) | (status[4] << 16) | (status[5] << 24)
        count = min(count, request_count, done)
        end = 1 if status[6] == 1 else 0
        return bytes(out[:count]), end

    # -- High-level GPIB transaction (mirrors GPIBBoard.cpp) ------------------

    def address_for_write(self, pad, pipeline):
        cmds = bytes([UNL, UNT, MTA(BOARD_PAD), MLA(pad)])
        self.log(f"address_for_write: {cmds.hex(' ')}")
        if pipeline == "firmware":
            self.send_command_bytes_via_firmware(cmds)
        else:
            self.send_command_bytes_via_registers(cmds)

    def address_for_read(self, pad, pipeline):
        cmds = bytes([UNL, UNT, MLA(BOARD_PAD), MTA(pad)])
        self.log(f"address_for_read: {cmds.hex(' ')}")
        if pipeline == "firmware":
            self.send_command_bytes_via_firmware(cmds)
        else:
            self.send_command_bytes_via_registers(cmds)

    def write_message(self, pad, message, pipeline, timeout_us=3_000_000):
        self.address_for_write(pad, pipeline)
        data = message.encode() if isinstance(message, str) else message
        self.log(f"writeData ({pipeline}): {data!r}")
        if pipeline == "firmware":
            n = self.write_data_via_firmware(data, True, timeout_us)
        else:
            n = self.write_data_via_registers(data, True, timeout_us)
        return n

    def read_reply(self, pad, pipeline, nbytes=256, timeout_us=3_000_000):
        self.address_for_read(pad, pipeline)
        self.log(f"readData ({pipeline}): up to {nbytes} bytes")
        if pipeline == "firmware":
            data, end = self.read_data_via_firmware(nbytes, 0, 0, timeout_us)
        else:
            data, end = self.read_data_via_registers(nbytes, 0, 0, timeout_us)
        return data, end


def cmd_idn(args):
    t = KusbTransport(verbose=args.verbose)
    t.open()
    try:
        t.run_bringup()
        t.pulse_interface_clear()
        t.set_remote_enable(True)
        n = t.write_message(args.pad, "*IDN?\n", args.pipeline)
        print(f"wrote {n} bytes")
        data, end = t.read_reply(args.pad, args.pipeline)
        print(f"reply ({len(data)} bytes, end={end}): {data!r}")
        try:
            print("decoded:", data.decode(errors="replace").strip())
        except Exception:
            pass
    finally:
        t.close()


def cmd_write(args):
    t = KusbTransport(verbose=args.verbose)
    t.open()
    try:
        t.run_bringup()
        t.pulse_interface_clear()
        t.set_remote_enable(True)
        n = t.write_message(args.pad, args.message, args.pipeline)
        print(f"wrote {n} bytes")
    finally:
        t.close()


def cmd_read(args):
    t = KusbTransport(verbose=args.verbose)
    t.open()
    try:
        t.run_bringup()
        t.pulse_interface_clear()
        t.set_remote_enable(True)
        data, end = t.read_reply(args.pad, args.pipeline, nbytes=args.nbytes)
        print(f"reply ({len(data)} bytes, end={end}): {data!r}")
    finally:
        t.close()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--pipeline", choices=["registers", "firmware"], default="registers",
                   help="which transfer pipeline to use (default: registers, the "
                        "proven-reliable path)")
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)

    p_idn = sub.add_parser("idn", help="bring up, write *IDN?\\n, read the reply")
    p_idn.add_argument("pad", type=int)
    p_idn.set_defaults(func=cmd_idn)

    p_write = sub.add_parser("write", help="bring up, write a message")
    p_write.add_argument("pad", type=int)
    p_write.add_argument("message")
    p_write.set_defaults(func=cmd_write)

    p_read = sub.add_parser("read", help="bring up, address as talker, read a reply")
    p_read.add_argument("pad", type=int)
    p_read.add_argument("nbytes", type=int, nargs="?", default=256)
    p_read.set_defaults(func=cmd_read)

    args = p.parse_args()
    try:
        args.func(args)
    except GPIBError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
