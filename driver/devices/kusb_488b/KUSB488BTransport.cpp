//
//  KUSB488BTransport.cpp
//  darwin-gpib driver
//

#include <os/log.h>
#include <string.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/IOUSBHostPipe.h>

#include "KUSB488BTransport.h"
#include "kusb_488b_protocol.h"
#include "gpib_user.h"    // BusXXX / ValidXXX line masks

namespace {

constexpr uint32_t kControlTimeoutMs = 1000;
constexpr uint32_t kStatusPollLimit  = 100000;   // safety bound on poll loops

// NEC7210 auxiliary commands. The FPGA core exposes AUXMR at register 5 and
// uses the standard encoding — confirmed from the Windows driver, which
// computes `enable ? 0x1F : 0x17` for REN and `enable ? 0x09 : 0x01` for ist.
constexpr uint8_t kAuxChipReset = 0x02;
constexpr uint8_t kAuxSetREN    = 0x1F;
constexpr uint8_t kAuxClearREN  = 0x17;

// Register selectors used with request 0xBD. These are 16-bit; the high byte
// appears to select a bank, with 0x107 being the bus-line status register.
constexpr uint16_t kRegAUXMR    = 0x0005;
constexpr uint16_t kRegBusLines = 0x0107;

uint32_t mapIOReturn(kern_return_t ret) {
    if (ret == kIOReturnSuccess) return GPIBT_OK;
    if (ret == kIOReturnTimeout) return GPIBT_ERR_TIMEOUT;
    if (ret == kIOReturnAborted) return GPIBT_ERR_ABORTED;
    if (ret == kIOReturnNotOpen) return GPIBT_ERR_NOT_READY;
    return GPIBT_ERR_IO;
}

// FIRMWARE PIPELINE IS NOW THE DEFAULT (2026-08-28).
//
// The firmware read is fixed. Three things were wrong, all now corrected:
//
//   1. wIndex on 0xBA (BEGIN_READ) must carry the EOS byte unconditionally.
//      With wIndex = 0 the engine never terminates: err=5 (timeout),
//      count=0, no data. This one field was the entire read bug.
//   2. Go-To-Standby between addressing and data must use the firmware's
//      own vendor request 0xB7 (== ibgts), not an AUXMR=AUX_GTS poke.
//   3. Bring-up must program ADMR to a valid addressing mode (mode 1).
//      Without it the core never asserts TA/LA from its own MTA/MLA, which
//      is what all the old per-transfer AUX_TCA / ADMR=TON pokes were
//      compensating for. Those pokes are now gone.
//
// Validated on tools/kusb_harness.py against a Keithley 2000 before this
// flag was flipped: 60x identical *IDN?; 10 rounds of 7 mixed-length
// queries; write-only bursts; 20x alternating long/short; capacity edge
// cases; multi-packet replies of 114/285/570/1140 bytes (the 1140 case
// exceeds the firmware's internal 1029-byte chunk clamp, so real chunking
// is exercised); and a randomized 120-cycle soak over sizes 57..1140 --
// zero failures. Truncated reads behave correctly too: partial data with
// END=0, the remainder stays in the instrument's output queue, and the
// adapter recovers on its own without a replug.
//
// END (status byte 6) now reports correctly on this path, which was the
// original motivation for the whole effort -- the register path's latched
// ISR1 END bit breaks pyvisa's chunked-read loop, and this one does not.
//
// If you need to fall back, set this to false: the register path is still
// present, still correct, and still passes its own 40/40 soak.
constexpr bool kUseFirmwarePipeline = true;

uint32_t msFromUsec(uint32_t timeout_us) {
    if (timeout_us == 0xFFFFFFFFu) return 0;   // infinite
    uint32_t ms = timeout_us / 1000;
    if (timeout_us != 0 && ms == 0) ms = 1;    // matches the Windows driver
    return ms;
}

// How many 1 ms poll iterations a caller's timeout is worth.
//
// The register-level loops below poll with IOSleep(1) between reads, so the
// iteration count *is* the timeout in milliseconds. These used to be
// hard-coded at 500, i.e. every transfer effectively gave up after half a
// second no matter what the client asked for — an ibdev() of T3s got 0.5s,
// and a slow instrument looked like a dead bus. Honour the requested value,
// with a floor so a tiny or zero timeout still gets a fair chance and an
// infinite timeout stays bounded.
uint32_t pollIterationsFor(uint32_t timeout_us) {
    if (timeout_us == 0xFFFFFFFFu) return 30000;   // "infinite" -> 30 s cap
    uint32_t ms = timeout_us / 1000;
    if (ms < 100)   ms = 100;                      // floor
    if (ms > 30000) ms = 30000;                    // ceiling
    return ms;
}

}  // namespace

bool KUSB488BTransport::init(IOUSBHostInterface *interface,
                             IOService *owningService) {
    if (!interface || !owningService) return false;
    interface_   = interface;
    owner_       = owningService;
    device_      = nullptr;
    dataOutPipe_ = nullptr;
    cmdOutPipe_  = nullptr;
    dataInPipe_  = nullptr;
    outBuffer_   = nullptr;
    inBuffer_    = nullptr;
    ctrlBuffer_  = nullptr;
    dataOutEP_   = 0;
    cmdOutEP_    = 0;
    dataInEP_    = 0;
    renAsserted_ = false;
    rfdHoldoffPending_ = false;
    return true;
}

void KUSB488BTransport::free() {
    closePipes();
    OSSafeReleaseNULL(outBuffer_);
    OSSafeReleaseNULL(inBuffer_);
    OSSafeReleaseNULL(ctrlBuffer_);
    if (device_) {
        device_->Close(owner_, 0);
        OSSafeReleaseNULL(device_);
    }
    interface_ = nullptr;
    owner_ = nullptr;
}

// The Windows driver addresses bulk pipes by their index in
// USBD_INTERFACE_INFORMATION.Pipes[], which is the order the endpoint
// descriptors appear in the interface. Resolve those indices to the
// bEndpointAddress values DriverKit's CopyPipe wants.
uint32_t KUSB488BTransport::resolveEndpoints() {
    const IOUSBConfigurationDescriptor *cfg =
        interface_->CopyConfigurationDescriptor();
    if (!cfg) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: no configuration descriptor");
        return GPIBT_ERR_NOT_READY;
    }
    const IOUSBInterfaceDescriptor *ifd = interface_->GetInterfaceDescriptor(cfg);
    if (!ifd) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: no interface descriptor");
        return GPIBT_ERR_NOT_READY;
    }

    // Walk the configuration descriptor by hand. DriverKit does not export
    // an endpoint iterator, and the raw USB descriptor layout is stable:
    // every descriptor is {bLength, bDescriptorType, ...}, a configuration
    // descriptor carries wTotalLength at bytes 2..3, an interface descriptor
    // (type 4) carries bInterfaceNumber at byte 2, and an endpoint descriptor
    // (type 5) carries bEndpointAddress at byte 2.
    const uint8_t *raw   = (const uint8_t *)cfg;
    const uint8_t *ifraw = (const uint8_t *)ifd;
    uint32_t total = (uint32_t)raw[2] | ((uint32_t)raw[3] << 8);
    const uint8_t wantIface = ifraw[2];
    const uint8_t wantAlt   = ifraw[3];

    uint8_t addrs[8];
    uint32_t count = 0;
    bool inOurInterface = false;

    for (uint32_t off = 0; off + 2 <= total; ) {
        uint8_t bLength = raw[off];
        uint8_t bType   = raw[off + 1];
        if (bLength < 2 || off + bLength > total) break;

        if (bType == 4 /* interface */) {
            inOurInterface = (bLength >= 4 && raw[off + 2] == wantIface &&
                              raw[off + 3] == wantAlt);
        } else if (bType == 5 /* endpoint */ && inOurInterface) {
            if (bLength >= 3 && count < 8) addrs[count++] = raw[off + 2];
        }
        off += bLength;
    }

    if (count < KUSB_PIPE_COUNT_MIN) {
        os_log(OS_LOG_DEFAULT,
               "kusb_488b: expected at least %u endpoints, found %u",
               KUSB_PIPE_COUNT_MIN, count);
        return GPIBT_ERR_NOT_READY;
    }

    dataOutEP_ = addrs[KUSB_PIPE_DATA_OUT];
    cmdOutEP_  = addrs[KUSB_PIPE_CMD_OUT];
    dataInEP_  = addrs[KUSB_PIPE_DATA_IN];

    os_log(OS_LOG_DEFAULT,
           "kusb_488b: endpoints data-out=0x%02x cmd-out=0x%02x data-in=0x%02x",
           dataOutEP_, cmdOutEP_, dataInEP_);

    // Sanity-check directions; a mismatch means the index assumption is wrong
    // for this unit and we should not drive it.
    if ((dataOutEP_ & 0x80) || (cmdOutEP_ & 0x80) || !(dataInEP_ & 0x80)) {
        os_log(OS_LOG_DEFAULT,
               "kusb_488b: endpoint directions do not match expected layout");
        return GPIBT_ERR_NOT_READY;
    }
    return GPIBT_OK;
}

