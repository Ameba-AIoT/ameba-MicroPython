// SPDX-License-Identifier: MIT
// machine.CAN port glue for ameba-rtos (AmebaGreen2 / RTL8711F only --
// AmebaDplus has no CAN controller hardware at all, see mpconfigport.h).
//
// INCLUDEFILE: textually #included from micropython/extmod/machine_can.c via
// MICROPY_PY_MACHINE_CAN_INCLUDEFILE. Implements the static functions
// declared in extmod/machine_can_port.h.
//
// Message buffer layout (CAN0/CAN1 each have 16 buffers, indices 0..15) --
// confirmed against the RTL8711F datasheet section 7.21 ("Up to 16 buffers",
// "Configurable FIFO mode for the upper 4 buffers") and
// CAN_RX_FIFO_READ_MSG_IDX == 12 in ameba_can.h:
//   0..CAN_TX_QUEUE_LEN-1 (0..7): TX queue, one CAN_TxMsgTypeDef per slot.
//   12..15 (CAN_RX_FILTER_BASE_IDX..+3): RX FIFO backing store, one slot per
//     set_filters() filter. CAN_ReadRxMsgFromFifo() always reads through a
//     single register window (CAN_RX_FIFO_READ_MSG_IDX == 12) regardless of
//     which physical slot actually captured the next queued frame, so each
//     slot's own ID/mask acts as an independently OR'd acceptance filter
//     feeding that shared read port -- matches the vendor's own
//     example/peripheral/raw/CAN/raw_can_lpbk example, which configures all
//     4 slots identically (there with an "accept all" filter).
//   8..11: unused margin.

#include <string.h>

#include "py/mperrno.h"  // MP_EINVAL -- extmod/machine_can.c uses it without including this itself
#include "ameba_soc.h"
#include "PinNames.h"

#define CAN_TX_QUEUE_LEN 8
#define CAN_HW_MAX_FILTER 4
#define CAN_RX_FILTER_BASE_IDX 12

// Hardware time-segment limits (ameba_can.h CAN_TIMING_MIN/MAX), expressed in
// the combined tseg1 (= PropSeg + PhaseSeg1) form the generic extmod layer
// uses -- overrides extmod/machine_can.c's #ifndef defaults.
#define CAN_TSEG1_MIN 4   // PropSeg(2) + PhaseSeg1(2)
#define CAN_TSEG1_MAX 14  // PropSeg(6) + PhaseSeg1(8)
#define CAN_TSEG2_MIN 2
#define CAN_TSEG2_MAX 8
#define CAN_SJW_MIN 1
#define CAN_SJW_MAX 4
#define CAN_BRP_MIN 1
#define CAN_BRP_MAX 32

#define TX_EMPTY UINT32_MAX

struct machine_can_port {
    CAN_TypeDef *CANx;
    IRQn_Type irq_num;
    uint32_t tx[CAN_TX_QUEUE_LEN];  // CAN ID occupying each TX slot, or TX_EMPTY
    bool irq_state_pending;
    bool error_passive;
};

// Only 2 fixed controllers exist, so use static storage instead of malloc.
static struct machine_can_port can_port_state[MICROPY_HW_NUM_CAN];

static void machine_can_port_clear_filters(machine_can_obj_t *self);

static void can_port_pins(mp_uint_t can_idx, PinName *tx, PinName *rx, u32 *func_tx, u32 *func_rx) {
    if (can_idx == 0) {
        *tx = MICROPY_HW_CAN0_TX;
        *rx = MICROPY_HW_CAN0_RX;
        *func_tx = PINMUX_FUNCTION_CAN0_TX;
        *func_rx = PINMUX_FUNCTION_CAN0_RX;
    } else {
        *tx = MICROPY_HW_CAN1_TX;
        *rx = MICROPY_HW_CAN1_RX;
        *func_tx = PINMUX_FUNCTION_CAN1_TX;
        *func_rx = PINMUX_FUNCTION_CAN1_RX;
    }
}

