//
//  ib_traditional.c
//  darwin-gpib libgpib
//
//  Implementation of the "ibXXX" traditional 488.1 API from <gpib/ib.h>.
//  Each function packs its arguments into the corresponding GPIB*In struct
//  from GPIBSelectors.h, issues an IOConnectCallStructMethod against the
//  matching selector, unpacks the ibsta/iberr, and updates the process-
//  and thread-local status globals.
//

#include "libgpib_internal.h"

#include <string.h>
#include <stdlib.h>

#include "gpib_user.h"
#include "ib.h"

// -----------------------------------------------------------------------------
// Local helpers
// -----------------------------------------------------------------------------

// Ensure `board_index` has an active board descriptor open on the dext.
// Called from ibdev / ibfind before allocating a device descriptor.
static int ensure_board_open(int board_index) {
    // Opening is handled lazily by gpib_lib_board_handle(); this just reports
    // whether the board could be brought up.
    return gpib_lib_board_handle(board_index) >= 0 ? 0 : -1;
}

// Common wrapper for GPIBHandleIn selectors returning GPIBStatusOut.
static int simple_handle_call(int ud, uint32_t selector) {
    gpib_conn_t c = gpib_lib_conn_for_board(gpib_lib_board_of(ud));
    if (c == GPIB_CONN_NULL) return ERR;
    GPIBHandleIn in = { gpib_lib_handle_of(ud), 0 };
    GPIBStatusOut out = {0};
    size_t sz = sizeof(out);
    if (gpib_lib_call_struct(c, selector, &in, sizeof(in), &out, &sz)
        != KERN_SUCCESS) return gpib_lib_return_err(EDVR);
    return gpib_lib_return_ok(out.ibsta, out.ibcnt) | (out.iberr ? 0 : 0);
    // (status write already sets iberr)
}

// -----------------------------------------------------------------------------
// Descriptor lifetime
// -----------------------------------------------------------------------------

int ibdev(int board_index, int pad, int sad, int timo, int send_eoi, int eosmode) {
    if (ensure_board_open(board_index) != 0) return -1;
    gpib_conn_t c = gpib_lib_conn_for_board(board_index);
    if (c == GPIB_CONN_NULL) return -1;

    GPIBOpenDescriptorIn in = {
        .pad = (uint32_t)pad,
        .sad = (uint32_t)sad,      // -1 (NO_SAD) is passed through
        .is_board = 0,
        .timeout_code = (uint32_t)timo,
        .eos_char = (uint32_t)(eosmode & 0xff),
        .eos_flags = (uint32_t)(eosmode & (REOS | XEOS | BIN)),
        .eot = send_eoi ? 1u : 0u,
    };
    GPIBOpenDescriptorOut out = {0};
    size_t sz = sizeof(out);
    if (gpib_lib_call_struct(c, kGPIBSel_OpenDescriptor,
                              &in, sizeof(in), &out, &sz) != KERN_SUCCESS) {
        gpib_lib_set_status(ERR, EDVR, 0);
        return -1;
    }
    if (out.handle < 0) {
        gpib_lib_set_status(out.ibsta, out.iberr, 0);
        return -1;
    }
    gpib_lib_set_status(out.ibsta, 0, 0);
    return gpib_lib_make_ud(board_index, out.handle);
}

int ibfind(const char *name) {
    // Accept "gpib0".."gpibN" or bare "0". Anything else fails EDVR.
    if (!name || !*name) return -1;
    int board_index = 0;
    if (strncmp(name, "gpib", 4) == 0) {
        board_index = atoi(name + 4);
    } else {
        board_index = atoi(name);
    }
    if (ensure_board_open(board_index) != 0) return -1;
    gpib_lib_set_status(CMPL | CIC | REM, 0, 0);
    // A board's descriptor is its index, per linux-gpib.
    return board_index;
}

int ibonl(int ud, int onl) {
    gpib_conn_t c = gpib_lib_conn_for_board(gpib_lib_board_of(ud));
    if (c == GPIB_CONN_NULL) return ERR;

    if (!onl) {
        // Close the descriptor.
        GPIBHandleIn in = { gpib_lib_handle_of(ud), 0 };
        GPIBStatusOut out = {0};
        size_t sz = sizeof(out);
        gpib_lib_call_struct(c, kGPIBSel_CloseDescriptor,
                              &in, sizeof(in), &out, &sz);
        gpib_lib_set_status(out.ibsta, out.iberr, 0);
        return out.ibsta;
    }
    // ibonl(ud, 1) — bring the whole board online. Handled at ibdev/ibfind
    // time; nothing else to do.
    return gpib_lib_return_ok(CMPL, 0);
}

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

