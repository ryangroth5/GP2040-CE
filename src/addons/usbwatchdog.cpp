#include "addons/usbwatchdog.h"
#include "storagemanager.h"
#include "system.h"
#include "usbdriver.h"
#include "enums.pb.h"

#include <string.h>

#include "pico/time.h"
#include "pico/platform.h"
#include "pico/platform/sections.h" // __uninitialized_ram
#include "hardware/watchdog.h"      // watchdog_caused_reboot
#include "hardware/structs/usb.h"   // usb_hw, usb_dpram
#include "tusb.h"                   // tud_disconnect / tud_connect
#include "device/usbd_pvt.h"        // usbd_edpt_busy

#define USBWD_MAGIC         0x55574431u  // 'U''W''D''1'
#define USBWD_VERSION       1u
#define USBWD_REBOOT_MARKER 0x5245424fu  // 'R''E''B''O'
#define USBWD_SOF_MASK      0x7ffu

// Reboot-surviving record. __uninitialized_ram keeps it out of .bss so it is NOT
// zeroed by crt0 on a warm (watchdog) reboot; the magic guard detects the garbage
// contents of a true power-on and re-initializes.
static USBWatchdogPersist __uninitialized_ram(g_persist);

const USBWatchdogPersist& usbWatchdogGetPersist() {
    return g_persist;
}

static inline uint32_t nowMs() {
    return to_ms_since_boot(get_absolute_time());
}

bool USBWatchdogAddon::available() {
    if (!USB_WATCHDOG_ENABLED) return false;
    // Endpoint 0x81 is the XInput interrupt-IN report endpoint; only arm for the
    // mode that uses it via the raw endpoint API (the one that can wedge).
    const GamepadOptions& options = Storage::getInstance().getGamepadOptions();
    return options.inputMode == INPUT_MODE_XINPUT;
}

void USBWatchdogAddon::setup() {
    endpoint = USB_WATCHDOG_ENDPOINT;

    const bool warm = watchdog_caused_reboot();

    // First valid boot or power-on garbage -> initialize the record.
    if (g_persist.magic != USBWD_MAGIC || g_persist.version != USBWD_VERSION) {
        memset(&g_persist, 0, sizeof(g_persist));
        g_persist.magic = USBWD_MAGIC;
        g_persist.version = USBWD_VERSION;
    }

    g_persist.bootCount++;
    if (!warm) g_persist.coldBootCount++;
    // Tally a recovery reboot we triggered on the previous (warm) boot.
    if (warm && g_persist.pendingReboot == USBWD_REBOOT_MARKER) {
        g_persist.rebootCount++;
    }
    g_persist.pendingReboot = 0;

    // Per-session detection state.
    prevBusy = false;
    suspendedPrev = false;
    busySince = 0;
    lastDeliveryMs = nowMs();
    lastRecoveryMs = 0;
    suspendTransitions = 0;
    reenumStage = 0;
}

// Runs in the main loop immediately after tud_task(), so the endpoint's busy
// state reflects the just-serviced USB device stack.
void USBWatchdogAddon::postprocess(bool sent) {
    const bool mounted = get_usb_mounted();
    const bool suspended = get_usb_suspended();

    if (suspended && !suspendedPrev) suspendTransitions++;
    suspendedPrev = suspended;

    // A suspended (or unmounted) bus keeps the endpoint legitimately armed; don't
    // count that as a stall.
    if (!mounted || suspended) {
        busySince = 0;
        prevBusy = false;
        return;
    }

    const uint32_t now = nowMs();
    const bool busy = usbd_edpt_busy(0, endpoint);

    // Observe delivery: a busy->free transition is a completion; a fresh send got
    // through the driver's free-endpoint guard. Either means the pipe is healthy.
    if ((prevBusy && !busy) || sent) lastDeliveryMs = now;
    prevBusy = busy;

    if (!busy) {
        busySince = 0;
        if (reenumStage != 0 && (now - lastRecoveryMs) > USB_WATCHDOG_HEALTHY_MS) {
            reenumStage = 0;  // sustained health clears the escalation counter
        }
        return;
    }

    // Endpoint is busy right now; time the stall.
    if (busySince == 0) {
        busySince = now;
        return;
    }

    if ((now - busySince) >= USB_WATCHDOG_STALL_MS) {
        handleWedge(now, now - busySince);
        busySince = 0;
        lastRecoveryMs = now;
    }
}

void USBWatchdogAddon::recordEvent(uint32_t now, uint32_t stuckMs, uint8_t action) {
    USBWatchdogEvent& ev = g_persist.ring[g_persist.head];
    ev.uptimeMs = now;
    ev.stuckMs = stuckMs;
    ev.msSinceDelivery = now - lastDeliveryMs;
    ev.sieStatus = usb_hw->sie_status;
    ev.bufStatus = usb_hw->buf_status;
    ev.ep1InBufCtrl = usb_dpram->ep_buf_ctrl[1].in;   // EP1 IN buffer control
    ev.ints = usb_hw->ints;
    ev.inte = usb_hw->inte;
    ev.sofFirst = (uint16_t)(usb_hw->sof_rd & USBWD_SOF_MASK);
    busy_wait_us(1500);                                // ~1.5 frames
    ev.sofSecond = (uint16_t)(usb_hw->sof_rd & USBWD_SOF_MASK);
    ev.suspendTransitions = suspendTransitions;
    ev.action = action;
    ev.flags = (uint8_t)(0x1u | (watchdog_caused_reboot() ? 0x2u : 0x0u));

    g_persist.head = (g_persist.head + 1) % USB_WATCHDOG_RING;
    if (g_persist.count < USB_WATCHDOG_RING) g_persist.count++;
    g_persist.stallCount++;
    g_persist.lastStallUptimeMs = now;
}

void USBWatchdogAddon::handleWedge(uint32_t now, uint32_t stuckMs) {
    const uint8_t action = (reenumStage < USB_WATCHDOG_MAX_REENUM)
                               ? USBWD_ACTION_REENUM
                               : USBWD_ACTION_REBOOT;

    // Persist the snapshot BEFORE any reboot so it survives to the readout.
    recordEvent(now, stuckMs, action);

    if (action == USBWD_ACTION_REENUM) {
        reenumStage++;
        g_persist.reenumCount++;
        // Force the host to tear down and re-open the pipe. Cheaper than a reboot
        // and preserves all runtime state.
        tud_disconnect();
        busy_wait_ms(USB_WATCHDOG_REENUM_MS);
        tud_connect();
    } else {
        // Re-enumeration didn't hold -> reboot into gamepad mode (the recovery the
        // user's manual replug / "reboot to device" already performs reliably).
        g_persist.pendingReboot = USBWD_REBOOT_MARKER;
        System::reboot(System::BootMode::GAMEPAD);  // does not return
    }
}