static uint32_t can_port_work_mode(machine_can_mode_t mode) {
    switch (mode) {
        case MP_CAN_MODE_LOOPBACK:
        case MP_CAN_MODE_SILENT_LOOPBACK:
            // CAN_MASK_TEST_CFG's register comment (ameba_can.h) distinguishes
            // "external loopback (enable can_tx_so)" -- TX pin still drives
            // the real bus and needs a real ACK from another node to complete
            // a frame -- from "internal loopback (can_tx_so tie 1)" -- ACK is
            // forced internally, no bus partner needed. The vendor's own
            // reference example (example/peripheral/raw/CAN/raw_can_lpbk)
            // uses CAN_INT_LOOPBACK_MODE for its standalone self-test, and
            // on-target testing confirmed CAN_EXT_LOOPBACK_MODE never
            // completes a receive with no partner on the bus (matches
            // machine.CAN.MODE_LOOPBACK's documented "no other device on the
            // bus needed" contract) -- so both LOOPBACK and SILENT_LOOPBACK
            // map to CAN_INT_LOOPBACK_MODE. This hardware has no register
            // state that additionally silences the bus while looping back
            // (see the CAN_SILENCE_MODE limitation below), so the two modes
            // are indistinguishable here -- known limitation, not a bug.
            return CAN_INT_LOOPBACK_MODE;
        case MP_CAN_MODE_SLEEP:
        case MP_CAN_MODE_SILENT:
            // ameba_can.h defines CAN_NORMAL_MODE == CAN_SILENCE_MODE == 0,
            // and CAN_Init()'s `if (CAN_WorkMode == CAN_NORMAL_MODE)` branch
            // (ameba_can.c) merges both into the same "not test mode" path --
            // this vendor driver genuinely cannot select a silence-only mode
            // distinct from NORMAL. Documented limitation, mirrors stm32's
            // non-FDCAN SLEEP mapping.
            return CAN_SILENCE_MODE;
        case MP_CAN_MODE_NORMAL:
        default:
            return CAN_NORMAL_MODE;
    }
}

static int machine_can_port_f_clock(const machine_can_obj_t *self) {
    (void)self;
    uint32_t rate = 0;
    CAN_CoreClockGet(&rate);
    return (int)rate;
}

static bool machine_can_port_supports_mode(const machine_can_obj_t *self, machine_can_mode_t mode) {
    (void)self;
    return mode < MP_CAN_MODE_MAX;
}

static mp_uint_t machine_can_port_max_data_len(mp_uint_t flags) {
    (void)flags;
    return 8;  // Classic CAN only -- MICROPY_HW_ENABLE_FDCAN is 0.
}

