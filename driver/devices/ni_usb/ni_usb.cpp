//
//  ni_usb.cpp
//  ni_usb
//
//  Created by Thiago Mattos on 14/07/25.
//

#include <os/log.h>

#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSString.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>

#include "DriverUtils.h"
#include "ni_usb.h"
#include "NIUSBTransport.h"
#include "GPIBBoard.h"

struct ni_usb_IVars {
    IOUSBHostInterface *interface;
    NIUSBTransport     *transport;
    GPIBBoard          *board;
};

bool ni_usb::init() {
    if (!super::init()) return false;
    ivars = IONewZero(ni_usb_IVars, 1);
    return ivars != nullptr;
}

void ni_usb::free() {
    if (ivars) {
        if (ivars->board) {
            ivars->board->free();
            IOSafeDeleteNULL(ivars->board, GPIBBoard, 1);
        }
        if (ivars->transport) {
            ivars->transport->free();
            IOSafeDeleteNULL(ivars->transport, NIUSBTransport, 1);
        }
        OSSafeReleaseNULL(ivars->interface);
        IOSafeDeleteNULL(ivars, ni_usb_IVars, 1);
    }
    super::free();
}

GPIBBoard *ni_usb::getBoard() {
    return ivars ? ivars->board : nullptr;
}

kern_return_t IMPL(ni_usb, Start) {
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "ni_usb: super::Start failed 0x%x", ret);
        return ret;
    }

    IOUSBHostInterface *usbInterface = OSDynamicCast(IOUSBHostInterface, provider);
    if (!usbInterface) {
        os_log(OS_LOG_DEFAULT, "ni_usb: provider is not IOUSBHostInterface");
        return kIOReturnUnsupported;
    }
    usbInterface->retain();
    ivars->interface = usbInterface;

    // Log basic descriptor info (preserves the existing diagnostic behaviour).
    const IOUSBConfigurationDescriptor *cfgDesc = usbInterface->CopyConfigurationDescriptor();
    const IOUSBInterfaceDescriptor *ifDesc =
        cfgDesc ? usbInterface->GetInterfaceDescriptor(cfgDesc) : nullptr;
    if (!cfgDesc || !ifDesc) {
        os_log(OS_LOG_DEFAULT, "ni_usb: failed to get USB descriptors");
        if (cfgDesc) IOFree((void *)cfgDesc, cfgDesc->bLength);
        return kIOReturnError;
    }

    IOUSBHostDevice *usbDevice = nullptr;
    if (usbInterface->CopyDevice(&usbDevice) == kIOReturnSuccess && usbDevice) {
        const IOUSBDeviceDescriptor *dev = usbDevice->CopyDeviceDescriptor();
        if (dev) {
            const IOUSBStringDescriptor *prodStr =
                usbDevice->CopyStringDescriptor(dev->iProduct);
            OSString *product = copyDeviceString(prodStr, "NI USB GPIB");
            os_log(OS_LOG_DEFAULT,
                   "ni_usb: USB device VID=0x%04x PID=0x%04x product=%{public}s",
                   dev->idVendor, dev->idProduct,
                   product ? product->getCStringNoCopy() : "?");
            if (prodStr) IOFree((void *)prodStr, prodStr->bLength);
            OSSafeReleaseNULL(product);
        }
        OSSafeReleaseNULL(usbDevice);
    }
    IOFree((void *)cfgDesc, cfgDesc->bLength);

    // Open the interface so we can use its pipes.
    ret = usbInterface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "ni_usb: IOUSBHostInterface::Open failed 0x%x", ret);
        return ret;
    }

    // Build the transport.
    // Constructed, not just allocated — see IONewZeroConstruct in DriverUtils.h.
    ivars->transport = IONewZeroConstruct<NIUSBTransport>();
    if (!ivars->transport || !ivars->transport->init(usbInterface)) {
        os_log(OS_LOG_DEFAULT, "ni_usb: transport init failed");
        return kIOReturnNoMemory;
    }
    uint32_t rc = ivars->transport->attach();
    if (rc != 0) {
        os_log(OS_LOG_DEFAULT, "ni_usb: transport attach failed rc=%u", rc);
        return kIOReturnError;
    }

    // Build the board state machine.
    ivars->board = IONewZero(GPIBBoard, 1);
    if (!ivars->board || !ivars->board->init(ivars->transport)) {
        os_log(OS_LOG_DEFAULT, "ni_usb: board init failed");
        return kIOReturnNoMemory;
    }

    // Bring the board online with M1 defaults (system controller, REN asserted).
    ivars->board->setOnline(true);

    // Publish to the IORegistry so the host app can open the user client.
    RegisterService();
    os_log(OS_LOG_DEFAULT, "ni_usb: started and registered");
    return kIOReturnSuccess;
}

kern_return_t IMPL(ni_usb, Stop) {
    os_log(OS_LOG_DEFAULT, "ni_usb: Stop");
    if (ivars && ivars->board) ivars->board->setOnline(false);
    if (ivars && ivars->interface) {
        ivars->interface->Close(this, 0);
    }
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t IMPL(ni_usb, NewUserClient) {
    if (type != 0) {
        os_log(OS_LOG_DEFAULT, "ni_usb: NewUserClient unknown type %u", type);
        return kIOReturnBadArgument;
    }
    IOService *client = nullptr;
    kern_return_t ret = Create(this, "GPIBUserClientProperties", &client);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "ni_usb: Create user client failed 0x%x", ret);
        return ret;
    }
    *userClient = OSDynamicCast(IOUserClient, client);
    if (!*userClient) {
        os_log(OS_LOG_DEFAULT, "ni_usb: created user client is not IOUserClient");
        client->release();
        return kIOReturnNoMemory;
    }
    return kIOReturnSuccess;
}
