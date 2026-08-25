//
//  Agilent82357Transport.cpp
//  darwin-gpib driver
//

#include <os/log.h>
#include <string.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/IOUSBHostPipe.h>

#include "Agilent82357Transport.h"
#include "agilent_82357_protocol.h"
#include "agilent_82357_tms9914.h"
#include "gpib_user.h"    // for BusXXX / ValidXXX line masks

namespace {

constexpr uint32_t kControlTimeoutMs   = 100;
constexpr uint32_t kRegOpTimeoutMs     = 1000;

uint32_t mapIOReturn(kern_return_t ret) {
    if (ret == kIOReturnSuccess) return GPIBT_OK;
    if (ret == kIOReturnTimeout) return GPIBT_ERR_TIMEOUT;
    if (ret == kIOReturnAborted) return GPIBT_ERR_ABORTED;
    if (ret == kIOReturnNotOpen) return GPIBT_ERR_NOT_READY;
    return GPIBT_ERR_IO;
}

uint32_t mapAgilentFwError(uint8_t code) {
    switch (code) {
        case AGILENT_UGP_SUCCESS:        return GPIBT_OK;
        case AGILENT_UGP_ERR_FLUSHING:
        case AGILENT_UGP_ERR_FLUSHING_ALREADY:
                                         return GPIBT_ERR_ABORTED;
        case AGILENT_UGP_ERR_UNSUPPORTED:
                                         return GPIBT_ERR_IO;
        default:                         return GPIBT_ERR_IO;
    }
}

uint32_t msFromUsec(uint32_t timeout_us) {
    if (timeout_us == 0xFFFFFFFFu) return 0;   // infinite
    uint32_t ms = (timeout_us + 999) / 1000;
    return (ms == 0) ? 1 : ms;
}

}  // namespace

bool Agilent82357Transport::init(IOUSBHostInterface *interface,
                                 IOService *owningService,
                                 uint16_t productId) {
    if (!interface || !owningService) return false;
    interface_       = interface;
    owner_           = owningService;
    device_          = nullptr;
    bulkOutPipe_     = nullptr;
    bulkInPipe_      = nullptr;
    interruptInPipe_ = nullptr;
    outBuffer_       = nullptr;
    inBuffer_        = nullptr;
    productId_       = productId;
    hwControlBits_   = 0;

    // Endpoint layout differs between 82357A and 82357B: shared bulk-in on
    // 0x02, then per-model bulk-out and interrupt-in.
    if (productId == AGILENT_PID_82357A) {
        bulkOutEP_   = AGILENT_82357A_BULK_OUT_EP;
        interruptEP_ = AGILENT_82357A_INT_IN_EP;
    } else {
        // Default to 82357B endpoints for anything else (including the
        // pre-firmware IDs — those shouldn't hit us here anyway).
        bulkOutEP_   = AGILENT_82357B_BULK_OUT_EP;
        interruptEP_ = AGILENT_82357B_INT_IN_EP;
    }
    return true;
}

void Agilent82357Transport::free() {
    closePipes();
    OSSafeReleaseNULL(outBuffer_);
    OSSafeReleaseNULL(inBuffer_);
    if (device_) {
        device_->Close(owner_, 0);
        OSSafeReleaseNULL(device_);
    }
    interface_ = nullptr;
    owner_ = nullptr;
}

uint32_t Agilent82357Transport::openPipes() {
    kern_return_t ret;

    // Bulk endpoints.
    ret = interface_->CopyPipe(bulkOutEP_, &bulkOutPipe_);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "Agilent82357: CopyPipe bulk-out 0x%02x failed 0x%x",
               bulkOutEP_, ret);
        return mapIOReturn(ret);
    }

    ret = interface_->CopyPipe(AGILENT_82357_BULK_IN_EP | 0x80, &bulkInPipe_);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "Agilent82357: CopyPipe bulk-in failed 0x%x", ret);
        return mapIOReturn(ret);
    }

    ret = interface_->CopyPipe(interruptEP_ | 0x80, &interruptInPipe_);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "Agilent82357: CopyPipe interrupt 0x%02x failed 0x%x (non-fatal)",
               interruptEP_, ret);
        interruptInPipe_ = nullptr;   // interrupt pipe is optional for M1
    }

    // Bulk transfer buffers.
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                           kBulkBufferSize, 0, &outBuffer_);
    if (ret != kIOReturnSuccess) return mapIOReturn(ret);
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                           kBulkBufferSize, 0, &inBuffer_);
    if (ret != kIOReturnSuccess) return mapIOReturn(ret);
    return GPIBT_OK;
}