uint32_t KUSB488BTransport::openPipes() {
    kern_return_t ret;

    ret = interface_->CopyPipe(dataOutEP_, &dataOutPipe_);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: CopyPipe data-out failed 0x%x", ret);
        return mapIOReturn(ret);
    }
    ret = interface_->CopyPipe(cmdOutEP_, &cmdOutPipe_);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: CopyPipe cmd-out failed 0x%x", ret);
        return mapIOReturn(ret);
    }
    ret = interface_->CopyPipe(dataInEP_, &dataInPipe_);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: CopyPipe data-in failed 0x%x", ret);
        return mapIOReturn(ret);
    }

    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                           kBulkBufferSize, 0, &outBuffer_);
    if (ret != kIOReturnSuccess) return mapIOReturn(ret);
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                           kBulkBufferSize, 0, &inBuffer_);
    if (ret != kIOReturnSuccess) return mapIOReturn(ret);
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                           64, 0, &ctrlBuffer_);
    if (ret != kIOReturnSuccess) return mapIOReturn(ret);
    return GPIBT_OK;
}

void KUSB488BTransport::closePipes() {
    OSSafeReleaseNULL(dataOutPipe_);
    OSSafeReleaseNULL(cmdOutPipe_);
    OSSafeReleaseNULL(dataInPipe_);
}

uint32_t KUSB488BTransport::attach() {
    kern_return_t ret = interface_->CopyDevice(&device_);
    if (ret != kIOReturnSuccess || !device_) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: CopyDevice failed 0x%x", ret);
        return mapIOReturn(ret);
    }
    ret = device_->Open(owner_, 0, 0);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: device Open failed 0x%x", ret);
        OSSafeReleaseNULL(device_);
        return mapIOReturn(ret);
    }

    uint32_t rc = resolveEndpoints();
    if (rc != GPIBT_OK) return rc;
    rc = openPipes();
    if (rc != GPIBT_OK) return rc;

    rc = runBringUpSequence();
    if (rc != GPIBT_OK) return rc;

    os_log(OS_LOG_DEFAULT, "kusb_488b: transport attached");
    return GPIBT_OK;
}

// The chip bring-up, factored out of attach() so it can be re-run to recover
// a wedged core without needing the USB device to be unplugged. This is
// exactly what attach() has always done, byte for byte — it starts with
// AUX_CR (chip reset) and ends with AUX_PON, so re-running it returns the
// core to the same state a fresh match produces.
uint32_t KUSB488BTransport::runBringUpSequence() {
    // Clear any armed-but-abandoned firmware transfer and any halted bulk
    // endpoint left over from a previous session before touching the chip.
    // This is done ONCE here, per bring-up -- not per transfer. Doing it per
    // transfer disturbs the engine mid-session (ENOL on the read following a
    // good write); omitting it entirely leaves a stale armed transfer that
    // makes the very first write report count=0. The validated harness
    // sequence is exactly this: recover once, then bring up, then transfer.
    recoverFirmwareEngine();

    // Board init, mirroring the Windows driver's start-up blocks. Each block
    // is a stream of `05 <value>` AUXMR writes.
    static const uint8_t kInitBlockA[] = {
        0x05, 0x02, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00
    };
    static const uint8_t kInitBlockB[] = { 0x05, 0x01, 0x02 };
    static const uint8_t kInitBlockC[] = {
        0x05, 0x70, 0x05, 0x82, 0x05, 0xC0, 0x05, 0xA8,
        0x05, 0xC0, 0x05, 0xD0, 0x05, 0x49, 0x05, 0xE1
    };

    uint32_t rc = writeCommandBlock(kInitBlockA, sizeof(kInitBlockA));
    if (rc != GPIBT_OK) return rc;
    rc = writeCommandBlock(kInitBlockB, sizeof(kInitBlockB));
    if (rc != GPIBT_OK) return rc;
    rc = writeCommandBlock(kInitBlockC, sizeof(kInitBlockC));
    if (rc != GPIBT_OK) return rc;

    // Go online.
    static const uint8_t kOnlineBlock[] = { KUSB_CMD_AUXMR, KUSB_AUX_ONLINE };
    rc = writeCommandBlock(kOnlineBlock, sizeof(kOnlineBlock));
    if (rc != GPIBT_OK) return rc;

    // Program the board address. Without this the address register comes up
    // with DT|DL set — talker and listener both disabled — so the board can
    // never address itself and command sequences go nowhere.
    rc = writeRawRegister(KUSB_CMD_ADR, KUSB_ADR_ARS | KUSB_ADR_DISABLE_TL);
    if (rc != GPIBT_OK) return rc;                  // ADR1: no secondary
    rc = writeRawRegister(KUSB_CMD_ADR, 0x00);      // ADR0: address 0, T/L on
    if (rc != GPIBT_OK) return rc;

    // Program the addressing mode. Without this ADMR stays 0, which is not a
    // valid NEC7210 addressing mode, and the core's address decoder never
    // asserts TA/LA — the board can be sent its own MTA/MLA and simply will
    // not become talker or listener. That is the real origin of the
    // long-standing "this core does not self-address from its own MTA" belief
    // recorded in this file's history: it was our incomplete init, not a core
    // quirk. With this write, addressing produces ADSR 0x82 (CIC|TA) for a
    // write and 0x84 (CIC|LA) for a read, and every per-transfer AUX_TCA /
    // ADMR=TON / ADMR=LON poke this driver used to need becomes unnecessary.
    //
    // CAVEAT, deliberately recorded: the vendor driver never writes register 4
    // anywhere on its normal path — this was searched exhaustively (all seven
    // callers of its 0xBD register-write helper, its only 0xB1 command-block
    // sender, and every other request it issues). So this value is NOT a
    // replication of vendor behaviour; it is our own, empirically-derived and
    // heavily-tested substitute for whatever the vendor relies on instead.
    // See reverse/notes/02-usb-protocol.md.
    rc = writeRawRegister(KUSB_CMD_ADMR,
                          KUSB_ADMR_ADM0 | KUSB_ADMR_TRM);   // 0x31
    if (rc != GPIBT_OK) return rc;

    // Release the chip from power-on hold. This is the last thing
    // nec7210_board_online() does, and omitting it leaves a core that looks
    // healthy over the register interface while ignoring the GPIB bus.
    rc = writeRawRegister(KUSB_CMD_AUXMR, KUSB_AUX_PON);
    if (rc != GPIBT_OK) return rc;

    // The chip reset at the head of this sequence also clears any pending RFD
    // holdoff — the specific latched state that used to require a physical
    // replug — so our record of it must be cleared too.
    rfdHoldoffPending_ = false;
    return GPIBT_OK;
}

// Recovery level 1: re-run the bring-up. Clears every latched bit in the
// core — RFD holdoff, stale TA/LA, handshake state — without disturbing USB.
uint32_t KUSB488BTransport::softReset() {
    os_log(OS_LOG_DEFAULT, "kusb_488b: soft reset (re-running bring-up)");
    return runBringUpSequence();
}

// Recovery level 2: make the USB device re-enumerate. This terminates this
// dext instance and everything under the device; IOKit then re-matches and a
// brand-new transport gets attach()ed. It is the programmatic equivalent of
// unplugging and replugging the adapter, and is the last resort when a soft
// reset cannot revive the core.
//
// Note the SDK's warning that Reset() must not be called from the port
// workloop thread; user-client calls do not run on it.
uint32_t KUSB488BTransport::resetDevice() {
    if (!device_) return GPIBT_ERR_NOT_READY;
    os_log(OS_LOG_DEFAULT, "kusb_488b: forcing USB re-enumeration");
    kern_return_t ret = device_->Reset();
    return mapIOReturn(ret);
}

void KUSB488BTransport::detach() {
    if (device_) abortTransfer();
    closePipes();
}

// ---------------------------------------------------------------------------
// Control transfers
// ---------------------------------------------------------------------------

uint32_t KUSB488BTransport::controlOut(uint8_t request, uint16_t wValue,
                                       uint16_t wIndex, const uint8_t *data,
                                       uint16_t len) {
    if (!device_ || !owner_) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: controlOut req=0x%02x with no device", request);
        return GPIBT_ERR_NOT_READY;
    }

    IOMemoryDescriptor *md = nullptr;
    if (len > 0) {
        if (!ctrlBuffer_ || len > 64) {
            os_log(OS_LOG_DEFAULT, "kusb_488b: controlOut req=0x%02x bad len %u",
                   request, len);
            return GPIBT_ERR_IO;
        }
        uint64_t addr = 0, sz = 0;
        kern_return_t kr = ctrlBuffer_->Map(0, 0, 0, 0, &addr, &sz);
        if (kr != kIOReturnSuccess) return mapIOReturn(kr);
        memcpy((void *)addr, data, len);
        md = ctrlBuffer_;
    }

    uint16_t transferred = 0;
    kern_return_t kr = device_->DeviceRequest(owner_,
                                              0x40,   // OUT | VENDOR | DEVICE
                                              request, wValue, wIndex, len,
                                              md, &transferred,
                                              kControlTimeoutMs);
    if (kr != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT,
               "kusb_488b: control OUT req=0x%02x failed 0x%x", request, kr);
        return mapIOReturn(kr);
    }
    if (transferred != len) {
        os_log(OS_LOG_DEFAULT,
               "kusb_488b: controlOut req=0x%02x short: sent %u of %u",
               request, transferred, len);
        return GPIBT_ERR_IO;
    }
    return GPIBT_OK;
}