static u32 can_port_isr(void *Data) {
    machine_can_obj_t *self = (machine_can_obj_t *)Data;
    struct machine_can_port *port = self->port;
    CAN_TypeDef *CANx = port->CANx;
    u32 int_status = CAN_GetINTStatus(CANx);
    bool call_irq = false;

    if (int_status & CAN_RX_INT) {
        CAN_ClearINT(CANx, CAN_BIT_RX_INT_FLAG);
        if (CAN_FifoStatusGet(CANx) & CAN_BIT_FIFO_MSG_OVERFLOW) {
            self->counters.rx_overruns++;
        }
        if (self->mp_irq_trigger & MP_CAN_IRQ_RX) {
            call_irq = true;
        }
    }

    if (int_status & CAN_TX_INT) {
        CAN_ClearINT(CANx, CAN_BIT_TX_INT_FLAG);
        if (self->mp_irq_trigger & MP_CAN_IRQ_TX) {
            // Let the user's next irq().flags() call do the bookkeeping
            // (matches stm32's asymmetric handling in machine_can_irq_handler).
            call_irq = true;
        } else {
            // No one is listening for IRQ_TX -- free finished slots ourselves
            // so future send() calls can reuse them.
            u32 pending = (CAN_TxDoneStatusGet(CANx) | CAN_TxMsgBufErrGet(CANx))
                & ((1u << CAN_TX_QUEUE_LEN) - 1);
            while (pending) {
                int idx = __builtin_ctz(pending);
                CAN_MsgBufTxDoneStatusClear(CANx, idx);
                CAN_TxMsgBufErrClear(CANx, (1u << idx));
                port->tx[idx] = TX_EMPTY;
                pending &= ~(1u << idx);
            }
        }
    }

    if (int_status & CAN_ERR_INT) {
        CAN_ClearINT(CANx, CAN_BIT_ERROR_INT_FLAG);
        u32 err_sts = CAN_GetErrStatus(CANx);
        CAN_ClearErrStatus(CANx, err_sts);
        u32 rx_err_sts = CAN_RXErrCntSTS(CANx);
        bool irq_state = false;
        if (rx_err_sts & CAN_BIT_ERROR_PASSIVE) {
            if (!port->error_passive) {
                self->counters.num_passive++;
                irq_state = true;
            }
            port->error_passive = true;
        } else {
            port->error_passive = false;
            if (rx_err_sts & CAN_BIT_ERROR_WARNING) {
                self->counters.num_warning++;
                irq_state = true;
            }
        }
        if (irq_state && (self->mp_irq_trigger & MP_CAN_IRQ_STATE)) {
            port->irq_state_pending = true;
            call_irq = true;
        }
    }

    if (int_status & CAN_BUSOFF_INT) {
        CAN_ClearINT(CANx, CAN_BIT_BUSOFF_INT_FLAG);
        self->counters.num_bus_off++;
        port->error_passive = false;
        if (self->mp_irq_trigger & MP_CAN_IRQ_STATE) {
            port->irq_state_pending = true;
            call_irq = true;
        }
    }

    if (int_status & CAN_WKUP_INT) {
        CAN_ClearINT(CANx, CAN_BIT_WAKEUP_INT_FLAG);
    }

    if (call_irq && self->mp_irq_obj != NULL) {
        mp_irq_handler(self->mp_irq_obj);
    }
    return 0;
}

// Splits the generic combined tseg1 (PropSeg + PhaseSeg1) the same way the
// vendor's own CAN_CalcTimeSegments() balances it between the two fields.
// extmod/machine_can.c guarantees tseg1 is within [CAN_TSEG1_MIN, CAN_TSEG1_MAX]
// before calling machine_can_port_init() (see calculate_brp()'s search bound
// and the manual-tseg1-kwarg ValueError check) -- verified for the full
// [4,14] domain with a host-side script before landing this function
// (Task 1 of docs/superpowers/plans/2026-07-13-p1-can.md).
static void machine_can_port_split_tseg1(uint8_t tseg1, uint8_t *prop_out, uint8_t *phase1_out) {
    uint8_t prop = tseg1 / 2;
    if (prop < 2) {
        prop = 2;
    }
    if (prop > 6) {
        prop = 6;
    }
    uint8_t phase1 = tseg1 - prop;
    if (phase1 < 2) {
        phase1 = 2;
        prop = tseg1 - phase1;
    } else if (phase1 > 8) {
        phase1 = 8;
        prop = tseg1 - phase1;
    }
    *prop_out = prop;
    *phase1_out = phase1;
}