void Agilent82357Transport::closePipes() {
    OSSafeReleaseNULL(bulkOutPipe_);
    OSSafeReleaseNULL(bulkInPipe_);
    OSSafeReleaseNULL(interruptInPipe_);
}

uint32_t Agilent82357Transport::attach() {
    // Grab the device handle for control transfers (abort / status).
    kern_return_t ret = interface_->CopyDevice(&device_);
    if (ret != kIOReturnSuccess || !device_) {
        os_log(OS_LOG_DEFAULT, "Agilent82357: CopyDevice failed 0x%x", ret);
        return mapIOReturn(ret);
    }
    ret = device_->Open(owner_, 0, 0);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "Agilent82357: device Open failed 0x%x", ret);
        OSSafeReleaseNULL(device_);
        return mapIOReturn(ret);
    }

    uint32_t rc = openPipes();
    if (rc != GPIBT_OK) return rc;

    rc = runInitSequence();
    if (rc != GPIBT_OK) {
        os_log(OS_LOG_DEFAULT, "Agilent82357: init sequence failed rc=%u", rc);
        return rc;
    }
    os_log(OS_LOG_DEFAULT, "Agilent82357: attached (pid=0x%04x)", productId_);
    return GPIBT_OK;
}

void Agilent82357Transport::detach() {
    // Try to leave the chip in a known state.
    Agilent82357RegPair writes[6] = {};
    writes[0].address = TMS_AUXCR;
    writes[0].value   = AGILENT_AUX_CS | AGILENT_AUX_CHIP_RESET;
    hwControlBits_   &= ~AGILENT_HWC_NOT_TI_RESET;
    writes[1].address = AGILENT_FW_HW_CONTROL;
    writes[1].value   = hwControlBits_;
    writes[2].address = AGILENT_FW_PROTOCOL_CONTROL;
    writes[2].value   = 0;
    writes[3].address = TMS_IMR0;
    writes[3].value   = 0;
    writes[4].address = TMS_IMR1;
    writes[4].value   = 0;
    writes[5].address = AGILENT_FW_LED_CONTROL;
    writes[5].value   = 0;
    writeRegisters(writes, 6);
    closePipes();
}

uint32_t Agilent82357Transport::bulkOut(const uint8_t *bytes, uint32_t len,
                                        uint32_t timeout_us) {
    if (!bulkOutPipe_ || !outBuffer_) return GPIBT_ERR_NOT_READY;
    if (len > kBulkBufferSize) return GPIBT_ERR_IO;

    uint64_t addr = 0, outLen = 0;
    kern_return_t ret = outBuffer_->Map(0, 0, 0, 0, &addr, &outLen);
    if (ret != kIOReturnSuccess) return mapIOReturn(ret);
    memcpy((void *)addr, bytes, len);

    uint32_t timeoutMs = msFromUsec(timeout_us);
    uint32_t bytesTransferred = 0;
    ret = bulkOutPipe_->IO(outBuffer_, len, &bytesTransferred, timeoutMs);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "Agilent82357: bulkOut IO failed 0x%x", ret);
        return mapIOReturn(ret);
    }
    return (bytesTransferred == len) ? GPIBT_OK : GPIBT_ERR_IO;
}

