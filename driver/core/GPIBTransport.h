//
//  GPIBTransport.h
//  darwin-gpib driver
//
//  Abstract transport interface. GPIBBoard drives the bus through this
//  interface, letting a single state-machine implementation sit on top of
//  any hardware backend (NI USB-HS, Agilent 82357A/B, ...). Concrete
//  transports subclass IGPIBTransport and are constructed by the matched
//  IOService (ni_usb, agilent_82357, ...).
//
//  Not an OSObject — these live in the dext process and are owned by the
//  hosting IOService. Two-phase init/free in the concrete subclass.
//

#ifndef GPIBTransport_h
#define GPIBTransport_h

#include <stdint.h>

// Transport-level error codes returned by IGPIBTransport methods.
// 0 = success. Concrete transports translate hardware-specific errors
// into these values so GPIBBoard can build ibsta/iberr consistently.
enum GPIBTransportError : uint32_t {
    GPIBT_OK              = 0,
    GPIBT_ERR_IO          = 1,   // generic bus / USB error
    GPIBT_ERR_TIMEOUT     = 2,   // operation timed out
    GPIBT_ERR_NO_LISTENER = 3,   // addressed device did not accept
    GPIBT_ERR_ABORTED     = 4,   // transfer was aborted
    GPIBT_ERR_NOT_READY   = 5,   // transport not attached / offline
};

class IGPIBTransport {
public:
    virtual ~IGPIBTransport() {}

    // Bring the adapter up (discover endpoints, upload firmware register
    // state, etc.). Called from the owning IOService's Start().
    virtual uint32_t attach() = 0;

    // Release pipes and stop any pending I/O.
    virtual void detach() = 0;

    // Assert IFC for ~100us, then release. Used by GPIBBoard::setOnline
    // and interfaceClear.
    virtual uint32_t pulseInterfaceClear() = 0;

    // Drive REN.
    virtual uint32_t setRemoteEnable(bool enable) = 0;

    // Send raw command bytes with ATN asserted. Used for addressing and
    // universal/addressed command messages (UNL, MLA, SDC, ...).
    virtual uint32_t sendCommandBytes(const uint8_t *cmds, uint32_t len) = 0;

    // Data-out. send_eoi asserts EOI on the final byte. *outBytesWritten
    // receives the number of bytes actually transferred.
    virtual uint32_t writeData(const uint8_t *buf, uint32_t len, bool send_eoi,
                               uint32_t timeout_us, uint32_t *outBytesWritten) = 0;

    // Data-in. Terminates on request_count bytes, EOI, or (if REOS set in
    // eos_flags) on eos_char. *outEnd is set to 1 iff END condition was
    // observed.
    virtual uint32_t readData(uint8_t *buf, uint32_t request_count,
                              uint8_t eos_char, uint8_t eos_flags,
                              uint32_t timeout_us,
                              uint32_t *outBytesRead, uint8_t *outEnd) = 0;

    // Optional operations. Default implementations return GPIBT_ERR_IO so
    // transports that don't yet implement them can be identified as "no
    // capability" by the board layer without every driver needing stubs.

    // Read the currently-visible bus control-line state, encoded as a
    // ValidXXX|BusXXX bitmask (see bus_control_line in gpib_user.h).
    virtual uint32_t readBusLines(uint16_t *outLines) {
        (void)outLines;
        return GPIBT_ERR_IO;
    }

    // Become controller-in-charge. Default is a no-op so transports that have
    // not implemented it keep their previous behaviour.
    virtual uint32_t takeControl(bool synchronous) {
        (void)synchronous;
        return GPIBT_OK;
    }

    // Release ATN without declaring any talk/listen role — the NEC7210
    // family's "go to standby". Used by GPIBBoard::listenerPresent() to run
    // the standard IEEE-488.2 FindListener trick: address a candidate as
    // listener under ATN, then release ATN and let the bus settle. A device
    // that is genuinely addressed holds NDAC asserted waiting for a byte
    // that never comes; an absent one lets the line float released. Sampling
    // NDAC while ATN is still asserted (mid command-byte handshake) tells
    // you nothing about a specific address, because every device on the bus
    // — not just the addressed one — must participate in that handshake.
    // Default returns GPIBT_ERR_IO so listenerPresent() degrades to "not
    // found" on transports that don't implement this yet, the same
    // convention as readBusLines().
    virtual uint32_t releaseAtn() {
        return GPIBT_ERR_IO;
    }

    // Read one hardware register by selector — backs gpibctl regs/peek, a
    // supported debugging aid, not scaffolding.
    virtual uint32_t readRawRegister(uint16_t reg, uint8_t *outValue) {
        (void)reg; (void)outValue;
        return GPIBT_ERR_IO;
    }

    // Companion to the above: write one register — backs gpibctl poke, so a
    // bring-up sequence can be tried from userspace without rebuilding the
    // dext or replugging the adapter.
    // Recovery hooks. A GPIB core can latch itself into a state where
    // nothing on the bus can handshake any more (on the NEC7210 family, a
    // pending RFD holdoff does exactly this). A driver that can only be
    // rescued by physically unplugging the adapter is not much use in
    // production, so transports that can recover in software should override
    // these. Defaults report "not supported" so existing transports keep
    // building unchanged.
    //
    // softReset(): re-run whatever bring-up attach() performs, clearing the
    // core's latched state without touching the USB connection.
    virtual uint32_t softReset() {
        return GPIBT_ERR_NOT_READY;
    }
    // resetDevice(): force the device to re-enumerate — the programmatic
    // equivalent of a replug. Expect the current transport instance to be
    // torn down as a result.
    virtual uint32_t resetDevice() {
        return GPIBT_ERR_NOT_READY;
    }

    /// Diagnostic: override the per-transfer data chunk size at runtime.
    /// 0 means "no chunking". Only meaningful for transports that chunk;
    /// exists so the KUSB-488B's ~85-byte unchunked write ceiling can be
    /// investigated without a rebuild-and-replug cycle per experiment.
    virtual uint32_t setMaxDataChunk(uint32_t /*bytes*/) {
        return GPIBT_ERR_NOT_READY;   // transport does not chunk
    }

    virtual uint32_t writeRawRegister(uint16_t reg, uint8_t value) {
        (void)reg; (void)value;
        return GPIBT_ERR_IO;
    }
};

#endif /* GPIBTransport_h */
