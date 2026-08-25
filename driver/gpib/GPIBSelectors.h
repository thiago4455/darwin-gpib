//
//  GPIBSelectors.h
//  darwin-gpib
//
//  Shared between the libgpib client library and the GPIB driver extension.
//  Defines the ExternalMethod selector indices and the wire-format payload
//  structs that cross the IOUserClient boundary.
//
//  The structs are POD with explicit fixed-width fields. They must be
//  binary-compatible between the client (LP64, arm64/x86_64) and the dext
//  (DriverKit ABI), so no implicit padding and no host-specific types.
//

#ifndef GPIBSelectors_h
#define GPIBSelectors_h

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPIB_USER_CLIENT_TYPE 0x00000000u

enum GPIBSelector {
    kGPIBSel_BoardOnline       = 0,
    kGPIBSel_OpenDescriptor    = 1,
    kGPIBSel_CloseDescriptor   = 2,
    kGPIBSel_Configure         = 3,
    kGPIBSel_Write             = 4,
    kGPIBSel_Read              = 5,
    kGPIBSel_DeviceClear       = 6,
    kGPIBSel_InterfaceClear    = 7,
    kGPIBSel_RemoteEnable      = 8,
    kGPIBSel_Wait              = 9,
    kGPIBSel_Ask               = 10,
    kGPIBSel_SerialPoll        = 11,   // ibrsp / ReadStatusByte / AllSPoll
    kGPIBSel_Trigger           = 12,   // ibtrg / Trigger / TriggerList
    kGPIBSel_GoToLocal         = 13,   // ibloc / EnableLocal
    kGPIBSel_LocalLockout      = 14,   // SendLLO
    kGPIBSel_LineStatus        = 15,   // iblines
    kGPIBSel_ListenerPresent   = 16,   // ibln
    kGPIBSel_SendCommand       = 17,   // ibcmd / SendCmds (raw command bytes)
    kGPIBSel_Count
};

enum GPIBConfigureKey {
    kGPIBCfg_PAD        = 1,
    kGPIBCfg_SAD        = 2,
    kGPIBCfg_Timeout    = 3,
    kGPIBCfg_EOT        = 4,
    kGPIBCfg_EOSChar    = 5,
    kGPIBCfg_EOSFlags   = 6,
};

#pragma pack(push, 8)

typedef struct GPIBOpenDescriptorIn {
    uint32_t pad;
    uint32_t sad;
    uint32_t is_board;
    uint32_t timeout_code;
    uint32_t eos_char;
    uint32_t eos_flags;
    uint32_t eot;
    uint32_t reserved;
} GPIBOpenDescriptorIn;

typedef struct GPIBOpenDescriptorOut {
    int32_t  handle;
    uint32_t ibsta;
    uint32_t iberr;
    uint32_t reserved;
} GPIBOpenDescriptorOut;

typedef struct GPIBConfigureIn {
    int32_t  handle;
    uint32_t key;
    int32_t  value;
    uint32_t reserved;
} GPIBConfigureIn;

typedef struct GPIBStatusOut {
    uint32_t ibsta;
    uint32_t iberr;
    uint32_t ibcnt;
    uint32_t reserved;
} GPIBStatusOut;

typedef struct GPIBWriteIn {
    int32_t  handle;
    uint32_t send_eoi;
    uint32_t length;
    uint32_t reserved;
    /* Payload follows in-line for small writes; larger writes use the
     * memory descriptor on IOUserClientMethodArguments.structureInput. */
    uint8_t  data[0];
} GPIBWriteIn;

typedef struct GPIBWriteOut {
    uint32_t ibsta;
    uint32_t iberr;
    uint32_t ibcnt;
    uint32_t reserved;
} GPIBWriteOut;

typedef struct GPIBReadIn {
    int32_t  handle;
    uint32_t request_count;
} GPIBReadIn;

typedef struct GPIBReadOut {
    uint32_t ibsta;
    uint32_t iberr;
    uint32_t ibcnt;
    uint32_t end_flag;
    /* Returned bytes follow in-line; large reads use structureOutput. */
    uint8_t  data[0];
} GPIBReadOut;

typedef struct GPIBHandleIn {
    int32_t  handle;
    uint32_t reserved;
} GPIBHandleIn;

typedef struct GPIBRemoteEnableIn {
    int32_t  handle;
    int32_t  enable;
} GPIBRemoteEnableIn;

typedef struct GPIBOnlineIn {
    uint32_t online;
    uint32_t reserved;
} GPIBOnlineIn;

typedef struct GPIBWaitIn {
    int32_t  handle;
    int32_t  mask;
    uint32_t timeout_us;
    uint32_t reserved;
} GPIBWaitIn;

typedef struct GPIBAskIn {
    int32_t  handle;
    int32_t  option;
} GPIBAskIn;

typedef struct GPIBAskOut {
    int32_t  value;
    uint32_t ibsta;
    uint32_t iberr;
    uint32_t reserved;
} GPIBAskOut;

typedef struct GPIBAddressIn {
    int32_t  handle;
    uint32_t pad;
    int32_t  sad;
    uint32_t reserved;
} GPIBAddressIn;

typedef struct GPIBSerialPollOut {
    uint8_t  status_byte;
    uint8_t  reserved0;
    uint16_t reserved1;
    uint32_t ibsta;
    uint32_t iberr;
    uint32_t reserved2;
} GPIBSerialPollOut;

typedef struct GPIBListenerOut {
    uint32_t present;
    uint32_t ibsta;
    uint32_t iberr;
    uint32_t reserved;
} GPIBListenerOut;

typedef struct GPIBLineStatusOut {
    uint32_t line_status;   // bus_control_line bitmask (ValidXXX | BusXXX)
    uint32_t ibsta;
    uint32_t iberr;
    uint32_t reserved;
} GPIBLineStatusOut;

typedef struct GPIBSendCommandIn {
    int32_t  handle;
    uint32_t length;
    uint8_t  data[0];   // trailing bytes hold the command payload
} GPIBSendCommandIn;

#pragma pack(pop)

/* Inline payload cap for short Write requests. Above this size, the
 * client must use IOConnectCallStructMethod with an out-of-line memory
 * descriptor. M1 only supports inline. */
#define kGPIBMaxInlineWrite  (4096 - sizeof(GPIBWriteIn))
#define kGPIBMaxInlineRead   (4096 - sizeof(GPIBReadOut))

#ifdef __cplusplus
}
#endif

#endif /* GPIBSelectors_h */
