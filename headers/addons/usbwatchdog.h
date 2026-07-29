#ifndef _USBWatchdog_H
#define _USBWatchdog_H

#include "gpaddon.h"
#include <stdint.h>

// -----------------------------------------------------------------------------
// USB Watchdog / failure recorder addon (XInput)
//
// Records and recovers a silently-wedged XInput IN endpoint. On the affected
// boards the device stays enumerated and the control endpoint keeps answering
// (web config still works via Start+X+Y), but the interrupt-IN report endpoint
// (0x81) stops being drained by the host, so input dies with no recovery path.
// The stock XInput driver only sends when !usbd_edpt_busy(0, endpoint_in) and
// never aborts/re-arms a stuck transfer (XInputDriver.cpp, v0.7.12).
//
// This addon runs in postprocess() (right after tud_task()) and watches the IN
// endpoint. If it stays continuously "busy" longer than STALL_MS while mounted
// and not suspended -- which never happens in healthy operation, since the host
// polls every ~1 ms -- it (1) snapshots the raw RP2040 USB SIE registers into a
// reboot-surviving noinit RAM ring, then (2) recovers: up to MAX_REENUM soft
// re-enumerations, then a reboot into gamepad mode.
//
// Self-contained; no driver changes. Disabled unless USB_WATCHDOG_ENABLED and
// input mode is XInput (the mode that uses endpoint 0x81 via the raw endpoint
// API). All thresholds are compile-time overridable.
// -----------------------------------------------------------------------------

#ifndef USB_WATCHDOG_ENABLED
#define USB_WATCHDOG_ENABLED 0
#endif

// Optional: expose recorded events over web config via /api/getUSBDiagnostics.
// Off by default so a plain build touches no base code beyond addon registration.
#ifndef USB_WATCHDOG_WEBAPI
#define USB_WATCHDOG_WEBAPI 0
#endif

// XInput interrupt-IN report endpoint (see XInputDescriptors.h config descriptor).
#ifndef USB_WATCHDOG_ENDPOINT
#define USB_WATCHDOG_ENDPOINT 0x81
#endif

// Continuous-busy time before the endpoint is judged wedged (ms). Healthy
// delivery clears busy within a couple ms, so anything > ~250 ms is safe.
#ifndef USB_WATCHDOG_STALL_MS
#define USB_WATCHDOG_STALL_MS 500
#endif

// Soft-disconnect duration during a re-enumeration attempt (ms).
#ifndef USB_WATCHDOG_REENUM_MS
#define USB_WATCHDOG_REENUM_MS 150
#endif

// Soft re-enumeration attempts before escalating to a reboot.
#ifndef USB_WATCHDOG_MAX_REENUM
#define USB_WATCHDOG_MAX_REENUM 2
#endif

// Sustained-healthy time after a recovery before the escalation counter resets.
#ifndef USB_WATCHDOG_HEALTHY_MS
#define USB_WATCHDOG_HEALTHY_MS 5000
#endif

// Number of most-recent events retained in the reboot-surviving ring.
#ifndef USB_WATCHDOG_RING
#define USB_WATCHDOG_RING 16
#endif

#define USBWatchdogName "USBWatchdog"

// Recovery action recorded per event.
enum USBWatchdogAction : uint8_t {
    USBWD_ACTION_NONE   = 0,
    USBWD_ACTION_REENUM = 1,
    USBWD_ACTION_REBOOT = 2,
};

// One recorded stall event. Raw register words are stored verbatim and decoded
// at read-out time, so this stays forward-compatible with silicon-level analysis.
typedef struct {
    uint32_t uptimeMs;        // ms-since-boot when the wedge was detected
    uint32_t stuckMs;         // how long the IN endpoint had been continuously busy
    uint32_t msSinceDelivery; // ms since the last observed successful IN delivery
    uint32_t sieStatus;       // usb_hw->sie_status  (error latches, suspend/resume)
    uint32_t bufStatus;       // usb_hw->buf_status  (per-EP buffer-done bits)
    uint32_t ep1InBufCtrl;    // usb_dpram->ep_buf_ctrl[1].in (AVAILABLE/FULL/PID/LEN)
    uint32_t ints;            // usb_hw->ints
    uint32_t inte;            // usb_hw->inte
    uint16_t sofFirst;        // frame number sampled at detection
    uint16_t sofSecond;       // frame number ~1.5 ms later (advancing => bus alive)
    uint16_t suspendTransitions;
    uint8_t  action;          // USBWatchdogAction taken
    uint8_t  flags;           // bit0 mounted, bit1 warmBoot
} USBWatchdogEvent;

// Reboot-surviving persistent record (lives in an __uninitialized_ram section).
typedef struct {
    uint32_t magic;              // validity guard vs cold-boot garbage
    uint32_t version;
    uint32_t bootCount;
    uint32_t coldBootCount;
    uint32_t stallCount;         // total wedges detected
    uint32_t reenumCount;        // soft re-enumerations performed
    uint32_t rebootCount;        // recovery reboots (counted after the reboot)
    uint32_t pendingReboot;      // marker set immediately before a recovery reboot
    uint32_t lastStallUptimeMs;
    uint32_t head;               // ring write index
    uint32_t count;              // valid events (saturates at USB_WATCHDOG_RING)
    USBWatchdogEvent ring[USB_WATCHDOG_RING];
} USBWatchdogPersist;

// Read-only accessor for the persistent record (used by the optional web API).
const USBWatchdogPersist& usbWatchdogGetPersist();

class USBWatchdogAddon : public GPAddon {
public:
    virtual bool available();
    virtual void setup();
    virtual void process() {}
    virtual void preprocess() {}
    virtual void postprocess(bool sent);
    virtual void reinit() {}
    virtual std::string name() { return USBWatchdogName; }

private:
    void handleWedge(uint32_t now, uint32_t stuckMs);
    void recordEvent(uint32_t now, uint32_t stuckMs, uint8_t action);

    uint8_t  endpoint = USB_WATCHDOG_ENDPOINT;
    bool     prevBusy = false;
    bool     suspendedPrev = false;
    uint32_t busySince = 0;        // ms the IN ep first went continuously busy (0 = not busy)
    uint32_t lastDeliveryMs = 0;   // ms of last observed successful IN delivery
    uint32_t lastRecoveryMs = 0;   // ms of last recovery action
    uint16_t suspendTransitions = 0;
    uint8_t  reenumStage = 0;      // consecutive recovery escalations
};

#endif  // _USBWatchdog_H
