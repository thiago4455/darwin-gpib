//
//  kusb_488b_protocol.h
//  darwin-gpib driver
//
//  Wire protocol for the Keithley KUSB-488B (OEM: ADLINK USB-3488A).
//
//  The adapter is a Cypress EZ-USB FX2 front-ending an Altera FPGA that
//  implements the GPIB core. Both are loaded by the host at attach: the
//  device first enumerates as PID 0xEEEE ("Loader"), the host downloads an
//  FPGA bitstream plus 8051 firmware, and it re-enumerates as PID 0x488B.
//
//  Derived from KUSB488B_X64.sys / kusbgpib_fwdlx64.sys; see
//  reverse/notes/01-firmware-loader.md and 02-usb-protocol.md.
//

#ifndef kusb_488b_protocol_h
#define kusb_488b_protocol_h

#include <stdint.h>

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

#define KUSB_VID                0x05E6   // Keithley Instruments
#define KUSB_PID_LOADER         0xEEEE   // pre-firmware ("KUSB-488B Loader")
#define KUSB_PID_488B           0x488B   // operational

// Units older than this bcdDevice also need the legacy EEPROM/8051 stage.
#define KUSB_LEGACY_BCD_DEVICE  0x0004

// ---------------------------------------------------------------------------
// Firmware-download vendor requests (loader stage, PID 0xEEEE)
// ---------------------------------------------------------------------------

#define KUSB_FW_REQ_RAM         0xA0   // 8051 code RAM; wValue = target address
#define KUSB_FW_REQ_EEPROM      0xA2   // EEPROM write (destructive — not replayed)
#define KUSB_FW_REQ_EXTRAM      0xA3   // external RAM; wValue = 0x4000 + offset
#define KUSB_FW_REQ_FPGA        0xA4   // FPGA configuration

// EZ-USB CPUCS register: write 1 to hold the 8051 in reset, 0 to release.
#define KUSB_FW_CPUCS_ADDR      0xE600

// Every firmware download chunks at this size.
#define KUSB_FW_CHUNK           0x1000

// ---------------------------------------------------------------------------
// Operational vendor requests (PID 0x488B)
// ---------------------------------------------------------------------------

#define KUSB_REQ_STATUS         0xB0   // IN  — read status / response block
#define KUSB_REQ_CMDBLOCK       0xB1   // OUT — register-write command block
#define KUSB_REQ_BEGIN_CMD      0xB3   // OUT — begin command (ATN) transfer
#define KUSB_REQ_PULSE_IFC      0xB4   // OUT — pulse IFC (width timed in firmware)
// Go To Standby (release ATN). This is `ibgts` — traced from gpib-32.dll's
// ibgts() -> IOCTL 0xA0002C -> bRequest 0xB7 — and it is REQUIRED between the
// addressing command bytes and the data phase of every transfer. Confirmed on
// hardware: returns status 02 01 (the value the vendor driver itself checks
// for) and flips ADSR's NATN bit, releasing ATN. This is the firmware's own
// standby operation; it replaces the AUXMR=AUX_GTS register poke this driver
// used to do by hand. STALLs if issued while a transfer is armed.
#define KUSB_REQ_GTS            0xB7   // OUT — go to standby (ibgts)
#define KUSB_REQ_BEGIN_WRITE    0xB9   // OUT — begin data write
#define KUSB_REQ_BEGIN_READ     0xBA   // OUT — begin data read
#define KUSB_REQ_ABORT          0xBB   // OUT — abort current transfer
// 0xBD is bidirectional: IN reads a register, OUT writes one. wValue selects
// the register in both directions.
#define KUSB_REQ_REG            0xBD

// wValue for KUSB_REQ_BEGIN_CMD is the system-controller flag. In the vendor
// driver it is `ctx+0x2df & 1` — the same bit IBSIC tests before it will
// assert IFC, i.e. "am I the system controller". We always are.
#define KUSB_CMD_SYSTEM_CONTROLLER  1

// wValue for KUSB_REQ_BEGIN_READ is always 0x100; wIndex carries the EOS char.
// wIndex MUST be passed unconditionally (not gated on REOS) — the vendor sends
// its persisted EOS byte (ctx+0xC1) every time. With wIndex = 0 the firmware
// never terminates the transfer: it reports err=5 (timeout) with count=0 and
// delivers nothing. This single field was the whole firmware-read bug.
#define KUSB_READ_WVALUE        0x0100
// wValue for KUSB_REQ_BEGIN_WRITE is send_eoi << 8.
#define KUSB_WRITE_EOI_SHIFT    8

// ---------------------------------------------------------------------------
// Bulk pipe indices (positions in the interface's endpoint descriptor list,
// matching USBD_INTERFACE_INFORMATION.Pipes[] in the Windows driver)
// ---------------------------------------------------------------------------