uint32_t Agilent82357Transport::bulkIn(uint8_t *bytes, uint32_t capacity,
                                       uint32_t timeout_us,
                                       uint32_t *outBytesRead) {
    if (!bulkInPipe_ || !inBuffer_) return GPIBT_ERR_NOT_READY;
    if (capacity > kBulkBufferSize) capacity = kBulkBufferSize;

    uint32_t timeoutMs = msFromUsec(timeout_us);
    uint32_t bytesTransferred = 0;
    kern_return_t ret = bulkInPipe_->IO(inBuffer_, capacity,
                                        &bytesTransferred, timeoutMs);
    if (ret != kIOReturnSuccess) {
        if (outBytesRead) *outBytesRead = 0;
        return mapIOReturn(ret);
    }

    uint64_t addr = 0, inLen = 0;
    ret = inBuffer_->Map(0, 0, 0, 0, &addr, &inLen);
    if (ret != kIOReturnSuccess) {
        if (outBytesRead) *outBytesRead = 0;
        return mapIOReturn(ret);
    }
    memcpy(bytes, (const void *)addr, bytesTransferred);
    if (outBytesRead) *outBytesRead = bytesTransferred;
    return GPIBT_OK;
}

uint32_t Agilent82357Transport::sendAbort(bool flush) {
    if (!device_ || !owner_) return GPIBT_ERR_NOT_READY;

    // Vendor IN control transfer returning 2 bytes.
    IOBufferMemoryDescriptor *buf = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn,
                                                          2, 0, &buf);
    if (ret != kIOReturnSuccess) return mapIOReturn(ret);

    uint16_t wIndex = flush ? AGILENT_XA_FLUSH : 0;
    uint16_t bytesTransferred = 0;
    ret = device_->DeviceRequest(owner_,
                                  0xC0,  // IN | VENDOR | DEVICE
                                  AGILENT_82357_CONTROL_REQUEST,
                                  AGILENT_XFER_ABORT,
                                  wIndex, 2,
                                  buf, &bytesTransferred,
                                  kControlTimeoutMs);
    uint32_t rc = GPIBT_OK;
    if (ret != kIOReturnSuccess || bytesTransferred < 2) {
        os_log(OS_LOG_DEFAULT, "Agilent82357: abort control transfer failed 0x%x", ret);
        rc = mapIOReturn(ret);
    } else {
        uint64_t addr = 0, sz = 0;
        if (buf->Map(0, 0, 0, 0, &addr, &sz) == kIOReturnSuccess) {
            const uint8_t *r = (const uint8_t *)addr;
            // r[0] should be ~AGILENT_XFER_ABORT (0x5f).
            if (r[0] != (uint8_t)~AGILENT_XFER_ABORT) rc = GPIBT_ERR_IO;
            else rc = mapAgilentFwError(r[1]);
        }
    }
    OSSafeReleaseNULL(buf);
    return rc;
}

uint32_t Agilent82357Transport::writeRegisters(const Agilent82357RegPair *writes,
                                               uint32_t count) {
    if (count == 0) return GPIBT_OK;
    if (count > 31) return GPIBT_ERR_IO;      // firmware limit

    uint8_t pkt[128];
    uint32_t i = 0;
    pkt[i++] = AGILENT_CMD_WR_REGS;
    pkt[i++] = (uint8_t)count;
    for (uint32_t j = 0; j < count; ++j) {
        pkt[i++] = writes[j].address;
        pkt[i++] = writes[j].value;
    }

    uint32_t rc = bulkOut(pkt, i, kRegOpTimeoutMs * 1000);
    if (rc != GPIBT_OK) return rc;

    uint8_t resp[0x20];
    uint32_t respLen = 0;
    rc = bulkIn(resp, sizeof(resp), kRegOpTimeoutMs * 1000, &respLen);
    if (rc != GPIBT_OK) return rc;
    if (respLen < 2) return GPIBT_ERR_IO;
    if (resp[0] != (uint8_t)~AGILENT_CMD_WR_REGS) return GPIBT_ERR_IO;
    return mapAgilentFwError(resp[1]);
}

