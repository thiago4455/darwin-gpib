//
//  DriverUtils.h
//  darwin-gpib
//
//  Created by Thiago Mattos on 18/07/25.
//

#ifndef DriverUtils_h
#define DriverUtils_h

#include <DriverKit/DriverKit.h>
#include <USBDriverKit/USBDriverKit.h>

OSString* copyDeviceString(const IOUSBStringDescriptor *stringDescriptor, const char *fallback);

#endif /* DriverUtils_h */
