//
//  GPIBBoard.cpp
//  darwin-gpib driver
//

#include <os/log.h>
#include <DriverKit/IOLib.h>

#include "GPIBBoard.h"
#include "GPIBTransport.h"
#include "gpib_user.h"

// Timeout-code → microseconds. Indexed by the gpib_timeout enum from gpib_user.h.
// TNONE = 0 means no timeout (infinite); we map that to UINT32_MAX which the
// transport interprets as no deadline.
// Bounded timeout for serialPollProbe(). A serial poll answers immediately,
// so this only has to outlast bus turnaround -- and it multiplies by every
// absent address in a discovery sweep.
static const uint32_t kProbeTimeoutUs = 100000;   // 100 ms

uint32_t GPIBBoard::timeoutCodeToMicros(uint32_t code) {
    switch (code) {
        case TNONE:   return 0xFFFFFFFFu;
        case T10us:   return 10;
        case T30us:   return 30;
        case T100us:  return 100;
        case T300us:  return 300;
        case T1ms:    return 1000;
        case T3ms:    return 3000;
        case T10ms:   return 10000;
        case T30ms:   return 30000;
        case T100ms:  return 100000;
        case T300ms:  return 300000;
        case T1s:     return 1000000;
        case T3s:     return 3000000;
        case T10s:    return 10000000;
        case T30s:    return 30000000;
        case T100s:   return 100000000;
        case T300s:   return 300000000;
        case T1000s:  return 1000000000;
        default:      return 3000000;     // sensible default = T3s
    }
}

bool GPIBBoard::init(IGPIBTransport *transport) {
    if (!transport) return false;
    transport_ = transport;

    for (int i = 0; i < kMaxDescriptors; ++i) {
        descriptors_[i].handle = -1;
    }

    // M1 defaults: board at PAD 0, system controller, online once Start completes.
    boardPAD_ = 0;
    boardSAD_ = -1;
    online_ = false;
    system_controller_ = true;
    renRequested_ = false;
    recoveryInProgress_ = false;
    ibsta_ = 0;
    return true;
}

void GPIBBoard::free() {
    // Transport is owned by ni_usb::Start, not by us; just drop the reference.
    transport_ = nullptr;
}

uint32_t GPIBBoard::setOnline(bool online) {
    if (!transport_) {
        ibsta_ = ERR;
        return ibsta_;
    }

    if (online) {
        // Pulse IFC, take control, raise REN. Taking control is a separate
        // step: IFC alone does not make the board controller-in-charge, and
        // without CIC every command transfer completes having moved nothing.
        uint32_t ifcRc = transport_->pulseInterfaceClear();
        uint32_t tcRc  = transport_->takeControl(false);
        uint32_t renRc = transport_->setRemoteEnable(true);
        renRequested_ = true;
        online_ = true;

        // Report what actually happened rather than asserting success — this
        // previously always claimed CIC|REM and hid a dead bus.
        ibsta_ = CMPL;
        if (ifcRc == 0 && tcRc == 0) ibsta_ |= CIC;
        if (renRc == 0)              ibsta_ |= REM;
    } else {
        transport_->setRemoteEnable(false);
        renRequested_ = false;
        online_ = false;
        ibsta_ = CMPL;
    }
    return ibsta_;
}

int32_t GPIBBoard::allocateHandle() {
    // Skip slot 0 — reserved for the implicit board descriptor opened during
    // setOnline(). Linux uses a monotonically increasing handle namespace
    // bounded by GPIB_MAX_NUM_DESCRIPTORS; we use the array index for now.
    for (int i = 1; i < kMaxDescriptors; ++i) {
        if (descriptors_[i].handle < 0) {
            descriptors_[i].handle = i;
            return i;
        }
    }
    return -1;
}

