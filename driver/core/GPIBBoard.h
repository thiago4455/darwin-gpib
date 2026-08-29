//
//  GPIBBoard.h
//  darwin-gpib driver
//
//  Per-adapter GPIB state machine. Owns the descriptor table, the configured
//  primary/secondary addresses, EOS / EOT / timeout state, and the last
//  computed ibsta/iberr. Operates by issuing high-level commands to the
//  attached NIUSBTransport (the hardware backend).
//
//  Lives entirely inside the dext process. Allocated via IONewZero and
//  initialized with init(); released with free(). No constructors / no
//  virtual methods — DriverKit's helper-class memory model does not run
//  C++ constructors on raw allocations.
//

#ifndef GPIBBoard_h
#define GPIBBoard_h

#include <stdint.h>

#include "GPIBDescriptor.h"
#include "GPIBTransport.h"
#include "gpib_user.h"

class GPIBBoard {
public:
    // Two-phase init pattern (no constructor).
    bool init(IGPIBTransport *transport);
    void free();

    // Bring the adapter online (assert IFC briefly, raise REN, reset addressing
    // state). When online==false, releases the bus.
    uint32_t setOnline(bool online);

    // Allocate a descriptor for a device at (pad,sad). is_board==true selects
    // the board itself (board descriptor). Returns -1 on failure with ibsta=ERR.
    int32_t openDescriptor(uint8_t pad, int8_t sad, bool is_board,
                           uint32_t timeout_us, uint8_t eos_char,
                           uint8_t eos_flags, bool eot,
                           uint32_t *outIbsta, uint32_t *outIberr);
    uint32_t closeDescriptor(int32_t handle, uint32_t *outIberr);

    // Update a single descriptor option (PAD/SAD/TIMO/EOT/EOSchar/EOSflags).
    uint32_t configure(int32_t handle, uint32_t key, int32_t value,
                       uint32_t *outIberr);

    // Read back a configured option (Iba* enum from ib.h).
    uint32_t ask(int32_t handle, int32_t option, int32_t *outValue,
                 uint32_t *outIberr);

    // Send bytes to the addressed device. send_eoi controls EOI on the last
    // byte. On return, *outIbcnt holds bytes actually sent.
    uint32_t write(int32_t handle, const uint8_t *buf, uint32_t len,
                   bool send_eoi, uint32_t *outIbcnt, uint32_t *outIberr);

    // Receive bytes from the addressed device. Terminates on EOI/EOS or
    // when request_count bytes have been received. *outEnd is 1 iff END
    // condition was seen.
    uint32_t read(int32_t handle, uint8_t *buf, uint32_t request_count,
                  uint32_t *outIbcnt, uint8_t *outEnd, uint32_t *outIberr);

    // GPIB bus operations.
    uint32_t deviceClear(int32_t handle, uint32_t *outIberr);
    uint32_t interfaceClear(int32_t handle, uint32_t *outIberr);
    uint32_t remoteEnable(int32_t handle, bool enable, uint32_t *outIberr);
    uint32_t waitForStatus(int32_t handle, int32_t mask, uint32_t timeout_us,
                           uint32_t *outIberr);

    // 488.2 helpers built on top of the addressed command bus.
    uint32_t serialPoll(int32_t handle, uint8_t *outStatusByte,
                        uint32_t *outIberr);
    uint32_t trigger(int32_t handle, uint32_t *outIberr);
    uint32_t goToLocal(int32_t handle, uint32_t *outIberr);
    uint32_t localLockout(int32_t handle, uint32_t *outIberr);

    // Explicit-address variants (used by ibln / listener probing where we
    // don't want a full descriptor).
    uint32_t listenerPresent(uint8_t pad, int8_t sad, bool *outPresent,
                             uint32_t *outIberr);

    // Raw command-byte injection (for ibcmd / SendCmds). Sends the bytes
    // with ATN asserted.
    uint32_t sendCommands(int32_t handle, const uint8_t *cmds, uint32_t len,
                          uint32_t *outIbcnt, uint32_t *outIberr);

    // Snapshot of the bus control lines (iblines).
    uint32_t busLineStatus(uint16_t *outLines, uint32_t *outIberr);
    // TEMPORARY diagnostic passthrough — see GPIBTransport.h.
    uint32_t readRawRegisterDiag(uint16_t reg, uint8_t *outValue);
    uint32_t writeRawRegisterDiag(uint16_t reg, uint8_t value);
    uint32_t setMaxDataChunkDiag(uint32_t bytes);
    // Operator-triggered recovery (see recoverBus). softResetDiag re-runs the
    // chip bring-up; resetDeviceDiag forces a USB re-enumeration and tears
    // this instance down.
    uint32_t softResetDiag();
    uint32_t resetDeviceDiag();

    static uint32_t timeoutCodeToMicros(uint32_t code);

private:
    // Presence probe used by listenerPresent() when the bus-line read cannot
    // decide. Self-contained: no descriptor, its own short timeout.
    bool serialPollProbe(uint8_t pad, int8_t sad);

    GPIBDescriptor *descriptorFor(int32_t handle);
    int32_t allocateHandle();

    // Address the bus for an upcoming talker/listener exchange. The board
    // becomes (listener|talker) and the descriptor's device becomes the
    // opposite role.
    uint32_t addressForWrite(const GPIBDescriptor *desc, uint32_t *outIberr);
    uint32_t addressForRead(const GPIBDescriptor *desc, uint32_t *outIberr);

    // Bring a wedged core back without physical intervention. See the
    // definition for why this is necessary at all.
    // Pass the descriptor whose operation failed so the instrument can be
    // Device Cleared too; nullptr recovers only the board.
    bool recoverBus(const GPIBDescriptor *desc);

    // Descriptor table. M1 uses a small fixed-size table to keep memory
    // bounded; size matches GPIB_MAX_NUM_BOARDS-style scaling and is
    // sufficient for typical (one board + a handful of instruments) use.
    static constexpr int kMaxDescriptors = 64;
    GPIBDescriptor descriptors_[kMaxDescriptors];

    IGPIBTransport *transport_;

    // Board addressing / config (descriptors_[0] is reserved for the board itself).
    uint8_t  boardPAD_;
    int8_t   boardSAD_;
    bool     online_;
    bool     system_controller_;
    // Whether the client asked for REN, so recoverBus() can restore it.
    bool     renRequested_;
    // Guards against a recovery attempt re-entering itself via the retry it
    // triggers.
    bool     recoveryInProgress_;

    // Last computed status (also returned from each public method).
    uint32_t ibsta_;
};

#endif /* GPIBBoard_h */