static int configure(int ud, uint32_t key, int value) {
    gpib_conn_t c = gpib_lib_conn_for_board(gpib_lib_board_of(ud));
    if (c == GPIB_CONN_NULL) return ERR;
    GPIBConfigureIn in = { gpib_lib_handle_of(ud), key, value, 0 };
    GPIBStatusOut out = {0};
    size_t sz = sizeof(out);
    if (gpib_lib_call_struct(c, kGPIBSel_Configure,
                              &in, sizeof(in), &out, &sz) != KERN_SUCCESS)
        return gpib_lib_return_err(EDVR);
    gpib_lib_set_status(out.ibsta, out.iberr, 0);
    return out.ibsta;
}

int ibpad(int ud, int v)  { return configure(ud, IbcPAD, v); }
int ibsad(int ud, int v)  { return configure(ud, IbcSAD, v); }
int ibtmo(int ud, int v)  { return configure(ud, IbcTMO, v); }
int ibeot(int ud, int v)  { return configure(ud, IbcEOT, v); }
int ibeos(int ud, int v)  { return configure(ud, IbcEOSchar, v & 0xff) |
                                    configure(ud, IbcEOSrd,   v & (REOS|XEOS|BIN)); }

int ibask(int ud, int option, int *value) {
    gpib_conn_t c = gpib_lib_conn_for_board(gpib_lib_board_of(ud));
    if (c == GPIB_CONN_NULL) return ERR;
    GPIBAskIn in = { gpib_lib_handle_of(ud), option };
    GPIBAskOut out = {0};
    size_t sz = sizeof(out);
    if (gpib_lib_call_struct(c, kGPIBSel_Ask,
                              &in, sizeof(in), &out, &sz) != KERN_SUCCESS)
        return gpib_lib_return_err(EDVR);
    if (value) *value = out.value;
    gpib_lib_set_status(out.ibsta, out.iberr, 0);
    return out.ibsta;
}

int ibconfig(int ud, int option, int value) {
    return configure(ud, (uint32_t)option, value);
}

// -----------------------------------------------------------------------------
// I/O
// -----------------------------------------------------------------------------

int ibwrt(int ud, const void *buf, long cnt) {
    gpib_conn_t c = gpib_lib_conn_for_board(gpib_lib_board_of(ud));
    if (c == GPIB_CONN_NULL) return ERR;
    if (cnt < 0 || (size_t)cnt > kGPIBMaxInlineWrite)
        return gpib_lib_return_err(EARG);

    size_t in_sz = sizeof(GPIBWriteIn) + (size_t)cnt;
    GPIBWriteIn *in = (GPIBWriteIn *)calloc(1, in_sz);
    if (!in) return gpib_lib_return_err(EDVR);
    in->handle   = gpib_lib_handle_of(ud);
    // Deliberately unconditional, and NOT a bug that blocks chunked writes.
    //
    // The board ANDs this with the descriptor's EOT setting
    // (GPIBBoard.cpp: `send_eoi && d->eot`, with IbcEOT setting d->eot), so the
    // effective EOI is exactly d->eot -- which is what linux-gpib does and what
    // ibeot() configures. A caller splitting a large message therefore works
    // today: ibeot(0) on the intermediate chunks, ibeot(1) on the last.
    //
    // libgpib does not keep a local copy of EOT (it lives in the descriptor,
    // driver-side), so passing anything but 1 here would mean duplicating
    // driver state in the client. Leaving the AND downstream is the cleaner
    // split; this comment exists because the hardcode reads like a bug.
    in->send_eoi = 1;
    in->length   = (uint32_t)cnt;
    memcpy(in->data, buf, (size_t)cnt);

    GPIBWriteOut out = {0};
    size_t sz = sizeof(out);
    kern_return_t kr = gpib_lib_call_struct(c, kGPIBSel_Write,
                                             in, in_sz, &out, &sz);
    free(in);
    if (kr != KERN_SUCCESS) return gpib_lib_return_err(EDVR);
    gpib_lib_set_status(out.ibsta, out.iberr, out.ibcnt);
    return out.ibsta;
}

