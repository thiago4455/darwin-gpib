//
//  gpibd.c
//  darwin-gpib — GPIB broker agent
//
//  The only process that holds com.apple.developer.driverkit.userclient-access
//  and therefore the only one that can open the driver's user client. Every
//  other process reaches the bus by sending this agent an XPC message.
//
//  Launched on demand by launchd via its MachServices registration; it may be
//  killed when idle and will be brought back on the next message.
//
//  Each client connection owns the descriptors it opens. When a client goes
//  away — cleanly or by being killed mid-transfer — its descriptors are closed
//  on its behalf, so a crashed script cannot strand the bus for everyone else.
//  That is the guarantee a Linux process gets from the kernel closing its file
//  descriptors, reconstructed a layer up.
//
//  IOKit access is serialised per board: work on different adapters proceeds
//  in parallel, work on the same adapter queues. A blocking three-second read
//  holds its own board and nothing else.
//

#include <dispatch/dispatch.h>
#include <xpc/xpc.h>
#include <os/log.h>
#include <string.h>
#include <stdlib.h>

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

#include <pthread.h>

#include "gpibd_protocol.h"
#include "GPIBSelectors.h"

// Keep in sync with driver/Info.plist. A driver missing here is invisible to
// every client, which is the same failure mode libgpib's client.c has.
static const char *kUserClasses[] = { "kusb_488b", "ni_usb", "agilent_82357" };
static const size_t kUserClassCount = sizeof(kUserClasses) / sizeof(kUserClasses[0]);

#define GPIBD_MAX_BOARDS 16
#define GPIBD_MAX_OUT    (1u << 20)   // 1 MiB ceiling on a single reply

static io_connect_t g_conn[GPIBD_MAX_BOARDS];
/// One serial queue per board: different adapters run concurrently, the same
/// adapter serialises. Created lazily so we do not spawn 16 idle queues.
static dispatch_queue_t g_board_queue[GPIBD_MAX_BOARDS];
static dispatch_queue_t g_setup_queue;   // guards lazy queue creation
static os_log_t g_log;

// ---------------------------------------------------------------------------
// Sessions — what each client has open
// ---------------------------------------------------------------------------

typedef struct gpibd_desc {
    int                board;
    int32_t            handle;
    struct gpibd_desc *next;
} gpibd_desc;

typedef struct {
    pid_t       pid;
    gpibd_desc *descs;
} gpibd_session;

/// Sessions are touched from every board queue and from connection teardown,
/// so the list is mutex-guarded rather than confined to one queue.
static pthread_mutex_t g_session_lock = PTHREAD_MUTEX_INITIALIZER;

static void session_add_desc(gpibd_session *s, int board, int32_t handle) {
    if (!s || handle < 0) return;
    gpibd_desc *d = calloc(1, sizeof(*d));
    if (!d) return;
    d->board = board;
    d->handle = handle;
    pthread_mutex_lock(&g_session_lock);
    d->next = s->descs;
    s->descs = d;
    pthread_mutex_unlock(&g_session_lock);
}

static void session_forget_desc(gpibd_session *s, int board, int32_t handle) {
    if (!s) return;
    pthread_mutex_lock(&g_session_lock);
    for (gpibd_desc **link = &s->descs; *link; link = &(*link)->next) {
        if ((*link)->board == board && (*link)->handle == handle) {
            gpibd_desc *dead = *link;
            *link = dead->next;
            free(dead);
            break;
        }
    }
    pthread_mutex_unlock(&g_session_lock);
}

static dispatch_queue_t queue_for_board(int board);
static int64_t connection_for_board(int board, io_connect_t *out);

/// Closes everything a departed client left open. Runs on each affected
/// board's queue so it cannot race an in-flight request.
static void session_reap(gpibd_session *s) {
    if (!s) return;

    pthread_mutex_lock(&g_session_lock);
    gpibd_desc *list = s->descs;
    s->descs = NULL;
    pthread_mutex_unlock(&g_session_lock);

    int reaped = 0;
    while (list) {
        gpibd_desc *d = list;
        list = list->next;
        dispatch_sync(queue_for_board(d->board), ^{
            io_connect_t conn = IO_OBJECT_NULL;
            if (connection_for_board(d->board, &conn) == 0) {
                GPIBHandleIn in = { d->handle, 0 };
                size_t outSize = 0;
                IOConnectCallStructMethod((mach_port_t)conn, kGPIBSel_CloseDescriptor,
                                          &in, sizeof(in), NULL, &outSize);
            }
        });
        ++reaped;
        free(d);
    }
    if (reaped) {
        os_log(g_log, "reaped %d descriptor(s) from pid %d", reaped, s->pid);
    }
    free(s);
}

