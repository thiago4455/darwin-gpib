//
//  KUSB488BLoader.h
//  darwin-gpib driver
//
//  Firmware download for the KUSB-488B loader stage (VID 0x05E6/PID 0xEEEE).
//
//  Sequence, mirroring kusbgpib_fwdlx64.sys:
//    1. FPGA bitstream (USB-GPIB.RBF) via vendor request 0xA4
//    2. external-RAM blob via 0xA3 at 0x4000
//    3. main 8051 firmware via 0xA0, bracketed by CPUCS reset
//  after which the device re-enumerates as PID 0x488B.
//
//  The legacy EEPROM stage (bcdDevice < 4, vendor request 0xA2) is
//  deliberately not implemented — it rewrites the adapter's EEPROM.
//

#ifndef KUSB488BLoader_h
#define KUSB488BLoader_h

#include <stdint.h>

class IOService;
class IOUSBHostDevice;
class IOBufferMemoryDescriptor;

class KUSB488BLoader {
public:
    bool init(IOUSBHostDevice *device, IOService *owningService);
    void free();

    // Runs the full download. Returns kIOReturnSuccess on success.
    kern_return_t downloadFirmware(uint16_t bcdDevice);

private:
    kern_return_t vendorWrite(uint8_t request, uint16_t wValue, uint16_t wIndex,
                              const uint8_t *data, uint16_t len);
    // Chunked download at KUSB_FW_CHUNK, wValue advancing by offset.
    kern_return_t downloadChunked(uint8_t request, uint16_t baseAddr,
                                  const uint8_t *data, uint32_t len);
    // FPGA configuration: begin, then chunks with wIndex=1 on the last one.
    kern_return_t downloadBitstream(const uint8_t *data, uint32_t len);
    kern_return_t setCPUReset(bool hold);

    IOUSBHostDevice          *device_;
    IOService                *owner_;
    IOBufferMemoryDescriptor *buffer_;
};

#endif /* KUSB488BLoader_h */
