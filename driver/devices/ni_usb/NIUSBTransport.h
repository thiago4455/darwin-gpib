//
//  NIUSBTransport.h
//  darwin-gpib driver
//
//  USB transport for the NI USB-HS GPIB adapter. Owns the bulk OUT, bulk IN,
//  and interrupt IN pipes and exposes the operations the GPIBBoard layer
//  needs: register read/write, raw GPIB command bytes, data write, data read,
//  control-line manipulation.
//
//  Two-phase init/free (no constructor). All I/O is synchronous, dispatched
//  on the dext's default dispatch queue.
//
//  Implements IGPIBTransport so GPIBBoard can drive either NI or Agilent
//  hardware through the same interface.
//

#ifndef NIUSBTransport_h
#define NIUSBTransport_h

#include <stdint.h>

#include "GPIBTransport.h"

class IOUSBHostInterface;
class IOUSBHostPipe;
class IOBufferMemoryDescriptor;

class NIUSBTransport : public IGPIBTransport {
public:
    bool init(IOUSBHostInterface *interface);
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

    // TNT4882 register access via the USB protocol (used by attach()).
    uint32_t writeRegister(uint8_t nec_reg, uint8_t value);
    uint32_t readRegister(uint8_t nec_reg, uint8_t *outValue);
    uint32_t writeAuxCmd(uint8_t aux_cmd);

private:
    enum { kBulkBufferSize = 4096 };

    uint32_t bulkOut(const uint8_t *bytes, uint32_t len, uint32_t timeout_us);
    uint32_t bulkIn(uint8_t *bytes, uint32_t capacity, uint32_t timeout_us,
                    uint32_t *outBytesRead);
    uint32_t runInitSequence();
    uint32_t openPipes();
    void     closePipes();

    IOUSBHostInterface       *interface_;
    IOUSBHostPipe            *bulkOutPipe_;
    IOUSBHostPipe            *bulkInPipe_;
    IOUSBHostPipe            *interruptInPipe_;

    IOBufferMemoryDescriptor *outBuffer_;
    IOBufferMemoryDescriptor *inBuffer_;
};

#endif /* NIUSBTransport_h */
