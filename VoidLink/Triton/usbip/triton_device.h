/* triton_device.h — public surface of the synthetic-Triton USB/IP device (Phase 1).
 * The device identity, 372-byte descriptor, and synthetic feature responder live in
 * triton_device.c (vendored from the Phase-0-GREEN hid-triton.c); input arrives via
 * triton_input_queue; Steam's writes leave via the registered write sink. */
#ifndef TRITON_DEVICE_H
#define TRITON_DEVICE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Steam -> controller write kinds delivered to the write sink. */
#define TRITON_WRITE_FEATURE 1   /* control SET_REPORT to a feature report (0x87 settings, etc.) */
#define TRITON_WRITE_OUTPUT  2   /* interrupt-OUT endpoint write (haptics/rumble)               */

typedef void (*triton_write_sink_fn)(int kind, const unsigned char *data, int len);

/* Register a sink that forwards Steam's SET_REPORT / interrupt-OUT writes onward (to BLE on
 * iOS). NULL (the default) simply drops them — correct for the Tier-1 standalone bench, where
 * the synthetic GET_ATTRIBUTES/echo responder alone kept Steam GREEN. */
void triton_set_write_sink(triton_write_sink_fn cb);

/* Start the USB/IP server (blocking — run on a dedicated thread). Returns when stopped. */
void triton_usbip_start(void);

/* Stop the server from another thread (unblocks accept, makes triton_usbip_start return). */
void triton_usbip_stop(void);

/* Print the device + configuration descriptor bytes (Tier-1 diagnostic / regression diff). */
void triton_dump_descriptors(void);

#ifdef __cplusplus
}
#endif

#endif /* TRITON_DEVICE_H */
