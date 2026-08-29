//
//  kusb_488b.cpp
//  darwin-gpib driver
//

#include <os/log.h>

#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSString.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostDevice.h>

#include "DriverUtils.h"
#include "kusb_488b.h"
#include "kusb_488b_protocol.h"
#include "KUSB488BTransport.h"
#include "GPIBBoard.h"

struct kusb_488b_IVars {
    IOUSBHostInterface *interface;
    KUSB488BTransport  *transport;
    GPIBBoard          *board;
};

bool kusb_488b::init() {
    if (!super::init()) return false;
    ivars = IONewZero(kusb_488b_IVars, 1);
    return ivars != nullptr;
}

void kusb_488b::free() {
    if (ivars) {
        if (ivars->board) {
            ivars->board->free();
            IOSafeDeleteNULL(ivars->board, GPIBBoard, 1);
        }
        if (ivars->transport) {
            ivars->transport->free();
            IOSafeDeleteNULL(ivars->transport, KUSB488BTransport, 1);
        }
        OSSafeReleaseNULL(ivars->interface);
        IOSafeDeleteNULL(ivars, kusb_488b_IVars, 1);
    }
    super::free();
}

GPIBBoard *kusb_488b::getBoard() {
    return ivars ? ivars->board : nullptr;
}

kern_return_t IMPL(kusb_488b, Start) {
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: super::Start failed 0x%x", ret);
        return ret;
    }

    IOUSBHostInterface *usbInterface = OSDynamicCast(IOUSBHostInterface, provider);
    if (!usbInterface) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: provider is not IOUSBHostInterface");
        return kIOReturnUnsupported;
    }
    usbInterface->retain();
    ivars->interface = usbInterface;

    uint16_t productId = 0;
    IOUSBHostDevice *usbDevice = nullptr;
    if (usbInterface->CopyDevice(&usbDevice) == kIOReturnSuccess && usbDevice) {
        const IOUSBDeviceDescriptor *dev = usbDevice->CopyDeviceDescriptor();
        if (dev) {
            productId = dev->idProduct;
            const IOUSBStringDescriptor *prodStr =
                usbDevice->CopyStringDescriptor(dev->iProduct);
            OSString *product = copyDeviceString(prodStr, "Keithley KUSB-488B");
            os_log(OS_LOG_DEFAULT,
                   "kusb_488b: USB device VID=0x%04x PID=0x%04x bcdDevice=0x%04x product=%{public}s",
                   dev->idVendor, dev->idProduct, dev->bcdDevice,
                   product ? product->getCStringNoCopy() : "?");
            if (prodStr) IOFree((void *)prodStr, prodStr->bLength);
            OSSafeReleaseNULL(product);
        }
        OSSafeReleaseNULL(usbDevice);
    }

    // The loader PID means firmware has not been downloaded yet; that is the
    // kusb_488b_loader service's job, not ours.
    if (productId == KUSB_PID_LOADER) {
        os_log(OS_LOG_DEFAULT,
               "kusb_488b: device is still in loader mode (PID=0x%04x)", productId);
        return kIOReturnUnsupported;
    }

    ret = usbInterface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: IOUSBHostInterface::Open failed 0x%x", ret);
        return ret;
    }

    // Constructed, not just allocated — see IONewZeroConstruct in DriverUtils.h.
    // Every failure from here on MUST close the interface before returning.
    //
    // DriverKit does not call Stop() when Start() fails, so nothing else will
    // undo the Open() above. free() releases our *retain*, which is a different
    // thing: the open session stays held, leaving a device that is claimed but
    // has no working driver, and a re-match cannot Open() it again. That is
    // reachable in practice because attach() runs the bring-up against a device
    // that may already be wedged, so a wedged adapter would otherwise turn into
    // a claimed-and-unusable one.
    ivars->transport = IONewZeroConstruct<KUSB488BTransport>();
    if (!ivars->transport || !ivars->transport->init(usbInterface, this)) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: transport init failed");
        usbInterface->Close(this, 0);
        return kIOReturnNoMemory;
    }
    uint32_t rc = ivars->transport->attach();
    if (rc != 0) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: transport attach failed rc=%u", rc);
        ivars->transport->detach();
        usbInterface->Close(this, 0);
        return kIOReturnError;
    }

    ivars->board = IONewZero(GPIBBoard, 1);
    if (!ivars->board || !ivars->board->init(ivars->transport)) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: board init failed");
        ivars->transport->detach();
        usbInterface->Close(this, 0);
        return kIOReturnNoMemory;
    }

    ivars->board->setOnline(true);

    RegisterService();
    os_log(OS_LOG_DEFAULT, "kusb_488b: started and registered");
    return kIOReturnSuccess;
}

kern_return_t IMPL(kusb_488b, Stop) {
    os_log(OS_LOG_DEFAULT, "kusb_488b: Stop");
    if (ivars && ivars->board) ivars->board->setOnline(false);
    if (ivars && ivars->transport) ivars->transport->detach();
    if (ivars && ivars->interface) {
        ivars->interface->Close(this, 0);
    }
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t IMPL(kusb_488b, NewUserClient) {
    // Unconditional: tells us whether the kernel dispatches into the dext at
    // all. If IOServiceOpen fails without this line appearing, the rejection
    // is kernel-side and nothing in this function is responsible.
    os_log(OS_LOG_DEFAULT, "kusb_488b: NewUserClient entered, type=%u", type);
    if (type != 0) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: NewUserClient unknown type %u", type);
        return kIOReturnBadArgument;
    }
    IOService *client = nullptr;
    kern_return_t ret = Create(this, "GPIBUserClientProperties", &client);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: Create user client failed 0x%x", ret);
        return ret;
    }
    *userClient = OSDynamicCast(IOUserClient, client);
    if (!*userClient) {
        os_log(OS_LOG_DEFAULT, "kusb_488b: created user client is not IOUserClient");
        client->release();
        return kIOReturnNoMemory;
    }
    return kIOReturnSuccess;
}
