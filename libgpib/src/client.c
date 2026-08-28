//
//  client.c
//  darwin-gpib libgpib
//
//  Talks to the gpibd broker over XPC.
//
//  This library is loaded into arbitrary, unentitled processes — python,
//  LabVIEW, whatever the user links it into — and DriverKit will not let such
//  a process open the driver's user client. So we do not open it: gpibd holds
//  that connection and we send it messages.
//
//  One XPC connection serves the whole process; the board index rides in each
//  message. Board reachability is cached and dropped the moment the broker
//  reports the adapter gone, so a replug recovers without a restart.
//

#include "libgpib_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <IOKit/IOReturn.h>   // kIOReturn* codes still form our return contract
#include <xpc/xpc.h>
#include <dispatch/dispatch.h>

#include "gpib_user.h"   // for iberr code enum
#include "gpibd_protocol.h"
#include "GPIBSelectors.h"


static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static gpib_board_slot g_slots[GPIB_LIB_MAX_BOARDS];
static int32_t         g_board_handles[GPIB_LIB_MAX_BOARDS];   // dext handles for open board descriptors

static xpc_connection_t g_broker = NULL;

// Lazily bring up the process-wide connection to gpibd. launchd starts the
// agent on demand, so there is nothing to wait for here.
static xpc_connection_t broker(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        xpc_connection_t c = xpc_connection_create_mach_service(
            GPIBD_SERVICE_NAME, NULL, 0);
        if (c) {
            // Failures surface on each synchronous reply; this handler exists
            // only because XPC requires one before activation. A broker crash
            // shows up as an error reply and launchd restarts it on the next
            // message, so there is no reconnect logic to write.
            xpc_connection_set_event_handler(c, ^(xpc_object_t event) { (void)event; });
            xpc_connection_activate(c);
        }
        g_broker = c;
    });
    return g_broker;
}

/// One round trip. Returns the broker's kr, or GPIBD_ERR_BAD_REQUEST if the
/// message could not be delivered at all.
static int64_t broker_call(int board, uint32_t selector,
                           const void *in, size_t inSize,
                           void *out, size_t *outSize) {
    xpc_connection_t c = broker();
    if (!c) return GPIBD_ERR_BAD_REQUEST;

    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(msg, GPIBD_KEY_OP, selector);
    xpc_dictionary_set_uint64(msg, GPIBD_KEY_BOARD, (uint64_t)board);
    if (in && inSize) xpc_dictionary_set_data(msg, GPIBD_KEY_IN, in, inSize);
    xpc_dictionary_set_uint64(msg, GPIBD_KEY_OUTCAP, outSize ? *outSize : 0);

    xpc_object_t reply = xpc_connection_send_message_with_reply_sync(c, msg);
    xpc_release(msg);

    if (xpc_get_type(reply) == XPC_TYPE_ERROR) {
        xpc_release(reply);
        if (outSize) *outSize = 0;
        return GPIBD_ERR_BAD_REQUEST;
    }

    int64_t kr = xpc_dictionary_get_int64(reply, GPIBD_KEY_KR);

    if (out && outSize && *outSize) {
        size_t got = 0;
        const void *data = xpc_dictionary_get_data(reply, GPIBD_KEY_OUT, &got);
        if (data) {
            if (got > *outSize) got = *outSize;
            memcpy(out, data, got);
            *outSize = got;
        } else {
            *outSize = 0;
        }
    }
    xpc_release(reply);
    return kr;
}

gpib_conn_t gpib_lib_conn_for_board(int board_index) {
    if (board_index < 0 || board_index >= GPIB_LIB_MAX_BOARDS) {
        gpib_lib_set_status(ERR, EDVR, 0);
        return GPIB_CONN_NULL;
    }

    pthread_mutex_lock(&g_lock);
    int known = g_slots[board_index].reachable;
    pthread_mutex_unlock(&g_lock);

    if (!known) {
        // Resolve once per board. Returning NULL here is what makes callers
        // report ENEB rather than EDVR — the same split the IOKit version had.
        if (broker_call(board_index, GPIBD_OP_PROBE, NULL, 0, NULL, NULL) != 0) {
            gpib_lib_set_status(ERR, ENEB, 0);
            return GPIB_CONN_NULL;
        }
        pthread_mutex_lock(&g_lock);
        g_slots[board_index].reachable = 1;
        pthread_mutex_unlock(&g_lock);
    }

    g_slots[board_index].board = board_index;
    return &g_slots[board_index];
}

