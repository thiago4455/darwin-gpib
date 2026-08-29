//
//  gpibctl.c
//  darwin-gpib — command-line client for the gpibd broker
//
//  Deliberately carries NO entitlements. If this can reach the bus, so can
//  python3, LabVIEW, or anything else a user links libgpib into — which is
//  the entire premise of the broker design.
//
//  It also replaces the one thing XPC costs us over a Unix socket: being able
//  to poke the daemon by hand while debugging.
//
//  Usage: gpibctl lines [board]
//

#include <xpc/xpc.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpibd_protocol.h"
#include "GPIBSelectors.h"

static const char *broker_error(int64_t kr) {
    switch (kr) {
        case GPIBD_ERR_NO_BOARD:    return "no adapter attached at that board index";
        case GPIBD_ERR_OPEN_FAILED: return "adapter found, but its user client would not open";
        case GPIBD_ERR_BAD_REQUEST: return "malformed request";
        default:                    return NULL;
    }
}

static void print_lines(uint32_t status) {
    static const struct { const char *name; uint32_t valid, bus; } L[] = {
        { "DAV",  0x01, 0x0100 }, { "NDAC", 0x02, 0x0200 },
        { "NRFD", 0x04, 0x0400 }, { "IFC",  0x08, 0x0800 },
        { "REN",  0x10, 0x1000 }, { "SRQ",  0x20, 0x2000 },
        { "ATN",  0x40, 0x4000 }, { "EOI",  0x80, 0x8000 },
    };
    printf("line status = 0x%04x\n", status & 0xffff);
    for (size_t i = 0; i < sizeof(L) / sizeof(L[0]); ++i) {
        if (!(status & L[i].valid)) {
            printf("  %-4s  not reported\n", L[i].name);
        } else {
            printf("  %-4s  %s\n", L[i].name,
                   (status & L[i].bus) ? "asserted" : "not asserted");
        }
    }
}

// TEMPORARY: walks the register map so an inert FPGA core (everything reads
// 0x00) can be told apart from a wrong selector (some registers non-zero).
static int sweep_registers(xpc_connection_t conn, int board) {
    static const uint16_t regs[] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x100,0x101,0x102,0x103,0x104,0x105,0x106,0x107
    };
    int nonzero = 0;
    for (size_t i = 0; i < sizeof(regs)/sizeof(regs[0]); ++i) {
        GPIBHandleIn in = { .handle = (int32_t)(0x40000000 | regs[i]), .reserved = 0 };
        xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_uint64(msg, GPIBD_KEY_OP, kGPIBSel_LineStatus);
        xpc_dictionary_set_uint64(msg, GPIBD_KEY_BOARD, (uint64_t)board);
        xpc_dictionary_set_data(msg, GPIBD_KEY_IN, &in, sizeof(in));
        xpc_dictionary_set_uint64(msg, GPIBD_KEY_OUTCAP, sizeof(GPIBLineStatusOut));
        xpc_object_t reply = xpc_connection_send_message_with_reply_sync(conn, msg);
        xpc_release(msg);
        if (xpc_get_type(reply) == XPC_TYPE_ERROR) { xpc_release(reply); continue; }
        size_t outSize = 0;
        const void *out = xpc_dictionary_get_data(reply, GPIBD_KEY_OUT, &outSize);
        if (out && outSize >= sizeof(GPIBLineStatusOut)) {
            GPIBLineStatusOut st;
            memcpy(&st, out, sizeof(st));
            if (st.line_status & 0x8000) {
                unsigned v = st.line_status & 0xFF;
                printf("  reg 0x%03x = 0x%02x%s\n", regs[i], v, v ? "   <-- non-zero" : "");
                if (v) nonzero++;
            } else {
                printf("  reg 0x%03x = read failed (iberr %u)\n", regs[i], st.iberr);
            }
        }
        xpc_release(reply);
    }
    printf("\n%d of %zu registers non-zero\n", nonzero, sizeof(regs)/sizeof(regs[0]));
    printf(nonzero ? "core is responding — 0x107 is likely the wrong selector\n"
                   : "every register reads 0x00 — the FPGA core looks inert\n");
    return 0;
}