#define KUSB_PIPE_DATA_OUT      2
#define KUSB_PIPE_CMD_OUT       3
#define KUSB_PIPE_DATA_IN       4
#define KUSB_PIPE_COUNT_MIN     5

// ---------------------------------------------------------------------------
// Transfer header — 8 bytes, sent as the payload of 0xB3 / 0xB9 / 0xBA
// ---------------------------------------------------------------------------
//
//   byte 0..2 : timeout in milliseconds, 24-bit little-endian
//   byte 3    : zero
//   byte 4..7 : transfer length, 32-bit little-endian
//
#define KUSB_XFER_HDR_LEN       8

// Status block lengths differ per operation (the firmware returns a
// differently-sized record for each).
#define KUSB_STATUS_LEN_CMD     7
#define KUSB_STATUS_LEN_WRITE   6
// 0x0B is the length REQUESTED for the read status block; the device actually
// returns 7 bytes: [state, err, count32(2..5), END(6)]. Do not require 0x0B
// bytes back or every read looks like a short-packet failure.
#define KUSB_STATUS_LEN_READ    0x0B
#define KUSB_STATUS_MIN_READ    7
#define KUSB_STATUS_LEN_IFC     1

// Status block layout (common prefix):
//   [0] state: 1 = busy, 2 = complete
//   [1] error code (5 = timeout/abort in the driver's handling)
//   [2..5] bytes transferred, 32-bit little-endian
//   [6] END flag on reads: 1 = EOI or EOS terminated the transfer
// Transferred-count width in the status block, per operation. Commands report
// 16 bits at bytes 2..3; data transfers report 32 bits at bytes 2..5.
#define KUSB_COUNT_WIDTH_CMD    2
#define KUSB_COUNT_WIDTH_DATA   4

#define KUSB_STATE_BUSY         1
#define KUSB_STATE_DONE         2
#define KUSB_ERR_ABORTED        5

// Command bytes are limited to this many per 0xB3 transfer.
#define KUSB_MAX_CMD_BYTES      0x200

// Maximum bytes per 0xB9 data-write transfer.
//
// RESOLVED 2026-08-28: the ~85-byte un-chunked write ceiling was never a
// firmware limit. It was pollStatus() giving up too early.
//
// The device stops answering status requests while it is busy with a transfer,
// for longer as the transfer grows. pollStatus() allowed 5 retries of
// IOSleep(2) -- a 10 ms budget -- and then returned GPIBT_ERR_IO with
// count = 0, which surfaced as a write that "failed above ~85 bytes". The
// giveaway was that the threshold MOVED between host builds (85/90, then
// exactly 90 OK / 91 fail): a firmware buffer limit cannot do that, a timing
// limit can. tools/kusb_harness.py never hit it because its equivalent budget
// is 5 x 5 ms and it sleeps between polls -- which is the whole of the
// long-standing "harness and driver diverge" mystery.
//
// With the budget made time-based (kStatusGlitchBudgetNs) and a 1 ms breath
// between BUSY polls, un-chunked writes now match the harness exactly: 90 /
// 100 / 200 / 400 / 700 / 1000 bytes all clean, 1000 B in 153.7 ms
// (~154 us/byte), empty instrument error queue, healthy device after each.
//
// 64 is now a choice rather than a workaround, and it is NOT free: measured on
// a 1000-byte write, chunk 64 costs ~18% against no chunking --
//   chunk 64  182 us/B | chunk 256  162 | chunk 512  157 | unchunked  154
// An earlier claim here that chunking "costs nothing measurable" was wrong; it
// came from measuring reads (one transfer, one poll loop) and un-chunked
// writes, never a large chunked write.
//
// Kept at 64 anyway, for now: the overhead is modest, chunking is transparent
// to GPIB framing (verified by splitting a command across a chunk boundary),
// and this ceiling has been misdiagnosed twice already. Raising it to 512 is
// the obvious win if a soak backs it up. The runtime override
// `gpibctl chunk <n>` (0 = no chunking) exists to test exactly that without a
// rebuild-and-replug cycle per experiment.
#define KUSB_MAX_DATA_CHUNK     64

// ---------------------------------------------------------------------------
// Command blocks (request 0xB1)
// ---------------------------------------------------------------------------
//
// Payloads are streams of 2-byte pairs `05 <value>`. Register 5 is the
// NEC7210 AUXMR position and the values match standard 7210 auxiliary
// programming, so the FPGA core is very likely 7210-compatible.
//
#define KUSB_CMD_CDOR           0x00   // command/data out register
#define KUSB_CMD_ISR2           0x02   // interrupt status 2 (read)
#define KUSB_CMD_ADMR           0x04   // address mode register (write)
#define KUSB_CMD_ADR            0x06   // address register (write)