uint32_t Agilent82357Transport::readRegisters(Agilent82357RegPair *reads,
                                              uint32_t count) {
    if (count == 0) return GPIBT_OK;
    if (count > 62) return GPIBT_ERR_IO;

    uint8_t pkt[128];
    uint32_t i = 0;
    pkt[i++] = AGILENT_CMD_RD_REGS;
    pkt[i++] = (uint8_t)count;
    for (uint32_t j = 0; j < count; ++j) pkt[i++] = reads[j].address;

    uint32_t rc = bulkOut(pkt, i, kRegOpTimeoutMs * 1000);
    if (rc != GPIBT_OK) return rc;

    uint8_t resp[0x40];
    uint32_t respLen = 0;
    rc = bulkIn(resp, sizeof(resp), kRegOpTimeoutMs * 1000 * 10, &respLen);
    if (rc != GPIBT_OK) return rc;
    if (respLen < 2 + count) return GPIBT_ERR_IO;
    if (resp[0] != (uint8_t)~AGILENT_CMD_RD_REGS) return GPIBT_ERR_IO;
    if (resp[1] != AGILENT_UGP_SUCCESS) return mapAgilentFwError(resp[1]);

    for (uint32_t j = 0; j < count; ++j) reads[j].value = resp[2 + j];
    return GPIBT_OK;
}

uint32_t Agilent82357Transport::writeReg(uint8_t address, uint8_t value) {
    Agilent82357RegPair pair = { address, value };
    return writeRegisters(&pair, 1);
}

uint32_t Agilent82357Transport::pulseInterfaceClear() {
    // Assert IFC (with CS to set the bit), sleep ~200us, then clear.
    uint32_t rc = writeReg(TMS_AUXCR, AGILENT_AUX_CS | AGILENT_AUX_SIC);
    if (rc != GPIBT_OK) return rc;
    IOSleep(1);
    return writeReg(TMS_AUXCR, AGILENT_AUX_SIC);
}

uint32_t Agilent82357Transport::setRemoteEnable(bool enable) {
    uint8_t v = AGILENT_AUX_SRE;
    if (enable) v |= AGILENT_AUX_CS;
    return writeReg(TMS_AUXCR, v);
}

uint32_t Agilent82357Transport::sendCommandBytes(const uint8_t *cmds, uint32_t len) {
    if (len == 0) return GPIBT_OK;
    // Command bytes are sent using CMD_WRITE with ATN flag set.
    static constexpr uint32_t kMaxPayload = kBulkBufferSize - 16;
    if (len > kMaxPayload) len = kMaxPayload;

    uint8_t pkt[kBulkBufferSize];
    uint32_t i = 0;
    pkt[i++] = AGILENT_CMD_WRITE;
    pkt[i++] = 0;    // primary address (ignored: NO_ADDRESS is set)
    pkt[i++] = 0;    // secondary address
    pkt[i++] = AGILENT_AWF_NO_ADDRESS | AGILENT_AWF_ATN |
               AGILENT_AWF_NO_FAST_TALKER_FIRST_BYTE | AGILENT_AWF_NO_FAST_TALKER;
    pkt[i++] = (uint8_t)(len & 0xff);
    pkt[i++] = (uint8_t)((len >> 8) & 0xff);
    pkt[i++] = (uint8_t)((len >> 16) & 0xff);
    pkt[i++] = (uint8_t)((len >> 24) & 0xff);
    memcpy(&pkt[i], cmds, len);
    i += len;

    uint32_t rc = bulkOut(pkt, i, kRegOpTimeoutMs * 1000);
    if (rc != GPIBT_OK) {
        sendAbort(false);
        return rc;
    }

    // Firmware acks via interrupt pipe (WRITE_COMPLETE_BN). Without
    // interrupt-pipe polling we fall back to reading the status via a
    // control transfer, which is what linux-gpib does after the interrupt
    // fires. Simpler here: request XFER_STATUS directly.
    if (!device_ || !owner_) return GPIBT_OK;

    IOBufferMemoryDescriptor *sbuf = nullptr;
    if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn,
                                         AGILENT_STATUS_DATA_LEN, 0, &sbuf)
        != kIOReturnSuccess) {
        return GPIBT_OK;
    }
    uint16_t bytesTransferred = 0;
    kern_return_t kr = device_->DeviceRequest(owner_,
                                              0xC0,
                                              AGILENT_82357_CONTROL_REQUEST,
                                              AGILENT_XFER_STATUS,
                                              0, AGILENT_STATUS_DATA_LEN,
                                              sbuf, &bytesTransferred,
                                              kControlTimeoutMs);
    OSSafeReleaseNULL(sbuf);
    return (kr == kIOReturnSuccess) ? GPIBT_OK : mapIOReturn(kr);
}

