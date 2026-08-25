//
//  agilent_82357_tms9914.h
//  darwin-gpib driver
//
//  TMS9914A register map used by the Agilent 82357A/B firmware. The USB
//  register-read/write commands take these offsets in the pairlet's
//  `address` field. Contents mirror linux-gpib's include/tms9914.h.
//

#ifndef agilent_82357_tms9914_h
#define agilent_82357_tms9914_h

#include <stdint.h>

// Write-only registers.
enum TMS9914_WriteReg : uint8_t {
    TMS_IMR0  = 0,       // interrupt mask 0
    TMS_IMR1  = 1,       // interrupt mask 1
    TMS_AUXCR = 3,       // auxiliary command
    TMS_ADR   = 4,       // address register
    TMS_SPMR  = 5,       // serial poll mode
    TMS_PPR   = 6,       // parallel poll
};

// Read-only registers.
enum TMS9914_ReadReg : uint8_t {
    TMS_ADSR = 2,        // address status
    TMS_BSR  = 3,        // bus status
    TMS_CPTR = 6,        // command pass through
};

// AUXCR command bytes. AUX_CS (0x80) is OR'd to set the corresponding bit
// instead of clearing it (some commands are edge-triggered so the CS bit
// matters).
enum TMS9914_AuxCmd : uint8_t {
    AGILENT_AUX_CS          = 0x80,
    AGILENT_AUX_CHIP_RESET  = 0x00,   // with CS: set reset. without CS: clear.
    AGILENT_AUX_INVAL       = 0x01,
    AGILENT_AUX_VAL         = 0x81,   // AUX_INVAL | AUX_CS
    AGILENT_AUX_RHDF        = 0x02,
    AGILENT_AUX_HLDA        = 0x03,
    AGILENT_AUX_HLDE        = 0x04,
    AGILENT_AUX_NBAF        = 0x05,
    AGILENT_AUX_FGET        = 0x06,
    AGILENT_AUX_RTL         = 0x07,
    AGILENT_AUX_SEOI        = 0x08,
    AGILENT_AUX_LON         = 0x09,
    AGILENT_AUX_TON         = 0x0a,
    AGILENT_AUX_GTS         = 0x0b,
    AGILENT_AUX_TCA         = 0x0c,
    AGILENT_AUX_TCS         = 0x0d,
    AGILENT_AUX_RPP         = 0x0e,
    AGILENT_AUX_SIC         = 0x0f,
    AGILENT_AUX_SRE         = 0x10,
    AGILENT_AUX_RQC         = 0x11,
    AGILENT_AUX_RLC         = 0x12,
    AGILENT_AUX_DAI         = 0x13,
    AGILENT_AUX_PTS         = 0x14,
    AGILENT_AUX_STDL        = 0x15,
    AGILENT_AUX_SHDW        = 0x16,
    AGILENT_AUX_VSTDL       = 0x17,
    AGILENT_AUX_RSV2        = 0x18,
};

// ADR register bit fields.
enum TMS9914_AdrBits : uint8_t {
    TMS_ADDRESS_MASK = 0x1f,
};

// ADSR bit definitions (address status).
enum TMS9914_AdsrBits : uint8_t {
    TMS_HR_ULPA = 0x01,
    TMS_HR_TA   = 0x02,       // Talker Addressed
    TMS_HR_LA   = 0x04,       // Listener Addressed
    TMS_HR_TPAS = 0x08,
    TMS_HR_LPAS = 0x10,
    TMS_HR_ATN  = 0x20,       // ATN asserted
    TMS_HR_LLO  = 0x40,       // Local lockout active
    TMS_HR_REM  = 0x80,       // Remote active
};

// BSR bit definitions (bus status).
enum TMS9914_BsrBits : uint8_t {
    TMS_BSR_REN_BIT  = 0x01,
    TMS_BSR_IFC_BIT  = 0x02,
    TMS_BSR_SRQ_BIT  = 0x04,
    TMS_BSR_EOI_BIT  = 0x08,
    TMS_BSR_NRFD_BIT = 0x10,
    TMS_BSR_NDAC_BIT = 0x20,
    TMS_BSR_DAV_BIT  = 0x40,
    TMS_BSR_ATN_BIT  = 0x80,
};

// IMR0 / IMR1 bits used by the init sequence.
enum TMS9914_ImrBits : uint8_t {
    TMS_HR_BOIE  = 0x10,      // byte out int enable
    TMS_HR_BIIE  = 0x20,      // byte in int enable
    TMS_HR_SRQIE = 0x02,      // SRQ int enable (IMR1)
};

// Firmware registers (not TMS9914 — internal to the 82357 firmware).
enum Agilent82357_FwReg : uint8_t {
    AGILENT_FW_HW_CONTROL       = 0x0a,
    AGILENT_FW_LED_CONTROL      = 0x0b,
    AGILENT_FW_RESET_TO_POWERUP = 0x0c,
    AGILENT_FW_PROTOCOL_CONTROL = 0x0d,
    AGILENT_FW_FAST_TALKER_T1   = 0x0e,
};

enum Agilent82357_HwControl : uint8_t {
    AGILENT_HWC_NOT_TI_RESET       = 0x01,
    AGILENT_HWC_SYSTEM_CONTROLLER  = 0x02,
    AGILENT_HWC_NOT_PARALLEL_POLL  = 0x04,
    AGILENT_HWC_OSCILLATOR_5V_ON   = 0x08,
    AGILENT_HWC_OUTPUT_5V_ON       = 0x20,
    AGILENT_HWC_CPLD_3V_ON         = 0x80,
};

enum Agilent82357_LedControl : uint8_t {
    AGILENT_LED_FW_CONTROL   = 0x01,
    AGILENT_LED_FAIL_ON      = 0x20,
    AGILENT_LED_READY_ON     = 0x40,
    AGILENT_LED_ACCESS_ON    = 0x80,
};

enum Agilent82357_Reset : uint8_t {
    AGILENT_RESET_SPACEBALL = 0x01,      // wait 2 msec after sending
};

enum Agilent82357_ProtocolControl : uint8_t {
    AGILENT_PROTO_WRITE_COMPLETE_IE = 0x01,
};

#endif /* agilent_82357_tms9914_h */