static void machine_can_port_init(machine_can_obj_t *self) {
    struct machine_can_port *port = &can_port_state[self->can_idx];
    memset(port, 0, sizeof(*port));
    for (int i = 0; i < CAN_TX_QUEUE_LEN; i++) {
        port->tx[i] = TX_EMPTY;
    }
    port->CANx = CAN_DEV_TABLE[self->can_idx].CANx;
    port->irq_num = CAN_DEV_TABLE[self->can_idx].IrqNum;
    self->port = port;

    PinName tx, rx;
    u32 func_tx, func_rx;
    can_port_pins(self->can_idx, &tx, &rx, &func_tx, &func_rx);
    Pinmux_Config((u8)tx, func_tx);
    Pinmux_Config((u8)rx, func_rx);

    CAN_CoreClockSet();
    u32 apb_periph = (self->can_idx == 0) ? APBPeriph_CAN0 : APBPeriph_CAN1;
    u32 apb_clock = (self->can_idx == 0) ? APBPeriph_CAN0_CLOCK : APBPeriph_CAN1_CLOCK;
    RCC_PeriphClockCmd(apb_periph, apb_clock, ENABLE);

    uint8_t prop, phase1;
    machine_can_port_split_tseg1(self->tseg1, &prop, &phase1);

    CAN_InitTypeDef init;
    CAN_StructInit(&init);
    init.CAN_RxFifoEn = ENABLE;
    init.CAN_WorkMode = can_port_work_mode(self->mode);
    init.CAN_Timing.Prescaler = self->brp;
    init.CAN_Timing.SJW = self->sjw;
    init.CAN_Timing.PropSeg = prop;
    init.CAN_Timing.PhaseSeg1 = phase1;
    init.CAN_Timing.PhaseSeg2 = self->tseg2;

    CAN_BusCmd(port->CANx, DISABLE);
    CAN_Init(port->CANx, &init);

    InterruptDis(port->irq_num);
    InterruptUnRegister(port->irq_num);
    InterruptRegister(can_port_isr, port->irq_num, (u32)(uintptr_t)self, INT_PRI_MIDDLE);
    InterruptEn(port->irq_num, INT_PRI_MIDDLE);

    // TX-completion and error/bus-off notifications are always enabled -- TX
    // slot bookkeeping (port->tx[]) and error-state tracking must stay
    // correct even if the user never calls CAN.irq(). RX notification is
    // toggled separately by machine_can_update_irqs(); the FIFO keeps
    // accepting frames regardless of whether its interrupt is enabled.
    CAN_INTConfig(port->CANx, CAN_TX_INT | CAN_ERR_INT | CAN_BUSOFF_INT | CAN_WKUP_INT, ENABLE);
    CAN_TxMsgBufINTConfig(port->CANx, CAN_MB_TXINT_EN((1u << CAN_TX_QUEUE_LEN) - 1), ENABLE);

    CAN_Cmd(port->CANx, ENABLE);
    CAN_BusCmd(port->CANx, ENABLE);
    // CAN_SetRxMsgBuf()'s underlying RAM-write handshake (the
    // `while (CANx->CAN_RAM_CMD & CAN_BIT_RAM_START);` spin in ameba_can.c)
    // never clears if the bus isn't enabled and on yet -- confirmed against
    // the vendor's own reference example (example/peripheral/raw/CAN/
    // raw_can_lpbk/example_raw_can_lpbk.c), which only calls
    // CAN_SetRxMsgBuf() after CAN_Cmd(ENABLE) + CAN_BusCmd(ENABLE) +
    // a CAN_BusStatusGet() wait. machine_can_port_clear_filters() (below)
    // calls CAN_SetRxMsgBuf() and MUST run after bus-on, not before -- an
    // earlier version of this file hung the whole system by calling it too
    // early (found via on-target testing during Phase 1 verification).
    //
    // Unlike the vendor example (which only ever tests loopback modes, where
    // bus-on is essentially instant because the hardware satisfies its own
    // "bus idle" condition internally), MP_CAN_MODE_NORMAL/SLEEP/SILENT need
    // a real transceiver+bus on the wire: CAN_BIT_BUS_ON_STATE only sets
    // once the controller has observed the required run of recessive bits
    // on the physical RX pin. With no transceiver connected (also confirmed
    // by on-target testing), that condition is never satisfied, so an
    // unconditional wait here would hang forever on every construction/
    // reinit that isn't loopback -- bounded with a timeout instead, raising
    // a clear error rather than freezing the whole system.
    mp_uint_t bus_on_deadline = mp_hal_ticks_ms() + 50;
    while (!CAN_BusStatusGet(port->CANx)) {
        if ((mp_int_t)(mp_hal_ticks_ms() - bus_on_deadline) >= 0) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
    }

    machine_can_port_clear_filters(self);
}