GPIBDescriptor *GPIBBoard::descriptorFor(int32_t handle) {
    if (handle < 0 || handle >= kMaxDescriptors) return nullptr;
    GPIBDescriptor *d = &descriptors_[handle];
    if (d->handle < 0) return nullptr;
    return d;
}

int32_t GPIBBoard::openDescriptor(uint8_t pad, int8_t sad, bool is_board,
                                  uint32_t timeout_us, uint8_t eos_char,
                                  uint8_t eos_flags, bool eot,
                                  uint32_t *outIbsta, uint32_t *outIberr) {
    int32_t h = allocateHandle();
    if (h < 0) {
        if (outIbsta) *outIbsta = ERR;
        if (outIberr) *outIberr = ETAB;
        return -1;
    }

    GPIBDescriptor *d = &descriptors_[h];
    d->handle     = h;
    d->is_board   = is_board ? 1 : 0;
    d->pad        = pad;
    d->sad        = sad;
    d->timeout_us = timeout_us;
    d->eos_char   = eos_char;
    d->eos_flags  = eos_flags;
    d->eot        = eot ? 1 : 0;

    if (outIbsta) *outIbsta = CMPL;
    if (outIberr) *outIberr = 0;
    return h;
}

uint32_t GPIBBoard::closeDescriptor(int32_t handle, uint32_t *outIberr) {
    GPIBDescriptor *d = descriptorFor(handle);
    if (!d) {
        if (outIberr) *outIberr = EARG;
        ibsta_ = ERR;
        return ibsta_;
    }
    d->handle = -1;
    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL;
    return ibsta_;
}

uint32_t GPIBBoard::configure(int32_t handle, uint32_t key, int32_t value,
                              uint32_t *outIberr) {
    GPIBDescriptor *d = descriptorFor(handle);
    if (!d) {
        if (outIberr) *outIberr = EARG;
        ibsta_ = ERR;
        return ibsta_;
    }

    switch (key) {
        case IbcPAD:
            d->pad = (uint8_t)(value & 0x1f);
            break;
        case IbcSAD:
            d->sad = (int8_t)value;
            break;
        case IbcTMO:
            d->timeout_us = timeoutCodeToMicros((uint32_t)value);
            break;
        case IbcEOT:
            d->eot = value ? 1 : 0;
            break;
        case IbcEOSchar:
            d->eos_char = (uint8_t)(value & 0xff);
            break;
        case IbcEOSrd:
        case IbcEOSwrt:
        case IbcEOScmp:
            // Aggregate REOS/XEOS/BIN bits.
            d->eos_flags = (uint8_t)(value & (REOS | XEOS | BIN));
            break;
        default:
            if (outIberr) *outIberr = ECAP;
            ibsta_ = ERR;
            return ibsta_;
    }

    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL;
    return ibsta_;
}

uint32_t GPIBBoard::ask(int32_t handle, int32_t option, int32_t *outValue,
                        uint32_t *outIberr) {
    GPIBDescriptor *d = descriptorFor(handle);
    if (!d) {
        if (outIberr) *outIberr = EARG;
        ibsta_ = ERR;
        return ibsta_;
    }
    int32_t v = 0;
    switch (option) {
        case IbaPAD:     v = d->pad; break;
        case IbaSAD:     v = d->sad; break;
        case IbaEOT:     v = d->eot; break;
        case IbaEOSchar: v = d->eos_char; break;
        default:
            if (outIberr) *outIberr = ECAP;
            ibsta_ = ERR;
            return ibsta_;
    }
    if (outValue) *outValue = v;
    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL;
    return ibsta_;
}

// Helpers: address-byte builders. These produce the bytes that go on the
// command bus (ATN asserted) to redirect the talker/listener.
static inline uint8_t MLA_byte(uint8_t pad)  { return (uint8_t)((pad & 0x1f) | LAD); }
static inline uint8_t MTA_byte(uint8_t pad)  { return (uint8_t)((pad & 0x1f) | TAD); }
static inline uint8_t MSA_byte(int8_t sad)   { return (uint8_t)(((uint8_t)sad & 0x1f) | SAD); }