uint32_t KUSB488BTransport::controlIn(uint8_t request, uint16_t wValue,
                                      uint16_t wIndex, uint8_t *data,
                                      uint16_t len, uint16_t *outLen) {
    if (!device_ || !owner_ || !ctrlBuffer_) return GPIBT_ERR_NOT_READY;
    if (len > 64) return GPIBT_ERR_IO;

    uint16_t transferred = 0;
    kern_return_t kr = device_->DeviceRequest(owner_,
                                              0xC0,   // IN | VENDOR | DEVICE
                                              request, wValue, wIndex, len,
                                              ctrlBuffer_, &transferred,
                                              kControlTimeoutMs);
    if (kr != kIOReturnSuccess) {
        if (outLen) *outLen = 0;
        return mapIOReturn(kr);
    }

    uint64_t addr = 0, sz = 0;
    kr = ctrlBuffer_->Map(0, 0, 0, 0, &addr, &sz);
    if (kr != kIOReturnSuccess) {
        if (outLen) *outLen = 0;
        return mapIOReturn(kr);
    }
    if (transferred > len) transferred = len;
    memcpy(data, (const void *)addr, transferred);
    if (outLen) *outLen = transferred;
    return GPIBT_OK;
}

// ---------------------------------------------------------------------------
// Bulk transfers
// ---------------------------------------------------------------------------

uint32_t KUSB488BTransport::bulkOut(IOUSBHostPipe *pipe, const uint8_t *bytes,
                                    uint32_t len, uint32_t timeout_us) {
    if (!pipe || !outBuffer_) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: bulkOut with no pipe (%p) or buffer",
               (void *)pipe);
        return GPIBT_ERR_NOT_READY;
    }

    uint32_t timeoutMs = msFromUsec(timeout_us);
    uint32_t done = 0;
    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > kBulkBufferSize) chunk = kBulkBufferSize;

        uint64_t addr = 0, sz = 0;
        kern_return_t kr = outBuffer_->Map(0, 0, 0, 0, &addr, &sz);
        if (kr != kIOReturnSuccess) return mapIOReturn(kr);
        memcpy((void *)addr, bytes + done, chunk);

        uint32_t transferred = 0;
        kr = pipe->IO(outBuffer_, chunk, &transferred, timeoutMs);
        if (kr != kIOReturnSuccess) {
            os_log(OS_LOG_DEFAULT, "kusb_488b: bulkOut failed 0x%x", kr);
            return mapIOReturn(kr);
        }
        if (transferred == 0) break;
        done += transferred;
    }
    if (done != len) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: bulkOut short: sent %u of %u", done, len);
        return GPIBT_ERR_IO;
    }
    return GPIBT_OK;
}

uint32_t KUSB488BTransport::bulkIn(IOUSBHostPipe *pipe, uint8_t *bytes,
                                   uint32_t capacity, uint32_t timeout_us,
                                   uint32_t *outBytesRead) {
    if (!pipe || !inBuffer_) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: bulkIn with no pipe or buffer");
        return GPIBT_ERR_NOT_READY;
    }
    if (capacity > kBulkBufferSize) capacity = kBulkBufferSize;

    uint32_t timeoutMs = msFromUsec(timeout_us);
    uint32_t transferred = 0;
    kern_return_t kr = pipe->IO(inBuffer_, capacity, &transferred, timeoutMs);
    if (kr != kIOReturnSuccess) {
        if (outBytesRead) *outBytesRead = 0;
        return mapIOReturn(kr);
    }

    uint64_t addr = 0, sz = 0;
    kr = inBuffer_->Map(0, 0, 0, 0, &addr, &sz);
    if (kr != kIOReturnSuccess) {
        if (outBytesRead) *outBytesRead = 0;
        return mapIOReturn(kr);
    }
    memcpy(bytes, (const void *)addr, transferred);
    if (outBytesRead) *outBytesRead = transferred;
    return GPIBT_OK;
}

// ---------------------------------------------------------------------------
// Transfer arming / status polling
// ---------------------------------------------------------------------------

uint32_t KUSB488BTransport::beginTransfer(uint8_t request, uint16_t wValue,
                                          uint16_t wIndex, uint32_t length,
                                          uint32_t timeout_us) {
    uint32_t ms = msFromUsec(timeout_us);
    uint8_t hdr[KUSB_XFER_HDR_LEN];
    hdr[0] = (uint8_t)(ms & 0xFF);
    hdr[1] = (uint8_t)((ms >> 8) & 0xFF);
    hdr[2] = (uint8_t)((ms >> 16) & 0xFF);
    hdr[3] = 0;
    hdr[4] = (uint8_t)(length & 0xFF);
    hdr[5] = (uint8_t)((length >> 8) & 0xFF);
    hdr[6] = (uint8_t)((length >> 16) & 0xFF);
    hdr[7] = (uint8_t)((length >> 24) & 0xFF);
    return controlOut(request, wValue, wIndex, hdr, KUSB_XFER_HDR_LEN);
}

// countWidth differs per operation and is not cosmetic: the command path
// reports a 16-bit transferred count at bytes 2..3, while write and read use
// 32 bits at bytes 2..5. Reading 32 bits from a command's status block pulls
// in the flag bytes, so the count never matches and every command looks like
// "no listener".
uint32_t KUSB488BTransport::pollStatus(uint32_t statusLen, uint32_t countWidth,
                                       uint32_t timeout_us,
                                       uint8_t *status, uint32_t *outCount,
                                       uint8_t *outEnd) {
    uint8_t buf[16];
    if (statusLen > sizeof(buf)) return GPIBT_ERR_IO;

    for (uint32_t spins = 0; spins < kStatusPollLimit; ++spins) {
        uint16_t got = 0;
        uint32_t rc = controlIn(KUSB_REQ_STATUS, 0, 0, buf,
                                (uint16_t)statusLen, &got);
        if (rc != GPIBT_OK) {
            // The very first status poll right after a bulk-out sometimes
            // fails at the USB transfer level (endpoint 0 still finishing
            // up from the just-completed bulk transfer). Retry a few times
            // before giving up instead of failing on the first glitch.
            if (spins < 5) { IOSleep(2); continue; }
            return GPIBT_ERR_IO;
        }
        if (got < statusLen) {
            // Same transient-glitch treatment as a hard controlIn failure.
            if (spins < 5) { IOSleep(2); continue; }
            return GPIBT_ERR_IO;
        }

        if (buf[0] != KUSB_STATE_BUSY) {
            if (status) memcpy(status, buf, statusLen);
            if (outCount) {
                uint32_t count = (uint32_t)buf[2] | ((uint32_t)buf[3] << 8);
                if (countWidth == 4 && statusLen >= 6) {
                    count |= ((uint32_t)buf[4] << 16) | ((uint32_t)buf[5] << 24);
                }
                *outCount = count;
            }
            if (outEnd && statusLen > 6) *outEnd = buf[6];
            if (buf[0] != KUSB_STATE_DONE) {
                return (buf[1] == KUSB_ERR_ABORTED) ? GPIBT_ERR_TIMEOUT
                                                    : GPIBT_ERR_IO;
            }
            return GPIBT_OK;
        }
    }
    (void)timeout_us;
    abortTransfer();
    return GPIBT_ERR_TIMEOUT;
}

uint32_t KUSB488BTransport::abortTransfer() {
    return controlOut(KUSB_REQ_ABORT, 0, 0, nullptr, 0);
}

// Recovery for a firmware transfer that failed part-way. Measured on
// hardware (tools/kusb_harness.py, see reverse/notes/02-usb-protocol.md):
// a failed transfer leaves an armed-but-never-serviced status the NEXT
// transfer's poll reports by mistake, and leaves the bulk endpoint it used
// STALLed, so every following bulk transfer on that pipe fails outright.
// Neither runBringUpSequence() (7210 core reset) nor resetDevice() (USB
// port reset) clears either one -- both measured, neither helps. This is
// also exactly what the vendor driver does on its own failure path
// (KUSB488B_X64.sys FUN_1400036e0/FUN_140002e1c: 0xBB abort, then reset
// the pipe) -- mirroring it, not a workaround. Cheap and harmless to call
// when nothing is actually wrong.
void KUSB488BTransport::recoverFirmwareEngine() {
    abortTransfer();
    if (dataOutPipe_) dataOutPipe_->ClearStall(false);
    if (cmdOutPipe_)  cmdOutPipe_->ClearStall(false);
    if (dataInPipe_)  dataInPipe_->ClearStall(false);
}

