//
//  ni_usb_tnt4882.h
//  darwin-gpib driver
//
//  TNT4882-on-NIUSB register map. The NI USB-HS adapter exposes a TNT4882
//  GPIB controller behind a USB protocol layer. Linux's nec7210/tnt4882
//  driver writes 8-bit registers using NIUSB_REG_WRITE_ID packets; the
//  register offset on the wire is twice the NEC7210 register offset
//  (per nec7210_to_tnt4882_offset() in linux-gpib's ni_usb_gpib.c).
//

#ifndef ni_usb_tnt4882_h
#define ni_usb_tnt4882_h

#include <stdint.h>

// NEC7210 register file. The wire offset used in NIUSB_REG_WRITE_ID /
// NIUSB_REG_READ_ID packets is NEC_TO_TNT(reg).
enum NEC7210_Register : uint8_t {
    NEC7210_DIR    = 0x00,  // Data In (read)
    NEC7210_CDOR   = 0x00,  // Command/Data Out (write)
    NEC7210_ISR1   = 0x01,  // Interrupt Status 1 (read)
    NEC7210_IMR1   = 0x01,  // Interrupt Mask 1 (write)
    NEC7210_ISR2   = 0x02,
    NEC7210_IMR2   = 0x02,
    NEC7210_SPSR   = 0x03,  // Serial Poll Status (read)
    NEC7210_SPMR   = 0x03,  // Serial Poll Mode (write)
    NEC7210_ADSR   = 0x04,  // Address Status (read)
    NEC7210_ADMR   = 0x04,  // Address Mode (write)
    NEC7210_CPTR   = 0x05,  // Command Pass Through (read)
    NEC7210_AUXMR  = 0x05,  // Auxiliary Mode (write)
    NEC7210_ADR0   = 0x06,
    NEC7210_ADR1   = 0x07,
    NEC7210_EOSR   = 0x07,
};

static inline uint8_t NEC_TO_TNT(uint8_t reg) { return (uint8_t)(reg * 2); }

// AUXMR auxiliary commands (written via AUXMR register).
enum NEC7210_AuxCmd : uint8_t {
    AUX_PON         = 0x00,  // immediate execute pon
    AUX_CR          = 0x02,  // chip reset
    AUX_HLDA        = 0x84,  // hand-shake hold off acceptor
    AUX_HLDI        = 0x05,  // hand-shake hold off on EOI
    AUX_NBAF        = 0x07,  // new byte available false
    AUX_FH          = 0x03,  // finish handshake
    AUX_FGET        = 0x08,  // force group execute trigger
    AUX_RTL         = 0x0d,  // return to local
    AUX_SEOI        = 0x06,  // send EOI with next byte
    AUX_GTS         = 0x10,  // go to standby
    AUX_TCA         = 0x11,  // take control asynchronously
    AUX_TCS         = 0x12,  // take control synchronously
    AUX_LTN         = 0x13,  // listen
    AUX_DSC         = 0x14,  // disable system control
    AUX_CIFC        = 0x16,  // clear ifc
    AUX_SIFC        = 0x1e,  // set ifc
    AUX_CREN        = 0x17,  // clear ren
    AUX_SREN        = 0x1f,  // set ren
    AUX_CLEAR       = 0x00,
    AUX_CLEAR_END   = 0x04,  // clear end-of-string bit
    AUX_NVAL        = 0x0d,  // non-valid secondary command
    AUX_VAL         = 0x0f,  // valid secondary command
};

#endif /* ni_usb_tnt4882_h */