uint32_t GPIBBoard::addressForWrite(const GPIBDescriptor *desc, uint32_t *outIberr) {
    // Board is talker, target is listener.
    //
    // UNT clears our own board's latched TA (talker-active) status from any
    // previous exchange. Without it, ADSR's TA bit stays set across calls —
    // verified on hardware — and an unrelated stale TA can collide with the
    // next addressed-talker handshake. IEEE-488 controllers conventionally
    // send both UNL and UNT before any fresh addressing sequence for exactly
    // this reason; we previously sent only UNL.
    uint8_t cmds[7];
    uint32_t n = 0;
    cmds[n++] = UNL;
    cmds[n++] = UNT;
    cmds[n++] = MTA_byte(boardPAD_);
    cmds[n++] = MLA_byte(desc->pad);
    if (desc->sad >= 0) cmds[n++] = MSA_byte(desc->sad);
    uint32_t rc = transport_->sendCommandBytes(cmds, n);
    if (rc != 0) {
        // Distinguish "the controller couldn't even take control of the bus"
        // (GPIBT_ERR_TIMEOUT — AUX_TCA never asserted ATN) from "we drove
        // ATN fine but a command byte went unhandshaked" (ENOL).
        if (outIberr) *outIberr = (rc == GPIBT_ERR_TIMEOUT) ? EABO : ENOL;
        return ERR | ((rc == GPIBT_ERR_TIMEOUT) ? TIMO : 0);
    }
    return 0;
}

uint32_t GPIBBoard::addressForRead(const GPIBDescriptor *desc, uint32_t *outIberr) {
    // Board is listener, target is talker. See addressForWrite for why UNT
    // is sent alongside UNL.
    uint8_t cmds[7];
    uint32_t n = 0;
    cmds[n++] = UNL;
    cmds[n++] = UNT;
    cmds[n++] = MLA_byte(boardPAD_);
    cmds[n++] = MTA_byte(desc->pad);
    if (desc->sad >= 0) cmds[n++] = MSA_byte(desc->sad);
    uint32_t rc = transport_->sendCommandBytes(cmds, n);
    if (rc != 0) {
        if (outIberr) *outIberr = (rc == GPIBT_ERR_TIMEOUT) ? EABO : ENOL;
        return ERR | ((rc == GPIBT_ERR_TIMEOUT) ? TIMO : 0);
    }
    return 0;
}

uint32_t GPIBBoard::write(int32_t handle, const uint8_t *buf, uint32_t len,
                          bool send_eoi, uint32_t *outIbcnt, uint32_t *outIberr) {
    GPIBDescriptor *d = descriptorFor(handle);
    if (!d || !transport_) {
        if (outIberr) *outIberr = EARG;
        if (outIbcnt) *outIbcnt = 0;
        ibsta_ = ERR;
        return ibsta_;
    }
    if (!online_) {
        if (outIberr) *outIberr = ENEB;
        if (outIbcnt) *outIbcnt = 0;
        ibsta_ = ERR;
        return ibsta_;
    }

    uint32_t addrRc = addressForWrite(d, outIberr);
    if (addrRc & ERR) {
        // Addressing failing is the signature of a wedged core: universal
        // command bytes cannot handshake. Recover in software and try once
        // more before reporting failure, so a latched holdoff doesn't turn
        // into "unplug the adapter to continue".
        if (recoverBus(d)) addrRc = addressForWrite(d, outIberr);
    }
    if (addrRc & ERR) {
        if (outIbcnt) *outIbcnt = 0;
        ibsta_ = ERR;
        return ibsta_;
    }

    uint32_t bytesWritten = 0;
    uint32_t rc = transport_->writeData(buf, len, send_eoi && d->eot,
                                        d->timeout_us, &bytesWritten);
    if (outIbcnt) *outIbcnt = bytesWritten;

    if (rc != 0) {
        // ECIC here is a temporary, deliberately distinct diagnostic code:
        // GPIBT_ERR_ABORTED means writeData()'s GTS-verify (ATN release)
        // specifically failed, as opposed to GPIBT_ERR_NO_LISTENER's
        // per-byte DO timeout — both used to be indistinguishable as EBUS.
        if (outIberr) *outIberr = (rc == GPIBT_ERR_TIMEOUT) ? EABO
                                 : (rc == GPIBT_ERR_ABORTED) ? ECIC
                                 : EBUS;
        ibsta_ = ERR | ((rc == GPIBT_ERR_TIMEOUT) ? TIMO : 0);
        return ibsta_;
    }

    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL;
    return ibsta_;
}