static void machine_can_port_deinit(machine_can_obj_t *self) {
    struct machine_can_port *port = self->port;
    InterruptDis(port->irq_num);
    InterruptUnRegister(port->irq_num);
    CAN_Cmd(port->CANx, DISABLE);
    CAN_BusCmd(port->CANx, DISABLE);
    CAN_ClearAllINT(port->CANx);
}

static mp_int_t machine_can_port_send(machine_can_obj_t *self, mp_uint_t id, const byte *data, size_t data_len, mp_uint_t flags) {
    struct machine_can_port *port = self->port;
    int idx_empty = -1;

    for (int i = 0; i < CAN_TX_QUEUE_LEN; i++) {
        uint32_t tx_id = port->tx[i];
        if (tx_id == TX_EMPTY) {
            if (idx_empty == -1) {
                idx_empty = i;
            }
        } else if (tx_id == id && !(flags & CAN_MSG_FLAG_UNORDERED)) {
            return -1;
        }
    }
    if (idx_empty == -1) {
        return -1;
    }

    CAN_TxMsgTypeDef tx;
    memset(&tx, 0, sizeof(tx));
    tx.ProtocolType = CAN_CAN20_PROTOCOL_FRAME;
    tx.IDE = (flags & CAN_MSG_FLAG_EXT_ID) ? CAN_EXTEND_FRAME : CAN_STANDARD_FRAME;
    tx.RTR = (flags & CAN_MSG_FLAG_RTR) ? CAN_REMOTE_FRAME : CAN_DATA_FRAME;
    if (flags & CAN_MSG_FLAG_EXT_ID) {
        tx.ExtId = id;
    } else {
        tx.StdId = id;
    }
    tx.MsgBufferIdx = idx_empty;
    tx.DLC = data_len;
    if (tx.RTR == CAN_DATA_FRAME && data_len > 0) {
        memcpy(tx.Data, data, data_len);
    }

    CAN_WriteMsg(port->CANx, &tx);
    port->tx[idx_empty] = id;
    return idx_empty;
}

static bool machine_can_port_cancel_send(machine_can_obj_t *self, mp_uint_t idx) {
    // The vendor driver exposes no per-buffer TX-abort register/primitive
    // (only bulk CAN_Cmd()/CAN_BusCmd() disable, which would also kill any
    // other buffer's pending transmission) -- cancellation of a single
    // in-flight message genuinely is not supported by this hardware/driver.
    // Always returns False, which is a truthful subset of the documented
    // "False: no message was pending, or it already sent" contract.
    (void)self;
    (void)idx;
    return false;
}

static bool machine_can_port_recv(machine_can_obj_t *self, void *data, size_t *dlen, mp_uint_t *id, mp_uint_t *flags, mp_uint_t *errors) {
    struct machine_can_port *port = self->port;
    if (CAN_FifoStatusGet(port->CANx) & CAN_BIT_FIFO_MSG_EMPTY) {
        return false;
    }

    CAN_RxMsgTypeDef rx;
    memset(&rx, 0, sizeof(rx));
    CAN_ReadRxMsgFromFifo(port->CANx, &rx);

    *flags = (rx.IDE == CAN_EXTEND_FRAME ? CAN_MSG_FLAG_EXT_ID : 0) |
        (rx.RTR == CAN_REMOTE_FRAME ? CAN_MSG_FLAG_RTR : 0);
    *id = (rx.IDE == CAN_EXTEND_FRAME) ? rx.ExtId : rx.StdId;
    *dlen = rx.DLC;
    if (rx.RTR != CAN_REMOTE_FRAME) {
        memcpy(data, rx.Data, rx.DLC);
    }

    u32 fifo_sts = CAN_FifoStatusGet(port->CANx);
    *errors = (fifo_sts & CAN_BIT_FIFO_MSG_OVERFLOW ? CAN_RECV_ERR_OVERRUN : 0) |
        (fifo_sts & CAN_BIT_FIFO_MSG_FULL ? CAN_RECV_ERR_FULL : 0);
    return true;
}

