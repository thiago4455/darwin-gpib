//
//  gpibd_protocol.h
//  darwin-gpib
//
//  Wire contract between libgpib (in any unentitled process) and the gpibd
//  broker agent.
//
//  The driver's user client already speaks a uniform struct-in / struct-out
//  protocol across all 18 selectors, so the XPC envelope only has to carry a
//  selector, a board index, and two opaque blobs.
//

#ifndef gpibd_protocol_h
#define gpibd_protocol_h

/// launchd-registered Mach service. Any process in the user session may look
/// this up; no entitlement is required on the client side. Must match the
/// MachServices key in the LaunchAgent plist.
#define GPIBD_SERVICE_NAME  "app.saturno.darwin-gpib.gpibd"

// ---------------------------------------------------------------------------
// Request keys
// ---------------------------------------------------------------------------

/// uint64 — a kGPIBSel_* value, or GPIBD_OP_PROBE.
#include <stdint.h>

#define GPIBD_KEY_OP        "op"

/// Pseudo-selector: resolve and open the board's user client without issuing
/// any driver call. Lets a client distinguish "no such board" (ENEB) from "the
/// operation failed" (EDVR) without burning a round trip on every ib* call,
/// preserving libgpib's original error semantics.
#define GPIBD_OP_PROBE      0xFFFFFFFFu

/// Pseudo-selector: enumerate attached boards. Answers "what is there?" without
/// the caller probing 0..15 blindly, each a full round trip against a broker
/// that may need launching. Deliberately does NOT open a user client for each
/// board -- enumeration must stay cheap and must not fail because one adapter
/// is busy or wedged. Reply carries GPIBD_KEY_BOARDS.
#define GPIBD_OP_LIST_BOARDS 0xFFFFFFFEu

/// The reply carries a GPIBDBoardList under the ordinary GPIBD_KEY_OUT, so
/// clients read it with the same path as any other call.
#define GPIBD_MODEL_MAX     64

typedef struct GPIBDBoardInfo {
    uint32_t index;                     ///< libgpib board index ("gpib0" == 0)
    char     model[GPIBD_MODEL_MAX];    ///< adapter model, NUL-terminated
} GPIBDBoardInfo;

typedef struct GPIBDBoardList {
    uint32_t       count;
    GPIBDBoardInfo boards[16];
} GPIBDBoardList;
/// uint64 — libgpib board index (0 == "gpib0").
#define GPIBD_KEY_BOARD     "board"
/// data — the selector's input struct, verbatim.
#define GPIBD_KEY_IN        "in"
/// uint64 — bytes of output the caller expects. Variable-length replies (reads)
/// use this as an upper bound.
#define GPIBD_KEY_OUTCAP    "outcap"

// ---------------------------------------------------------------------------
// Reply keys
// ---------------------------------------------------------------------------

/// int64 — kern_return_t from IOConnectCallStructMethod, or a GPIBD_ERR_* below.
#define GPIBD_KEY_KR        "kr"
/// data — the selector's output struct.
#define GPIBD_KEY_OUT       "out"
/// string — present only on failure, for logging. Never parsed by the client.
#define GPIBD_KEY_MSG       "msg"

// ---------------------------------------------------------------------------
// Broker-level failures
// ---------------------------------------------------------------------------
//
// These occupy a range that cannot collide with kern_return_t values, so a
// client can always tell "the driver said no" from "the broker could not get
// to the driver".

#define GPIBD_ERR_NO_BOARD    (-1000)  /// no adapter is attached at that index
#define GPIBD_ERR_OPEN_FAILED (-1001)  /// found the service but IOServiceOpen failed
#define GPIBD_ERR_BAD_REQUEST (-1002)  /// malformed message

#endif /* gpibd_protocol_h */
