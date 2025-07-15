//
//  DriverUtils.cpp
//  darwin-gpib
//
//  Created by Thiago Mattos on 18/07/25.
//

#include "DriverUtils.h"
#include <DriverKit/IOLib.h>

OSString *copyDeviceString(const IOUSBStringDescriptor *stringDescriptor, const char *fallback) {
    /// Retrieve string from descriptor / fallback
    if (stringDescriptor == NULL) {
            const char *str = fallback ? fallback : "Unknown";
            return OSString::withCString(str, strlen(str));
        }

        // USB string descriptors are UTF-16LE
        uint16_t *utf16String = (uint16_t *)stringDescriptor->bString;
        size_t utf16Length = (stringDescriptor->bLength - 2) / 2; // Number of UTF-16 chars
        
        // Convert UTF-16LE to UTF-8
        char *utf8String = (char *)IOMalloc(utf16Length * 4 + 1); // Max possible UTF-8 size
        if (!utf8String) return NULL;
        
        size_t utf8Pos = 0;
        for (size_t i = 0; i < utf16Length; i++) {
            uint16_t ch = OSSwapLittleToHostInt16(utf16String[i]);
            if (ch < 0x80) {
                utf8String[utf8Pos++] = ch;
            } else if (ch < 0x800) {
                utf8String[utf8Pos++] = 0xC0 | (ch >> 6);
                utf8String[utf8Pos++] = 0x80 | (ch & 0x3F);
            } else {
                utf8String[utf8Pos++] = 0xE0 | (ch >> 12);
                utf8String[utf8Pos++] = 0x80 | ((ch >> 6) & 0x3F);
                utf8String[utf8Pos++] = 0x80 | (ch & 0x3F);
            }
        }
        utf8String[utf8Pos] = '\0';
        
        OSString *result = OSString::withCString(utf8String, utf8Pos);
        IOFree(utf8String, utf16Length * 4 + 1);
        
        return result;
    
}
