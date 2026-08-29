//
//  GPIBUserClient.cpp
//  darwin-gpib driver
//

#include <os/log.h>
#include <string.h>

#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>
#include <DriverKit/IOMemoryDescriptor.h>

#include "GPIBUserClient.h"
#include "ni_usb.h"
#include "agilent_82357.h"
#include "kusb_488b.h"
#include "GPIBBoard.h"
#include "GPIBSelectors.h"
#include "gpib_user.h"

struct GPIBUserClient_IVars {
    IOService  *provider;
    GPIBBoard  *board;
};

// Forward declaration of the static dispatch entry-point.
static kern_return_t GPIBUserClient_Dispatch(OSObject *target,
                                              void *reference,
                                              IOUserClientMethodArguments *arguments);

// -----------------------------------------------------------------------------
// Per-selector dispatch table. Selector index = array index.
// checkStructureInputSize = kIOUserClientVariableStructureSize allows the
// client to pass variable-length payloads (used by Write).
// -----------------------------------------------------------------------------
// Sequential init — selector indices line up 1:1 with array positions
// (verified by static_asserts below). C++20 doesn't accept C99 [index]=
// designators in array initializers.
static const IOUserClientMethodDispatch sExternalMethodChecks[kGPIBSel_Count] = {
    /* kGPIBSel_BoardOnline     */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBOnlineIn),         0, sizeof(GPIBStatusOut) },
    /* kGPIBSel_OpenDescriptor  */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBOpenDescriptorIn), 0, sizeof(GPIBOpenDescriptorOut) },
    /* kGPIBSel_CloseDescriptor */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBHandleIn),         0, sizeof(GPIBStatusOut) },
    /* kGPIBSel_Configure       */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBConfigureIn),      0, sizeof(GPIBStatusOut) },
    /* kGPIBSel_Write           */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, kIOUserClientVariableStructureSize, 0, sizeof(GPIBWriteOut) },
    /* kGPIBSel_Read            */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBReadIn),           0, kIOUserClientVariableStructureSize },
    /* kGPIBSel_DeviceClear     */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBHandleIn),         0, sizeof(GPIBStatusOut) },
    /* kGPIBSel_InterfaceClear  */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBHandleIn),         0, sizeof(GPIBStatusOut) },
    /* kGPIBSel_RemoteEnable    */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBRemoteEnableIn),   0, sizeof(GPIBStatusOut) },
    /* kGPIBSel_Wait            */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBWaitIn),           0, sizeof(GPIBStatusOut) },
    /* kGPIBSel_Ask             */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBAskIn),            0, sizeof(GPIBAskOut) },
    /* kGPIBSel_SerialPoll      */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBHandleIn),         0, sizeof(GPIBSerialPollOut) },
    /* kGPIBSel_Trigger         */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBHandleIn),         0, sizeof(GPIBStatusOut) },
    /* kGPIBSel_GoToLocal       */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBHandleIn),         0, sizeof(GPIBStatusOut) },
    /* kGPIBSel_LocalLockout    */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBHandleIn),         0, sizeof(GPIBStatusOut) },
    /* kGPIBSel_LineStatus      */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBHandleIn),         0, sizeof(GPIBLineStatusOut) },
    /* kGPIBSel_ListenerPresent */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, sizeof(GPIBAddressIn),        0, sizeof(GPIBListenerOut) },
    /* kGPIBSel_SendCommand     */ { (IOUserClientMethodFunction)&GPIBUserClient_Dispatch, false, 0, kIOUserClientVariableStructureSize, 0, sizeof(GPIBWriteOut) },
};

static_assert(kGPIBSel_Count == 18, "Selector table needs an entry per selector");

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

bool GPIBUserClient::init() {
    if (!super::init()) return false;
    ivars = IONewZero(GPIBUserClient_IVars, 1);
    return ivars != nullptr;
}

void GPIBUserClient::free() {
    IOSafeDeleteNULL(ivars, GPIBUserClient_IVars, 1);
    super::free();
}

