//
//  Agilent82357Transport.h
//  darwin-gpib driver
//
//  USB transport for the Agilent 82357A / 82357B USB→GPIB adapters. Speaks
//  the bulk-pipe register R/W protocol and the control-pipe abort/status
//  requests used by linux-gpib's agilent_82357a driver.
//
//  Two-phase init/free (no constructor). Owned by the agilent_82357
//  IOService. Implements IGPIBTransport so GPIBBoard doesn't need to know
//  which hardware backend is underneath.
//

#ifndef Agilent82357Transport_h
#define Agilent82357Transport_h

#include <stdint.h>

#include "GPIBTransport.h"

class IOService;
class IOUSBHostInterface;
class IOUSBHostPipe;
class IOUSBHostDevice;
class IOBufferMemoryDescriptor;

// One register write or read.
struct Agilent82357RegPair {
    uint8_t address;
    uint8_t value;
};

class Agilent82357Transport : public IGPIBTransport {
public:
    // productId picks 82357A vs 82357B (endpoints and behaviour differ).
    // owningService is used as the `forClient` on device-level control
    // requests (abort / status).
    bool init(IOUSBHostInterface *interface, IOService *owningService,
              uint16_t productId);
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

    // Register helpers (exposed for the init sequence / status reads).
    uint32_t writeRegisters(const Agilent82357RegPair *writes, uint32_t count);
    uint32_t readRegisters(Agilent82357RegPair *reads, uint32_t count);

private:
    enum { kBulkBufferSize = 4096 };

    uint32_t openPipes();
    void     closePipes();
    uint32_t runInitSequence();
    uint32_t sendAbort(bool flush);
    uint32_t bulkOut(const uint8_t *bytes, uint32_t len, uint32_t timeout_us);
    uint32_t bulkIn(uint8_t *bytes, uint32_t capacity, uint32_t timeout_us,
                    uint32_t *outBytesRead);
    // Convenience — write a single register.
    uint32_t writeReg(uint8_t address, uint8_t value);
    // Fast talker t1 value from nanoseconds (matches linux-gpib helper).
    static uint8_t nanosToFastTalker(uint32_t *nanosec_inout);

    IOUSBHostInterface       *interface_;
    IOUSBHostDevice          *device_;
    IOService                *owner_;
    IOUSBHostPipe            *bulkOutPipe_;
    IOUSBHostPipe            *bulkInPipe_;
    IOUSBHostPipe            *interruptInPipe_;

    IOBufferMemoryDescriptor *outBuffer_;
    IOBufferMemoryDescriptor *inBuffer_;

    uint16_t productId_;                 // 0x0107 for 82357A, 0x0718 for 82357B
    uint8_t  bulkOutEP_;
    uint8_t  interruptEP_;
    uint8_t  hwControlBits_;             // cached HW_CONTROL register value
};

#endif /* Agilent82357Transport_h */