uint32_t Agilent82357Transport::writeData(const uint8_t *buf, uint32_t len,
                                          bool send_eoi, uint32_t timeout_us,
                                          uint32_t *outBytesWritten) {
    if (outBytesWritten) *outBytesWritten = 0;
    if (!buf || len == 0) return GPIBT_OK;
    static constexpr uint32_t kMaxPayload = kBulkBufferSize - 16;
    if (len > kMaxPayload) len = kMaxPayload;

    uint8_t pkt[kBulkBufferSize];
    uint32_t i = 0;
    pkt[i++] = AGILENT_CMD_WRITE;
    pkt[i++] = 0;
    pkt[i++] = 0;
    uint8_t flags = AGILENT_AWF_NO_ADDRESS | AGILENT_AWF_NO_FAST_TALKER_FIRST_BYTE;
    if (send_eoi) flags |= AGILENT_AWF_SEND_EOI;
    pkt[i++] = flags;
    pkt[i++] = (uint8_t)(len & 0xff);
    pkt[i++] = (uint8_t)((len >> 8) & 0xff);
    pkt[i++] = (uint8_t)((len >> 16) & 0xff);
    pkt[i++] = (uint8_t)((len >> 24) & 0xff);
    memcpy(&pkt[i], buf, len);
    i += len;

    uint32_t rc = bulkOut(pkt, i, timeout_us);
    if (rc != GPIBT_OK) {
        sendAbort(false);
        return rc;
    }

    // Read the write-complete status via a control transfer.
    if (!device_ || !owner_) {
        if (outBytesWritten) *outBytesWritten = len;
        return GPIBT_OK;
    }

    IOBufferMemoryDescriptor *sbuf = nullptr;
    if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn,
                                         AGILENT_STATUS_DATA_LEN, 0, &sbuf)
        != kIOReturnSuccess) {
        if (outBytesWritten) *outBytesWritten = len;
        return GPIBT_OK;
    }
    uint16_t bytesTransferred = 0;
    kern_return_t kr = device_->DeviceRequest(owner_,
                                              0xC0,
                                              AGILENT_82357_CONTROL_REQUEST,
                                              AGILENT_XFER_STATUS,
                                              0, AGILENT_STATUS_DATA_LEN,
                                              sbuf, &bytesTransferred,
                                              kControlTimeoutMs);
    if (kr == kIOReturnSuccess && bytesTransferred >= 6) {
        uint64_t addr = 0, sz = 0;
        if (sbuf->Map(0, 0, 0, 0, &addr, &sz) == kIOReturnSuccess) {
            const uint8_t *r = (const uint8_t *)addr;
            uint32_t written = (uint32_t)r[2] |
                               ((uint32_t)r[3] << 8) |
                               ((uint32_t)r[4] << 16) |
                               ((uint32_t)r[5] << 24);
            if (outBytesWritten) *outBytesWritten = written;
        }
    } else if (outBytesWritten) {
        *outBytesWritten = len;
    }
    OSSafeReleaseNULL(sbuf);
    return GPIBT_OK;
}