uint32_t GPIBBoard::read(int32_t handle, uint8_t *buf, uint32_t request_count,
                        uint32_t *outIbcnt, uint8_t *outEnd, uint32_t *outIberr) {
    GPIBDescriptor *d = descriptorFor(handle);
    if (!d || !transport_) {
        if (outIberr) *outIberr = EARG;
        if (outIbcnt) *outIbcnt = 0;
        if (outEnd) *outEnd = 0;
        ibsta_ = ERR;
        return ibsta_;
    }
    if (!online_) {
        if (outIberr) *outIberr = ENEB;
        if (outIbcnt) *outIbcnt = 0;
        if (outEnd) *outEnd = 0;
        ibsta_ = ERR;
        return ibsta_;
    }

    uint32_t addrRc = addressForRead(d, outIberr);
    if (addrRc & ERR) {
        // See the matching comment in write().
        if (recoverBus(d)) addrRc = addressForRead(d, outIberr);
    }
    if (addrRc & ERR) {
        if (outIbcnt) *outIbcnt = 0;
        if (outEnd) *outEnd = 0;
        ibsta_ = ERR;
        return ibsta_;
    }

    uint32_t bytesRead = 0;
    uint8_t  endFlag = 0;
    uint32_t rc = transport_->readData(buf, request_count,
                                       d->eos_char, d->eos_flags,
                                       d->timeout_us,
                                       &bytesRead, &endFlag);
    if (outIbcnt) *outIbcnt = bytesRead;
    if (outEnd) *outEnd = endFlag;

    if (rc != 0) {
        // See the matching comment in write(): ECIC here means readData()'s
        // GTS-verify (ATN release) specifically failed, distinct from the
        // per-byte DI timeout.
        if (outIberr) *outIberr = (rc == GPIBT_ERR_TIMEOUT) ? EABO
                                 : (rc == GPIBT_ERR_ABORTED) ? ECIC
                                 : EBUS;
        ibsta_ = ERR | ((rc == GPIBT_ERR_TIMEOUT) ? TIMO : 0);
        return ibsta_;
    }

    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL | (endFlag ? END : 0);
    return ibsta_;
}

uint32_t GPIBBoard::deviceClear(int32_t handle, uint32_t *outIberr) {
    GPIBDescriptor *d = descriptorFor(handle);
    if (!d || !transport_) {
        if (outIberr) *outIberr = EARG;
        ibsta_ = ERR;
        return ibsta_;
    }
    uint8_t cmds[4];
    uint32_t n = 0;
    cmds[n++] = UNL;
    cmds[n++] = MLA_byte(d->pad);
    if (d->sad >= 0) cmds[n++] = MSA_byte(d->sad);
    cmds[n++] = SDC;
    uint32_t rc = transport_->sendCommandBytes(cmds, n);
    if (rc != 0) {
        if (outIberr) *outIberr = EBUS;
        ibsta_ = ERR;
        return ibsta_;
    }
    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL;
    return ibsta_;
}

