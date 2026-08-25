//
//  NIUSBTransport.cpp
//  darwin-gpib driver
//

#include <os/log.h>
#include <string.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>

#include "NIUSBTransport.h"
#include "ni_usb_protocol.h"
#include "ni_usb_tnt4882.h"
#include "gpib_user.h"    // for END bit definition

// NI USB-HS wire-protocol opcodes for the addressed data path. These live
// alongside the register R/W IDs defined in ni_usb_protocol.h — kept local
// because they're only used inside the transport.
namespace {

constexpr uint8_t kNiOpcCommand   = 0x0c;   // send command bytes with ATN
constexpr uint8_t kNiOpcWrite     = 0x0d;   // send data bytes
constexpr uint8_t kNiOpcRead      = 0x0a;   // request data bytes
constexpr uint8_t kNiSubdevTNT    = 0x01;   // NIUSB_SUBDEV_TNT4882

// Status-block layout (8 bytes, big-endian ibsta, little-endian count).
struct StatusBlock {
    uint8_t  id;
    uint16_t ibsta;
    uint8_t  error_code;
    uint16_t count;          // remaining bytes; length - count = bytes_transferred
};

// Convert to the wire timeout code used in every addressed request. Matches
// linux-gpib's ni_usb_timeout_code().
uint8_t niTimeoutCode(uint32_t usec) {
    if (usec == 0xFFFFFFFFu || usec == 0)   return 0xf0;  // infinite / disabled
    if (usec <=       10) return 0xf1;
    if (usec <=       30) return 0xf2;
    if (usec <=      100) return 0xf3;
    if (usec <=      300) return 0xf4;
    if (usec <=     1000) return 0xf5;
    if (usec <=     3000) return 0xf6;
    if (usec <=    10000) return 0xf7;
    if (usec <=    30000) return 0xf8;
    if (usec <=   100000) return 0xf9;
    if (usec <=   300000) return 0xfa;
    if (usec <=  1000000) return 0xfb;
    if (usec <=  3000000) return 0xfc;
    if (usec <= 10000000) return 0xfd;
    if (usec <= 30000000) return 0xfe;
    if (usec <= 100000000) return 0xff;
    if (usec <= 300000000) return 0x01;
    return 0x02;
}

// Append the 4-byte termination trailer required by every bulk-out packet.
uint32_t appendTermination(uint8_t *buf, uint32_t i) {
    buf[i++] = NIUSB_TERM_ID;
    buf[i++] = 0x00;
    buf[i++] = 0x00;
    buf[i++] = 0x00;
    return i;
}

uint32_t padToBoundary(uint8_t *buf, uint32_t i, uint32_t boundary) {
    while (i % boundary) buf[i++] = 0x00;
    return i;
}

// Parse the 8-byte status block into a StatusBlock. count is transmitted
// as ~count (one's complement + 1 == twos complement of remaining), see
// ni_usb_parse_status_block in linux-gpib.
uint32_t parseStatusBlock(const uint8_t *buf, StatusBlock *out) {
    out->id         = buf[0];
    out->ibsta      = (uint16_t)((buf[1] << 8) | buf[2]);
    out->error_code = buf[3];
    uint16_t raw    = (uint16_t)(buf[4] | (buf[5] << 8));
    out->count      = (uint16_t)(~raw + 1);
    return 8;
}

// Map NI wire error codes to our transport-level error enum.
uint32_t mapNiError(uint8_t code) {
    switch (code) {
        case NIUSB_ERR_OK:              return GPIBT_OK;
        case NIUSB_ERR_ABORTED:         return GPIBT_ERR_ABORTED;
        case NIUSB_ERR_TIMEOUT:         return GPIBT_ERR_TIMEOUT;
        case NIUSB_ERR_NO_LISTENER:     return GPIBT_ERR_NO_LISTENER;
        case NIUSB_ERR_NO_BUS:          return GPIBT_ERR_NO_LISTENER;
        default:                        return GPIBT_ERR_IO;
    }
}

// Convert a kIOReturn* code from the USB stack into our transport enum.
uint32_t mapIOReturn(kern_return_t ret) {
    if (ret == kIOReturnSuccess) return GPIBT_OK;
    if (ret == kIOReturnTimeout) return GPIBT_ERR_TIMEOUT;
    if (ret == kIOReturnAborted) return GPIBT_ERR_ABORTED;
    if (ret == kIOReturnNotOpen) return GPIBT_ERR_NOT_READY;
    return GPIBT_ERR_IO;
}

}  // namespace