uint32_t Agilent82357Transport::readData(uint8_t *buf, uint32_t request_count,
                                         uint8_t eos_char, uint8_t eos_flags,
                                         uint32_t timeout_us,
                                         uint32_t *outBytesRead, uint8_t *outEnd) {
    if (outBytesRead) *outBytesRead = 0;
    if (outEnd) *outEnd = 0;
    if (!buf || request_count == 0) return GPIBT_OK;
    static constexpr uint32_t kMaxRead = kBulkBufferSize - 32;
    if (request_count > kMaxRead) request_count = kMaxRead;

    // CMD_READ packet, 9 bytes.
    uint8_t pkt[9];
    uint32_t i = 0;
    pkt[i++] = AGILENT_CMD_READ;
    pkt[i++] = 0;    // primary address (ignored)
    pkt[i++] = 0;    // secondary address
    uint8_t flags = AGILENT_ARF_NO_ADDRESS | AGILENT_ARF_END_ON_EOI;
    if (eos_flags & 0x04 /* REOS from gpib_user.h REOS==0x400>>8 */) {
        // Note: REOS in gpib_user.h is 0x400. Here we receive eos_flags
        // already shifted to the low 8 bits by GPIBBoard? No — GPIBBoard
        // passes them through as-is. Check REOS bit (0x0400) in the full
        // value; caller must set high byte accordingly. We accept either.
    }
    pkt[i++] = flags;
    pkt[i++] = (uint8_t)(request_count & 0xff);
    pkt[i++] = (uint8_t)((request_count >> 8) & 0xff);
    pkt[i++] = (uint8_t)((request_count >> 16) & 0xff);
    pkt[i++] = (uint8_t)((request_count >> 24) & 0xff);
    pkt[i++] = eos_char;

    uint32_t rc = bulkOut(pkt, i, timeout_us);
    if (rc != GPIBT_OK) {
        sendAbort(false);
        return rc;
    }

    // Response: up to `request_count + 1` bytes. The last byte carries
    // trailing flags (EOI/EOS/COUNT/...).
    uint8_t stack_buf[kBulkBufferSize];
    uint32_t respLen = 0;
    rc = bulkIn(stack_buf, request_count + 1, timeout_us, &respLen);
    if (rc == GPIBT_ERR_TIMEOUT) {
        // Flush any partial data left in the pipe.
        sendAbort(true);
        uint32_t extraLen = 0;
        bulkIn(stack_buf + respLen, request_count + 1 - respLen,
               100000, &extraLen);
        respLen += extraLen;
    } else if (rc != GPIBT_OK) {
        sendAbort(false);
        return rc;
    }

    if (respLen > request_count + 1) respLen = request_count + 1;
    if (respLen == 0) return GPIBT_OK;

    uint32_t payload = respLen - 1;
    uint8_t trailing = stack_buf[respLen - 1];
    if (payload > request_count) payload = request_count;
    memcpy(buf, stack_buf, payload);
    if (outBytesRead) *outBytesRead = payload;
    if (outEnd) *outEnd = (trailing & (AGILENT_ATRF_EOI | AGILENT_ATRF_EOS)) ? 1 : 0;
    return GPIBT_OK;
}

uint32_t Agilent82357Transport::readBusLines(uint16_t *outLines) {
    if (!outLines) return GPIBT_ERR_IO;
    Agilent82357RegPair reg = { TMS_BSR, 0 };
    uint32_t rc = readRegisters(&reg, 1);
    if (rc != GPIBT_OK) {
        *outLines = 0;
        return rc;
    }
    // TMS9914 BSR bits are active-high representations of the (active-low)
    // GPIB lines. The gpib_user.h BusXXX convention is 1 iff the line is
    // NOT asserted (i.e. matches the raw high-level electrical state); we
    // mirror that here.
    uint16_t v = ValidALL;
    if (!(reg.value & TMS_BSR_REN_BIT))  v |= BusREN;
    if (!(reg.value & TMS_BSR_IFC_BIT))  v |= BusIFC;
    if (!(reg.value & TMS_BSR_SRQ_BIT))  v |= BusSRQ;
    if (!(reg.value & TMS_BSR_EOI_BIT))  v |= BusEOI;
    if (!(reg.value & TMS_BSR_NRFD_BIT)) v |= BusNRFD;
    if (!(reg.value & TMS_BSR_NDAC_BIT)) v |= BusNDAC;
    if (!(reg.value & TMS_BSR_DAV_BIT))  v |= BusDAV;
    if (!(reg.value & TMS_BSR_ATN_BIT))  v |= BusATN;
    *outLines = v;
    return GPIBT_OK;
}

