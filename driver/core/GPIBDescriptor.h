//
//  GPIBDescriptor.h
//  darwin-gpib driver
//
//  Per-handle state. One slot per open ibdev()/ibfind() call.
//

#ifndef GPIBDescriptor_h
#define GPIBDescriptor_h

#include <stdint.h>

struct GPIBDescriptor {
    int32_t  handle;        // >=0 when active, -1 when free
    uint8_t  is_board;       // 1 for board descriptor, 0 for device descriptor
    uint8_t  pad;            // primary address 0..30
    int8_t   sad;            // secondary address 0..30 or -1 (NO_SAD)
    uint8_t  eot;            // assert EOI on last byte of write
    uint8_t  eos_char;       // end-of-string byte
    uint8_t  eos_flags;      // REOS | XEOS | BIN
    uint16_t reserved;
    uint32_t timeout_us;     // resolved timeout in microseconds (0 = none)
};

#endif /* GPIBDescriptor_h */