uint32_t GPIBBoard::interfaceClear(int32_t /*handle*/, uint32_t *outIberr) {
    if (!transport_) {
        if (outIberr) *outIberr = ENEB;
        ibsta_ = ERR;
        return ibsta_;
    }
    transport_->pulseInterfaceClear();
    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL | CIC;
    return ibsta_;
}

uint32_t GPIBBoard::remoteEnable(int32_t /*handle*/, bool enable, uint32_t *outIberr) {
    if (!transport_) {
        if (outIberr) *outIberr = ENEB;
        ibsta_ = ERR;
        return ibsta_;
    }
    transport_->setRemoteEnable(enable);
    renRequested_ = enable;
    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL | (enable ? REM : 0);
    return ibsta_;
}

uint32_t GPIBBoard::serialPoll(int32_t handle, uint8_t *outStatusByte,
                               uint32_t *outIberr) {
    GPIBDescriptor *d = descriptorFor(handle);
    if (!d || !transport_) {
        if (outIberr) *outIberr = EARG;
        ibsta_ = ERR;
        return ibsta_;
    }
    if (!online_) {
        if (outIberr) *outIberr = ENEB;
        ibsta_ = ERR;
        return ibsta_;
    }

    // Sequence: UNL, MTA(device pad), MSA(device sad) if any, SPE, then
    // one byte read (the STB), then SPD, UNT to clean up.
    uint8_t cmds[8];
    uint32_t n = 0;
    cmds[n++] = UNL;
    cmds[n++] = (uint8_t)((d->pad & 0x1f) | TAD);
    if (d->sad >= 0) cmds[n++] = (uint8_t)(((uint8_t)d->sad & 0x1f) | SAD);
    cmds[n++] = (uint8_t)((uint8_t)(boardPAD_ & 0x1f) | LAD);
    cmds[n++] = SPE;

    uint32_t rc = transport_->sendCommandBytes(cmds, n);
    if (rc != 0) {
        if (outIberr) *outIberr = EBUS;
        ibsta_ = ERR;
        return ibsta_;
    }

    uint8_t  stb = 0;
    uint32_t got = 0;
    uint8_t  endFlag = 0;
    rc = transport_->readData(&stb, 1, 0, 0, d->timeout_us, &got, &endFlag);

    // Tear down: SPD + UNT. This MUST happen on the failure path too.
    //
    // It used to return early when the read failed, which leaves the bus in
    // serial-poll mode (SPE still in force) with a device addressed to talk
    // that is not going to talk. Measured 2026-08-28: serial-polling an
    // address with no instrument on it wedges the bus so hard that command
    // bytes themselves stop handshaking -- every later ibcmd returns EBUS,
    // ibrd returns ENOL, and `gpibctl reinit` cannot clear it. Only a
    // physical replug does. Polling an absent address is exactly what a
    // presence probe does, so this path is reached in normal use.
    uint8_t teardown[4] = { SPD, UNT, 0, 0 };
    transport_->sendCommandBytes(teardown, 2);

    if (rc != 0 || got == 0) {
        if (outIberr) *outIberr = (rc == GPIBT_ERR_TIMEOUT) ? EABO : EBUS;
        ibsta_ = ERR | ((rc == GPIBT_ERR_TIMEOUT) ? TIMO : 0);
        return ibsta_;
    }

    if (outStatusByte) *outStatusByte = stb;
    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL | ((stb & 0x40) ? RQS : 0);
    return ibsta_;
}

uint32_t GPIBBoard::trigger(int32_t handle, uint32_t *outIberr) {
    GPIBDescriptor *d = descriptorFor(handle);
    if (!d || !transport_) {
        if (outIberr) *outIberr = EARG;
        ibsta_ = ERR;
        return ibsta_;
    }
    uint8_t cmds[6];
    uint32_t n = 0;
    cmds[n++] = UNL;
    cmds[n++] = MLA_byte(d->pad);
    if (d->sad >= 0) cmds[n++] = MSA_byte(d->sad);
    cmds[n++] = GET;
    uint32_t rc = transport_->sendCommandBytes(cmds, n);
    if (rc != 0) {
        if (outIberr) *outIberr = EBUS;
        ibsta_ = ERR;
        return ibsta_;
    }
    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL;
    return ibsta_;
}