static void machine_can_update_irqs(machine_can_obj_t *self) {
    struct machine_can_port *port = self->port;
    bool want_rx = (self->mp_irq_trigger & MP_CAN_IRQ_RX) != 0;
    CAN_RxMsgBufINTConfig(port->CANx, CAN_MB_RXINT_EN(0xF000), want_rx ? ENABLE : DISABLE);
    CAN_INTConfig(port->CANx, CAN_RX_INT, want_rx ? ENABLE : DISABLE);
}

// Called by the extmod layer's CAN.irq().flags(). Also responsible for
// clearing TX done/error bits and freeing port->tx[] slots when the user IS
// subscribed to IRQ_TX (the ISR itself does this only when nobody is
// listening -- see can_port_isr above).
static mp_uint_t machine_can_port_irq_flags(machine_can_obj_t *self) {
    struct machine_can_port *port = self->port;
    mp_uint_t flags = 0;

    if ((self->mp_irq_trigger & MP_CAN_IRQ_STATE) && port->irq_state_pending) {
        flags |= MP_CAN_IRQ_STATE;
        port->irq_state_pending = false;
    }

    if ((self->mp_irq_trigger & MP_CAN_IRQ_RX) && !(CAN_FifoStatusGet(port->CANx) & CAN_BIT_FIFO_MSG_EMPTY)) {
        flags |= MP_CAN_IRQ_RX;
    }

    if (self->mp_irq_trigger & MP_CAN_IRQ_TX) {
        u32 done = CAN_TxDoneStatusGet(port->CANx);
        u32 err = CAN_TxMsgBufErrGet(port->CANx);
        u32 pending = (done | err) & ((1u << CAN_TX_QUEUE_LEN) - 1);
        if (pending) {
            int idx = __builtin_ctz(pending);
            bool failed = (err & (1u << idx)) != 0;
            CAN_MsgBufTxDoneStatusClear(port->CANx, idx);
            CAN_TxMsgBufErrClear(port->CANx, (1u << idx));
            port->tx[idx] = TX_EMPTY;
            flags |= MP_CAN_IRQ_TX | ((mp_uint_t)idx << MP_CAN_IRQ_IDX_SHIFT);
            if (failed) {
                flags |= MP_CAN_IRQ_TX_FAILED;
            }
        }
    }

    return flags;
}

// KNOWN OPEN ISSUE (found during Phase 1 on-target verification, root cause
// NOT yet found -- see findings.md [P1] "recv() 在拒绝态发送后失效"):
// if any frame is transmitted while every filter slot is still set to this
// reject-everything sentinel (i.e. before the first machine_can_port_set_filter()/
// set_filters() call after init), CAN0's receive path silently stops
// delivering frames to recv() even after a later set_filters() call opens a
// matching filter -- get_counters().rx_pending stays 0, so the FIFO itself
// genuinely never receives anything afterwards, this isn't a read-timing
// race. Reproduces with a single CAN controller, independent of whether a
// second CAN() instance was ever constructed. Suspected: the RX FIFO's
// internal write-pointer/state machine may need an explicit reset after a
// filter-less-match event that this driver doesn't currently perform, but
// no register-level documentation confirms this -- unconfirmed. Workaround
// until root-caused: call set_filters() to open the desired filter BEFORE
// the first send() on that controller.
static void machine_can_port_clear_filters(machine_can_obj_t *self) {
    struct machine_can_port *port = self->port;
    for (int i = 0; i < CAN_HW_MAX_FILTER; i++) {
        CAN_RxMsgTypeDef rx;
        memset(&rx, 0, sizeof(rx));
        // No hardware "disable this buffer" bit exists -- approximate
        // "reject everything" with a maximally-specific filter (exact 29-bit
        // extended ID 0, both RTR and IDE pinned) that ordinary standard-ID
        // traffic won't match.
        rx.RTR = CAN_REMOTE_FRAME;
        rx.RTR_Mask = 1;
        rx.IDE = CAN_EXTEND_FRAME;
        rx.IDE_Mask = 1;
        rx.ExtId = 0;
        rx.ID_MASK = CAN_EXT_ID_MASK;
        rx.MsgBufferIdx = CAN_RX_FILTER_BASE_IDX + i;
        CAN_SetRxMsgBuf(port->CANx, &rx);
    }
}