// ---------------------------------------------------------------------------
// Driver lookup
// ---------------------------------------------------------------------------

/// Returns the Nth service published by one of our driver personalities, in
/// registry order — the same ordering libgpib used, so board indices agree.
static io_service_t copy_service_for_board(int board) {
    io_iterator_t iter = IO_OBJECT_NULL;
    CFMutableDictionaryRef matching = IOServiceMatching("IOUserService");
    if (!matching) return IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter) != KERN_SUCCESS)
        return IO_OBJECT_NULL;

    int index = -1;
    io_service_t result = IO_OBJECT_NULL, service;
    while ((service = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        CFStringRef userClass = IORegistryEntryCreateCFProperty(
            service, CFSTR("IOUserClass"), kCFAllocatorDefault, 0);
        int isOurs = 0;
        if (userClass) {
            char name[128] = {0};
            if (CFStringGetCString(userClass, name, sizeof(name), kCFStringEncodingUTF8)) {
                for (size_t i = 0; i < kUserClassCount; ++i) {
                    if (strcmp(name, kUserClasses[i]) == 0) { isOurs = 1; break; }
                }
            }
            CFRelease(userClass);
        }
        if (isOurs && ++index == board) { result = service; break; }
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
    return result;
}

/// Human-readable adapter model for a driver's IOUserClass. Keep in sync with
/// kUserClasses.
static const char *model_for_user_class(const char *userClass) {
    if (strcmp(userClass, "kusb_488b") == 0)     return "Keithley KUSB-488B";
    if (strcmp(userClass, "ni_usb") == 0)        return "NI GPIB-USB-HS";
    if (strcmp(userClass, "agilent_82357") == 0) return "Agilent 82357";
    return userClass;
}

/// Fill `list` with every attached board, in the same registry order
/// copy_service_for_board() uses, so indices agree with every other call.
static void enumerate_boards(GPIBDBoardList *list) {
    memset(list, 0, sizeof(*list));

    io_iterator_t iter = IO_OBJECT_NULL;
    CFMutableDictionaryRef matching = IOServiceMatching("IOUserService");
    if (!matching) return;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter) != KERN_SUCCESS)
        return;

    io_service_t service;
    while ((service = IOIteratorNext(iter)) != IO_OBJECT_NULL &&
           list->count < GPIBD_MAX_BOARDS) {
        CFStringRef userClass = IORegistryEntryCreateCFProperty(
            service, CFSTR("IOUserClass"), kCFAllocatorDefault, 0);
        if (userClass) {
            char name[128] = {0};
            if (CFStringGetCString(userClass, name, sizeof(name), kCFStringEncodingUTF8)) {
                for (size_t i = 0; i < kUserClassCount; ++i) {
                    if (strcmp(name, kUserClasses[i]) != 0) continue;
                    GPIBDBoardInfo *info = &list->boards[list->count];
                    info->index = (uint32_t)list->count;
                    snprintf(info->model, sizeof(info->model), "%s",
                             model_for_user_class(name));
                    list->count++;
                    break;
                }
            }
            CFRelease(userClass);
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
}

/// Opens (and caches) the user client for a board. Cached connections are
/// dropped on failure so a replugged adapter recovers without a restart.
static int64_t connection_for_board(int board, io_connect_t *out) {
    if (board < 0 || board >= GPIBD_MAX_BOARDS) return GPIBD_ERR_BAD_REQUEST;

    if (g_conn[board] != IO_OBJECT_NULL) {
        *out = g_conn[board];
        return 0;
    }

    io_service_t service = copy_service_for_board(board);
    if (service == IO_OBJECT_NULL) {
        os_log(g_log, "no adapter at board %d", board);
        return GPIBD_ERR_NO_BOARD;
    }

    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS || conn == IO_OBJECT_NULL) {
        os_log_error(g_log, "IOServiceOpen(board %d) failed 0x%x", board, kr);
        return GPIBD_ERR_OPEN_FAILED;
    }

    os_log(g_log, "opened user client for board %d", board);
    g_conn[board] = conn;
    *out = conn;
    return 0;
}