kern_return_t IMPL(GPIBUserClient, Start) {
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "GPIBUserClient::Start super failed 0x%x", ret);
        return ret;
    }

    // Accept any of our driver personalities as the provider. Each exposes
    // an in-process getBoard() accessor via LOCALONLY. Keep this in sync with
    // driver/Info.plist — a driver missing here fails Start with
    // kIOReturnBadArgument, which surfaces as "Create user client failed".
    ivars->provider = provider;
    if (ni_usb *ni = OSDynamicCast(ni_usb, provider)) {
        ivars->board = ni->getBoard();
    } else if (agilent_82357 *ag = OSDynamicCast(agilent_82357, provider)) {
        ivars->board = ag->getBoard();
    } else if (kusb_488b *ku = OSDynamicCast(kusb_488b, provider)) {
        ivars->board = ku->getBoard();
    } else {
        os_log(OS_LOG_DEFAULT, "GPIBUserClient: unrecognised provider class");
        return kIOReturnBadArgument;
    }
    if (!ivars->board) {
        os_log(OS_LOG_DEFAULT, "GPIBUserClient: provider has no board");
        return kIOReturnNotReady;
    }

    os_log(OS_LOG_DEFAULT, "GPIBUserClient: started");
    return kIOReturnSuccess;
}

kern_return_t IMPL(GPIBUserClient, Stop) {
    os_log(OS_LOG_DEFAULT, "GPIBUserClient: stop");
    // ivars->board is a raw pointer BORROWED from the provider's ivars, with
    // no retain -- and the provider's free() deletes it. Drop it here so a
    // dispatch that races termination fails cleanly (boardForDispatch()
    // returns null, which every handler already checks) instead of touching
    // freed memory. Termination ordering usually makes this unreachable; it
    // is not guaranteed to under sudden device removal.
    if (ivars) ivars->board = nullptr;
    return Stop(provider, SUPERDISPATCH);
}

// -----------------------------------------------------------------------------
// ExternalMethod entry point — validates selector and forwards to the
// per-selector dispatch handler. Declared LOCAL in the .iig, so we
// implement it as a plain C++ override (no IMPL macro).
// -----------------------------------------------------------------------------

kern_return_t GPIBUserClient::ExternalMethod(uint64_t selector,
                                              IOUserClientMethodArguments *arguments,
                                              const IOUserClientMethodDispatch *dispatch,
                                              OSObject *target,
                                              void *reference) {
    if (selector < (uint64_t)kGPIBSel_Count) {
        dispatch  = &sExternalMethodChecks[selector];
        if (!target) target = this;
        reference = (void *)(uintptr_t)selector;
    }
    return super::ExternalMethod(selector, arguments, dispatch, target, reference);
}

GPIBBoard *GPIBUserClient::boardForDispatch() {
    return ivars ? ivars->board : nullptr;
}

// -----------------------------------------------------------------------------
// Selector handlers. Each reads inputs from `arguments->structureInput`,
// writes outputs via `arguments->structureOutput = OSData::withBytes(...)`.
// -----------------------------------------------------------------------------

static const void *structInBytes(IOUserClientMethodArguments *args, size_t *outLen) {
    if (args->structureInput) {
        if (outLen) *outLen = args->structureInput->getLength();
        return args->structureInput->getBytesNoCopy();
    }
    if (outLen) *outLen = 0;
    return nullptr;
}

static void setStatusOut(IOUserClientMethodArguments *args,
                          uint32_t ibsta, uint32_t iberr, uint32_t ibcnt) {
    GPIBStatusOut out = { ibsta, iberr, ibcnt, 0 };
    args->structureOutput = OSData::withBytes(&out, sizeof(out));
}

