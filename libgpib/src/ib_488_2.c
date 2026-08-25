//
//  ib_488_2.c
//  darwin-gpib libgpib
//
//  Implementation of the 488.2-flavour helpers declared in <gpib/ib.h>
//  (Send, Receive, DevClear, EnableRemote, ReadStatusByte, TestSRQ, etc.).
//  All are built on top of the ibXXX traditional API — no direct dext
//  calls here — so behaviour matches whatever the underlying driver
//  supports.
//

#include "libgpib_internal.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "gpib_user.h"
#include "ib.h"

static int addr_valid(Addr4882_t a) { return a != NOADDR && a != 0xffff; }

// Open a scoped device descriptor addressed to `address`. Caller closes
// via ibonl(ud, 0).
static int open_device(int board_desc, Addr4882_t address) {
    unsigned pad = GetPAD(address);
    int sad = (int)GetSAD(address);
    if (sad == 0) sad = -1;
    return ibdev(gpib_lib_board_of(board_desc), (int)pad, sad, T3s, 1, 0);
}

// -----------------------------------------------------------------------------
// Simple bus operations.
// -----------------------------------------------------------------------------

void SendIFC(int board_desc) {
    ibsic(board_desc);
}

void SendLLO(int board_desc) {
    gpib_conn_t c = gpib_lib_conn_for_board(gpib_lib_board_of(board_desc));
    if (c == GPIB_CONN_NULL) return;
    GPIBHandleIn in = { gpib_lib_handle_of(board_desc), 0 };
    GPIBStatusOut out = {0};
    size_t sz = sizeof(out);
    gpib_lib_call_struct(c, kGPIBSel_LocalLockout, &in, sizeof(in), &out, &sz);
    gpib_lib_set_status(out.ibsta, out.iberr, 0);
}

void DevClear(int board_desc, Addr4882_t address) {
    int ud = open_device(board_desc, address);
    if (ud < 0) return;
    ibclr(ud);
    ibonl(ud, 0);
}

void DevClearList(int board_desc, const Addr4882_t list[]) {
    if (!list) return;
    for (int i = 0; addr_valid(list[i]); ++i) DevClear(board_desc, list[i]);
}

void EnableLocal(int board_desc, const Addr4882_t list[]) {
    if (!list) return;
    for (int i = 0; addr_valid(list[i]); ++i) {
        int ud = open_device(board_desc, list[i]);
        if (ud < 0) continue;
        ibloc(ud);
        ibonl(ud, 0);
    }
}

void EnableRemote(int board_desc, const Addr4882_t list[]) {
    // Asserting REN once brings the bus to REMS; addressing devices as
    // listeners then puts them in RWLS.
    ibsre(board_desc, 1);
    (void)list;
}

void Trigger(int board_desc, Addr4882_t address) {
    int ud = open_device(board_desc, address);
    if (ud < 0) return;
    ibtrg(ud);
    ibonl(ud, 0);
}

void TriggerList(int board_desc, const Addr4882_t list[]) {
    if (!list) return;
    for (int i = 0; addr_valid(list[i]); ++i) Trigger(board_desc, list[i]);
}

// -----------------------------------------------------------------------------
// Data transfer wrappers.
// -----------------------------------------------------------------------------

void Send(int board_desc, Addr4882_t address, const void *buffer,
          long count, int eot_mode) {
    int ud = open_device(board_desc, address);
    if (ud < 0) return;

    if (eot_mode == NLend) {
        unsigned char *tmp = (unsigned char *)malloc((size_t)count + 1);
        if (tmp) {
            memcpy(tmp, buffer, (size_t)count);
            tmp[count] = '\n';
            ibwrt(ud, tmp, count + 1);
            free(tmp);
        }
    } else {
        ibwrt(ud, buffer, count);
    }
    ibonl(ud, 0);
}

void SendList(int board_desc, const Addr4882_t list[], const void *buffer,
              long count, int eotmode) {
    if (!list) return;
    for (int i = 0; addr_valid(list[i]); ++i) {
        Send(board_desc, list[i], buffer, count, eotmode);
    }
}

void SendDataBytes(int board_desc, const void *buffer, long count, int eotmode) {
    (void)eotmode;
    ibwrt(board_desc, buffer, count);
}

void SendCmds(int board_desc, const void *cmds, long count) {
    ibcmd(board_desc, cmds, count);
}

void SendSetup(int board_desc, const Addr4882_t list[]) {
    if (!list) return;
    unsigned char cmd[32];
    int n = 0;
    cmd[n++] = UNL;
    for (int i = 0; addr_valid(list[i]) && n < 30; ++i) {
        unsigned pad = GetPAD(list[i]);
        int sad = (int)GetSAD(list[i]);
        cmd[n++] = (unsigned char)((pad & 0x1f) | LAD);
        if (sad != 0) cmd[n++] = (unsigned char)((sad & 0x1f) | SAD);
    }
    ibcmd(board_desc, cmd, n);
}

