//
//  agilent_82357_protocol.h
//  darwin-gpib driver
//
//  Wire-protocol constants for the Agilent 82357A / 82357B USB→GPIB adapters.
//  Values mirror the public linux-gpib driver so behaviour can be verified
//  against the upstream reference implementation.
//

#ifndef agilent_82357_protocol_h
#define agilent_82357_protocol_h

#include <stdint.h>

enum Agilent82357_VendorProduct : uint16_t {
    AGILENT_VID              = 0x0957,
    AGILENT_PID_82357A       = 0x0107,
    AGILENT_PID_82357A_PRE   = 0x0007,   // pre-firmware
    AGILENT_PID_82357B       = 0x0718,
    AGILENT_PID_82357B_PRE   = 0x0518,   // pre-firmware
};

// USB endpoints. 82357A and 82357B share the bulk-in endpoint but use
// different bulk-out and interrupt-in endpoints.
enum Agilent82357_Endpoints : uint8_t {
    AGILENT_82357_BULK_IN_EP         = 0x02,     // shared
    AGILENT_82357A_BULK_OUT_EP       = 0x04,
    AGILENT_82357A_INT_IN_EP         = 0x06,
    AGILENT_82357B_BULK_OUT_EP       = 0x06,
    AGILENT_82357B_INT_IN_EP         = 0x08,
};

// Bulk pipe opcodes. First byte of every bulk-out packet.
enum Agilent82357_BulkCmd : uint8_t {
    AGILENT_CMD_WRITE     = 0x01,   // send data bytes
    AGILENT_CMD_READ      = 0x03,   // request data bytes
    AGILENT_CMD_WR_REGS   = 0x04,   // register writes
    AGILENT_CMD_RD_REGS   = 0x05,   // register reads
};

// Flags for AGILENT_CMD_READ (byte offset 3 of the request).
enum Agilent82357_ReadFlags : uint8_t {
    AGILENT_ARF_END_ON_EOI       = 0x01,
    AGILENT_ARF_NO_ADDRESS       = 0x02,
    AGILENT_ARF_END_ON_EOS_CHAR  = 0x04,
    AGILENT_ARF_SPOLL            = 0x08,
};

// Trailing flags in the last byte of a read response.
enum Agilent82357_ReadTrailingFlags : uint8_t {
    AGILENT_ATRF_EOI          = 0x01,
    AGILENT_ATRF_ATN          = 0x02,
    AGILENT_ATRF_IFC          = 0x04,
    AGILENT_ATRF_EOS          = 0x08,
    AGILENT_ATRF_ABORT        = 0x10,
    AGILENT_ATRF_COUNT        = 0x20,
    AGILENT_ATRF_DEAD_BUS     = 0x40,
    AGILENT_ATRF_UNADDRESSED  = 0x80,
};

// Flags for AGILENT_CMD_WRITE (byte offset 3 of the request).
enum Agilent82357_WriteFlags : uint8_t {
    AGILENT_AWF_SEND_EOI                   = 0x01,
    AGILENT_AWF_NO_FAST_TALKER_FIRST_BYTE  = 0x02,
    AGILENT_AWF_NO_FAST_TALKER             = 0x04,
    AGILENT_AWF_NO_ADDRESS                 = 0x08,
    AGILENT_AWF_ATN                        = 0x10,
    AGILENT_AWF_SEPARATE_HEADER            = 0x80,
};

// Interrupt-pipe bit numbers.
enum Agilent82357_InterruptBits : uint8_t {
    AGILENT_AIF_SRQ            = 0,
    AGILENT_AIF_WRITE_COMPLETE = 1,
    AGILENT_AIF_READ_COMPLETE  = 2,
};

// Firmware-error return codes surfaced in register-response bytes.
enum Agilent82357_Error : uint8_t {
    AGILENT_UGP_SUCCESS            = 0,
    AGILENT_UGP_ERR_INVALID_CMD    = 1,
    AGILENT_UGP_ERR_INVALID_PARAM  = 2,
    AGILENT_UGP_ERR_INVALID_REG    = 3,
    AGILENT_UGP_ERR_GPIB_READ      = 4,
    AGILENT_UGP_ERR_GPIB_WRITE     = 5,
    AGILENT_UGP_ERR_FLUSHING       = 6,
    AGILENT_UGP_ERR_FLUSHING_ALREADY = 7,
    AGILENT_UGP_ERR_UNSUPPORTED    = 8,
    AGILENT_UGP_ERR_OTHER          = 9,
};

// Vendor-specific control-transfer selectors.
enum Agilent82357_ControlValue : uint8_t {
    AGILENT_XFER_ABORT   = 0xa0,
    AGILENT_XFER_STATUS  = 0xb0,
};

enum Agilent82357_XferStatusBits : uint8_t {
    AGILENT_XS_COMPLETED = 0x01,
    AGILENT_XS_READ      = 0x02,
};

enum Agilent82357_XferAbortType : uint16_t {
    AGILENT_XA_FLUSH = 0x0001,
};

static constexpr uint8_t AGILENT_82357_CONTROL_REQUEST = 0x04;

#define AGILENT_STATUS_DATA_LEN  8

#endif /* agilent_82357_protocol_h */
