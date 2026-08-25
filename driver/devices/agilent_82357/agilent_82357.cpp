//
//  agilent_82357.cpp
//  darwin-gpib driver
//

#include <os/log.h>

#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSString.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostDevice.h>

#include "DriverUtils.h"
#include "agilent_82357.h"
#include "agilent_82357_protocol.h"
#include "Agilent82357Transport.h"
#include "GPIBBoard.h"

struct agilent_82357_IVars {
    IOUSBHostInterface     *interface;
    Agilent82357Transport  *transport;
    GPIBBoard              *board;
};

bool agilent_82357::init() {
    if (!super::init()) return false;
    ivars = IONewZero(agilent_82357_IVars, 1);
    return ivars != nullptr;
}

void agilent_82357::free() {
    if (ivars) {
        if (ivars->board) {
            ivars->board->free();
            IOSafeDeleteNULL(ivars->board, GPIBBoard, 1);
        }
        if (ivars->transport) {
            ivars->transport->free();
            IOSafeDeleteNULL(ivars->transport, Agilent82357Transport, 1);
        }
        OSSafeReleaseNULL(ivars->interface);
        IOSafeDeleteNULL(ivars, agilent_82357_IVars, 1);
    }
    super::free();
}

GPIBBoard *agilent_82357::getBoard() {
    return ivars ? ivars->board : nullptr;
}

kern_return_t IMPL(agilent_82357, Start) {
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "agilent_82357: super::Start failed 0x%x", ret);
        return ret;
    }

    IOUSBHostInterface *usbInterface = OSDynamicCast(IOUSBHostInterface, provider);
    if (!usbInterface) {
        os_log(OS_LOG_DEFAULT, "agilent_82357: provider is not IOUSBHostInterface");
        return kIOReturnUnsupported;
    }
    usbInterface->retain();
    ivars->interface = usbInterface;

    // Grab the PID from the device descriptor so we can select 82357A vs
    // 82357B endpoints in the transport.
    uint16_t productId = 0;
    IOUSBHostDevice *usbDevice = nullptr;
    if (usbInterface->CopyDevice(&usbDevice) == kIOReturnSuccess && usbDevice) {
        const IOUSBDeviceDescriptor *dev = usbDevice->CopyDeviceDescriptor();
        if (dev) {
            productId = dev->idProduct;
            const IOUSBStringDescriptor *prodStr =
                usbDevice->CopyStringDescriptor(dev->iProduct);
            OSString *product = copyDeviceString(prodStr, "Agilent 82357");
            os_log(OS_LOG_DEFAULT,
                   "agilent_82357: USB device VID=0x%04x PID=0x%04x product=%{public}s",
                   dev->idVendor, dev->idProduct,
                   product ? product->getCStringNoCopy() : "?");
            if (prodStr) IOFree((void *)prodStr, prodStr->bLength);
            OSSafeReleaseNULL(product);
        }
        OSSafeReleaseNULL(usbDevice);
    }

    // Reject pre-firmware IDs — those need firmware upload first. We
    // don't yet ship that.
    if (productId == AGILENT_PID_82357A_PRE || productId == AGILENT_PID_82357B_PRE) {
        os_log(OS_LOG_DEFAULT, "agilent_82357: pre-firmware device (PID=0x%04x); "
               "firmware upload not yet supported", productId);
        return kIOReturnUnsupported;
    }

    // Open the interface so we can walk pipes.
    ret = usbInterface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "agilent_82357: IOUSBHostInterface::Open failed 0x%x", ret);
        return ret;
    }

    // Build and attach the transport.
    // Constructed, not just allocated — see IONewZeroConstruct in DriverUtils.h.
    ivars->transport = IONewZeroConstruct<Agilent82357Transport>();
    if (!ivars->transport || !ivars->transport->init(usbInterface, this, productId)) {
        os_log(OS_LOG_DEFAULT, "agilent_82357: transport init failed");
        return kIOReturnNoMemory;
    }
    uint32_t rc = ivars->transport->attach();
    if (rc != 0) {
        os_log(OS_LOG_DEFAULT, "agilent_82357: transport attach failed rc=%u", rc);
        return kIOReturnError;
    }

    // Build the board state machine.
    ivars->board = IONewZero(GPIBBoard, 1);
    if (!ivars->board || !ivars->board->init(ivars->transport)) {
        os_log(OS_LOG_DEFAULT, "agilent_82357: board init failed");
        return kIOReturnNoMemory;
    }

    // Bring the board online (system controller, REN asserted).
    ivars->board->setOnline(true);

    RegisterService();
    os_log(OS_LOG_DEFAULT, "agilent_82357: started and registered");
    return kIOReturnSuccess;
}

kern_return_t IMPL(agilent_82357, Stop) {
    os_log(OS_LOG_DEFAULT, "agilent_82357: Stop");
    if (ivars && ivars->board) ivars->board->setOnline(false);
    if (ivars && ivars->interface) {
        ivars->interface->Close(this, 0);
    }
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t IMPL(agilent_82357, NewUserClient) {
    if (type != 0) {
        os_log(OS_LOG_DEFAULT, "agilent_82357: NewUserClient unknown type %u", type);
        return kIOReturnBadArgument;
    }
    IOService *client = nullptr;
    kern_return_t ret = Create(this, "GPIBUserClientProperties", &client);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "agilent_82357: Create user client failed 0x%x", ret);
        return ret;
    }
    *userClient = OSDynamicCast(IOUserClient, client);
    if (!*userClient) {
        os_log(OS_LOG_DEFAULT, "agilent_82357: created user client is not IOUserClient");
        client->release();
        return kIOReturnNoMemory;
    }
    return kIOReturnSuccess;
}