// ---------------------------------------------------------------------------
// Register access
// ---------------------------------------------------------------------------

uint32_t KUSB488BTransport::readRegister(uint8_t reg, uint8_t *outValue) {
    uint8_t v = 0;
    uint16_t got = 0;
    uint32_t rc = controlIn(KUSB_REQ_REG, reg, 0, &v, 1, &got);
    if (rc != GPIBT_OK) return rc;
    if (got < 1) return GPIBT_ERR_IO;
    if (outValue) *outValue = v;
    return GPIBT_OK;
}

uint32_t KUSB488BTransport::writeCommandBlock(const uint8_t *block, uint32_t len) {
    if (!block || len == 0 || len > 64) return GPIBT_ERR_IO;
    return controlOut(KUSB_REQ_CMDBLOCK, 0, 0, block, (uint16_t)len);
}

uint32_t KUSB488BTransport::writeAux(uint8_t value) {
    return writeRawRegister(KUSB_CMD_AUXMR, value);
}

// ---------------------------------------------------------------------------
// IGPIBTransport
// ---------------------------------------------------------------------------

// Matches the vendor exactly (KUSB488B_X64.sys FUN_140002ddc): request 0xBD
// OUT, wValue = register index, one data byte. This is the vendor's ONLY
// per-transfer register-write primitive -- init uses the 0xB1 command-block
// path (writeCommandBlock) for its multi-pair blocks, but every single-
// register poke issued around a transfer goes through 0xBD.
//
// CONFIRMED on hardware this is not just vendor-accurate but functionally
// necessary: 0xB1 STALLs (Pipe error) while a firmware transfer is armed,
// while 0xBD keeps working right through it. Code that used 0xB1 here for
// per-transfer pokes (take-control, bus-role, ATN release) produced
// exactly the "adapter looks wedged, even unrelated register writes fail"
// symptom this fixes. See reverse/notes/02-usb-protocol.md.
uint32_t KUSB488BTransport::writeRawRegister(uint16_t reg, uint8_t value) {
    return controlOut(KUSB_REQ_REG, reg, 0, &value, 1);
}

// Written through the 0xB1 command-block path rather than the 0xBD register
// path, because that is exactly how the vendor driver programs AUXMR during
// init and it is the one we have seen the hardware accept.
uint32_t KUSB488BTransport::takeControl(bool synchronous) {
    const uint8_t block[] = {
        KUSB_CMD_AUXMR,
        (uint8_t)(synchronous ? KUSB_AUX_TCS : KUSB_AUX_TCA)
    };
    return writeCommandBlock(block, sizeof(block));
}

// "Go to standby" — release ATN without declaring a talk/listen role.
// GPIBBoard::listenerPresent() (ibln) needs this: the FindListener trick only
// works once ATN is released, because a genuinely addressed listener then
// holds NDAC waiting for a data byte that never comes.
//
// This used to be a plain AUXMR=GTS register poke, on the reasoning that a
// one-shot presence probe had no business going through the firmware's
// arm/poll machinery. That reasoning was wrong: measured 2026-08-28, the poke
// DOES NOT RELEASE ATN AT ALL. Bus lines read 0x42ff (NDAC|ATN) before
// addressing, after addressing, and after the poke, so ibln could never
// discriminate and pyvisa's list_resources() found zero instruments on a bus
// whose instrument answered *IDN? perfectly.
//
// The firmware owns ATN. It releases it itself inside the 0xB9/0xBA transfer
// engines -- which is why ordinary reads and writes work and only this
// standalone path was broken -- and request 0xB7 is how the host asks for it
// outside a transfer. That is exactly how the vendor uses it: 0xB7 is
// reachable in KUSB488B_X64.sys only from ibgts()/ibln()/FindLstn()
// (IOCTL 0xA0002C), never during ibwrt/ibrd. So this is the one place it
// belongs, and removing it from the per-transfer paths (commit 20adfd9) left
// goToStandby() written but unreachable.
uint32_t KUSB488BTransport::releaseAtn() {
    uint32_t rc = goToStandby();
    if (rc == GPIBT_OK) return rc;
    // Fall back to the register poke rather than failing outright: it is
    // useless for releasing ATN but harmless, and a hard error here would
    // turn a failed presence probe into a failed enumeration.
    return writeRawRegister(KUSB_CMD_AUXMR, KUSB_AUX_GTS);
}

// linux-gpib drives IFC through AUXMR (AUX_SIFC, hold, AUX_CIFC) rather than
// a firmware pulse request. The 0xB4 request reports success but leaves the
// board off-CIC, so this follows the reference sequence instead.
uint32_t KUSB488BTransport::pulseInterfaceClearViaAux() {
    uint32_t rc = writeRawRegister(KUSB_CMD_AUXMR, KUSB_AUX_SIFC);
    if (rc != GPIBT_OK) return rc;
    IOSleep(1);                                  // ≥100us assertion window
    return writeRawRegister(KUSB_CMD_AUXMR, KUSB_AUX_CIFC);
}

uint32_t KUSB488BTransport::pulseInterfaceClear() {
    // Prefer the AUXMR sequence: the firmware's 0xB4 pulse completes but does
    // not leave the board controller-in-charge.
    uint32_t auxRc = pulseInterfaceClearViaAux();
    if (auxRc == GPIBT_OK) return GPIBT_OK;

    uint32_t rc = controlOut(KUSB_REQ_PULSE_IFC, 0, 0, nullptr, 0);
    if (rc != GPIBT_OK) return rc;

    uint8_t status[KUSB_STATUS_LEN_IFC] = { 0 };
    uint16_t got = 0;
    rc = controlIn(KUSB_REQ_STATUS, 0, 0, status, KUSB_STATUS_LEN_IFC, &got);
    if (rc != GPIBT_OK) return rc;

    if (status[0] != KUSB_STATE_DONE) {
        // The Windows driver retries the pulse exactly once.
        rc = controlOut(KUSB_REQ_PULSE_IFC, 0, 0, nullptr, 0);
        if (rc != GPIBT_OK) return rc;
        rc = controlIn(KUSB_REQ_STATUS, 0, 0, status, KUSB_STATUS_LEN_IFC, &got);
        if (rc != GPIBT_OK) return rc;
        if (status[0] != KUSB_STATE_DONE) return GPIBT_ERR_IO;
    }
    return GPIBT_OK;
}

uint32_t KUSB488BTransport::setRemoteEnable(bool enable) {
    uint32_t rc = writeAux(enable ? kAuxSetREN : kAuxClearREN);
    if (rc == GPIBT_OK) renAsserted_ = enable;
    return rc;
}

// Command bytes go out a byte at a time through CDOR, waiting for ISR2's CO
// bit between bytes — the same thing linux-gpib's nec7210 does.
//
// The vendor firmware's bulk path (0xB3 + pipe 3 + status poll) would be
// faster, but once the chip is released from pon it reports no active
// transfer at all, and the register path is verified working: writing AUXMR
// drives ATN on and off exactly as expected. Correctness first; the bulk path
// can come back if its arm sequence is ever understood.
// Release an RFD holdoff, but only when one is actually outstanding.
//
// Why it matters: during a read *we* are the acceptor, so a standing holdoff
// means we are clamping NRFD low. The talker then waits forever and the read
// times out having received nothing — even though the instrument had a reply
// ready. The same clamp blocks the next transaction's command bytes, which is
// why the symptom usually shows up first as the *following* write failing.
//
// Why conditional: firing AUX_FH with no holdoff pending force-completes a
// handshake that was never started, and the chip hands back a byte that never
// arrived. Measured directly on hardware — an unconditional release at the
// top of a read turned every cycle into a single fabricated byte ('K', 'E',
// 'K', … — successive fragments of an earlier reply) reported as a clean
// CMPL|END. Silent corruption is worse than the timeout it replaces.
//
// linux-gpib guards the identical call the identical way, with
// RFD_HOLDOFF_BN, in nec7210_release_rfd_holdoff().
//
// The chip is left in AUXRA=HLDE by the bring-up attach() replays, so the
// holdoff arises at exactly one place: a read that terminated on END.
void KUSB488BTransport::releaseRfdHoldoffIfPending() {
    if (!rfdHoldoffPending_) return;
    writeRawRegister(KUSB_CMD_AUXMR, KUSB_AUX_FH);
    rfdHoldoffPending_ = false;
}

