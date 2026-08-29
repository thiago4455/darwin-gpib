//
//  KUSB488BTransport.h
//  darwin-gpib driver
//
//  USB transport for the Keithley KUSB-488B (ADLINK USB-3488A).
//
//  Every bus operation is a vendor control request that arms the transfer,
//  followed by a bulk transfer on a dedicated pipe, followed by polling a
//  status block over a second control request. See kusb_488b_protocol.h.
//
//  Two-phase init/free (no constructor). Owned by the kusb_488b IOService.
//

#ifndef KUSB488BTransport_h
#define KUSB488BTransport_h

#include <stdint.h>

#include "GPIBTransport.h"
#include "kusb_488b_protocol.h"

class IOService;
class IOUSBHostInterface;
class IOUSBHostPipe;
class IOUSBHostDevice;
class IOBufferMemoryDescriptor;

class KUSB488BTransport : public IGPIBTransport {
public:
    bool init(IOUSBHostInterface *interface, IOService *owningService);
    void free();

    // IGPIBTransport
    virtual uint32_t attach() override;
    virtual void     detach() override;
    virtual uint32_t pulseInterfaceClear() override;
    virtual uint32_t setRemoteEnable(bool enable) override;
    virtual uint32_t sendCommandBytes(const uint8_t *cmds, uint32_t len) override;
    virtual uint32_t writeData(const uint8_t *buf, uint32_t len, bool send_eoi,
                               uint32_t timeout_us, uint32_t *outBytesWritten) override;
    virtual uint32_t readData(uint8_t *buf, uint32_t request_count,
                              uint8_t eos_char, uint8_t eos_flags,
                              uint32_t timeout_us,
                              uint32_t *outBytesRead, uint8_t *outEnd) override;
    virtual uint32_t readBusLines(uint16_t *outLines) override;
    virtual uint32_t readRawRegister(uint16_t reg, uint8_t *outValue) override;
    virtual uint32_t takeControl(bool synchronous) override;
    virtual uint32_t releaseAtn() override;
    virtual uint32_t writeRawRegister(uint16_t reg, uint8_t value) override;
    virtual uint32_t softReset() override;
    virtual uint32_t resetDevice() override;

    // Read one core register (the USBGPIB_PORT_READ equivalent).
    uint32_t readRegister(uint8_t reg, uint8_t *outValue);
    // Write a raw command block (stream of `05 <value>` pairs).
    uint32_t writeCommandBlock(const uint8_t *block, uint32_t len);

private:
    enum { kBulkBufferSize = 0x1000 };

    // Endpoint discovery: the Windows driver addresses pipes by their index
    // in the interface descriptor, so we resolve indices to addresses the
    // same way.
    // Chip bring-up, shared by attach() and softReset().
    uint32_t runBringUpSequence();
    uint32_t resolveEndpoints();
    uint32_t openPipes();
    void     closePipes();

    // Arm a transfer: sends the 8-byte {timeout_ms, length} header.
    uint32_t beginTransfer(uint8_t request, uint16_t wValue, uint16_t wIndex,
                           uint32_t length, uint32_t timeout_us);
    // Poll the status block until it leaves the busy state. countWidth is 2
    // for command transfers and 4 for data — see KUSB_COUNT_WIDTH_* .
    uint32_t pollStatus(uint32_t statusLen, uint32_t countWidth,
                        uint32_t timeout_us,
                        uint8_t *status, uint32_t *outCount, uint8_t *outEnd);
    uint32_t abortTransfer();

    // Abort any armed transfer AND clear a halted bulk endpoint. A failed
    // firmware transfer leaves both behind (measured on hardware): an
    // armed-but-never-serviced transfer whose status the next call reports
    // by mistake, and a STALLed bulk endpoint that fails every following
    // bulk transfer with kIOReturnAborted-equivalent errors. Neither the
    // 7210 core reset in runBringUpSequence() nor a USB port reset
    // (resetDevice()) clears either one. The vendor driver does exactly
    // this on its own failure path (FUN_1400036e0/FUN_140002e1c call 0xBB
    // then reset the pipe) -- this mirrors that, not a workaround. Call
    // before any fresh firmware transfer; harmless when nothing is wrong.
    void recoverFirmwareEngine();

    // Go To Standby (release ATN) via the firmware's own operation, vendor
    // request 0xB7 == ibgts. Must be issued between the addressing command
    // bytes and the data phase of every firmware-path transfer.
    uint32_t goToStandby();