static void machine_can_port_set_filter(machine_can_obj_t *self, int filter_idx, mp_uint_t can_id, mp_uint_t mask, mp_uint_t flags) {
    if (filter_idx >= CAN_HW_MAX_FILTER) {
        mp_raise_ValueError(MP_ERROR_TEXT("too many filters"));
    }
    if (flags & ~CAN_MSG_FLAG_EXT_ID) {
        mp_raise_ValueError(MP_ERROR_TEXT("flags"));
    }

    CAN_RxMsgTypeDef rx;
    memset(&rx, 0, sizeof(rx));
    rx.RTR = CAN_DATA_FRAME;
    rx.RTR_Mask = 0;  // filters don't discriminate data vs remote frames
    rx.IDE = (flags & CAN_MSG_FLAG_EXT_ID) ? CAN_EXTEND_FRAME : CAN_STANDARD_FRAME;
    rx.IDE_Mask = 1;
    if (rx.IDE == CAN_EXTEND_FRAME) {
        rx.ExtId = can_id;
    } else {
        rx.StdId = can_id;
    }
    rx.ID_MASK = mask;
    rx.MsgBufferIdx = CAN_RX_FILTER_BASE_IDX + filter_idx;
    CAN_SetRxMsgBuf(self->port->CANx, &rx);
}

static machine_can_state_t machine_can_port_get_state(machine_can_obj_t *self) {
    u32 sts = CAN_RXErrCntSTS(self->port->CANx);
    if (sts & CAN_BIT_ERROR_BUSOFF) {
        return MP_CAN_STATE_BUS_OFF;
    }
    if (sts & CAN_BIT_ERROR_PASSIVE) {
        return MP_CAN_STATE_PASSIVE;
    }
    if (sts & CAN_BIT_ERROR_WARNING) {
        return MP_CAN_STATE_WARNING;
    }
    return MP_CAN_STATE_ACTIVE;
}

static void machine_can_port_update_counters(machine_can_obj_t *self) {
    struct machine_can_port *port = self->port;
    machine_can_counters_t *counters = &self->counters;

    counters->tec = CAN_TXErrCntGet(port->CANx);
    counters->rec = CAN_RXErrCntGet(port->CANx);

    int tx_pending = 0;
    for (int i = 0; i < CAN_TX_QUEUE_LEN; i++) {
        if (port->tx[i] != TX_EMPTY) {
            tx_pending++;
        }
    }
    counters->tx_pending = tx_pending;
    counters->rx_pending = (CAN_FifoStatusGet(port->CANx) & CAN_BIT_FIFO_MSG_EMPTY) ? 0 : CAN_FifoLvlGet(port->CANx);
    // num_warning/num_passive/num_bus_off/rx_overruns are accumulated in the
    // ISR (can_port_isr above), not recomputed here.
}

static mp_obj_t machine_can_port_get_additional_timings(machine_can_obj_t *self, mp_obj_t optional_arg) {
    (void)self;
    (void)optional_arg;
    return mp_const_none;
}

static void machine_can_port_restart(machine_can_obj_t *self) {
    struct machine_can_port *port = self->port;
    // No per-buffer cancel primitive (see machine_can_port_cancel_send) --
    // drop the bookkeeping for any still-pending TX slots without an
    // IRQ_TX notification, per CAN.restart()'s documented contract.
    for (int i = 0; i < CAN_TX_QUEUE_LEN; i++) {
        port->tx[i] = TX_EMPTY;
    }
    CAN_BusCmd(port->CANx, DISABLE);
    CAN_TXErrCntClear(port->CANx);
    CAN_RXErrCntClear(port->CANx);
    CAN_ClearErrStatus(port->CANx, CAN_GetErrStatus(port->CANx));
    CAN_ClearAllINT(port->CANx);
    port->error_passive = false;
    port->irq_state_pending = false;
    CAN_BusCmd(port->CANx, ENABLE);
}