bool NIUSBTransport::init(IOUSBHostInterface *interface) {
    if (!interface) return false;
    interface_       = interface;
    bulkOutPipe_     = nullptr;
    bulkInPipe_      = nullptr;
    interruptInPipe_ = nullptr;
    outBuffer_       = nullptr;
    inBuffer_        = nullptr;
    return true;
}

void NIUSBTransport::free() {
    closePipes();
    OSSafeReleaseNULL(outBuffer_);
    OSSafeReleaseNULL(inBuffer_);
    interface_ = nullptr;
}

uint32_t NIUSBTransport::openPipes() {
    kern_return_t ret;

    ret = interface_->CopyPipe(NIUSB_HS_BULK_OUT_EP, &bulkOutPipe_);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "NIUSBTransport: CopyPipe bulk-out failed 0x%x", ret);
        return mapIOReturn(ret);
    }

    ret = interface_->CopyPipe(NIUSB_HS_BULK_IN_EP | 0x80, &bulkInPipe_);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "NIUSBTransport: CopyPipe bulk-in failed 0x%x", ret);
        return mapIOReturn(ret);
    }

    ret = interface_->CopyPipe(NIUSB_HS_INTERRUPT_IN_EP | 0x80, &interruptInPipe_);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "NIUSBTransport: CopyPipe interrupt-in failed 0x%x (non-fatal)", ret);
        interruptInPipe_ = nullptr;   // interrupt pipe is optional
    }

    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                           kBulkBufferSize, 0, &outBuffer_);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "NIUSBTransport: outBuffer create failed 0x%x", ret);
        return mapIOReturn(ret);
    }

    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                           kBulkBufferSize, 0, &inBuffer_);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "NIUSBTransport: inBuffer create failed 0x%x", ret);
        return mapIOReturn(ret);
    }

    return GPIBT_OK;
}

void NIUSBTransport::closePipes() {
    OSSafeReleaseNULL(bulkOutPipe_);
    OSSafeReleaseNULL(bulkInPipe_);
    OSSafeReleaseNULL(interruptInPipe_);
}

uint32_t NIUSBTransport::attach() {
    uint32_t rc = openPipes();
    if (rc != GPIBT_OK) return rc;

    rc = runInitSequence();
    if (rc != GPIBT_OK) {
        os_log(OS_LOG_DEFAULT, "NIUSBTransport: init sequence failed rc=%u", rc);
        return rc;
    }
    os_log(OS_LOG_DEFAULT, "NIUSBTransport: attached and initialized");
    return GPIBT_OK;
}

void NIUSBTransport::detach() {
    closePipes();
}

uint32_t NIUSBTransport::bulkOut(const uint8_t *bytes, uint32_t len,
                                 uint32_t timeout_us) {
    if (!bulkOutPipe_ || !outBuffer_) return GPIBT_ERR_NOT_READY;
    if (len > kBulkBufferSize) return GPIBT_ERR_IO;

    uint64_t addr = 0;
    uint64_t outLen = 0;
    kern_return_t ret = outBuffer_->Map(0, 0, 0, 0, &addr, &outLen);
    if (ret != kIOReturnSuccess) return mapIOReturn(ret);
    memcpy((void *)addr, bytes, len);

    uint32_t timeoutMs = (timeout_us == 0xFFFFFFFFu) ? 0 : (timeout_us / 1000u);
    if (timeoutMs == 0 && timeout_us != 0xFFFFFFFFu) timeoutMs = 1;   // minimum 1ms

    uint32_t bytesTransferred = 0;
    ret = bulkOutPipe_->IO(outBuffer_, len, &bytesTransferred, timeoutMs);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "NIUSBTransport: bulkOut IO failed 0x%x", ret);
        return mapIOReturn(ret);
    }
    return GPIBT_OK;
}