static dispatch_queue_t queue_for_board(int board) {
    if (board < 0 || board >= GPIBD_MAX_BOARDS) board = 0;
    __block dispatch_queue_t q;
    dispatch_sync(g_setup_queue, ^{
        if (!g_board_queue[board]) {
            char label[64];
            snprintf(label, sizeof(label),
                     "app.saturno.darwin-gpib.gpibd.board%d", board);
            g_board_queue[board] = dispatch_queue_create(label, DISPATCH_QUEUE_SERIAL);
        }
        q = g_board_queue[board];
    });
    return q;
}

/// True when the failure means our port is gone rather than the operation
/// failing. Mach send errors occupy 0x1000000x; IOKit reports device removal
/// through its own codes.
static int connection_is_dead(kern_return_t kr) {
    if (kr == kIOReturnNoDevice || kr == kIOReturnNotAttached) return 1;
    if (kr == MACH_SEND_INVALID_DEST) return 1;
    if (kr == MACH_SEND_INVALID_REPLY) return 1;
    return 0;
}

static void drop_board(int board) {
    if (board >= 0 && board < GPIBD_MAX_BOARDS && g_conn[board] != IO_OBJECT_NULL) {
        IOServiceClose(g_conn[board]);
        g_conn[board] = IO_OBJECT_NULL;
    }
}

// ---------------------------------------------------------------------------
// Request handling
// ---------------------------------------------------------------------------

static void reply_error(xpc_object_t request, int64_t code, const char *msg) {
    xpc_object_t reply = xpc_dictionary_create_reply(request);
    if (!reply) return;
    xpc_dictionary_set_int64(reply, GPIBD_KEY_KR, code);
    if (msg) xpc_dictionary_set_string(reply, GPIBD_KEY_MSG, msg);
    xpc_connection_send_message(xpc_dictionary_get_remote_connection(request), reply);
    xpc_release(reply);
}

static void handle_request(xpc_object_t request, gpibd_session *session) {
    uint64_t op    = xpc_dictionary_get_uint64(request, GPIBD_KEY_OP);
    uint64_t board = xpc_dictionary_get_uint64(request, GPIBD_KEY_BOARD);
    uint64_t cap   = xpc_dictionary_get_uint64(request, GPIBD_KEY_OUTCAP);

    size_t inSize = 0;
    const void *inData = xpc_dictionary_get_data(request, GPIBD_KEY_IN, &inSize);

    if (cap > GPIBD_MAX_OUT) {
        reply_error(request, GPIBD_ERR_BAD_REQUEST, "output cap too large");
        return;
    }

    // Enumeration is answered before any board is resolved: "nothing attached"
    // is a valid answer, not an error, and opening a user client per board
    // would make listing fail whenever one adapter is busy or wedged.
    if (op == GPIBD_OP_LIST_BOARDS) {
        GPIBDBoardList list;
        enumerate_boards(&list);
        xpc_object_t reply = xpc_dictionary_create_reply(request);
        if (reply) {
            xpc_dictionary_set_int64(reply, GPIBD_KEY_KR, 0);
            xpc_dictionary_set_data(reply, GPIBD_KEY_OUT, &list, sizeof(list));
            xpc_connection_send_message(
                xpc_dictionary_get_remote_connection(request), reply);
            xpc_release(reply);
        }
        return;
    }

    io_connect_t conn = IO_OBJECT_NULL;
    int64_t rc = connection_for_board((int)board, &conn);
    if (rc != 0) {
        reply_error(request, rc, "no reachable adapter");
        return;
    }

    // Probe stops here: the board resolved and its user client is open, which
    // is all the caller wanted to know.
    if (op == GPIBD_OP_PROBE) {
        xpc_object_t reply = xpc_dictionary_create_reply(request);
        if (reply) {
            xpc_dictionary_set_int64(reply, GPIBD_KEY_KR, 0);
            xpc_connection_send_message(xpc_dictionary_get_remote_connection(request), reply);
            xpc_release(reply);
        }
        return;
    }

    void *outData = cap ? calloc(1, (size_t)cap) : NULL;
    if (cap && !outData) {
        reply_error(request, GPIBD_ERR_BAD_REQUEST, "out of memory");
        return;
    }

    size_t outSize = (size_t)cap;
    kern_return_t kr = IOConnectCallStructMethod((mach_port_t)conn, (uint32_t)op,
                                                 inData, inSize,
                                                 outData, &outSize);

    // A cached connection outlives the dext it points at: replug the adapter,
    // or let the driver restart, and the port is dead. IOKit reports that as a
    // Mach send failure (MACH_SEND_INVALID_DEST) rather than an IOKit error,
    // so checking only kIOReturnNoDevice leaves the broker wedged until it is
    // restarted. Drop the stale port and retry once — the client never notices
    // the adapter was reseated.
    if (connection_is_dead(kr)) {
        os_log(g_log, "board %llu connection is stale (0x%x); re-resolving",
               board, kr);
        drop_board((int)board);
        conn = IO_OBJECT_NULL;
        if (connection_for_board((int)board, &conn) == 0) {
            outSize = (size_t)cap;
            if (outData) memset(outData, 0, (size_t)cap);
            kr = IOConnectCallStructMethod((mach_port_t)conn, (uint32_t)op,
                                           inData, inSize, outData, &outSize);
        }
        if (connection_is_dead(kr)) drop_board((int)board);
    }

    // Track what this client owns, so we can clean up if it dies.
    if (kr == KERN_SUCCESS && outData) {
        if (op == kGPIBSel_OpenDescriptor && outSize >= sizeof(GPIBOpenDescriptorOut)) {
            const GPIBOpenDescriptorOut *od = (const GPIBOpenDescriptorOut *)outData;
            if (od->handle >= 0) session_add_desc(session, (int)board, od->handle);
        } else if (op == kGPIBSel_CloseDescriptor && inData &&
                   inSize >= sizeof(GPIBHandleIn)) {
            const GPIBHandleIn *hin = (const GPIBHandleIn *)inData;
            session_forget_desc(session, (int)board, hin->handle);
        }
    }

    os_log(g_log, "op=%llu board=%llu in=%zu outcap=%llu -> kr=0x%x out=%zu",
           op, board, inSize, cap, kr, outSize);

    xpc_object_t reply = xpc_dictionary_create_reply(request);
    if (reply) {
        xpc_dictionary_set_int64(reply, GPIBD_KEY_KR, (int64_t)kr);
        if (outData && outSize > 0) {
            xpc_dictionary_set_data(reply, GPIBD_KEY_OUT, outData, outSize);
        }
        xpc_connection_send_message(xpc_dictionary_get_remote_connection(request), reply);
        xpc_release(reply);
    }
    free(outData);
}

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