uint32_t GPIBBoard::goToLocal(int32_t handle, uint32_t *outIberr) {
    GPIBDescriptor *d = descriptorFor(handle);
    if (!d || !transport_) {
        if (outIberr) *outIberr = EARG;
        ibsta_ = ERR;
        return ibsta_;
    }
    uint8_t cmds[6];
    uint32_t n = 0;
    cmds[n++] = UNL;
    cmds[n++] = MLA_byte(d->pad);
    if (d->sad >= 0) cmds[n++] = MSA_byte(d->sad);
    cmds[n++] = GTL;
    uint32_t rc = transport_->sendCommandBytes(cmds, n);
    if (rc != 0) {
        if (outIberr) *outIberr = EBUS;
        ibsta_ = ERR;
        return ibsta_;
    }
    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL;
    return ibsta_;
}

uint32_t GPIBBoard::localLockout(int32_t /*handle*/, uint32_t *outIberr) {
    if (!transport_) {
        if (outIberr) *outIberr = ENEB;
        ibsta_ = ERR;
        return ibsta_;
    }
    uint8_t cmds[2] = { LLO, 0 };
    uint32_t rc = transport_->sendCommandBytes(cmds, 1);
    if (rc != 0) {
        if (outIberr) *outIberr = EBUS;
        ibsta_ = ERR;
        return ibsta_;
    }
    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL | LOK;
    return ibsta_;
}

// Serial-poll presence probe. Address the device to talk under SPE and see
// whether it sources its status byte: a device that is there answers in a few
// milliseconds, one that is not there never does.
//
// This exists because the NDAC trick in listenerPresent() cannot work on the
// KUSB-488B. Once ATN is released the adapter's bus-line register reads 0x00
// for every line, identically whether or not an instrument is addressed
// (measured 2026-08-28: ibln on a present and an absent address both sample
// 0x00), so there is no signal there to read.
//
// Uses its own short timeout rather than any caller's: a serial poll answers
// immediately, so 100 ms is generous, and a sweep of all 30 addresses must
// stay bounded. The SPD/UNT teardown runs unconditionally -- skipping it on
// the failure path is what used to wedge the bus beyond software recovery
// (see readStatusByte).
bool GPIBBoard::serialPollProbe(uint8_t pad, int8_t sad) {
    if (!transport_) return false;

    uint8_t cmds[8];
    uint32_t n = 0;
    cmds[n++] = UNL;
    cmds[n++] = (uint8_t)((pad & 0x1f) | TAD);
    if (sad >= 0) cmds[n++] = (uint8_t)(((uint8_t)sad & 0x1f) | SAD);
    cmds[n++] = (uint8_t)((uint8_t)(boardPAD_ & 0x1f) | LAD);
    cmds[n++] = SPE;

    bool present = false;
    if (transport_->sendCommandBytes(cmds, n) == 0) {
        uint8_t  stb = 0;
        uint32_t got = 0;
        uint8_t  endFlag = 0;
        uint32_t rc = transport_->readData(&stb, 1, 0, 0, kProbeTimeoutUs,
                                           &got, &endFlag);
        present = (rc == 0 && got == 1);
    }

    uint8_t teardown[4] = { SPD, UNT, 0, 0 };
    transport_->sendCommandBytes(teardown, 2);
    return present;
}