uint32_t NIUSBTransport::bulkIn(uint8_t *bytes, uint32_t capacity,
                                uint32_t timeout_us, uint32_t *outBytesRead) {
    if (!bulkInPipe_ || !inBuffer_) return GPIBT_ERR_NOT_READY;
    if (capacity > kBulkBufferSize) capacity = kBulkBufferSize;

    uint32_t timeoutMs = (timeout_us == 0xFFFFFFFFu) ? 0 : (timeout_us / 1000u);
    if (timeoutMs == 0 && timeout_us != 0xFFFFFFFFu) timeoutMs = 1;

    uint32_t bytesTransferred = 0;
    kern_return_t ret = bulkInPipe_->IO(inBuffer_, capacity,
                                        &bytesTransferred, timeoutMs);
    if (ret != kIOReturnSuccess) {
        if (outBytesRead) *outBytesRead = 0;
        return mapIOReturn(ret);
    }

    uint64_t addr = 0;
    uint64_t inLen = 0;
    ret = inBuffer_->Map(0, 0, 0, 0, &addr, &inLen);
    if (ret != kIOReturnSuccess) {
        if (outBytesRead) *outBytesRead = 0;
        return mapIOReturn(ret);
    }
    memcpy(bytes, (const void *)addr, bytesTransferred);
    if (outBytesRead) *outBytesRead = bytesTransferred;
    return GPIBT_OK;
}

uint32_t NIUSBTransport::writeRegister(uint8_t nec_reg, uint8_t value) {
    // Packet format:
    //   [REG_WRITE_ID][count=1][addr=NEC_TO_TNT(reg)][value][TERM_ID][pad...]
    uint8_t pkt[8] = {
        NIUSB_REG_WRITE_ID,
        0x01,                                   // count = 1 register
        (uint8_t)NEC_TO_TNT(nec_reg),
        value,
        NIUSB_TERM_ID,
        0x00, 0x00, 0x00,
    };
    uint32_t rc = bulkOut(pkt, sizeof(pkt), 100000);   // 100ms
    if (rc != GPIBT_OK) return rc;

    // Drain the status response (we don't parse it here).
    uint8_t resp[64];
    uint32_t respLen = 0;
    bulkIn(resp, sizeof(resp), 100000, &respLen);
    return GPIBT_OK;
}

uint32_t NIUSBTransport::readRegister(uint8_t nec_reg, uint8_t *outValue) {
    uint8_t pkt[8] = {
        NIUSB_REG_READ_ID,
        0x01,
        (uint8_t)NEC_TO_TNT(nec_reg),
        NIUSB_TERM_ID,
        0x00, 0x00, 0x00, 0x00,
    };
    uint32_t rc = bulkOut(pkt, sizeof(pkt), 100000);
    if (rc != GPIBT_OK) return rc;

    uint8_t resp[64];
    uint32_t respLen = 0;
    rc = bulkIn(resp, sizeof(resp), 100000, &respLen);
    if (rc != GPIBT_OK) return rc;

    if (respLen >= 3 && resp[0] == NIUSB_REGISTER_READ_DATA_START_ID) {
        if (outValue) *outValue = resp[2];
        return GPIBT_OK;
    }
    return GPIBT_ERR_IO;
}

uint32_t NIUSBTransport::writeAuxCmd(uint8_t aux_cmd) {
    return writeRegister(NEC7210_AUXMR, aux_cmd);
}

uint32_t NIUSBTransport::pulseInterfaceClear() {
    uint32_t rc = writeAuxCmd(AUX_SIFC);
    if (rc != GPIBT_OK) return rc;
    IOSleep(1);
    return writeAuxCmd(AUX_CIFC);
}

uint32_t NIUSBTransport::setRemoteEnable(bool enable) {
    return writeAuxCmd(enable ? AUX_SREN : AUX_CREN);
}

uint32_t NIUSBTransport::sendCommandBytes(const uint8_t *cmds, uint32_t len) {
    // Bulk-out framing for command bytes:
    //   [0x0c][~(len-1)&0xff][0x00][timeout_code][cmd0..cmdN-1]
    //   pad-to-4 + [TERM_ID][0,0,0]
    // 82357/USB-B accepts a maximum of 16 command bytes per request.
    if (len == 0) return GPIBT_OK;
    if (len > 16) len = 16;

    uint8_t pkt[64];
    uint32_t i = 0;
    uint16_t complement = (uint16_t)~((uint16_t)(len - 1));
    pkt[i++] = kNiOpcCommand;
    pkt[i++] = (uint8_t)(complement & 0xff);
    pkt[i++] = 0x00;
    pkt[i++] = niTimeoutCode(3000000);      // T3s default
    for (uint32_t j = 0; j < len; ++j) pkt[i++] = cmds[j];
    i = padToBoundary(pkt, i, 4);
    i = appendTermination(pkt, i);

    uint32_t rc = bulkOut(pkt, i, 3000000);
    if (rc != GPIBT_OK) return rc;

    // Response is a 12-byte status block. Parse it to detect errors like
    // no-listener (Err 5/8).
    uint8_t resp[32];
    uint32_t respLen = 0;
    rc = bulkIn(resp, sizeof(resp), 3000000, &respLen);
    if (rc != GPIBT_OK) return rc;
    if (respLen < 8) return GPIBT_ERR_IO;

    StatusBlock st;
    parseStatusBlock(resp, &st);
    return mapNiError(st.error_code);
}