// --- Firmware bulk-transfer path -------------------------------------------
//
// Arm with a vendor request that carries an 8-byte {timeout_ms, length}
// header, bulk-transfer the payload, poll a status block over 0xB0. Traced
// from KUSB488B_X64.sys (FUN_140002e1c/FUN_1400036e0/FUN_140003184).
//
// CORRECTED 2026-08-27 (see reverse/notes/02-usb-protocol.md, sessions 3-6):
// the claim that used to live here -- "the firmware owns bus-role management
// for the whole transfer, no AUXMR/ADMR write needed" -- is FALSE for this
// hardware, confirmed on a pristine device with zero prior state. The pure
// vendor sequence (bring-up, then only 0xB3/0xB9/0xBA, no register writes at
// all) makes the write report DONE with count=0 every time: an arm that
// transfers nothing. This core does not self-address from its own MTA (see
// kusb_488b_protocol.h) -- that applies to the firmware engine too, not
// just the register path. What actually works, CONFIRMED 5/5 repeatable:
//
//   1. AUX_TCA (take control) before the 0xB3 addressing -- without it,
//      the write still reports count=0 even with everything else right.
//   2. 0xB3 addressing (UNL, MLA(target), MTA(self) for a write).
//   3. ADMR = TON|TRM -- declares this board the actual talker.
//   4. AUX_GTS (release ATN) -- the firmware command path leaves ATN
//      asserted afterward and does not release it itself; that is our job.
//   5. THEN arm 0xB9 and bulk out.
//
// All four register writes go through writeRawRegister(), which is the
// vendor's own 0xBD single-register primitive -- not 0xB1. This matters
// operationally, not just stylistically: 0xB1 STALLs while a transfer is
// armed, so a per-transfer poke that used it could itself fail and look
// like a wedged adapter. See recoverFirmwareEngine() and
// reverse/notes/02-usb-protocol.md for the "0xB1 vs 0xBD" finding.
//
// The read direction (0xBA) is NOT fixed by the analogous recipe (AUX_TCA +
// ADMR=LON|TRM + AUX_GTS before arming). It is a separate, deeper,
// UNRESOLVED bug: the 7210 core genuinely receives the reply byte (ISR1
// shows DI set) but the firmware's read engine still reports count=0 and
// times out, immune to every register-reachable variable tried. See the
// notes file for the full investigation, including a first pass at the
// 8051 firmware disassembly that did not resolve it.
//
// The addressing byte sequence itself (GPIBBoard::addressForWrite/Read) is
// UNCHANGED here and still includes UNT and the register-path's byte order
// -- that sequence is shared with the register pipeline, which has its own
// hardware-confirmed need for UNT (a stale ADSR TA bit across calls). The
// harness's confirmed-working firmware recipe used the vendor-exact
// sequence instead (no UNT, listener address before talker), traced from
// gpib-32.dll's ibwrt/ibrd. Whether the current shared sequence (with UNT)
// also works for the firmware write path together with the ADMR/TCA fixes
// above has NOT been separately verified on hardware -- only the vendor-
// exact sequence has. kUseFirmwarePipeline stays false, so this is dormant
// code either way; resolve this before ever flipping it, ideally by making
// the addressing sequence pipeline-aware rather than assuming either
// sequence works for both.
//
// `sendCommandBytes`/`writeData`/`readData` still all delegate to one
// pipeline or the other together, per kUseFirmwarePipeline -- mixing
// register and firmware within one attach() session is confirmed to
// corrupt reads (0xBD hands back a stale shadow of DIR once the firmware
// pipeline has touched the bus at all in that session), so this must stay
// an all-or-nothing switch until the read bug above is actually fixed.

// Go To Standby -- release ATN -- using vendor request 0xB7 (ibgts) rather
// than an AUXMR=AUX_GTS register poke. Traced from gpib-32.dll's ibgts()
// through IOCTL 0xA0002C to bRequest 0xB7, and confirmed on hardware: it
// returns a 2-byte status of 02 01 (exactly what the vendor driver checks
// for) and clears ATN, which shows up as NATN setting in ADSR.
//
// The vendor retries once on a bad status before giving up; mirrored here.
// Note this request STALLs if issued while a transfer is already armed, so
// it must come before beginTransfer(), never after.
uint32_t KUSB488BTransport::goToStandby() {
    for (int attempt = 0; attempt < 2; ++attempt) {
        uint32_t rc = controlOut(KUSB_REQ_GTS, 0, 0, nullptr, 0);
        if (rc != GPIBT_OK) return rc;

        uint8_t  st[4] = { 0 };
        uint16_t slen = 0;
        rc = controlIn(KUSB_REQ_STATUS, 0, 0, st, 2, &slen);
        if (rc == GPIBT_OK && slen >= 1 && st[0] == KUSB_STATE_DONE) {
            return GPIBT_OK;
        }
    }
    return GPIBT_ERR_IO;
}

uint32_t KUSB488BTransport::sendCommandBytesViaFirmware(const uint8_t *cmds, uint32_t len) {
    if (!cmds || len == 0) return GPIBT_OK;
    releaseRfdHoldoffIfPending();

    // NOTE: no recoverFirmwareEngine() here. This path sends addressing for
    // BOTH reads and writes, and aborting before read addressing tears it
    // down (ENOL on every read). Instead a write cleans up after itself --
    // see the end of writeDataViaFirmware().
    // NOTE: deliberately NO recoverFirmwareEngine() here. It sends 0xBB
    // (abort) + ClearStall on all three pipes; issued before the read
    // addressing it disturbs the engine and the following 0xB3 reports a
    // short count, surfacing as ENOL on every read. Recovery happens once in
    // runBringUpSequence() and thereafter only on a real failure (below),
    // which is exactly the sequence validated on the harness.
    //
    // This only works if nothing else is talking to the bus behind our back:
    // the app's device auto-scan is a second client and will leave a stale
    // armed transfer here. Launch the app with OPENGPIB_NO_AUTOSCAN=1 when
    // testing.

    // No AUX_TCA poke here any more. That used to be needed only because
    // bring-up left ADMR unprogrammed, so the core never became talker from
    // its own MTA; with ADMR set to addressing mode 1 in runBringUp() the
    // firmware handles ATN for the command phase itself, exactly as the
    // vendor driver (which never pokes AUXMR/ADMR) relies on.

    // No caller-supplied timeout for command bytes (IGPIBTransport's
    // sendCommandBytes takes none); 3 s matches what the register path
    // effectively allows via its spin counts.
    const uint32_t timeout_us = 3000000;

    uint32_t done = 0;
    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > KUSB_MAX_CMD_BYTES) chunk = KUSB_MAX_CMD_BYTES;

        // wValue is the system-controller flag (ctx+0x2df & 1 in the
        // vendor driver — the same bit IBSIC tests before asserting IFC).
        // We are always the system controller.
        uint32_t rc = beginTransfer(KUSB_REQ_BEGIN_CMD,
                                    KUSB_CMD_SYSTEM_CONTROLLER, 0,
                                    chunk, timeout_us);
        if (rc != GPIBT_OK) return rc;

        rc = bulkOut(cmdOutPipe_, cmds + done, chunk, timeout_us);
        if (rc != GPIBT_OK) { recoverFirmwareEngine(); return rc; }

        uint32_t count = 0;
        rc = pollStatus(KUSB_STATUS_LEN_CMD, KUSB_COUNT_WIDTH_CMD, timeout_us,
                        nullptr, &count, nullptr);
        if (rc != GPIBT_OK) return rc;
        if (count != chunk) return GPIBT_ERR_NO_LISTENER;
        done += chunk;
    }
    return GPIBT_OK;
}