int ibrd(int ud, void *buf, long cnt) {
    gpib_conn_t c = gpib_lib_conn_for_board(gpib_lib_board_of(ud));
    if (c == GPIB_CONN_NULL) return ERR;
    if (cnt < 0) return gpib_lib_return_err(EARG);
    // Clamp rather than reject. ibrd is allowed to return fewer bytes than
    // asked for -- callers loop until ibsta reports END -- and rejecting a
    // large request outright breaks every generic VISA layer: pyvisa-py asks
    // for its default 20480-byte chunk on every read and used to get EARG,
    // which surfaced as VI_ERROR_TMO and made the board unusable from pyvisa
    // regardless of which transport pipeline was active.
    if ((size_t)cnt > kGPIBMaxInlineRead) cnt = (long)kGPIBMaxInlineRead;

    GPIBReadIn in = { gpib_lib_handle_of(ud), (uint32_t)cnt };
    size_t out_sz = sizeof(GPIBReadOut) + (size_t)cnt;
    GPIBReadOut *out = (GPIBReadOut *)calloc(1, out_sz);
    if (!out) return gpib_lib_return_err(EDVR);
    size_t sz = out_sz;
    kern_return_t kr = gpib_lib_call_struct_var(c, kGPIBSel_Read,
                                                 &in, sizeof(in),
                                                 out, &sz);
    if (kr != KERN_SUCCESS) { free(out); return gpib_lib_return_err(EDVR); }
    uint32_t got = out->ibcnt;
    if (got > (uint32_t)cnt) got = (uint32_t)cnt;
    memcpy(buf, out->data, got);
    gpib_lib_set_status(out->ibsta, out->iberr, out->ibcnt);
    int ret = out->ibsta;
    free(out);
    return ret;
}

int ibcmd(int ud, const void *cmd, long cnt) {
    gpib_conn_t c = gpib_lib_conn_for_board(gpib_lib_board_of(ud));
    if (c == GPIB_CONN_NULL) return ERR;
    if (cnt < 0 || (size_t)cnt > kGPIBMaxInlineWrite)
        return gpib_lib_return_err(EARG);

    size_t in_sz = sizeof(GPIBSendCommandIn) + (size_t)cnt;
    GPIBSendCommandIn *in = (GPIBSendCommandIn *)calloc(1, in_sz);
    if (!in) return gpib_lib_return_err(EDVR);
    in->handle = gpib_lib_handle_of(ud);
    in->length = (uint32_t)cnt;
    memcpy(in->data, cmd, (size_t)cnt);

    GPIBWriteOut out = {0};
    size_t sz = sizeof(out);
    kern_return_t kr = gpib_lib_call_struct(c, kGPIBSel_SendCommand,
                                             in, in_sz, &out, &sz);
    free(in);
    if (kr != KERN_SUCCESS) return gpib_lib_return_err(EDVR);
    gpib_lib_set_status(out.ibsta, out.iberr, out.ibcnt);
    return out.ibsta;
}

// -----------------------------------------------------------------------------
// Simple GPIB bus operations
// -----------------------------------------------------------------------------

int ibclr(int ud) { return simple_handle_call(ud, kGPIBSel_DeviceClear); }
int ibsic(int ud) { return simple_handle_call(ud, kGPIBSel_InterfaceClear); }
int ibtrg(int ud) { return simple_handle_call(ud, kGPIBSel_Trigger); }
int ibloc(int ud) { return simple_handle_call(ud, kGPIBSel_GoToLocal); }

int ibsre(int ud, int v) {
    gpib_conn_t c = gpib_lib_conn_for_board(gpib_lib_board_of(ud));
    if (c == GPIB_CONN_NULL) return ERR;
    GPIBRemoteEnableIn in = { gpib_lib_handle_of(ud), v ? 1 : 0 };
    GPIBStatusOut out = {0};
    size_t sz = sizeof(out);
    if (gpib_lib_call_struct(c, kGPIBSel_RemoteEnable,
                              &in, sizeof(in), &out, &sz) != KERN_SUCCESS)
        return gpib_lib_return_err(EDVR);
    gpib_lib_set_status(out.ibsta, out.iberr, 0);
    return out.ibsta;
}

int ibwait(int ud, int mask) {
    gpib_conn_t c = gpib_lib_conn_for_board(gpib_lib_board_of(ud));
    if (c == GPIB_CONN_NULL) return ERR;
    // timeout 0 = "use the descriptor's", so ibtmo() actually governs ibwait.
    // This used to hardcode 3 s regardless of what the caller had configured.
    GPIBWaitIn in = { gpib_lib_handle_of(ud), mask, 0, 0 };
    GPIBStatusOut out = {0};
    size_t sz = sizeof(out);
    if (gpib_lib_call_struct(c, kGPIBSel_Wait,
                              &in, sizeof(in), &out, &sz) != KERN_SUCCESS)
        return gpib_lib_return_err(EDVR);
    gpib_lib_set_status(out.ibsta, out.iberr, 0);
    return out.ibsta;
}

int ibrsp(int ud, char *spr) {
    gpib_conn_t c = gpib_lib_conn_for_board(gpib_lib_board_of(ud));
    if (c == GPIB_CONN_NULL) return ERR;
    GPIBHandleIn in = { gpib_lib_handle_of(ud), 0 };
    GPIBSerialPollOut out = {0};
    size_t sz = sizeof(out);
    if (gpib_lib_call_struct(c, kGPIBSel_SerialPoll,
                              &in, sizeof(in), &out, &sz) != KERN_SUCCESS)
        return gpib_lib_return_err(EDVR);
    if (spr) *spr = (char)out.status_byte;
    gpib_lib_set_status(out.ibsta, out.iberr, 0);
    return out.ibsta;
}

