//
//  gpib_probe.c — entitled CLI companion to tools/gpib_diag.py
//
//  Opening a DriverKit user client requires the *calling process* to carry
//  com.apple.developer.driverkit.userclient-access. A plain `python3` can
//  never carry it, so the ctypes path in gpib_diag.py cannot reach the
//  driver no matter how correct the driver is. This tiny host does carry it
//  (see tools/build_gpib_probe.sh) and performs the same probes.
//
//  Usage: gpib_probe lines | ifc | idn <pad>
//

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int (*p_ibfind)(const char *);
static int (*p_ibonl)(int, int);
static int (*p_ibsic)(int);
static int (*p_iblines)(int, short *);
static int (*p_ibdev)(int, int, int, int, int, int);
static int (*p_ibwrt)(int, const void *, long);
static int (*p_ibrd)(int, void *, long);
static const char *(*p_errstr)(int);
static int *p_ibsta, *p_iberr, *p_ibcnt;

static void post(const char *tag) {
    printf("  %s: ibsta=0x%04x iberr=%d (%s) ibcnt=%d\n",
           tag, *p_ibsta & 0xffff, *p_iberr,
           p_errstr ? p_errstr(*p_iberr) : "?", *p_ibcnt);
}

#define SYM(v, n) do { *(void **)(&v) = dlsym(h, n); \
    if (!v) { fprintf(stderr, "missing symbol %s\n", n); return 2; } } while (0)

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s lines|ifc|idn <pad>\n", argv[0]); return 1; }
    const char *libpath = getenv("LIBGPIB");
    if (!libpath) { fprintf(stderr, "set LIBGPIB to the dylib path\n"); return 1; }

    void *h = dlopen(libpath, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 2; }
    SYM(p_ibfind, "ibfind"); SYM(p_ibonl, "ibonl"); SYM(p_ibsic, "ibsic");
    SYM(p_iblines, "iblines"); SYM(p_ibdev, "ibdev");
    SYM(p_ibwrt, "ibwrt"); SYM(p_ibrd, "ibrd");
    p_errstr = dlsym(h, "gpib_error_string");
    p_ibsta = dlsym(h, "ibsta"); p_iberr = dlsym(h, "iberr"); p_ibcnt = dlsym(h, "ibcnt");
    if (!p_ibsta || !p_iberr || !p_ibcnt) { fprintf(stderr, "missing globals\n"); return 2; }

    int board = p_ibfind("gpib0");
    printf("ibfind(\"gpib0\") = %d\n", board);
    post("after ibfind");
    if (board < 0) return 3;

    if (!strcmp(argv[1], "lines")) {
        short lines = 0;
        p_iblines(board, &lines);
        unsigned v = (unsigned)lines & 0xffff;
        printf("line status = 0x%04x\n", v);
        static const struct { const char *n; unsigned m; } L[] = {
            {"DAV",0x0100},{"NDAC",0x0200},{"NRFD",0x0400},{"IFC",0x0800},
            {"REN",0x1000},{"SRQ",0x2000},{"ATN",0x4000},{"EOI",0x8000},
        };
        for (unsigned i = 0; i < sizeof(L)/sizeof(L[0]); ++i)
            printf("  %-4s = %s\n", L[i].n, (v & L[i].m) ? "asserted" : "not asserted");
        post("after iblines");
    } else if (!strcmp(argv[1], "ifc")) {
        p_ibsic(board);
        post("after ibsic");
    } else if (!strcmp(argv[1], "idn")) {
        if (argc < 3) { fprintf(stderr, "idn needs a PAD\n"); return 1; }
        int pad = atoi(argv[2]);
        int ud = p_ibdev(0, pad, 0, 13 /* T3s */, 1, 0);
        printf("ibdev(0,%d,0,T3s,1,0) = %d\n", pad, ud);
        post("after ibdev");
        if (ud < 0) return 3;
        const char *q = "*IDN?\n";
        p_ibwrt(ud, q, (long)strlen(q));
        post("after ibwrt");
        char buf[256]; memset(buf, 0, sizeof(buf));
        p_ibrd(ud, buf, (long)sizeof(buf) - 1);
        post("after ibrd");
        printf("reply: %s\n", buf);
        p_ibonl(ud, 0);
    } else {
        fprintf(stderr, "unknown command %s\n", argv[1]); return 1;
    }
    p_ibonl(board, 0);
    return 0;
}