uint32_t GPIBBoard::listenerPresent(uint8_t pad, int8_t sad, bool *outPresent,
                                    uint32_t *outIberr) {
    if (!transport_) {
        if (outIberr) *outIberr = ENEB;
        if (outPresent) *outPresent = false;
        ibsta_ = ERR;
        return ibsta_;
    }
    // The IEEE-488.2 FindListener protocol, matching linux-gpib's
    // listenerFound() (lib/ibFindLstn.c) exactly, because the previous
    // implementation here got two things wrong at once and neither is
    // obvious in isolation:
    //
    // 1. It sampled the bus lines immediately after addressing, with ATN
    //    still asserted. That is meaningless: while ATN is asserted, every
    //    device on the bus — not just the one just addressed — must
    //    participate in the command-byte handshake, so NDAC settles back to
    //    idle the moment the address bytes are acked, regardless of whether
    //    anyone is specifically listening at that address. (An empty bus
    //    still completes that handshake — nothing drives NDAC low to
    //    object — which is a documented trap elsewhere in this project.)
    //    The real trick only works *after* releasing ATN: a genuinely
    //    addressed listener then holds NDAC asserted indefinitely, waiting
    //    for a data byte that never comes, while an absent one lets the
    //    line float released.
    // 2. The polarity check was inverted. BusNDAC/ValidNDAC are the shared
    //    linux-gpib bit definitions (gpib_user.h is taken from there
    //    verbatim), where the bit being SET means the line is asserted —
    //    confirmed directly against linux-gpib's own check,
    //    `if (line_status & BusNDAC) return 1;`. This code was checking
    //    `== 0` instead, the opposite of every other consumer of these bits.
    uint8_t cmds[7];
    uint32_t n = 0;
    cmds[n++] = UNL;
    cmds[n++] = UNT;
    cmds[n++] = (uint8_t)((pad & 0x1f) | LAD);
    if (sad >= 0) cmds[n++] = (uint8_t)(((uint8_t)sad & 0x1f) | SAD);
    uint32_t rc = transport_->sendCommandBytes(cmds, n);
    if (rc == 0) rc = transport_->releaseAtn();

    bool present = false;
    if (rc == 0) {
        // 1.5ms settle time, per linux-gpib's own usleep(1500) here.
        IOSleep(2);
        uint16_t lines = 0;
        bool decided = false;
        if (transport_->readBusLines(&lines) == 0) {
            // A live GPIB bus always has SOMETHING asserted. An all-clear
            // state byte means the adapter is not reporting the bus rather
            // than that the bus is idle, so it is not evidence of absence --
            // treat it as no answer and probe properly instead. The KUSB-488B
            // reads exactly this once ATN is released; adapters that do report
            // their lines still take the cheap path.
            if ((lines & 0xFF00) != 0) {
                decided = true;
                present = (lines & ValidNDAC) && (lines & BusNDAC);
            }
        }
        if (!decided) present = serialPollProbe(pad, sad);
    }
    if (outPresent) *outPresent = present;
    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL;
    return ibsta_;
}

uint32_t GPIBBoard::sendCommands(int32_t /*handle*/, const uint8_t *cmds,
                                 uint32_t len, uint32_t *outIbcnt,
                                 uint32_t *outIberr) {
    if (!transport_) {
        if (outIberr) *outIberr = ENEB;
        if (outIbcnt) *outIbcnt = 0;
        ibsta_ = ERR;
        return ibsta_;
    }
    uint32_t rc = transport_->sendCommandBytes(cmds, len);
    if (outIbcnt) *outIbcnt = (rc == 0) ? len : 0;
    if (rc != 0) {
        if (outIberr) *outIberr = EBUS;
        ibsta_ = ERR;
        return ibsta_;
    }
    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL;
    return ibsta_;
}

