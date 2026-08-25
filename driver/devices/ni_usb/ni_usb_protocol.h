//
//  ni_usb_protocol.h
//  darwin-gpib driver
//
//  Bulk command opcodes used to talk to the NI USB-HS adapter. The adapter
//  exposes a packet-oriented framing on its bulk OUT pipe; each command is
//  a small TLV-like blob terminated by NIUSB_TERM_ID, optionally followed
//  by payload bytes. The bulk IN pipe returns status blocks marked by the
//  data/status IDs.
//
//  Constants reproduce the public ones from the Linux ni_usb_gpib.h
//  reference (drivers/gpib/ni_usb/ni_usb_gpib.h) so behaviour can be
//  compared one-to-one against the upstream driver.
//

#ifndef ni_usb_protocol_h
#define ni_usb_protocol_h

#include <stdint.h>

enum NIUSB_BulkID : uint8_t {
    NIUSB_IBCAC_ID                     = 0x01,
    NIUSB_UNKNOWN3_ID                  = 0x03,
    NIUSB_TERM_ID                      = 0x04,
    NIUSB_IBGTS_ID                     = 0x06,
    NIUSB_IBRPP_ID                     = 0x07,
    NIUSB_REG_READ_ID                  = 0x08,
    NIUSB_REG_WRITE_ID                 = 0x09,
    NIUSB_IBSIC_ID                     = 0x0F,
    NIUSB_IB_WRITE_DATA_ID             = 0x0D, // payload preceded by this
    NIUSB_IBRD_DATA_ID                 = 0x36,
    NIUSB_IBRD_EXTENDED_DATA_ID        = 0x37,
    NIUSB_IBRD_STATUS_ID               = 0x38,
    NIUSB_REGISTER_READ_DATA_START_ID  = 0x34,
    NIUSB_REGISTER_READ_DATA_END_ID    = 0x35,
};

enum NIUSB_ControlRequest : uint8_t {
    NI_USB_STOP_REQUEST           = 0x20,
    NI_USB_WAIT_REQUEST           = 0x21,
    NI_USB_POLL_READY_REQUEST     = 0x40,
    NI_USB_SERIAL_NUMBER_REQUEST  = 0x41,
};

enum NIUSB_Error : uint8_t {
    NIUSB_ERR_OK                = 0,
    NIUSB_ERR_ABORTED           = 1,
    NIUSB_ERR_ATN_STATE         = 2,
    NIUSB_ERR_ADDRESSING        = 3,
    NIUSB_ERR_EOSMODE           = 4,
    NIUSB_ERR_NO_BUS            = 5,
    NIUSB_ERR_NO_LISTENER       = 8,
    NIUSB_ERR_TIMEOUT           = 10,
};

// USB endpoints for NI USB-HS, per Linux ni_usb_gpib.h. The actual numbers
// must still be discovered from the interface descriptor at runtime; these
// are the expected values used for sanity-checking.
#define NIUSB_HS_BULK_OUT_EP        0x06
#define NIUSB_HS_BULK_IN_EP         0x08
#define NIUSB_HS_INTERRUPT_IN_EP    0x01

#endif /* ni_usb_protocol_h */