// This core does NOT self-address from its own MTA/MLA on the bus the way a
// real NEC7210 does — after sending MTA(own address), ADSR's TA bit stays
// clear and no data can be sent. Declaring talk-only / listen-only works and
// is verified on hardware: ADMR=TON gives ADSR 0x82 (CIC|TA) and the next
// CDOR write handshakes (ISR1 DO set).
#define KUSB_ADMR_ADM0          0x01   // addressing mode 1
#define KUSB_ADMR_TRM           0x30   // TRM0 | TRM1, as linux-gpib sets
#define KUSB_ADMR_LON           0x40   // listen only
#define KUSB_ADMR_TON           0x80   // talk only
#define KUSB_CMD_AUXMR          0x05

// ISR2 bit 3: command output ready — the previous byte has been handshaked.
#define KUSB_ISR2_CO            0x08

#define KUSB_CMD_DIR            0x00   // data in register (read side of CDOR)
#define KUSB_CMD_ISR1           0x01   // interrupt status 1 (read)
#define KUSB_ISR1_DI            0x01   // a byte arrived
#define KUSB_ISR1_DO            0x02   // ready for the next byte out
#define KUSB_ISR1_END           0x10   // EOI or EOS seen with this byte

// Address status register — read side of the same index as ADMR. linux-gpib
// (nec7210_take_control / nec7210_go_to_standby) never assumes AUX_TCA/GTS
// took effect: it polls this register for NATN to actually flip before
// proceeding. Our driver did not do this at all — worth checking, since a
// silent take-control failure would explain command bytes going nowhere.
#define KUSB_CMD_ADSR           0x04   // address status register (read)
#define KUSB_ADSR_TA            0x02   // talker active
#define KUSB_ADSR_LA            0x04   // listener active
#define KUSB_ADSR_NATN          0x40   // NOT-ATN: 0 means ATN is asserted
#define KUSB_ADSR_CIC           0x80   // controller in charge

#define KUSB_AUX_SEOI           0x06   // assert EOI with the next byte
#define KUSB_AUX_GTS            0x10   // go to standby (release ATN)

// AUX_FH, "finish handshake" — releases an RFD (ready-for-data) holdoff.
//
// CONFIRMED, and this is the key to the long-standing "cycle 1 works, every
// cycle after it fails" bug. Our attach() init replays the vendor's block
// verbatim, and one of those writes is AUXMR=0x82 — that is AUXRA (0x80)
// with HR_HLDE (0x02), i.e. **hold off RFD when END is received**. So every
// read that terminates on EOI leaves this chip holding NRFD low. While NRFD
// is held, *nothing* can handshake on the bus — not the next cycle's data,
// and not even its UNL/UNT command bytes, which is why the failure surfaces
// first as the *write* of the following cycle failing to get ISR2's CO bit.
// It never self-heals, because only AUX_FH clears it, and a replug "fixes"
// it only because attach() resets the chip.
//
// This is also why the holdoff was invisible to every ADSR-based
// investigation: RFD holdoff is acceptor-handshake state and is not
// reflected in ADSR's CIC/TA/LA/NATN bits at all.
//
// linux-gpib does exactly this: nec7210_release_rfd_holdoff() issues AUX_FH,
// and pio_read() calls it after each byte, with nec7210_read() also
// releasing a stale holdoff before starting.
//
// Fire it ONLY where a holdoff is actually known to be pending (for HLDE,
// that means right after END was received). Issuing it when no handshake is
// pending force-completes one that isn't there and fabricates a phantom
// byte — an earlier attempt did that unconditionally on every transfer and
// had to be reverted twice.
#define KUSB_AUX_FH             0x03   // finish handshake / release RFD holdoff

// ADR write format: bit7 ARS (0 = ADR0, 1 = ADR1), bit6 DT, bit5 DL.
#define KUSB_ADR_ARS            0x80
#define KUSB_ADR_DISABLE_TL     0x60   // DT | DL

// NEC7210 auxiliary commands we issue by name. Taking control is not
// optional: pulsing IFC alone leaves the board off-CIC, and every command
// transfer then completes having moved zero bytes.
// "Immediate Execute pon" — releases the 7210 from power-on hold. Without it
// the chip answers register reads and accepts writes but ignores the bus
// entirely: never controller-in-charge, never handshakes, and the bus-line
// register reads 0x00. linux-gpib issues this as the last step of
// nec7210_board_online().
#define KUSB_AUX_PON            0x00

#define KUSB_AUX_SIFC           0x1E   // assert IFC
#define KUSB_AUX_CIFC           0x16   // release IFC
#define KUSB_AUX_TCA            0x11   // take control asynchronously
#define KUSB_AUX_TCS            0x12   // take control synchronously

// Auxiliary values observed in the Windows init path.
#define KUSB_AUX_CLEAR          0x02
#define KUSB_AUX_ONLINE         0x20   // sent by the go-online path

#endif /* kusb_488b_protocol_h */