void Receive(int board_desc, Addr4882_t address, void *buffer, long count,
             int termination) {
    (void)termination;
    int ud = open_device(board_desc, address);
    if (ud < 0) return;
    ibrd(ud, buffer, count);
    ibonl(ud, 0);
}

void ReceiveSetup(int board_desc, Addr4882_t address) {
    unsigned pad = GetPAD(address);
    int sad = (int)GetSAD(address);
    unsigned char cmd[8];
    int n = 0;
    cmd[n++] = UNL;
    cmd[n++] = (unsigned char)((pad & 0x1f) | TAD);
    if (sad != 0) cmd[n++] = (unsigned char)((sad & 0x1f) | SAD);
    ibcmd(board_desc, cmd, n);
}

void RcvRespMsg(int board_desc, void *buffer, long count, int termination) {
    (void)termination;
    ibrd(board_desc, buffer, count);
}

// -----------------------------------------------------------------------------
// Status polling.
// -----------------------------------------------------------------------------

void ReadStatusByte(int board_desc, Addr4882_t address, short *result) {
    int ud = open_device(board_desc, address);
    if (ud < 0) { if (result) *result = 0; return; }
    char sb = 0;
    ibrsp(ud, &sb);
    if (result) *result = (short)(uint8_t)sb;
    ibonl(ud, 0);
}

void AllSPoll(int board_desc, const Addr4882_t list[], short results[]) {
    if (!list) return;
    for (int i = 0; addr_valid(list[i]); ++i) {
        ReadStatusByte(board_desc, list[i], results ? &results[i] : NULL);
    }
}

// Historical misspelling in linux-gpib; kept as an alias.
void AllSpoll(int board_desc, const Addr4882_t list[], short results[]) {
    AllSPoll(board_desc, list, results);
}

void TestSRQ(int board_desc, short *result) {
    short lines = 0;
    iblines(board_desc, &lines);
    if (result) *result = (lines & BusSRQ) ? 1 : 0;
}

void WaitSRQ(int board_desc, short *result) {
    // Poll iblines until BusSRQ shows up (~3s). Coarse; a real
    // implementation would use ibwait with SRQI once we support async
    // events on the dext side.
    for (int i = 0; i < 300; ++i) {
        short lines = 0;
        iblines(board_desc, &lines);
        if (lines & BusSRQ) { if (result) *result = 1; return; }
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (result) *result = 0;
}

void FindRQS(int board_desc, const Addr4882_t list[], short *result) {
    if (!list) { if (result) *result = -1; return; }
    for (int i = 0; addr_valid(list[i]); ++i) {
        short sb = 0;
        ReadStatusByte(board_desc, list[i], &sb);
        if (sb & 0x40) {
            if (result) *result = (short)i;
            return;
        }
    }
    if (result) *result = -1;
}

void FindLstn(int board_desc, const Addr4882_t padList[],
              Addr4882_t results[], int maxNumResults) {
    int n = 0;
    if (!padList) return;
    for (int i = 0; addr_valid(padList[i]) && n < maxNumResults; ++i) {
        short present = 0;
        ibln(board_desc, (int)GetPAD(padList[i]),
             (int)GetSAD(padList[i]), &present);
        if (present) results[n++] = padList[i];
    }
    if (n < maxNumResults) results[n] = NOADDR;
}

// -----------------------------------------------------------------------------
// Operations we don't yet support end-to-end.
// -----------------------------------------------------------------------------

void PassControl(int board_desc, Addr4882_t address) {
    (void)board_desc; (void)address;
    gpib_lib_set_status(ERR, ECAP, 0);
}

void PPoll(int board_desc, short *result) {
    (void)board_desc; if (result) *result = 0;
    gpib_lib_set_status(ERR, ECAP, 0);
}

void PPollConfig(int board_desc, Addr4882_t address, int dataLine, int lineSense) {
    (void)board_desc; (void)address; (void)dataLine; (void)lineSense;
    gpib_lib_set_status(ERR, ECAP, 0);
}

void PPollUnconfig(int board_desc, const Addr4882_t list[]) {
    (void)board_desc; (void)list;
    gpib_lib_set_status(ERR, ECAP, 0);
}

void ResetSys(int board_desc, const Addr4882_t list[]) {
    SendIFC(board_desc);
    ibsre(board_desc, 1);
    unsigned char dcl = DCL;
    ibcmd(board_desc, &dcl, 1);
    if (list) {
        for (int i = 0; addr_valid(list[i]); ++i) {
            static const char rst[] = "*RST";
            Send(board_desc, list[i], rst, (long)sizeof(rst) - 1, NLend);
        }
    }
}

void SetRWLS(int board_desc, const Addr4882_t list[]) {
    ibsre(board_desc, 1);
    SendLLO(board_desc);
    (void)list;
}

void TestSys(int board_desc, const Addr4882_t list[], short results[]) {
    (void)board_desc; (void)list;
    if (results) results[0] = 0;
}
