//
//  kusb_488b_loader.cpp
//  darwin-gpib driver
//

#include <os/log.h>

#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>
#include <USBDriverKit/IOUSBHostDevice.h>

#include "kusb_488b_loader.h"
#include "kusb_488b_protocol.h"
#include "KUSB488BLoader.h"

struct kusb_488b_loader_IVars {
    IOUSBHostDevice *device;
    KUSB488BLoader  *loader;
};

bool kusb_488b_loader::init() {
    if (!super::init()) return false;
    ivars = IONewZero(kusb_488b_loader_IVars, 1);
    return ivars != nullptr;
}

void kusb_488b_loader::free() {
    if (ivars) {
        if (ivars->loader) {
            ivars->loader->free();
            IOSafeDeleteNULL(ivars->loader, KUSB488BLoader, 1);
        }
        OSSafeReleaseNULL(ivars->device);
        IOSafeDeleteNULL(ivars, kusb_488b_loader_IVars, 1);
    }
    super::free();
}

kern_return_t IMPL(kusb_488b_loader, Start) {
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "kusb_488b_loader: super::Start failed 0x%x", ret);
        return ret;
    }

    // We match the whole device here, not an interface: the firmware download
    // is all default-endpoint control traffic.
    IOUSBHostDevice *usbDevice = OSDynamicCast(IOUSBHostDevice, provider);
    if (!usbDevice) {
        os_log(OS_LOG_DEFAULT, "kusb_488b_loader: provider is not IOUSBHostDevice");
        return kIOReturnUnsupported;
    }
    usbDevice->retain();
    ivars->device = usbDevice;

    uint16_t bcdDevice = 0;
    const IOUSBDeviceDescriptor *dev = usbDevice->CopyDeviceDescriptor();
    if (dev) {
        bcdDevice = dev->bcdDevice;
        os_log(OS_LOG_DEFAULT,
               "kusb_488b_loader: VID=0x%04x PID=0x%04x bcdDevice=0x%04x",
               dev->idVendor, dev->idProduct, dev->bcdDevice);
        if (dev->idProduct != KUSB_PID_LOADER) {
            os_log(OS_LOG_DEFAULT,
                   "kusb_488b_loader: PID 0x%04x is not the loader ID", dev->idProduct);
            return kIOReturnUnsupported;
        }
    }

    ret = usbDevice->Open(this, 0, 0);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "kusb_488b_loader: device Open failed 0x%x", ret);
        return ret;
    }

    ivars->loader = IONewZero(KUSB488BLoader, 1);
    if (!ivars->loader || !ivars->loader->init(usbDevice, this)) {
        os_log(OS_LOG_DEFAULT, "kusb_488b_loader: loader init failed");
        usbDevice->Close(this, 0);
        return kIOReturnNoMemory;
    }

    ret = ivars->loader->downloadFirmware(bcdDevice);
    usbDevice->Close(this, 0);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT,
               "kusb_488b_loader: firmware download failed 0x%x", ret);
        return ret;
    }

    // The device drops off the bus and comes back as PID 0x488B; there is
    // nothing further for this service to do.
    os_log(OS_LOG_DEFAULT, "kusb_488b_loader: done, awaiting re-enumeration");
    return kIOReturnSuccess;
}

kern_return_t IMPL(kusb_488b_loader, Stop) {
    os_log(OS_LOG_DEFAULT, "kusb_488b_loader: Stop");
    return Stop(provider, SUPERDISPATCH);
}