uint32_t KUSB488BTransport::writeDataViaFirmware(const uint8_t *buf, uint32_t len,
                                                 bool send_eoi, uint32_t timeout_us,
                                                 uint32_t *outBytesWritten) {
    if (outBytesWritten) *outBytesWritten = 0;
    if (!buf || len == 0) return GPIBT_OK;
    releaseRfdHoldoffIfPending();

    // NO goToStandby() here, and none in readDataViaFirmware() either.
    //
    // 0xB7 is `ibgts`, and the vendor issues it ONLY from ibgts()/ibln()/
    // FindLstn() (IOCTL 0xA0002C) -- never during ibwrt or ibrd. Confirmed by
    // caller enumeration in KUSB488B_X64.sys: the sole caller of its 0xB7
    // sender is reachable only from that IOCTL.
    //
    // We used to send it on every transfer, which matters because 0xB7 primes
    // dispatch tag 0xB8 -- the only route to FUN_CODE_2020, the firmware's
    // transfer engine. That restarted the engine on every single transfer.
    //
    // It is not needed: the earlier "0xB7 is required" conclusion came from
    // runs where ADMR was still unprogrammed, which is the real reason those
    // failed. With ADMR = 0x31 set in runBringUpSequence(), removing 0xB7
    // measured strictly better on hardware -- 100-cycle randomised soak over
    // 2..1140 byte replies with zero failures, single writes clean to 1000 B,
    // and it repairs the short-preceding-write case of the back-to-back write
    // bug (a 3-part compound query after a 6 B write went from 114 B/1
    // response to the correct 171 B/2).
    uint32_t rc = GPIBT_OK;

    uint32_t done = 0;
    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > KUSB_MAX_DATA_CHUNK) chunk = KUSB_MAX_DATA_CHUNK;
        const bool lastChunk = (done + chunk == len);

        // wValue bit 8 is send_eoi, and matches the vendor exactly. EOI is
        // only meaningful with the last byte of the whole message, not of
        // every chunk, so it is only set on the final chunk.
        uint16_t wValue = (uint16_t)(((send_eoi && lastChunk) ? 1u : 0u)
                                     << KUSB_WRITE_EOI_SHIFT);
        rc = beginTransfer(KUSB_REQ_BEGIN_WRITE, wValue, 0, chunk, timeout_us);
        if (rc != GPIBT_OK) return rc;

        rc = bulkOut(dataOutPipe_, buf + done, chunk, timeout_us);
        if (rc != GPIBT_OK) { recoverFirmwareEngine(); return rc; }

        uint32_t count = 0;
        rc = pollStatus(KUSB_STATUS_LEN_WRITE, KUSB_COUNT_WIDTH_DATA, timeout_us,
                        nullptr, &count, nullptr);
        if (rc != GPIBT_OK) return rc;
        if (outBytesWritten) *outBytesWritten = done + count;
        if (count != chunk) return GPIBT_ERR_NO_LISTENER;
        done += chunk;
    }

    // NOT A DRIVER BUG, despite how this reads in earlier revisions of this
    // file. It used to say the second of two back-to-back writes was
    // "silently TRUNCATED". It is not: the writes are byte-exact.
    //
    // What actually happens is a K2000 message-exchange behaviour. After the
    // instrument receives a program message that is never followed by a read,
    // it answers the NEXT message's compound query as several separate
    // EOI-terminated response messages instead of one. The later ones stay in
    // its output queue and surface on the following read, which is where the
    // SCPI -410 "Query INTERRUPTED" comes from.
    //
    // Proof that the transport is innocent (2026-08-28, harness on hardware):
    //   * a 30 B command sent back-to-back reads back verbatim via
    //     DISP:TEXT:DATA? with an EMPTY error queue -- three writes in a row
    //     too;
    //   * a ';' inside a quoted string survives, so nothing splits on the byte;
    //   * the split point tracks the SCPI ';' wherever it sits (byte 5, 9, 15,
    //     25, 45 -- tested), which a transport-level truncation could not do;
    //   * it is unaffected by content, length, a 500 ms delay, every
    //     combination of EOI on both writes, and by whether we re-address in
    //     between;
    //   * only a Device Clear or a real data read between the writes fixes it,
    //     and both reset instrument state, not adapter state.
    //
    // So there is nothing to fix here. Ordinary write->read traffic --
    // everything pyvisa generates -- is unaffected. See
    // reverse/notes/02-usb-protocol.md section 4 for the full trigger matrix.
    return GPIBT_OK;
}

uint32_t KUSB488BTransport::sendCommandBytesViaRegisters(const uint8_t *cmds, uint32_t len) {
    if (!cmds || len == 0) return GPIBT_OK;

    // Command bytes need the bus as much as data does, and a holdoff left by
    // the previous read blocks them too.
    releaseRfdHoldoffIfPending();

    // Re-assert ATN before every command sequence. A preceding data phase
    // ends in standby with ATN released, so without this the command bytes
    // would go out as data to whatever was addressed last.
    uint32_t modeRc = writeRawRegister(KUSB_CMD_AUXMR, KUSB_AUX_TCA);
    if (modeRc != GPIBT_OK) return modeRc;

    // linux-gpib's nec7210_take_control() never assumes AUX_TCA took effect
    // immediately — it polls ADSR for NATN to actually clear before sending
    // anything. We previously didn't, and on hardware confirmed that once
    // wedged, AUX_TCA can silently fail to assert ATN at all: ADSR stayed
    // byte-for-byte identical before and after, and every following CDOR
    // byte then went nowhere with no diagnostic to say why. Verify here so
    // that failure surfaces as a clear timeout instead of a confusing
    // "no listener" further down.
    {
        bool atnAsserted = false;
        for (uint32_t spin = 0; spin < 200; ++spin) {
            uint8_t adsr = 0;
            if (readRawRegister(KUSB_CMD_ADSR, &adsr) != GPIBT_OK) break;
            if ((adsr & KUSB_ADSR_NATN) == 0) { atnAsserted = true; break; }
            IOSleep(1);
        }
        if (!atnAsserted) {
            os_log(OS_LOG_DEFAULT, "kusb_488b: FAIL take-control (AUX_TCA) never asserted ATN");
            return GPIBT_ERR_TIMEOUT;
        }
    }

    // Neither talker nor listener while driving the command bus.
    modeRc = writeRawRegister(KUSB_CMD_ADMR, KUSB_ADMR_TRM);
    if (modeRc != GPIBT_OK) return modeRc;

    for (uint32_t i = 0; i < len; ++i) {
        uint32_t rc = writeRawRegister(KUSB_CMD_CDOR, cmds[i]);
        if (rc != GPIBT_OK) return rc;

        // Wait for the byte to be handshaked onto the bus. ISR2 is
        // clear-on-read, so a set CO bit means "the byte went out".
        bool accepted = false;
        for (uint32_t spin = 0; spin < 200; ++spin) {
            uint8_t isr2 = 0;
            if (readRawRegister(KUSB_CMD_ISR2, &isr2) != GPIBT_OK) break;
            if (isr2 & KUSB_ISR2_CO) { accepted = true; break; }
            IOSleep(1);
        }
        if (!accepted) {
            os_log(OS_LOG_DEFAULT, "kusb_488b: FAIL command byte 0x%02x never got CO (i=%u/%u)", cmds[i], i, len);
            return GPIBT_ERR_NO_LISTENER;
        }
    }
    return GPIBT_OK;
}

// Which pipeline actually runs is decided once, for all three functions
// together, by kUseFirmwarePipeline above.
//
// History worth keeping: an earlier attempt at firmware-based addressing was
// rejected because ADSR read back TA *and* LA still latched after a
// bulk-sent UNT that should have cleared TA. That observation was made
// through the same 0xBD register path later proven to hand back a stale
// firmware shadow for ISR1 rather than live state, so it is not trustworthy
// evidence on its own — it may have been measuring the shadow, not the bus.
// This time around, judge the firmware command path by whether the
// instrument answers correctly, not by a register peek made through a path
// already known to lie.
uint32_t KUSB488BTransport::sendCommandBytes(const uint8_t *cmds, uint32_t len) {
    if (kUseFirmwarePipeline) return sendCommandBytesViaFirmware(cmds, len);
    return sendCommandBytesViaRegisters(cmds, len);
}

// Data moves through CDOR/DIR with ATN released, waiting on ISR1's DO/DI
// bits — the nec7210 approach. Same reasoning as sendCommandBytes: the
// firmware's bulk path does not arm once the chip is out of pon, and the
// register path is verified.
uint32_t KUSB488BTransport::writeDataViaRegisters(const uint8_t *buf, uint32_t len,
                                      bool send_eoi, uint32_t timeout_us,
                                      uint32_t *outBytesWritten) {
    if (outBytesWritten) *outBytesWritten = 0;
    if (!buf || len == 0) return GPIBT_OK;
    const uint32_t maxSpins = pollIterationsFor(timeout_us);

    // Declare ourselves the talker, then release ATN so the addressed listener
    // takes data rather than commands.
    uint32_t rc = writeRawRegister(KUSB_CMD_ADMR, KUSB_ADMR_TON | KUSB_ADMR_TRM);
    if (rc != GPIBT_OK) return rc;
    rc = writeRawRegister(KUSB_CMD_AUXMR, KUSB_AUX_GTS);
    if (rc != GPIBT_OK) return rc;

    // linux-gpib's nec7210_go_to_standby() polls ADSR for NATN to actually
    // set (ATN released) before treating standby as complete; we didn't.
    // If GTS silently fails to release ATN, the data-phase bytes below would
    // go out onto the bus while ATN is still asserted — which every other
    // device on the bus interprets as universal/addressed commands, not
    // data. That would explain corruption that looks nothing like a normal
    // data-phase timeout.
    {
        bool atnReleased = false;
        for (uint32_t spin = 0; spin < 500; ++spin) {
            uint8_t adsr = 0;
            if (readRawRegister(KUSB_CMD_ADSR, &adsr) != GPIBT_OK) break;
            if (adsr & KUSB_ADSR_NATN) { atnReleased = true; break; }
            IOSleep(1);
        }
        if (!atnReleased) {
            os_log(OS_LOG_DEFAULT, "kusb_488b: FAIL writeData: GTS never released ATN");
            writeRawRegister(KUSB_CMD_ADMR, KUSB_ADMR_TRM);
            return GPIBT_ERR_ABORTED;  // distinct from the DO-timeout below, deliberately
        }
    }

    for (uint32_t i = 0; i < len; ++i) {
        if (send_eoi && i == len - 1) {
            rc = writeRawRegister(KUSB_CMD_AUXMR, KUSB_AUX_SEOI);
            if (rc != GPIBT_OK) return rc;
        }
        rc = writeRawRegister(KUSB_CMD_CDOR, buf[i]);
        if (rc != GPIBT_OK) return rc;

        bool sent = false;
        for (uint32_t spin = 0; spin < maxSpins; ++spin) {
            uint8_t isr1 = 0;
            if (readRawRegister(KUSB_CMD_ISR1, &isr1) != GPIBT_OK) break;
            if (isr1 & KUSB_ISR1_DO) { sent = true; break; }
            IOSleep(1);
        }
        if (!sent) {
            os_log(OS_LOG_DEFAULT, "kusb_488b: FAIL writeData: byte 0x%02x never got DO (i=%u/%u)", buf[i], i, len);
            if (outBytesWritten) *outBytesWritten = i;
            // Drop back to neutral role rather than leaving TON latched —
            // AUX_FH was also tried here and rejected: it force-completes
            // the handshake even when nothing real is in flight, which
            // synthesizes a phantom byte that the next readData() then
            // hands back as if it were genuine data (confirmed on
            // hardware). A plain ADMR reset carries no such risk.
            writeRawRegister(KUSB_CMD_ADMR, KUSB_ADMR_TRM);
            return GPIBT_ERR_NO_LISTENER;
        }
        if (outBytesWritten) *outBytesWritten = i + 1;
    }
    return GPIBT_OK;
}

