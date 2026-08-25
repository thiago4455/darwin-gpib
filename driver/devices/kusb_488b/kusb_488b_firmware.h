//
//  kusb_488b_firmware.h
//  darwin-gpib driver
//
//  The KUSB-488B has no on-board flash for its FPGA bitstream or its main
//  8051 firmware — the host downloads both on every attach. Those images are
//  Keithley/ADLINK property and are NOT shipped with this source, exactly as
//  linux-gpib keeps adapter firmware in a separate gpib_firmware package.
//
//  Generate the data header from a Windows KI-488 installation:
//
//      tools/extract_kusb_firmware.py \
//          --driver  path/to/kusbgpib_fwdlx64.sys \
//          --rbf     path/to/USB-GPIB.RBF \
//          --out     driver/devices/kusb_488b/kusb_488b_firmware_data.h
//
//  Without it the loader still builds; it just declines to bring the device
//  up and logs what is missing.
//

#ifndef kusb_488b_firmware_h
#define kusb_488b_firmware_h

#include <stdint.h>

struct KUSBFirmwareBlob {
    const uint8_t *data;
    uint32_t       length;
};

#if __has_include("kusb_488b_firmware_data.h")
  #include "kusb_488b_firmware_data.h"
  #define KUSB_HAVE_FIRMWARE 1
#else
  #define KUSB_HAVE_FIRMWARE 0
#endif

#endif /* kusb_488b_firmware_h */