int iblines(int ud, short *line_status) {
    gpib_conn_t c = gpib_lib_conn_for_board(gpib_lib_board_of(ud));
    if (c == GPIB_CONN_NULL) return ERR;
    GPIBHandleIn in = { gpib_lib_handle_of(ud), 0 };
    GPIBLineStatusOut out = {0};
    size_t sz = sizeof(out);
    if (gpib_lib_call_struct(c, kGPIBSel_LineStatus,
                              &in, sizeof(in), &out, &sz) != KERN_SUCCESS)
        return gpib_lib_return_err(EDVR);
    if (line_status) *line_status = (short)out.line_status;
    gpib_lib_set_status(out.ibsta, out.iberr, 0);
    return out.ibsta;
}

int ibln(int ud, int pad, int sad, short *found_listener) {
    gpib_conn_t c = gpib_lib_conn_for_board(gpib_lib_board_of(ud));
    if (c == GPIB_CONN_NULL) return ERR;
    GPIBAddressIn in = { gpib_lib_handle_of(ud), (uint32_t)pad, sad, 0 };
    GPIBListenerOut out = {0};
    size_t sz = sizeof(out);
    if (gpib_lib_call_struct(c, kGPIBSel_ListenerPresent,
                              &in, sizeof(in), &out, &sz) != KERN_SUCCESS)
        return gpib_lib_return_err(EDVR);
    if (found_listener) *found_listener = (short)out.present;
    gpib_lib_set_status(out.ibsta, out.iberr, 0);
    return out.ibsta;
}

// -----------------------------------------------------------------------------
// Unsupported operations — return ECAP so callers see a clean "no capability"
// -----------------------------------------------------------------------------

static int not_supported(void) { return gpib_lib_return_err(ECAP); }

int ibbna(int ud, char *board_name)                    { (void)ud;(void)board_name; return not_supported(); }
int ibcac(int ud, int synchronous)                     { (void)ud;(void)synchronous; return not_supported(); }
int ibcmda(int ud, const void *cmd, long cnt)          { (void)ud;(void)cmd;(void)cnt; return not_supported(); }
int ibdma(int ud, int v)                               { (void)ud;(void)v; return not_supported(); }
int ibevent(int ud, short *event)                      { (void)ud;(void)event; return not_supported(); }
int ibgts(int ud, int shadow_handshake)                { (void)ud;(void)shadow_handshake; return not_supported(); }
int ibist(int ud, int ist)                             { (void)ud;(void)ist; return not_supported(); }
int ibpct(int ud)                                      { (void)ud; return not_supported(); }
int ibppc(int ud, int v)                               { (void)ud;(void)v; return not_supported(); }
int ibrda(int ud, void *buf, long count)               { (void)ud;(void)buf;(void)count; return not_supported(); }
int ibrpp(int ud, char *ppr)                           { (void)ud;(void)ppr; return not_supported(); }
int ibrsc(int ud, int v)                               { (void)ud;(void)v; return not_supported(); }
int ibrsv(int ud, int status_byte)                     { (void)ud;(void)status_byte; return not_supported(); }
int ibrsv2(int ud, int sb, int r)                      { (void)ud;(void)sb;(void)r; return not_supported(); }
int ibspb(int ud, short *sp_bytes)                     { (void)ud;(void)sp_bytes; return not_supported(); }
int ibstop(int ud)                                     { (void)ud; return not_supported(); }
int ibwrta(int ud, const void *buf, long count)        { (void)ud;(void)buf;(void)count; return not_supported(); }

// -----------------------------------------------------------------------------
// File-based I/O — implemented on top of ibwrt / ibrd
// -----------------------------------------------------------------------------

#include <stdio.h>

int ibwrtf(int ud, const char *file_path) {
    if (!file_path) return gpib_lib_return_err(EARG);
    FILE *f = fopen(file_path, "rb");
    if (!f) return gpib_lib_return_err(EFSO);
    unsigned char buf[4096];
    size_t n;
    int last_sta = CMPL;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        last_sta = ibwrt(ud, buf, (long)n);
        if (last_sta & ERR) break;
    }
    fclose(f);
    return last_sta;
}

int ibrdf(int ud, const char *file_path) {
    if (!file_path) return gpib_lib_return_err(EARG);
    FILE *f = fopen(file_path, "wb");
    if (!f) return gpib_lib_return_err(EFSO);
    unsigned char buf[4096];
    int last_sta = CMPL;
    do {
        last_sta = ibrd(ud, buf, sizeof(buf));
        if (last_sta & ERR) break;
        if (ibcntl > 0) fwrite(buf, 1, (size_t)ibcntl, f);
    } while (!(last_sta & END));
    fclose(f);
    return last_sta;
}
