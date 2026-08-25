//
//  KUSB488BLoader.cpp
//  darwin-gpib driver
//

#include <os/log.h>
#include <string.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <USBDriverKit/IOUSBHostDevice.h>

#include "KUSB488BLoader.h"
#include "kusb_488b_protocol.h"
#include "kusb_488b_firmware.h"

namespace {
constexpr uint32_t kFwTimeoutMs = 5000;
}

bool KUSB488BLoader::init(IOUSBHostDevice *device, IOService *owningService) {
    if (!device || !owningService) return false;
    device_ = device;
    owner_  = owningService;
    buffer_ = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOut,
                                                          KUSB_FW_CHUNK, 0,
                                                          &buffer_);
    return ret == kIOReturnSuccess && buffer_ != nullptr;
}

void KUSB488BLoader::free() {
    OSSafeReleaseNULL(buffer_);
    device_ = nullptr;
    owner_  = nullptr;
}

kern_return_t KUSB488BLoader::vendorWrite(uint8_t request, uint16_t wValue,
                                          uint16_t wIndex, const uint8_t *data,
                                          uint16_t len) {
    if (!device_ || !owner_) return kIOReturnNotReady;

    IOMemoryDescriptor *md = nullptr;
    if (len > 0) {
        if (!buffer_ || len > KUSB_FW_CHUNK) return kIOReturnBadArgument;
        uint64_t addr = 0, sz = 0;
        kern_return_t kr = buffer_->Map(0, 0, 0, 0, &addr, &sz);
        if (kr != kIOReturnSuccess) return kr;
        memcpy((void *)addr, data, len);
        md = buffer_;
    }

    uint16_t transferred = 0;
    kern_return_t kr = device_->DeviceRequest(owner_,
                                              0x40,   // OUT | VENDOR | DEVICE
                                              request, wValue, wIndex, len,
                                              md, &transferred, kFwTimeoutMs);
    if (kr != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT,
               "kusb_488b_loader: vendor write 0x%02x wValue=0x%04x failed 0x%x",
               request, wValue, kr);
        return kr;
    }
    return (transferred == len) ? kIOReturnSuccess : kIOReturnUnderrun;
}

kern_return_t KUSB488BLoader::setCPUReset(bool hold) {
    uint8_t v = hold ? 1 : 0;
    return vendorWrite(KUSB_FW_REQ_RAM, KUSB_FW_CPUCS_ADDR, 0, &v, 1);
}

kern_return_t KUSB488BLoader::downloadChunked(uint8_t request, uint16_t baseAddr,
                                              const uint8_t *data, uint32_t len) {
    uint32_t offset = 0;
    while (offset < len) {
        uint32_t chunk = len - offset;
        if (chunk > KUSB_FW_CHUNK) chunk = KUSB_FW_CHUNK;
        kern_return_t kr = vendorWrite(request,
                                       (uint16_t)(baseAddr + offset), 0,
                                       data + offset, (uint16_t)chunk);
        if (kr != kIOReturnSuccess) return kr;
        offset += chunk;
    }
    return kIOReturnSuccess;
}

kern_return_t KUSB488BLoader::downloadBitstream(const uint8_t *data, uint32_t len) {
    // Begin: zero-length request with wValue = 0.
    kern_return_t kr = vendorWrite(KUSB_FW_REQ_FPGA, 0, 0, nullptr, 0);
    if (kr != kIOReturnSuccess) return kr;

    uint32_t offset = 0;
    while (offset < len) {
        uint32_t chunk = len - offset;
        bool last = false;
        if (chunk > KUSB_FW_CHUNK) {
            chunk = KUSB_FW_CHUNK;
        } else {
            last = true;
        }
        // wValue = 1 for data chunks; wIndex flags the final one.
        kr = vendorWrite(KUSB_FW_REQ_FPGA, 1, last ? 1 : 0,
                         data + offset, (uint16_t)chunk);
        if (kr != kIOReturnSuccess) return kr;
        offset += chunk;
    }
    return kIOReturnSuccess;
}

kern_return_t KUSB488BLoader::downloadFirmware(uint16_t bcdDevice) {
#if !KUSB_HAVE_FIRMWARE
    (void)bcdDevice;
    os_log(OS_LOG_DEFAULT,
           "kusb_488b_loader: no firmware compiled in. Generate "
           "kusb_488b_firmware_data.h with tools/extract_kusb_firmware.py "
           "from a Windows KI-488 installation (needs USB-GPIB.RBF and "
           "kusbgpib_fwdlx64.sys).");
    return kIOReturnUnsupported;
#else
    if (bcdDevice < KUSB_LEGACY_BCD_DEVICE) {
        // The Windows driver would rewrite the adapter's EEPROM here via
        // request 0xA2. That is destructive and irreversible, so we stop
        // rather than replay it.
        os_log(OS_LOG_DEFAULT,
               "kusb_488b_loader: bcdDevice 0x%04x needs the legacy EEPROM "
               "stage, which is not implemented (it rewrites the adapter's "
               "EEPROM). Use the Windows driver once to update this unit.",
               bcdDevice);
        return kIOReturnUnsupported;
    }

    os_log(OS_LOG_DEFAULT, "kusb_488b_loader: downloading FPGA bitstream (%u bytes)",
           kKusbFpgaBitstream.length);
    kern_return_t kr = downloadBitstream(kKusbFpgaBitstream.data,
                                         kKusbFpgaBitstream.length);
    if (kr != kIOReturnSuccess) return kr;

    os_log(OS_LOG_DEFAULT, "kusb_488b_loader: downloading ext-RAM blob (%u bytes)",
           kKusbExtRamBlob.length);
    kr = downloadChunked(KUSB_FW_REQ_EXTRAM, 0x4000,
                         kKusbExtRamBlob.data, kKusbExtRamBlob.length);
    if (kr != kIOReturnSuccess) return kr;

    os_log(OS_LOG_DEFAULT, "kusb_488b_loader: downloading 8051 firmware (%u bytes)",
           kKusbMainFirmware.length);
    kr = setCPUReset(true);
    if (kr != kIOReturnSuccess) return kr;
    kr = downloadChunked(KUSB_FW_REQ_RAM, 0,
                         kKusbMainFirmware.data, kKusbMainFirmware.length);
    if (kr != kIOReturnSuccess) {
        setCPUReset(false);
        return kr;
    }
    kr = setCPUReset(false);
    if (kr != kIOReturnSuccess) return kr;

    os_log(OS_LOG_DEFAULT,
           "kusb_488b_loader: firmware download complete; device should "
           "re-enumerate as PID 0x%04x", KUSB_PID_488B);
    return kIOReturnSuccess;
#endif
}