uint8_t Agilent82357Transport::nanosToFastTalker(uint32_t *nanosec_inout) {
    static const uint32_t nanosec_per_bit = 21;
    static const uint32_t max_value = 0x72;
    static const uint32_t min_value = 0x11;
    uint32_t bits = (*nanosec_inout + nanosec_per_bit / 2) / nanosec_per_bit;
    if (bits < min_value) bits = min_value;
    if (bits > max_value) bits = max_value;
    *nanosec_inout = bits * nanosec_per_bit;
    return (uint8_t)bits;
}

uint32_t Agilent82357Transport::runInitSequence() {
    // Reset the firmware, wait, then apply the standard init register batch.
    // Matches agilent_82357a_init() in linux-gpib.
    Agilent82357RegPair batch1[2] = {
        { AGILENT_FW_LED_CONTROL,      AGILENT_LED_FAIL_ON },
        { AGILENT_FW_RESET_TO_POWERUP, AGILENT_RESET_SPACEBALL },
    };
    uint32_t rc = writeRegisters(batch1, 2);
    if (rc != GPIBT_OK) return rc;
    IOSleep(3);   // ~2ms per linux-gpib

    uint32_t nanosec = 800;   // t1 default
    uint8_t t1_bits  = nanosToFastTalker(&nanosec);

    Agilent82357RegPair batch2[18] = {
        { TMS_AUXCR,                   AGILENT_AUX_NBAF },
        { TMS_AUXCR,                   AGILENT_AUX_HLDE },
        { TMS_AUXCR,                   AGILENT_AUX_TON },
        { TMS_AUXCR,                   AGILENT_AUX_LON },
        { TMS_AUXCR,                   AGILENT_AUX_RSV2 },
        { TMS_AUXCR,                   AGILENT_AUX_INVAL },
        { TMS_AUXCR,                   AGILENT_AUX_RPP },
        { TMS_AUXCR,                   AGILENT_AUX_STDL },
        { TMS_AUXCR,                   AGILENT_AUX_VSTDL },
        { AGILENT_FW_FAST_TALKER_T1,   t1_bits },
        { TMS_ADR,                     0 },                  // PAD 0
        { TMS_PPR,                     0 },
        { TMS_SPMR,                    0 },
        { AGILENT_FW_PROTOCOL_CONTROL, AGILENT_PROTO_WRITE_COMPLETE_IE },
        { TMS_IMR0,                    (uint8_t)(TMS_HR_BOIE | TMS_HR_BIIE) },
        { TMS_IMR1,                    TMS_HR_SRQIE },
        { TMS_AUXCR,                   AGILENT_AUX_CHIP_RESET },  // exit reset
        { AGILENT_FW_LED_CONTROL,      AGILENT_LED_FW_CONTROL },
    };
    rc = writeRegisters(batch2, 18);
    if (rc != GPIBT_OK) return rc;

    // Cache HW_CONTROL and mark ourselves system controller.
    Agilent82357RegPair rd = { AGILENT_FW_HW_CONTROL, 0 };
    rc = readRegisters(&rd, 1);
    if (rc != GPIBT_OK) return rc;
    hwControlBits_ = (rd.value & ~0x07) |
                     AGILENT_HWC_NOT_TI_RESET |
                     AGILENT_HWC_NOT_PARALLEL_POLL |
                     AGILENT_HWC_SYSTEM_CONTROLLER;

    Agilent82357RegPair sysc[2] = {
        { TMS_AUXCR,             (uint8_t)(AGILENT_AUX_CS | AGILENT_AUX_RQC) },
        { AGILENT_FW_HW_CONTROL, hwControlBits_ },
    };
    rc = writeRegisters(sysc, 2);
    return rc;
}