int main(int argc, char **argv) {
    const char *cmd = (argc > 1) ? argv[1] : "lines";
    int board = (argc > 2) ? atoi(argv[2]) : 0;

    int sweep = (strcmp(cmd, "regs") == 0);
    int poke  = (strcmp(cmd, "poke") == 0);
    int peek  = (strcmp(cmd, "peek") == 0);
    int reinit = (strcmp(cmd, "reinit") == 0);
    int chunk  = (strcmp(cmd, "chunk") == 0);
    int usbrst = (strcmp(cmd, "usbreset") == 0);
    if (strcmp(cmd, "lines") != 0 && !sweep && !poke && !peek && !reinit && !usbrst && !chunk) {
        fprintf(stderr,
                "usage: %s lines|regs|peek <reg>|poke <reg> <val>|reinit|usbreset|chunk <n> [board]\n"
                "  reinit    re-run the chip bring-up to clear a wedged core\n"
                "  usbreset  force USB re-enumeration (software replug)\n",
                argv[0]);
        return 2;
    }
    // peek reads ONE register. `regs` starts at DIR, and reading DIR pops the
    // data-in register and disturbs handshake state, so the sweep is not safe
    // to use mid-transfer.
    if (peek && argc < 3) { fprintf(stderr, "peek needs a register\n"); return 2; }
    if (poke && argc < 4) {
        fprintf(stderr, "usage: %s poke <reg> <val> [board]\n", argv[0]);
        return 2;
    }
    if (poke)  board = (argc > 4) ? atoi(argv[4]) : 0;
    if (peek)  board = (argc > 3) ? atoi(argv[3]) : 0;
    // `chunk <n>` takes its own argument, so the default board parse at the top
    // would otherwise read the chunk size as a board index.
    if (chunk) board = (argc > 3) ? atoi(argv[3]) : 0;

    xpc_connection_t conn = xpc_connection_create_mach_service(
        GPIBD_SERVICE_NAME, NULL, 0);
    if (!conn) {
        fprintf(stderr, "could not create connection to %s\n", GPIBD_SERVICE_NAME);
        return 1;
    }
    // Errors surface on the synchronous reply below; this handler only exists
    // because XPC requires one before activation.
    xpc_connection_set_event_handler(conn, ^(xpc_object_t event) { (void)event; });
    xpc_connection_activate(conn);

    GPIBHandleIn in = { .handle = -1, .reserved = 0 };   // -1 == the board itself
    if (sweep) return sweep_registers(conn, board);
    if (reinit || usbrst) {
        int32_t tagged = (int32_t)(reinit ? 0x10000000u : 0x18000000u);
        GPIBHandleIn pin = { .handle = tagged, .reserved = 0 };
        xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_uint64(m, GPIBD_KEY_OP, kGPIBSel_LineStatus);
        xpc_dictionary_set_uint64(m, GPIBD_KEY_BOARD, (uint64_t)board);
        xpc_dictionary_set_data(m, GPIBD_KEY_IN, &pin, sizeof(pin));
        xpc_dictionary_set_uint64(m, GPIBD_KEY_OUTCAP, sizeof(GPIBLineStatusOut));
        xpc_object_t r = xpc_connection_send_message_with_reply_sync(conn, m);
        xpc_release(m);
        size_t osz = 0;
        const void *o = xpc_dictionary_get_data(r, GPIBD_KEY_OUT, &osz);
        if (o && osz >= sizeof(GPIBLineStatusOut)) {
            GPIBLineStatusOut st; memcpy(&st, o, sizeof(st));
            printf("%s -> %s (iberr %u)\n", cmd,
                   (st.line_status & 0x8000) ? "ok" : "failed", st.iberr);
        } else if (usbrst) {
            // Expected: resetting the device tears down the user client that
            // was serving this request, so the reply often never arrives.
            printf("usbreset requested; device is re-enumerating\n");
        } else {
            printf("%s -> no reply\n", cmd);
        }
        xpc_release(r);
        return 0;
    }
    if (peek) {
        unsigned reg = (unsigned)strtoul(argv[2], NULL, 0);
        GPIBHandleIn pin = { .handle = (int32_t)(0x40000000u | (reg & 0xFFFF)), .reserved = 0 };
        xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_uint64(m, GPIBD_KEY_OP, kGPIBSel_LineStatus);
        xpc_dictionary_set_uint64(m, GPIBD_KEY_BOARD, (uint64_t)board);
        xpc_dictionary_set_data(m, GPIBD_KEY_IN, &pin, sizeof(pin));
        xpc_dictionary_set_uint64(m, GPIBD_KEY_OUTCAP, sizeof(GPIBLineStatusOut));
        xpc_object_t r = xpc_connection_send_message_with_reply_sync(conn, m);
        xpc_release(m);
        size_t osz = 0;
        const void *o = xpc_dictionary_get_data(r, GPIBD_KEY_OUT, &osz);
        if (o && osz >= sizeof(GPIBLineStatusOut)) {
            GPIBLineStatusOut st; memcpy(&st, o, sizeof(st));
            if (st.line_status & 0x8000) printf("reg 0x%03x = 0x%02x\n", reg, st.line_status & 0xFF);
            else printf("reg 0x%03x = read failed (iberr %u)\n", reg, st.iberr);
        }
        xpc_release(r);
        return 0;
    }
    if (chunk) {
        // Diagnostic: set the per-transfer data chunk size. 0 = do not chunk,
        // which is the configuration that fails above ~85 bytes.
        unsigned n = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 64;
        int32_t tagged = (int32_t)(0x50000000u | (n & 0xFFFFu));
        GPIBHandleIn pin = { .handle = tagged, .reserved = 0 };
        xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_uint64(m, GPIBD_KEY_OP, kGPIBSel_LineStatus);
        xpc_dictionary_set_uint64(m, GPIBD_KEY_BOARD, (uint64_t)board);
        xpc_dictionary_set_data(m, GPIBD_KEY_IN, &pin, sizeof(pin));
        xpc_dictionary_set_uint64(m, GPIBD_KEY_OUTCAP, sizeof(GPIBLineStatusOut));
        xpc_object_t r = xpc_connection_send_message_with_reply_sync(conn, m);
        xpc_release(m);
        size_t osz = 0;
        const void *o = xpc_dictionary_get_data(r, GPIBD_KEY_OUT, &osz);
        if (o && osz >= sizeof(GPIBLineStatusOut)) {
            const GPIBLineStatusOut *out = (const GPIBLineStatusOut *)o;
            printf("chunk -> %u (%s, iberr %u)\n", n,
                   out->line_status ? "ok" : "failed", out->iberr);
        } else {
            int64_t kr = xpc_dictionary_get_int64(r, GPIBD_KEY_KR);
            const char *msg = xpc_dictionary_get_string(r, GPIBD_KEY_MSG);
            printf("chunk -> no reply (kr=%lld osz=%zu msg=%s)\n",
                   (long long)kr, osz, msg ? msg : "-");
        }
        xpc_release(r);
        return 0;
    }
    if (poke) {
        unsigned reg = (unsigned)strtoul(argv[2], NULL, 0);
        unsigned val = (unsigned)strtoul(argv[3], NULL, 0);
        int32_t tagged = (int32_t)(0x20000000u | ((reg & 0xFFF) << 8) | (val & 0xFF));
        GPIBHandleIn pin = { .handle = tagged, .reserved = 0 };
        xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_uint64(m, GPIBD_KEY_OP, kGPIBSel_LineStatus);
        xpc_dictionary_set_uint64(m, GPIBD_KEY_BOARD, (uint64_t)board);
        xpc_dictionary_set_data(m, GPIBD_KEY_IN, &pin, sizeof(pin));
        xpc_dictionary_set_uint64(m, GPIBD_KEY_OUTCAP, sizeof(GPIBLineStatusOut));
        xpc_object_t r = xpc_connection_send_message_with_reply_sync(conn, m);
        xpc_release(m);
        size_t osz = 0;
        const void *o = xpc_dictionary_get_data(r, GPIBD_KEY_OUT, &osz);
        if (o && osz >= sizeof(GPIBLineStatusOut)) {
            GPIBLineStatusOut st; memcpy(&st, o, sizeof(st));
            printf("poke reg 0x%03x = 0x%02x -> %s (iberr %u)\n", reg, val,
                   (st.line_status & 0x8000) ? "ok" : "failed", st.iberr);
        }
        xpc_release(r);
        return 0;
    }

    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(msg, GPIBD_KEY_OP, kGPIBSel_LineStatus);
    xpc_dictionary_set_uint64(msg, GPIBD_KEY_BOARD, (uint64_t)board);
    xpc_dictionary_set_data(msg, GPIBD_KEY_IN, &in, sizeof(in));
    xpc_dictionary_set_uint64(msg, GPIBD_KEY_OUTCAP, sizeof(GPIBLineStatusOut));

    xpc_object_t reply = xpc_connection_send_message_with_reply_sync(conn, msg);
    xpc_release(msg);

    if (xpc_get_type(reply) == XPC_TYPE_ERROR) {
        const char *desc = xpc_dictionary_get_string(reply, XPC_ERROR_KEY_DESCRIPTION);
        fprintf(stderr, "xpc error: %s\n", desc ? desc : "unknown");
        fprintf(stderr, "is gpibd installed? try gpibd/install.sh\n");
        return 1;
    }

    int64_t kr = xpc_dictionary_get_int64(reply, GPIBD_KEY_KR);
    printf("broker replied: kr=0x%llx\n", (unsigned long long)kr);

    const char *why = broker_error(kr);
    if (why) {
        printf("  -> %s\n", why);
        // Reaching the broker at all is the thing phase 1 is testing, so this
        // is still a successful round trip.
        printf("\nXPC round trip OK — the unentitled client reached gpibd.\n");
        xpc_release(reply);
        return 0;
    }

    if (kr != 0) {
        const char *msgText = xpc_dictionary_get_string(reply, GPIBD_KEY_MSG);
        printf("  -> driver returned an error%s%s\n",
               msgText ? ": " : "", msgText ? msgText : "");
        xpc_release(reply);
        return 1;
    }

    size_t outSize = 0;
    const void *out = xpc_dictionary_get_data(reply, GPIBD_KEY_OUT, &outSize);
    if (!out || outSize < sizeof(GPIBLineStatusOut)) {
        fprintf(stderr, "short reply (%zu bytes)\n", outSize);
        xpc_release(reply);
        return 1;
    }

    GPIBLineStatusOut status;
    memcpy(&status, out, sizeof(status));
    print_lines(status.line_status);
    printf("ibsta=0x%04x iberr=%u\n", status.ibsta, status.iberr);

    xpc_release(reply);
    return 0;
}