void gpib_lib_close_board(int board_index) {
    if (board_index < 0 || board_index >= GPIB_LIB_MAX_BOARDS) return;
    pthread_mutex_lock(&g_lock);
    // The user client belongs to gpibd; dropping our cached reachability is
    // all this side owns. gpibd reaps descriptors when we disconnect.
    g_slots[board_index].reachable = 0;
    g_board_handles[board_index] = -1;
    pthread_mutex_unlock(&g_lock);
}

/// Brings a board online and opens its descriptor. Callers must have checked
/// that no handle is cached — this does not re-check.
static int32_t open_board(int board_index) {
    gpib_conn_t c = gpib_lib_conn_for_board(board_index);
    if (c == GPIB_CONN_NULL) return -1;

    GPIBOnlineIn on_in = { 1, 0 };
    GPIBStatusOut on_out = {0};
    size_t sz = sizeof(on_out);
    if (gpib_lib_call_struct(c, kGPIBSel_BoardOnline,
                              &on_in, sizeof(on_in), &on_out, &sz) != KERN_SUCCESS) {
        gpib_lib_set_status(ERR, EDVR, 0);
        return -1;
    }

    GPIBOpenDescriptorIn od_in = {
        .pad = 0, .sad = (uint32_t)-1, .is_board = 1,
        .timeout_code = T3s, .eos_char = 0, .eos_flags = 0, .eot = 1,
    };
    GPIBOpenDescriptorOut od_out = {0};
    sz = sizeof(od_out);
    if (gpib_lib_call_struct(c, kGPIBSel_OpenDescriptor,
                              &od_in, sizeof(od_in), &od_out, &sz) != KERN_SUCCESS) {
        gpib_lib_set_status(ERR, EDVR, 0);
        return -1;
    }
    if (od_out.handle < 0) {
        gpib_lib_set_status(od_out.ibsta, od_out.iberr, 0);
        return -1;
    }
    gpib_lib_set_board_handle(board_index, od_out.handle);
    return od_out.handle;
}

/// The board's dext-side descriptor, opened on first use.
///
/// Lazy rather than requiring ibfind, because linux-gpib callers are entitled
/// to use a bare board index as a descriptor without opening anything first —
/// pyvisa-py does exactly that in _find_boards(), calling ibask(0, IbaPAD).
int32_t gpib_lib_board_handle(int board_index) {
    if (board_index < 0 || board_index >= GPIB_LIB_MAX_BOARDS) return -1;
    pthread_mutex_lock(&g_lock);
    int32_t h = g_board_handles[board_index];
    pthread_mutex_unlock(&g_lock);
    if (h >= 0) return h;
    return open_board(board_index);
}

void gpib_lib_set_board_handle(int board_index, int32_t handle) {
    if (board_index < 0 || board_index >= GPIB_LIB_MAX_BOARDS) return;
    pthread_mutex_lock(&g_lock);
    g_board_handles[board_index] = handle;
    pthread_mutex_unlock(&g_lock);
}

__attribute__((constructor)) static void gpib_lib_init(void) {
    for (int i = 0; i < GPIB_LIB_MAX_BOARDS; ++i) {
        g_slots[i].board = i;
        g_slots[i].reachable = 0;
        g_slots[i].online = 0;
        g_board_handles[i] = -1;
    }
}

// -----------------------------------------------------------------------------
// The seam. Everything above libgpib is unchanged; only these two functions
// know that the driver is reached through a broker rather than directly.
// -----------------------------------------------------------------------------

kern_return_t gpib_lib_call_struct(gpib_conn_t conn, uint32_t selector,
                                   const void *inStruct, size_t inSize,
                                   void *outStruct, size_t *outSize) {
    if (!conn) return kIOReturnBadArgument;

    size_t sz = outSize ? *outSize : 0;
    int64_t kr = broker_call(conn->board, selector, inStruct, inSize,
                             outStruct, outSize ? &sz : NULL);
    if (outSize) *outSize = sz;

    // The adapter went away: forget it so the next call re-resolves instead of
    // failing forever against a stale cache.
    if (kr == GPIBD_ERR_NO_BOARD || kr == GPIBD_ERR_OPEN_FAILED) {
        pthread_mutex_lock(&g_lock);
        g_slots[conn->board].reachable = 0;
        pthread_mutex_unlock(&g_lock);
        return kIOReturnNoDevice;
    }
    if (kr != 0) return (kern_return_t)kr;
    return KERN_SUCCESS;
}

kern_return_t gpib_lib_call_struct_var(gpib_conn_t conn, uint32_t selector,
                                       const void *inStruct, size_t inSize,
                                       void *outStruct, size_t *outSize) {
    // Variable-size output rides the same path; kept separate to signal
    // intent at the call site.
    return gpib_lib_call_struct(conn, selector, inStruct, inSize,
                                 outStruct, outSize);
}
