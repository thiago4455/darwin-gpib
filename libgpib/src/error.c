//
//  error.c
//  darwin-gpib libgpib
//
//  Thread-local ibsta/iberr/ibcnt state + gpib_error_string / ibvers.
//  The public globals from <gpib/ib.h> are defined here as ordinary
//  volatile ints (matching linux-gpib's ABI); reads outside a per-call
//  context are best-effort.
//

#include "libgpib_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpib_user.h"
#include "gpib_version.h"

// Public globals — writable by wrapper functions, read by callers.
volatile int  ibsta  = 0;
volatile int  ibcnt  = 0;
volatile int  iberr  = 0;
volatile long ibcntl = 0;

// Per-thread mirror for Thread*() accessors.
static pthread_key_t g_tls_key;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;

typedef struct thread_status {
    int  ibsta;
    int  iberr;
    long ibcntl;
} thread_status;

static void tls_dtor(void *p) { free(p); }
static void tls_init(void) { pthread_key_create(&g_tls_key, tls_dtor); }

static thread_status *tls_get(void) {
    pthread_once(&g_tls_once, tls_init);
    thread_status *ts = (thread_status *)pthread_getspecific(g_tls_key);
    if (!ts) {
        ts = (thread_status *)calloc(1, sizeof(*ts));
        pthread_setspecific(g_tls_key, ts);
    }
    return ts;
}

void gpib_lib_set_status(int sta, int err, long cnt) {
    ibsta = sta;
    iberr = err;
    ibcntl = cnt;
    ibcnt  = (int)cnt;
    thread_status *ts = tls_get();
    if (ts) { ts->ibsta = sta; ts->iberr = err; ts->ibcntl = cnt; }
}

int gpib_lib_return_ok(int sta, long cnt) {
    gpib_lib_set_status(sta, 0, cnt);
    return sta;
}

int gpib_lib_return_err(int err) {
    gpib_lib_set_status(ERR, err, 0);
    return ERR;
}

int ThreadIbsta(void)  { thread_status *ts = tls_get(); return ts ? ts->ibsta  : 0; }
int ThreadIberr(void)  { thread_status *ts = tls_get(); return ts ? ts->iberr  : 0; }
int ThreadIbcnt(void)  { thread_status *ts = tls_get(); return ts ? (int)ts->ibcntl : 0; }
long ThreadIbcntl(void){ thread_status *ts = tls_get(); return ts ? ts->ibcntl : 0; }

// Async accessors — we don't support async I/O yet, mirror the sync ones.
int  AsyncIbsta(void)  { return ThreadIbsta(); }
int  AsyncIberr(void)  { return ThreadIberr(); }
int  AsyncIbcnt(void)  { return ThreadIbcnt(); }
long AsyncIbcntl(void) { return ThreadIbcntl(); }

const char *gpib_error_string(int err) {
    switch (err) {
        case EDVR: return "EDVR: system error";
        case ECIC: return "ECIC: not CIC";
        case ENOL: return "ENOL: no listeners on the bus";
        case EADR: return "EADR: CIC but not addressed before I/O";
        case EARG: return "EARG: bad argument";
        case ESAC: return "ESAC: not system controller";
        case EABO: return "EABO: I/O aborted (timeout)";
        case ENEB: return "ENEB: interface board offline";
        case EDMA: return "EDMA: DMA hardware error";
        case EOIP: return "EOIP: new I/O attempted with old I/O in progress";
        case ECAP: return "ECAP: no capability for intended operation";
        case EFSO: return "EFSO: file system operation error";
        case EBUS: return "EBUS: bus error";
        case ESTB: return "ESTB: lost serial poll bytes";
        case ESRQ: return "ESRQ: SRQ stuck on";
        case ETAB: return "ETAB: table overflow";
        default:   return "unknown iberr";
    }
}

void ibvers(char **out) {
    static char buf[64];
    if (!buf[0]) {
        snprintf(buf, sizeof(buf), "%d.%d.%d (darwin-gpib)",
                 GPIB_MAJOR_VERSION, GPIB_MINOR_VERSION, GPIB_MICRO_VERSION);
    }
    if (out) *out = buf;
}