// Try to bring a wedged bus back to life, cheapest option first.
//
// The failure this exists for: the NEC7210 core can be left holding RFD (for
// example after a read that ended on EOI, since the vendor bring-up programs
// AUXRA=HLDE). While that holdoff stands, *nothing* handshakes — not data,
// not even universal command bytes — and no amount of retrying at the
// protocol level helps. Historically the only cure was physically replugging
// the adapter, which is not an acceptable thing to ask of a running system.
//
// Returns true if a recovery step was actually performed.
bool GPIBBoard::recoverBus(const GPIBDescriptor *desc) {
    if (!transport_) return false;
    if (recoveryInProgress_) return false;   // don't recurse from a retry
    recoveryInProgress_ = true;

    bool recovered = (transport_->softReset() == 0);
    if (recovered) {
        // The core came back as if freshly attached, so re-establish the
        // controller state the board layer assumes: controller-in-charge,
        // and REN if the client had asked for it.
        transport_->pulseInterfaceClear();
        transport_->takeControl(false);
        if (renRequested_) transport_->setRemoteEnable(true);

        // Resetting our own core is only half of it. An instrument can be
        // stuck on its own — typically with a response queued that nobody
        // read, or an input buffer it will not drain — and in that state it
        // keeps acknowledging command bytes while refusing to accept data,
        // so addressing looks healthy and every write dies in the data
        // phase. Confirmed on hardware: after a chip reset alone, writes
        // still failed; a Selected Device Clear to the target as well, and
        // the very next *IDN? returned all 82 bytes.
        //
        // SDC is addressed to just this device rather than a universal DCL,
        // so recovering one instrument does not disturb others on the bus.
        if (desc) {
            uint8_t cmds[4];
            uint32_t n = 0;
            cmds[n++] = UNL;
            cmds[n++] = MLA_byte(desc->pad);
            if (desc->sad >= 0) cmds[n++] = MSA_byte(desc->sad);
            cmds[n++] = SDC;
            transport_->sendCommandBytes(cmds, n);
        }
    }

    recoveryInProgress_ = false;
    return recovered;
}

uint32_t GPIBBoard::softResetDiag() {
    if (!transport_) return ENEB;
    // No descriptor here: an operator-triggered reinit fixes the board, and
    // clearing instruments is left to whoever knows which ones matter.
    return recoverBus(nullptr) ? 0 : EBUS;
}

uint32_t GPIBBoard::resetDeviceDiag() {
    if (!transport_) return ENEB;
    return transport_->resetDevice();
}

uint32_t GPIBBoard::readRawRegisterDiag(uint16_t reg, uint8_t *outValue) {
    if (!transport_) return 0xFF;
    return transport_->readRawRegister(reg, outValue);
}

uint32_t GPIBBoard::writeRawRegisterDiag(uint16_t reg, uint8_t value) {
    if (!transport_) return 0xFF;
    return transport_->writeRawRegister(reg, value);
}

uint32_t GPIBBoard::busLineStatus(uint16_t *outLines, uint32_t *outIberr) {
    if (!transport_) {
        if (outIberr) *outIberr = ENEB;
        if (outLines) *outLines = 0;
        ibsta_ = ERR;
        return ibsta_;
    }
    uint16_t lines = 0;
    uint32_t rc = transport_->readBusLines(&lines);
    if (rc != 0) {
        if (outIberr) *outIberr = ECAP;
        if (outLines) *outLines = 0;
        ibsta_ = ERR;
        return ibsta_;
    }
    if (outLines) *outLines = lines;
    if (outIberr) *outIberr = 0;
    ibsta_ = CMPL;
    return ibsta_;
}

uint32_t GPIBBoard::waitForStatus(int32_t handle, int32_t mask,
                                  uint32_t /*timeout_us*/, uint32_t *outIberr) {
    // M1: every other op is synchronous so ibwait returns the last status.
    // Async masks (SRQI, RQS, EVENT...) need interrupt-pipe support — M2.
    GPIBDescriptor *d = descriptorFor(handle);
    if (!d) {
        if (outIberr) *outIberr = EARG;
        ibsta_ = ERR;
        return ibsta_;
    }
    (void)mask;
    if (outIberr) *outIberr = 0;
    return ibsta_;
}