    /// Runtime-overridable chunk size; defaults to KUSB_MAX_DATA_CHUNK.
    uint32_t setMaxDataChunk(uint32_t bytes) override;
    uint32_t maxDataChunk_ = KUSB_MAX_DATA_CHUNK;

    uint32_t controlOut(uint8_t request, uint16_t wValue, uint16_t wIndex,
                        const uint8_t *data, uint16_t len);
    uint32_t controlIn(uint8_t request, uint16_t wValue, uint16_t wIndex,
                       uint8_t *data, uint16_t len, uint16_t *outLen);

    uint32_t bulkOut(IOUSBHostPipe *pipe, const uint8_t *bytes, uint32_t len,
                     uint32_t timeout_us);
    uint32_t bulkIn(IOUSBHostPipe *pipe, uint8_t *bytes, uint32_t capacity,
                    uint32_t timeout_us, uint32_t *outBytesRead);

    // Send a single AUXMR-style auxiliary command.
    uint32_t writeAux(uint8_t value);
    // Release an outstanding RFD holdoff, if and only if one is pending.
    void     releaseRfdHoldoffIfPending();
    uint32_t pulseInterfaceClearViaAux();
    // Register-bit-banged fallback implementations, kept for reference/
    // rollback. As of this experiment the public sendCommandBytes/writeData/
    // readData delegate to the firmware bulk-transfer path instead (see the
    // .cpp) — decompiling the vendor Windows driver showed it exclusively
    // uses this bulk path (arm via 0xB3/0xB9/0xBA, bulk transfer, poll 0xB0)
    // and never bit-bangs AUXMR/CDOR directly for actual transfers.
    uint32_t sendCommandBytesViaRegisters(const uint8_t *cmds, uint32_t len);
    uint32_t writeDataViaRegisters(const uint8_t *buf, uint32_t len, bool send_eoi,
                                  uint32_t timeout_us, uint32_t *outBytesWritten);
    uint32_t readDataViaRegisters(uint8_t *buf, uint32_t request_count,
                                 uint8_t eos_char, uint8_t eos_flags,
                                 uint32_t timeout_us,
                                 uint32_t *outBytesRead, uint8_t *outEnd);

    // Firmware bulk-transfer implementations, matching the vendor Windows
    // driver's own sequence exactly (arm via 0xB3/0xB9/0xBA, bulk transfer,
    // poll status via 0xB0) with no manual AUXMR/ADMR register writes for
    // bus role anywhere — the vendor issues none, and the working theory
    // for why an earlier, register-addressed + firmware-data hybrid failed
    // is that the firmware keeps its own model of bus state that register
    // addressing behind its back leaves stale.
    uint32_t sendCommandBytesViaFirmware(const uint8_t *cmds, uint32_t len);
    uint32_t writeDataViaFirmware(const uint8_t *buf, uint32_t len, bool send_eoi,
                                 uint32_t timeout_us, uint32_t *outBytesWritten);
    uint32_t readDataViaFirmware(uint8_t *buf, uint32_t request_count,
                                uint8_t eos_char, uint8_t eos_flags,
                                uint32_t timeout_us,
                                uint32_t *outBytesRead, uint8_t *outEnd);

    IOUSBHostInterface       *interface_;
    IOUSBHostDevice          *device_;
    IOService                *owner_;

    IOUSBHostPipe            *dataOutPipe_;
    IOUSBHostPipe            *cmdOutPipe_;
    IOUSBHostPipe            *dataInPipe_;

    IOBufferMemoryDescriptor *outBuffer_;
    IOBufferMemoryDescriptor *inBuffer_;
    IOBufferMemoryDescriptor *ctrlBuffer_;

    // bEndpointAddress for pipe indices 2, 3, 4.
    uint8_t                   dataOutEP_;
    uint8_t                   cmdOutEP_;
    uint8_t                   dataInEP_;

    bool                      renAsserted_;
    // True while this chip is holding NRFD low after a read that ended on
    // END (the bring-up leaves AUXRA=HLDE). Tracked explicitly so AUX_FH is
    // only ever issued when a holdoff really is outstanding — see
    // releaseRfdHoldoffIfPending().
    bool                      rfdHoldoffPending_;
};

#endif /* KUSB488BTransport_h */