// Which pipeline runs is decided once, for all three functions together, by
// kUseFirmwarePipeline (see sendCommandBytes).
//
// History worth keeping: mixing a firmware-based write with register-based
// addressing and a register-based read previously broke the read in a
// specific way — the K2400 visibly started responding (byte 0 was 'K', the
// first character of "KEITHLEY...") but the read loop kept re-reading that
// same stale byte 255 times instead of advancing. That is consistent with
// the same firmware-state hypothesis: the register-path read's DI/DIR
// clear-on-read handshake depends on something the register-path write's own
// per-byte completion does, that a firmware write bypasses. Addressing all
// three through the firmware together, so nothing is done behind its back,
// is the point of this attempt.
uint32_t KUSB488BTransport::writeData(const uint8_t *buf, uint32_t len,
                                      bool send_eoi, uint32_t timeout_us,
                                      uint32_t *outBytesWritten) {
    if (kUseFirmwarePipeline) {
        return writeDataViaFirmware(buf, len, send_eoi, timeout_us, outBytesWritten);
    }
    return writeDataViaRegisters(buf, len, send_eoi, timeout_us, outBytesWritten);
}

uint32_t KUSB488BTransport::readDataViaRegisters(uint8_t *buf, uint32_t request_count,
                                     uint8_t eos_char, uint8_t eos_flags,
                                     uint32_t timeout_us,
                                     uint32_t *outBytesRead, uint8_t *outEnd) {
    if (outBytesRead) *outBytesRead = 0;
    if (outEnd) *outEnd = 0;
    if (!buf || request_count == 0) return GPIBT_OK;
    const uint32_t maxSpins = pollIterationsFor(timeout_us);

    // Clear a holdoff we are still carrying, before declaring ourselves a
    // listener. See releaseRfdHoldoffIfPending() for why this is conditional.
    releaseRfdHoldoffIfPending();

    // Listen-only for the same reason writes use talk-only.
    uint32_t rc = writeRawRegister(KUSB_CMD_ADMR, KUSB_ADMR_LON | KUSB_ADMR_TRM);
    if (rc != GPIBT_OK) return rc;
    rc = writeRawRegister(KUSB_CMD_AUXMR, KUSB_AUX_GTS);
    if (rc != GPIBT_OK) return rc;

    // See the matching comment in writeData(): verify GTS actually released
    // ATN rather than assuming it did.
    {
        bool atnReleased = false;
        for (uint32_t spin = 0; spin < 500; ++spin) {
            uint8_t adsr = 0;
            if (readRawRegister(KUSB_CMD_ADSR, &adsr) != GPIBT_OK) break;
            if (adsr & KUSB_ADSR_NATN) { atnReleased = true; break; }
            IOSleep(1);
        }
        if (!atnReleased) {
            os_log(OS_LOG_DEFAULT, "kusb_488b: FAIL readData: GTS never released ATN");
            writeRawRegister(KUSB_CMD_ADMR, KUSB_ADMR_TRM);
            return GPIBT_ERR_ABORTED;  // distinct from the DI-timeout below, deliberately
        }
    }

    // ISR1's END bit latches and is NOT cleared by reading it back through
    // vendor request 0xBD — measured directly: clean at 0x00 on a fresh
    // attach, 0x10 after the first read that ends on EOI, and 0x10/0x11 from
    // then on no matter how often it is read. (The vendor driver never takes
    // END from this register; it reads it out of the firmware's status block,
    // which is a good hint that 0xBD hands back a shadow copy rather than
    // performing the real, clear-on-read register access.)
    //
    // Breaking on `isr1 & END` therefore poisons every read after the first:
    // once the latch is set, the very first byte looks like the last one, so
    // each subsequent read returns exactly one byte and reports CMPL|END.
    // That single fact accounts for the whole "cycle 1 works, everything
    // after it is broken" behaviour.
    //
    // So only a 0 -> 1 transition of END counts. If the bit is already set
    // when the read starts, it carries no information about this transfer and
    // is ignored; termination then falls to EOS, the requested count, or the
    // idle gap below.
    uint8_t isr1Start = 0;
    readRawRegister(KUSB_CMD_ISR1, &isr1Start);
    const bool endUnusable = (isr1Start & KUSB_ISR1_END) != 0;

    // Once bytes are flowing, a gap this long means the talker has finished.
    // Needed because with END unusable there is otherwise nothing to stop the
    // read short of the full timeout.
    const uint32_t kIdleSpins = 250;

    uint32_t got = 0;
    bool timedOut = false;
    bool sawEnd = false;
    while (got < request_count) {
        uint8_t isr1 = 0;
        bool ready = false;
        // Wait the caller's full timeout for the first byte; after that, only
        // the idle gap, so a finished message returns promptly.
        const uint32_t waitSpins = (got == 0) ? maxSpins : kIdleSpins;
        for (uint32_t spin = 0; spin < waitSpins; ++spin) {
            if (readRawRegister(KUSB_CMD_ISR1, &isr1) != GPIBT_OK) break;
            if (isr1 & KUSB_ISR1_DI) { ready = true; break; }
            IOSleep(1);
        }
        if (!ready) {
            // Ran out of patience. With bytes already in hand this is the
            // normal end of a message whose END we could not trust, not a
            // failure — but either way stop being an addressed listener, or
            // we sit on the bus holding NRFD low and the next transaction
            // cannot get its data through.
            writeRawRegister(KUSB_CMD_ADMR, KUSB_ADMR_TRM);
            if (got == 0) timedOut = true;
            break;
        }

        uint8_t byte = 0;
        if (readRawRegister(KUSB_CMD_DIR, &byte) != GPIBT_OK) break;
        buf[got++] = byte;

        // END must be taken from the same ISR1 read that reported DI — the
        // register is clear-on-read, so a second read would lose it.
        if (!endUnusable && (isr1 & KUSB_ISR1_END)) {
            if (outEnd) *outEnd = 1;
            sawEnd = true;
            break;
        }
        if ((eos_flags & REOS) && byte == eos_char) { if (outEnd) *outEnd = 1; break; }
    }

    // END put the chip into RFD holdoff (AUXRA=HLDE). Record the debt rather
    // than clearing it here: releasing while still an addressed listener lets
    // the chip accept one more byte. Whoever needs the bus next releases it,
    // by which point we are no longer addressed.
    // Record the holdoff whenever anything was received, not only when END
    // was visible.
    //
    // The bring-up leaves AUXRA=HLDE, so the *hardware* holds off as soon as a
    // byte arrives carrying EOI — whether or not our software was in a
    // position to observe the END bit. When END is unusable (the latch
    // described above) sawEnd stays false even though the instrument really
    // did end its message with EOI, so keying the flag off sawEnd missed the
    // holdoff entirely: NRFD stayed clamped and the *next* transaction's write
    // died with EBUS. Measured as exactly two good cycles followed by a dead
    // bus, which is what this fixes.
    //
    // Erring towards recording it is safe: the release is conditional and
    // happens before the next transfer while we are still TRM rather than an
    // addressed listener.
    if (sawEnd || got > 0) rfdHoldoffPending_ = true;

    if (outBytesRead) *outBytesRead = got;
    // Only escalate to a hard timeout when nothing came back at all; a
    // partial read that timed out after collecting some bytes still reports
    // what it got, matching the pre-existing behavior for that case.
    return (timedOut && got == 0) ? GPIBT_ERR_TIMEOUT : GPIBT_OK;
}

