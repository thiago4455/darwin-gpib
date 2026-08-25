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

// IONew/IONewZero hand back raw storage without running a C++ constructor.
// That is fine for a plain struct, but a class with virtual methods gets its
// vtable pointer set *by construction* — so an IONewZero'd instance has a null
// vtable and faults on its first virtual call (EXC_BAD_ACCESS reading the
// vtable slot). GPIBBoard sidesteps this by having no virtuals; the transports
// cannot, since they implement IGPIBTransport.
//
// Allocate zeroed, then construct in place. Frees are unchanged: these classes
// use explicit free() plus IOSafeDeleteNULL on the raw storage, and their
// destructors are trivial.
template <typename T>
static inline T *IONewZeroConstruct() {
    T *object = IONewZero(T, 1);
    if (object) new (object) T;
    return object;
}

#endif /* DriverUtils_h */