static kern_return_t handleBoardOnline(GPIBUserClient *uc,
                                        GPIBBoard *board,
                                        IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBOnlineIn *in = (const GPIBOnlineIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    uint32_t ibsta = board->setOnline(in->online != 0);
    setStatusOut(args, ibsta, 0, 0);
    return kIOReturnSuccess;
}

static kern_return_t handleOpenDescriptor(GPIBUserClient *uc,
                                           GPIBBoard *board,
                                           IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBOpenDescriptorIn *in = (const GPIBOpenDescriptorIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;

    uint32_t timeout_us = 3000000; // T3s default
    // The client encodes either a timeout code (gpib_timeout enum) or raw
    // microseconds in this field. For M1 we always treat it as a timeout code.
    timeout_us = GPIBBoard::timeoutCodeToMicros(in->timeout_code);

    uint32_t ibsta = 0, iberr = 0;
    int32_t handle = board->openDescriptor(
        (uint8_t)in->pad, (int8_t)in->sad, in->is_board != 0,
        timeout_us, (uint8_t)in->eos_char, (uint8_t)in->eos_flags,
        in->eot != 0, &ibsta, &iberr);

    GPIBOpenDescriptorOut out = { handle, ibsta, iberr, 0 };
    args->structureOutput = OSData::withBytes(&out, sizeof(out));
    return kIOReturnSuccess;
}

static kern_return_t handleCloseDescriptor(GPIBUserClient *uc,
                                            GPIBBoard *board,
                                            IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBHandleIn *in = (const GPIBHandleIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    uint32_t iberr = 0;
    uint32_t ibsta = board->closeDescriptor(in->handle, &iberr);
    setStatusOut(args, ibsta, iberr, 0);
    return kIOReturnSuccess;
}

static kern_return_t handleConfigure(GPIBUserClient *uc,
                                      GPIBBoard *board,
                                      IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBConfigureIn *in = (const GPIBConfigureIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    uint32_t iberr = 0;
    uint32_t ibsta = board->configure(in->handle, in->key, in->value, &iberr);
    setStatusOut(args, ibsta, iberr, 0);
    return kIOReturnSuccess;
}

static kern_return_t handleWrite(GPIBUserClient *uc,
                                  GPIBBoard *board,
                                  IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBWriteIn *in = (const GPIBWriteIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    if (inLen < sizeof(*in) + in->length) return kIOReturnBadArgument;

    uint32_t ibcnt = 0, iberr = 0;
    uint32_t ibsta = board->write(in->handle, in->data, in->length,
                                   in->send_eoi != 0, &ibcnt, &iberr);

    GPIBWriteOut out = { ibsta, iberr, ibcnt, 0 };
    args->structureOutput = OSData::withBytes(&out, sizeof(out));
    return kIOReturnSuccess;
}

static kern_return_t handleRead(GPIBUserClient *uc,
                                 GPIBBoard *board,
                                 IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBReadIn *in = (const GPIBReadIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;

    uint32_t request = in->request_count;
    if (request > kGPIBMaxInlineRead) request = (uint32_t)kGPIBMaxInlineRead;

    // Allocate a temporary buffer the size of the response header plus the
    // requested payload. The data follows immediately after the GPIBReadOut.
    size_t outBytes = sizeof(GPIBReadOut) + request;
    OSData *outData = OSData::withCapacity((uint32_t)outBytes);
    if (!outData) return kIOReturnNoMemory;

    // We need a writable backing — OSData has a private append-only API in
    // DriverKit. Build the response in a stack-or-IOMalloc'd buffer, then
    // copy into OSData via appendBytes.
    uint8_t *scratch = (uint8_t *)IOMallocZero(outBytes);
    if (!scratch) { OSSafeReleaseNULL(outData); return kIOReturnNoMemory; }

    GPIBReadOut *header = (GPIBReadOut *)scratch;
    uint32_t ibcnt = 0, iberr = 0;
    uint8_t  endFlag = 0;
    uint32_t ibsta = board->read(in->handle, scratch + sizeof(GPIBReadOut),
                                  request, &ibcnt, &endFlag, &iberr);

    header->ibsta    = ibsta;
    header->iberr    = iberr;
    header->ibcnt    = ibcnt;
    header->end_flag = endFlag;

    size_t finalLen = sizeof(GPIBReadOut) + ibcnt;
    outData->appendBytes(scratch, (uint32_t)finalLen);
    IOFree(scratch, outBytes);

    args->structureOutput = outData;
    return kIOReturnSuccess;
}

static kern_return_t handleDeviceClear(GPIBUserClient *uc,
                                        GPIBBoard *board,
                                        IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBHandleIn *in = (const GPIBHandleIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    uint32_t iberr = 0;
    uint32_t ibsta = board->deviceClear(in->handle, &iberr);
    setStatusOut(args, ibsta, iberr, 0);
    return kIOReturnSuccess;
}

static kern_return_t handleInterfaceClear(GPIBUserClient *uc,
                                           GPIBBoard *board,
                                           IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBHandleIn *in = (const GPIBHandleIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    uint32_t iberr = 0;
    uint32_t ibsta = board->interfaceClear(in->handle, &iberr);
    setStatusOut(args, ibsta, iberr, 0);
    return kIOReturnSuccess;
}

static kern_return_t handleRemoteEnable(GPIBUserClient *uc,
                                         GPIBBoard *board,
                                         IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBRemoteEnableIn *in = (const GPIBRemoteEnableIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    uint32_t iberr = 0;
    uint32_t ibsta = board->remoteEnable(in->handle, in->enable != 0, &iberr);
    setStatusOut(args, ibsta, iberr, 0);
    return kIOReturnSuccess;
}

static kern_return_t handleWait(GPIBUserClient *uc,
                                 GPIBBoard *board,
                                 IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBWaitIn *in = (const GPIBWaitIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    uint32_t iberr = 0;
    uint32_t ibsta = board->waitForStatus(in->handle, in->mask, in->timeout_us, &iberr);
    setStatusOut(args, ibsta, iberr, 0);
    return kIOReturnSuccess;
}

static kern_return_t handleAsk(GPIBUserClient *uc,
                                GPIBBoard *board,
                                IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBAskIn *in = (const GPIBAskIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    int32_t value = 0;
    uint32_t iberr = 0;
    uint32_t ibsta = board->ask(in->handle, in->option, &value, &iberr);
    GPIBAskOut out = { value, ibsta, iberr, 0 };
    args->structureOutput = OSData::withBytes(&out, sizeof(out));
    return kIOReturnSuccess;
}

static kern_return_t handleSerialPoll(GPIBUserClient *uc,
                                       GPIBBoard *board,
                                       IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBHandleIn *in = (const GPIBHandleIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    uint8_t stb = 0;
    uint32_t iberr = 0;
    uint32_t ibsta = board->serialPoll(in->handle, &stb, &iberr);
    GPIBSerialPollOut out = { stb, 0, 0, ibsta, iberr, 0 };
    args->structureOutput = OSData::withBytes(&out, sizeof(out));
    return kIOReturnSuccess;
}

static kern_return_t handleTrigger(GPIBUserClient *uc,
                                    GPIBBoard *board,
                                    IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBHandleIn *in = (const GPIBHandleIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    uint32_t iberr = 0;
    uint32_t ibsta = board->trigger(in->handle, &iberr);
    setStatusOut(args, ibsta, iberr, 0);
    return kIOReturnSuccess;
}

static kern_return_t handleGoToLocal(GPIBUserClient *uc,
                                      GPIBBoard *board,
                                      IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBHandleIn *in = (const GPIBHandleIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    uint32_t iberr = 0;
    uint32_t ibsta = board->goToLocal(in->handle, &iberr);
    setStatusOut(args, ibsta, iberr, 0);
    return kIOReturnSuccess;
}

static kern_return_t handleLocalLockout(GPIBUserClient *uc,
                                         GPIBBoard *board,
                                         IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBHandleIn *in = (const GPIBHandleIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    uint32_t iberr = 0;
    uint32_t ibsta = board->localLockout(in->handle, &iberr);
    setStatusOut(args, ibsta, iberr, 0);
    return kIOReturnSuccess;
}

static kern_return_t handleLineStatus(GPIBUserClient *uc,
                                       GPIBBoard *board,
                                       IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBHandleIn *in = (const GPIBHandleIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    uint16_t lines = 0;
    uint32_t iberr = 0;
    // TEMPORARY: handle values tagged 0x40000000 mean "read raw register
    // (handle & 0xFFFF)" instead of decoding bus lines, so a caller can sweep
    // the register map and distinguish an inert core from a wrong selector.
    uint32_t ibsta;
    // Match on the whole tag nibble, never on a single bit. The board's own
    // handle is -1 (0xFFFFFFFF), which sets *every* bit — so a bare
    // `handle & 0x20000000` test matched it and quietly turned the ordinary
    // `gpibctl lines` bus-line read into a register write of 0xFF into
    // register 0xFFF. A read-only diagnostic was corrupting chip state on
    // every call, which is exactly the kind of thing that makes hardware
    // behaviour look irreproducible.
    const uint32_t h   = (uint32_t)in->handle;
    const uint32_t tag = h & 0xF0000000u;
    if (h == 0x10000000u) {
        // Recovery: re-run the chip bring-up (see GPIBBoard::recoverBus).
        uint32_t rc = board->softResetDiag();
        lines = (uint16_t)(rc == 0 ? 0x8000 : 0);
        iberr = rc;
        ibsta = 0x0100;
    } else if (h == 0x18000000u) {
        // Recovery, last resort: force the USB device to re-enumerate. This
        // tears down the dext instance serving this very call, so the caller
        // should expect the connection to drop rather than a tidy reply.
        uint32_t rc = board->resetDeviceDiag();
        lines = (uint16_t)(rc == 0 ? 0x8000 : 0);
        iberr = rc;
        ibsta = 0x0100;
    } else if (tag == 0x20000000u) {
        // TEMPORARY: write register (handle >> 8) & 0xFFF = handle & 0xFF
        uint32_t rc = board->writeRawRegisterDiag(
            (uint16_t)(((uint32_t)in->handle >> 8) & 0xFFF),
            (uint8_t)((uint32_t)in->handle & 0xFF));
        lines = (uint16_t)(rc == 0 ? 0x8000 : 0);
        iberr = rc;
        ibsta = 0x0100;
    } else if (tag == 0x40000000u) {
        uint8_t raw = 0;
        uint32_t rc = board->readRawRegisterDiag((uint16_t)(in->handle & 0xFFFF), &raw);
        lines = (uint16_t)((rc == 0) ? (0x8000 | raw) : 0);
        iberr = rc;
        ibsta = 0x0100;
    } else {
        ibsta = board->busLineStatus(&lines, &iberr);
    }
    GPIBLineStatusOut out = { lines, ibsta, iberr, 0 };
    args->structureOutput = OSData::withBytes(&out, sizeof(out));
    return kIOReturnSuccess;
}

static kern_return_t handleListenerPresent(GPIBUserClient *uc,
                                            GPIBBoard *board,
                                            IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBAddressIn *in = (const GPIBAddressIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    bool present = false;
    uint32_t iberr = 0;
    uint32_t ibsta = board->listenerPresent((uint8_t)in->pad, (int8_t)in->sad,
                                             &present, &iberr);
    GPIBListenerOut out = { present ? 1u : 0u, ibsta, iberr, 0 };
    args->structureOutput = OSData::withBytes(&out, sizeof(out));
    return kIOReturnSuccess;
}

static kern_return_t handleSendCommand(GPIBUserClient *uc,
                                        GPIBBoard *board,
                                        IOUserClientMethodArguments *args) {
    (void)uc;
    size_t inLen = 0;
    const GPIBSendCommandIn *in = (const GPIBSendCommandIn *)structInBytes(args, &inLen);
    if (!in || inLen < sizeof(*in)) return kIOReturnBadArgument;
    if (inLen < sizeof(*in) + in->length) return kIOReturnBadArgument;

    uint32_t ibcnt = 0, iberr = 0;
    uint32_t ibsta = board->sendCommands(in->handle, in->data, in->length,
                                          &ibcnt, &iberr);
    GPIBWriteOut out = { ibsta, iberr, ibcnt, 0 };
    args->structureOutput = OSData::withBytes(&out, sizeof(out));
    return kIOReturnSuccess;
}

// -----------------------------------------------------------------------------
// Static dispatch trampoline — the entry stored in IOUserClientMethodDispatch.
// Uses the *reference* parameter (set by the IIG runtime to the selector index
// after our ExternalMethod override) to demultiplex.
// Actually IOKit passes the dispatch entry's index through `reference`, but
// to keep things simple we re-derive the selector from the table pointer.
// -----------------------------------------------------------------------------

static kern_return_t GPIBUserClient_Dispatch(OSObject *target,
                                              void *reference,
                                              IOUserClientMethodArguments *arguments) {
    GPIBUserClient *uc = OSDynamicCast(GPIBUserClient, target);
    if (!uc) return kIOReturnBadArgument;

    GPIBBoard *board = uc->boardForDispatch();
    if (!board) return kIOReturnNotReady;

    // `reference` carries the selector index we encoded when calling super.
    // (See GPIBUserClient::ExternalMethod.)
    uint64_t selector = (uint64_t)(uintptr_t)reference;

    switch (selector) {
        case kGPIBSel_BoardOnline:      return handleBoardOnline(uc, board, arguments);
        case kGPIBSel_OpenDescriptor:   return handleOpenDescriptor(uc, board, arguments);
        case kGPIBSel_CloseDescriptor:  return handleCloseDescriptor(uc, board, arguments);
        case kGPIBSel_Configure:        return handleConfigure(uc, board, arguments);
        case kGPIBSel_Write:            return handleWrite(uc, board, arguments);
        case kGPIBSel_Read:             return handleRead(uc, board, arguments);
        case kGPIBSel_DeviceClear:      return handleDeviceClear(uc, board, arguments);
        case kGPIBSel_InterfaceClear:   return handleInterfaceClear(uc, board, arguments);
        case kGPIBSel_RemoteEnable:     return handleRemoteEnable(uc, board, arguments);
        case kGPIBSel_Wait:             return handleWait(uc, board, arguments);
        case kGPIBSel_Ask:              return handleAsk(uc, board, arguments);
        case kGPIBSel_SerialPoll:       return handleSerialPoll(uc, board, arguments);
        case kGPIBSel_Trigger:          return handleTrigger(uc, board, arguments);
        case kGPIBSel_GoToLocal:        return handleGoToLocal(uc, board, arguments);
        case kGPIBSel_LocalLockout:     return handleLocalLockout(uc, board, arguments);
        case kGPIBSel_LineStatus:       return handleLineStatus(uc, board, arguments);
        case kGPIBSel_ListenerPresent:  return handleListenerPresent(uc, board, arguments);
        case kGPIBSel_SendCommand:      return handleSendCommand(uc, board, arguments);
    }
    return kIOReturnUnsupported;
}