// UNRESOLVED. Sequence matches FUN_140003184 in KUSB488B_X64.sys byte for
// byte: arm with 0xBA (wValue 0x100, wIndex = EOS state), bulk-in on the
// data-in pipe, poll 0xB0 for an 0x0B-byte status block, count from bytes
// 2..5 and END/EOI from byte 6.
//
// The write's fix (AUX_TCA + ADMR=TON|TRM + AUX_GTS before arming) does NOT
// carry over: with the analogous ADMR=LON|TRM + AUX_GTS below, the K2000's
// reply genuinely arrives at the 7210 core (ISR1 shows DI set, confirmed by
// direct register read immediately before arming 0xBA) and this engine
// still reports DONE with count=0 and error 5 (timeout) every time. Every
// register-reachable variable was tried on hardware and changed nothing:
// with/without AUX_FH, HLDE cleared, with/without AUX_GTS, wIndex 0 vs a
// real EOS byte, arming before vs after releasing ATN, IMR1/IMR2 interrupt
// enables, the newly-found 0xB5 (set system controller), and per-transfer
// pokes via 0xBD instead of 0xB1. A first pass at disassembling the 8051
// firmware itself found the transfer-state pair the engine likely uses
// (XDATA 0x6040/0x6041) but hit unreliable automated decompilation in the
// actual dispatch function before finding what gates it. See
// reverse/notes/02-usb-protocol.md for the full investigation.
//
// The role/ATN setup below is kept (rather than reverted to no prologue)
// because it is CONFIRMED necessary infrastructure -- the reply cannot even
// reach the 7210 without it -- even though it is not sufficient. Do not
// read its presence as a claim that this function works; it does not.
uint32_t KUSB488BTransport::readDataViaFirmware(uint8_t *buf, uint32_t request_count,
                                     uint8_t eos_char, uint8_t eos_flags,
                                     uint32_t timeout_us,
                                     uint32_t *outBytesRead, uint8_t *outEnd) {
    if (outBytesRead) *outBytesRead = 0;
    if (outEnd) *outEnd = 0;
    if (!buf || request_count == 0) return GPIBT_OK;
    // No unconditional recover here either -- see sendCommandBytesViaFirmware.
    // Aborting here would tear down the addressing that was just established.

    // No goToStandby() here -- see writeDataViaFirmware() for why.

    // wIndex carries the EOS character and MUST be sent unconditionally --
    // NOT gated on REOS. The vendor passes its persisted EOS byte (ctx+0xC1)
    // on every read. With wIndex = 0 the firmware never terminates: it
    // reports err=5 (timeout) with count=0 and delivers nothing. Passing a
    // real EOS byte is what made the firmware read work at all, and it also
    // makes status byte 6 (END) report correctly -- the "END never lights up"
    // question in this file's history was an artefact of wIndex = 0.
    //
    // A caller that did not ask for REOS still needs a sane terminator here;
    // \n is what every SCPI instrument this driver targets uses.
    const uint16_t wIndex = (eos_flags & REOS) ? eos_char : (uint16_t)'\n';
    uint32_t rc = beginTransfer(KUSB_REQ_BEGIN_READ, KUSB_READ_WVALUE, wIndex,
                                request_count, timeout_us);
    if (rc != GPIBT_OK) return rc;

    uint32_t done = 0;
    uint8_t  status[16];
    uint32_t glitchRetries = 0;

    // Read one bulk chunk BEFORE the first status poll: the engine reports
    // BUSY until the data has been handed over, so polling first would just
    // spin. After each chunk, poll; only come back for more while the engine
    // is still BUSY and the caller's buffer has room. Never issue a
    // speculative bulkIn once status has left BUSY -- there is nothing left
    // to fetch and the request just times out at the USB level.
    bool needData = true;
    for (;;) {
        if (needData && done < request_count) {
            uint32_t got = 0;
            rc = bulkIn(dataInPipe_, buf + done, request_count - done,
                        timeout_us, &got);
            if (rc != GPIBT_OK) {
                recoverFirmwareEngine();
                return rc;
            }
            done += got;
        }
        needData = false;

        uint16_t slen = 0;
        rc = controlIn(KUSB_REQ_STATUS, 0, 0, status, KUSB_STATUS_LEN_READ, &slen);
        // Transient-glitch tolerance, as the command path needs. Note the
        // device returns KUSB_STATUS_MIN_READ (7) bytes, not the 0x0B we
        // request; while BUSY it returns one byte fewer still. Requiring the
        // full requested length here made every completed read look like a
        // short-packet failure.
        if (rc != GPIBT_OK || slen < KUSB_STATUS_MIN_READ) {
            if (glitchRetries++ < 5) { IOSleep(2); continue; }
            recoverFirmwareEngine();
            return GPIBT_ERR_IO;
        }
        glitchRetries = 0;

        if (status[0] == KUSB_STATE_DONE) break;
        if (status[0] != KUSB_STATE_BUSY) {
            recoverFirmwareEngine();
            return (status[1] == KUSB_ERR_ABORTED) ? GPIBT_ERR_TIMEOUT
                                                   : GPIBT_ERR_IO;
        }
        if (done >= request_count) {
            // Buffer full and the firmware is still going — nothing more we
            // can accept, so stop it rather than spinning. The caller sees
            // END = 0 and knows the message is incomplete.
            recoverFirmwareEngine();
            break;
        }
        // Still BUSY with room left: go fetch the next chunk immediately.
        // No sleep here -- bulkIn() is itself a blocking USB read with a
        // timeout, so sleeping before it would only add latency.
        //
        // Honest note on performance: an IOSleep(1) used to sit here on the
        // theory that it dominated read time. Removing it measured as no
        // improvement at all (570 B read: 174.7 ms before, 177.6 ms after).
        // The per-byte cost is NOT in this loop. Evidence: a read of any
        // size completes in a single ibrd/bulkIn round trip regardless of
        // the requested capacity, and the WRITE path -- one bulkOut, no
        // loop -- scales per byte too (5 B: 7.9 ms, 50 B: 13.3 ms). So the
        // ~0.1-0.3 ms/byte is the GPIB bus / firmware engine below us, not
        // driver overhead. Do not try to optimise it here again without new
        // evidence.
        needData = true;
    }

    uint32_t count = (uint32_t)status[2] | ((uint32_t)status[3] << 8) |
                     ((uint32_t)status[4] << 16) | ((uint32_t)status[5] << 24);
    if (count > request_count) count = request_count;
    if (count > done)          count = done;

    if (outBytesRead) *outBytesRead = count;
    if (outEnd)       *outEnd = (status[6] == 1) ? 1 : 0;
    return GPIBT_OK;
}

uint32_t KUSB488BTransport::readData(uint8_t *buf, uint32_t request_count,
                                     uint8_t eos_char, uint8_t eos_flags,
                                     uint32_t timeout_us,
                                     uint32_t *outBytesRead, uint8_t *outEnd) {
    // Which pipeline runs is decided once, for all three functions together,
    // by kUseFirmwarePipeline (see sendCommandBytes).
    //
    // History worth keeping: this exact function, tried previously with
    // register-based addressing ahead of it, delivered nothing from
    // bulkIn() — and the armed-then-aborted transfer it left behind broke a
    // register-path read attempted right after, so it could not even be
    // tried as a first choice with a register fallback. The working theory
    // is that addressing through registers left the firmware's own model of
    // the bus stale for this call. This attempt addresses through the
    // firmware too, so that theory gets a real test instead of being assumed.
    if (kUseFirmwarePipeline) {
        return readDataViaFirmware(buf, request_count, eos_char, eos_flags,
                                   timeout_us, outBytesRead, outEnd);
    }
    return readDataViaRegisters(buf, request_count, eos_char, eos_flags,
                                timeout_us, outBytesRead, outEnd);
}

// Exposes the 0xBD register read for gpibctl regs/peek — a supported
// debugging aid, not scaffolding to remove.
uint32_t KUSB488BTransport::readRawRegister(uint16_t reg, uint8_t *outValue) {
    uint8_t v = 0;
    uint16_t got = 0;
    uint32_t rc = controlIn(KUSB_REQ_REG, reg, 0, &v, 1, &got);
    if (rc != GPIBT_OK) return rc;
    if (got < 1) return GPIBT_ERR_IO;
    if (outValue) *outValue = v;
    return GPIBT_OK;
}

uint32_t KUSB488BTransport::readBusLines(uint16_t *outLines) {
    if (!outLines) return GPIBT_ERR_IO;

    uint8_t v = 0;
    uint16_t got = 0;
    uint32_t rc = controlIn(KUSB_REQ_REG, kRegBusLines, 0, &v, 1, &got);
    if (rc != GPIBT_OK) return rc;
    if (got < 1) return GPIBT_ERR_IO;

    // All eight lines are readable, so every Valid bit is set. The bit->line
    // mapping is taken verbatim from the Windows driver.
    uint16_t lines = ValidDAV | ValidNDAC | ValidNRFD | ValidIFC |
                     ValidREN | ValidSRQ | ValidATN | ValidEOI;
    if (v & 0x01) lines |= BusREN;
    if (v & 0x02) lines |= BusIFC;
    if (v & 0x04) lines |= BusSRQ;
    if (v & 0x08) lines |= BusEOI;
    if (v & 0x10) lines |= BusNRFD;
    if (v & 0x20) lines |= BusNDAC;
    if (v & 0x40) lines |= BusDAV;
    if (v & 0x80) lines |= BusATN;

    *outLines = lines;
    return GPIBT_OK;
}