static void handle_peer_event(xpc_connection_t peer, xpc_object_t event) {
    xpc_type_t type = xpc_get_type(event);
    gpibd_session *session = (gpibd_session *)xpc_connection_get_context(peer);

    if (type == XPC_TYPE_ERROR) {
        // The client is gone — cleanly, or killed mid-transfer. Either way it
        // is not coming back to close what it opened, so we do it.
        if (event == XPC_ERROR_CONNECTION_INVALID) {
            os_log(g_log, "peer %d disconnected", xpc_connection_get_pid(peer));
            xpc_connection_set_context(peer, NULL);
            if (session) {
                dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                    session_reap(session);
                });
            }
        }
        return;
    }

    if (type != XPC_TYPE_DICTIONARY) return;

    // Onto the owning board's queue. Different adapters proceed in parallel;
    // the same adapter serialises, which the hardware requires anyway.
    uint64_t board = xpc_dictionary_get_uint64(event, GPIBD_KEY_BOARD);
    xpc_retain(event);
    dispatch_async(queue_for_board((int)board), ^{
        handle_request(event, session);
        xpc_release(event);
    });
}

static void handle_new_connection(xpc_connection_t peer) {
    pid_t pid = xpc_connection_get_pid(peer);
    os_log(g_log, "peer connected: pid %d uid %d", pid, xpc_connection_get_euid(peer));

    gpibd_session *session = calloc(1, sizeof(*session));
    if (session) {
        session->pid = pid;
        xpc_connection_set_context(peer, session);
    }

    xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
        handle_peer_event(peer, event);
    });
    xpc_connection_activate(peer);
}

int main(void) {
    g_log = os_log_create("app.saturno.darwin-gpib.gpibd", "broker");
    g_setup_queue = dispatch_queue_create("app.saturno.darwin-gpib.gpibd.setup",
                                           DISPATCH_QUEUE_SERIAL);
    for (int i = 0; i < GPIBD_MAX_BOARDS; ++i) g_conn[i] = IO_OBJECT_NULL;

    xpc_connection_t listener = xpc_connection_create_mach_service(
        GPIBD_SERVICE_NAME, NULL, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener) {
        os_log_error(g_log, "could not create listener for %s", GPIBD_SERVICE_NAME);
        return 1;
    }

    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) == XPC_TYPE_CONNECTION) {
            handle_new_connection((xpc_connection_t)peer);
        }
    });
    xpc_connection_activate(listener);

    os_log(g_log, "gpibd listening on %s", GPIBD_SERVICE_NAME);
    dispatch_main();
    return 0;
}
