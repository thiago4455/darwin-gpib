//
//  libgpib_internal.h
//  darwin-gpib libgpib
//
//  Shared internals for the linux-gpib-compatible userland shim. Each
//  translation unit implementing a public ibXXX or 488.2 helper pulls this
//  header for the IOKit connection accessors, the thread-local status
//  vector setters, and the shared descriptor bookkeeping.
//

#ifndef libgpib_internal_h
#define libgpib_internal_h

#include <stddef.h>
#include <stdint.h>
#include <IOKit/IOKitLib.h>

#include "GPIBSelectors.h"

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Per-board state. The user client itself lives in gpibd, not here — this
// process only caches whether a board is reachable. Most programs touch a
// single board (index 0), so the table stays tiny and resolves on demand.
// -----------------------------------------------------------------------------

typedef struct gpib_board_slot {
    int board;              // index, so a slot pointer identifies its board
    int reachable;          // broker has confirmed an adapter here
    int online;             // last known online state
} gpib_board_slot;

/// Opaque board handle threaded through the ib* layer. Not a mach port any
/// more — it just identifies which board a call is about.
typedef gpib_board_slot *gpib_conn_t;
#define GPIB_CONN_NULL ((gpib_conn_t)0)

// Resolve a board, asking the broker on first use. Returns GPIB_CONN_NULL and
// sets iberr = ENEB when no adapter is present at that index.
gpib_conn_t gpib_lib_conn_for_board(int board_index);

// Reset the process-wide connection cache (used by ibonl(ud,0) closing
// the last handle to a board).
void gpib_lib_close_board(int board_index);

// -----------------------------------------------------------------------------
// Descriptor bookkeeping. Each ud returned by ibdev()/ibfind() is packed
// with the board_index it belongs to and the dext-side handle for the
// underlying GPIBDescriptor.
// -----------------------------------------------------------------------------

// linux-gpib's descriptor convention, which callers depend on: a board's ud
// *is* its index, so ud 0 means gpib0. pyvisa-py relies on this directly — it
// calls ibask(board, …) with a bare board number and never goes through
// ibfind. Device descriptors therefore have to live somewhere that cannot
// collide with 0..15, hence the tag bit.
#define GPIB_LIB_MAX_BOARDS 16
#define GPIB_LIB_DEVICE_TAG (1 << 23)

static inline int gpib_lib_is_board_ud(int ud) {
    return ud >= 0 && ud < GPIB_LIB_MAX_BOARDS;
}

static inline int gpib_lib_board_of(int ud) {
    if (gpib_lib_is_board_ud(ud)) return ud;
    return (ud >> 24) & 0x7f;
}

// Declared fully below; needed here because a board ud carries no handle of
// its own and has to look one up.
int32_t gpib_lib_board_handle(int board_index);

static inline int32_t gpib_lib_handle_of(int ud) {
    if (gpib_lib_is_board_ud(ud)) return gpib_lib_board_handle(ud);
    return (int32_t)(ud & 0x007fffff);
}

// Device ud layout:
//   bits 30..24 : board_index (0..127)
//   bit  23     : device tag, keeping devices clear of board uds 0..15
//   bits 22..0  : dext handle
static inline int gpib_lib_make_ud(int board_index, int32_t dext_handle) {
    return ((board_index & 0x7f) << 24) | GPIB_LIB_DEVICE_TAG
           | (dext_handle & 0x007fffff);
}

// Board descriptors are opened implicitly when a program first accesses
// board `n` (typically via ibfind("gpib0") or a bare ibonl(0,1)). We
// remember them so ibfind by board name returns the same ud twice in a
// row, matching linux-gpib.
int32_t gpib_lib_board_handle(int board_index);
void    gpib_lib_set_board_handle(int board_index, int32_t handle);

// -----------------------------------------------------------------------------
// Thread-local status vector. The public API exposes globals ibsta/iberr/
// ibcnt/ibcntl through <ib.h>, backed here by __thread storage. Each
// ibXXX/488.2 wrapper updates them via gpib_lib_set_status() before
// returning.
// -----------------------------------------------------------------------------

void gpib_lib_set_status(int ibsta, int iberr, long ibcntl);

// Convenience: return CMPL-only, or ERR|... with the given iberr set.
int  gpib_lib_return_ok(int ibsta, long count);
int  gpib_lib_return_err(int iberr);

// Public linkage — matches ib.h. Defined in libgpib_error.c.
extern volatile int  ibsta;
extern volatile int  ibcnt;
extern volatile int  iberr;
extern volatile long ibcntl;

// -----------------------------------------------------------------------------
// Low-level selector calls. Return kIOReturnSuccess on success or a
// kIOReturnXxx error; the wrappers translate to iberr = EDVR on failure.
// -----------------------------------------------------------------------------

kern_return_t gpib_lib_call_struct(gpib_conn_t conn, uint32_t selector,
                                   const void *inStruct, size_t inSize,
                                   void *outStruct, size_t *outSize);

// Variable-output-size structs (Read, Ask with dyn payload). Caller
// provides a buffer + capacity; `outSize` is updated with the actual size.
kern_return_t gpib_lib_call_struct_var(gpib_conn_t conn, uint32_t selector,
                                       const void *inStruct, size_t inSize,
                                       void *outStruct, size_t *outSize);

#ifdef __cplusplus
}
#endif

#endif /* libgpib_internal_h */