uint32_t NIUSBTransport::writeData(const uint8_t *buf, uint32_t len,
                                   bool send_eoi, uint32_t timeout_us,
                                   uint32_t *outBytesWritten) {
    if (outBytesWritten) *outBytesWritten = 0;
    if (!buf || len == 0) {
        if (outBytesWritten) *outBytesWritten = 0;
        return GPIBT_OK;
    }
    // We use the fixed 4K bulk buffer, so cap the payload accordingly.
    static constexpr uint32_t kMaxPayload = kBulkBufferSize - 16;
    if (len > kMaxPayload) len = kMaxPayload;

    // Framing:
    //   [0x0d][~(len-1)&0xff][~(len-1)>>8 &0xff][timeout_code]
    //   [0][0][send_eoi?0x08:0x00][0][payload...]
    //   pad-to-4 + [TERM_ID][0,0,0]
    uint8_t pkt[kBulkBufferSize];
    uint32_t i = 0;
    uint16_t complement = (uint16_t)~((uint16_t)(len - 1));
    pkt[i++] = kNiOpcWrite;
    pkt[i++] = (uint8_t)(complement & 0xff);
    pkt[i++] = (uint8_t)((complement >> 8) & 0xff);
    pkt[i++] = niTimeoutCode(timeout_us);
    pkt[i++] = 0x00;
    pkt[i++] = 0x00;
    pkt[i++] = send_eoi ? 0x08 : 0x00;
    pkt[i++] = 0x00;
    memcpy(&pkt[i], buf, len);
    i += len;
    i = padToBoundary(pkt, i, 4);
    i = appendTermination(pkt, i);

    uint32_t rc = bulkOut(pkt, i, timeout_us);
    if (rc != GPIBT_OK) return rc;

    // Response: 8-byte status + 4-byte termination = 12 bytes.
    uint8_t resp[32];
    uint32_t respLen = 0;
    rc = bulkIn(resp, sizeof(resp), timeout_us, &respLen);
    if (rc != GPIBT_OK) return rc;
    if (respLen < 8) return GPIBT_ERR_IO;

    StatusBlock st;
    parseStatusBlock(resp, &st);
    uint32_t written = (st.count <= len) ? (len - st.count) : 0;
    if (outBytesWritten) *outBytesWritten = written;
    return mapNiError(st.error_code);
}

