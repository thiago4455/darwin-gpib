//
//  ni_usb.cpp
//  ni_usb
//
//  Created by Thiago Mattos on 14/07/25.
//

#include <os/log.h>

#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>
#include <DriverKit/OSString.h>

#include "DriverUtils.h"
#include "ni_usb.h"


kern_return_t
IMPL(ni_usb, Start)
{
    kern_return_t ret;
    ret = Start(provider, SUPERDISPATCH);
    
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "Driver failed to start");
        return ret;
    }
    
    os_log(OS_LOG_DEFAULT, "Driver started successfully");
    
    
//    IOUSBHostDevice *usbDevice = OSDynamicCast(IOUSBHostDevice, provider);
//    if (usbDevice == nullptr) {
//        os_log(OS_LOG_DEFAULT, "Provider is not a USB device");
//        return kIOReturnUnsupported;
//    }
    
    
    IOUSBHostInterface *usbInterface = OSDynamicCast(IOUSBHostInterface, provider);
    if (usbInterface == nullptr) {
        os_log(OS_LOG_DEFAULT, "Provider is not a USB interface");
        return kIOReturnUnsupported;
    }

    
    // Retrieve basic device information
    uint16_t vendorID = 0;
    uint16_t productID = 0;
    
    
    const IOUSBConfigurationDescriptor *_configurationDescriptor;
    const IOUSBInterfaceDescriptor *_interfaceDescriptor;
    const IOUSBStringDescriptor *_stringDescriptor;
    
    _configurationDescriptor = usbInterface->CopyConfigurationDescriptor();
    _interfaceDescriptor = usbInterface->GetInterfaceDescriptor(_configurationDescriptor);
    
    if (!_configurationDescriptor || !_interfaceDescriptor) {
        os_log(OS_LOG_DEFAULT, "Failed to get USB descriptors");
        return kIOReturnError;
    }
    
    IOUSBHostDevice *usbDevice;
    const IOUSBDeviceDescriptor *descriptor;
    if(usbInterface->CopyDevice(&usbDevice) == kIOReturnSuccess){
        descriptor = usbDevice->CopyDeviceDescriptor();
        vendorID = descriptor->idVendor;
        productID = descriptor->idProduct;
        
        _stringDescriptor = usbDevice->CopyStringDescriptor(descriptor->iProduct);
        OSString *productString = copyDeviceString(_stringDescriptor, "Unknown");
        const char *productCString = productString ? productString->getCStringNoCopy() : "Unknown";

        os_log(OS_LOG_DEFAULT, "USB Device connected: Vendor ID = 0x%04x, Product ID = 0x%04x, Product = %{public}s",
                   vendorID, productID, productCString);
        /// Free string descriptor
        IOFree((void *)_stringDescriptor, _stringDescriptor->bLength);
        OSSafeReleaseNULL(productString);
    } else {
        os_log(OS_LOG_DEFAULT, "Failed to get USB device descriptor");
    }
    
    OSSafeReleaseNULL(usbDevice);
    IOFree((void *)_configurationDescriptor, _configurationDescriptor->bLength);
    
    
    
    
    return ret;
}