uint32_t NIUSBTransport::readData(uint8_t *buf, uint32_t request_count,
                                  uint8_t eos_char, uint8_t eos_flags,
                                  uint32_t timeout_us,
                                  uint32_t *outBytesRead, uint8_t *outEnd) {
    if (outBytesRead) *outBytesRead = 0;
    if (outEnd) *outEnd = 0;
    if (!buf || request_count == 0) return GPIBT_OK;

    static constexpr uint32_t kMaxRead = kBulkBufferSize - 64;
    if (request_count > kMaxRead) request_count = kMaxRead;

    // Framing:
    //   [0x0a][eos_mode_hi][eos_char][timeout_code]
    //   [~(len-1)&0xff][~(len-1)>>8 &0xff][0][0]
    //   [REG_WRITE_ID][2][0]
    //   [SUBDEV_TNT][NEC_TO_TNT(AUXMR)][AUX_HLDI]
    //   [SUBDEV_TNT][NEC_TO_TNT(AUXMR)][AUX_CLEAR_END]
    //   pad-to-4 + [TERM_ID][0,0,0]
    uint8_t pkt[64];
    uint32_t i = 0;
    uint16_t complement = (uint16_t)~((uint16_t)(request_count - 1));
    pkt[i++] = kNiOpcRead;
    pkt[i++] = (uint8_t)((eos_flags >> 8) & 0xff);
    pkt[i++] = eos_char;
    pkt[i++] = niTimeoutCode(timeout_us);
    pkt[i++] = (uint8_t)(complement & 0xff);
    pkt[i++] = (uint8_t)((complement >> 8) & 0xff);
    pkt[i++] = 0x00;
    pkt[i++] = 0x00;
    // Register-write block: two writes to AUXMR to clear held-off state.
    pkt[i++] = NIUSB_REG_WRITE_ID;
    pkt[i++] = 0x02;
    pkt[i++] = 0x00;
    pkt[i++] = kNiSubdevTNT;
    pkt[i++] = NEC_TO_TNT(NEC7210_AUXMR);
    pkt[i++] = AUX_HLDI;
    pkt[i++] = kNiSubdevTNT;
    pkt[i++] = NEC_TO_TNT(NEC7210_AUXMR);
    pkt[i++] = AUX_CLEAR_END;
    i = padToBoundary(pkt, i, 4);
    i = appendTermination(pkt, i);

    uint32_t rc = bulkOut(pkt, i, timeout_us);
    if (rc != GPIBT_OK) return rc;

    // Response is a variable number of IBRD data blocks followed by an
    // IBRD_STATUS block. Each IBRD_DATA_ID block carries 15 payload bytes;
    // IBRD_EXTENDED_DATA_ID blocks carry 30. Read up to the max response
    // size we sized inBuffer_ for.
    uint8_t resp[kBulkBufferSize];
    uint32_t respLen = 0;
    rc = bulkIn(resp, sizeof(resp), timeout_us, &respLen);
    if (rc != GPIBT_OK) return rc;

    uint32_t bytesOut = 0;
    uint32_t p = 0;
    uint32_t blockPayload = 0;

    // Walk data blocks.
    while (p < respLen &&
           (resp[p] == NIUSB_IBRD_DATA_ID || resp[p] == NIUSB_IBRD_EXTENDED_DATA_ID)) {
        if (resp[p] == NIUSB_IBRD_DATA_ID) {
            blockPayload = 15;
            ++p;
        } else {
            blockPayload = 30;
            p += 2;   // skip id + one reserved byte
        }
        for (uint32_t k = 0; k < blockPayload && p < respLen; ++k, ++p) {
            if (bytesOut < request_count) buf[bytesOut++] = resp[p];
        }
    }

    // Then the IBRD status block.
    if (p + 8 > respLen) {
        if (outBytesRead) *outBytesRead = bytesOut;
        return GPIBT_ERR_IO;
    }
    StatusBlock st;
    parseStatusBlock(&resp[p], &st);
    p += 8;
    if (st.id != NIUSB_IBRD_STATUS_ID) {
        if (outBytesRead) *outBytesRead = bytesOut;
        return GPIBT_ERR_IO;
    }

    // Trim overshoot: firmware always fills the last block completely,
    // and the "extra" trailing byte after the status tells us how many
    // bytes of the *final* block were actually valid.
    if (blockPayload && p < respLen && bytesOut > 0) {
        uint8_t tail = resp[p++];
        uint32_t completeBlocks = 0;
        if (bytesOut % blockPayload == 0)
            completeBlocks = bytesOut / blockPayload;
        else
            completeBlocks = (bytesOut / blockPayload);
        uint32_t actualBytes = completeBlocks
            ? (completeBlocks - 1) * blockPayload + tail
            : tail;
        if (actualBytes > bytesOut) actualBytes = bytesOut;
        bytesOut = actualBytes;
    }

    if (outEnd) *outEnd = (st.ibsta & END) ? 1 : 0;
    if (outBytesRead) *outBytesRead = bytesOut;
    return mapNiError(st.error_code);
}

uint32_t NIUSBTransport::runInitSequence() {
    // Minimal NEC7210 / TNT4882 bring-up: chip reset, then clear pon to
    // bring the chip out of power-on hold.
    uint32_t rc;
    if ((rc = writeAuxCmd(AUX_CR)) != GPIBT_OK) return rc;
    IOSleep(2);
    if ((rc = writeAuxCmd(AUX_PON)) != GPIBT_OK) return rc;
    return GPIBT_OK;
}
