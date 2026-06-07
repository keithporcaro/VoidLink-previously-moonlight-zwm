/* triton_input_queue.{c,h} — Phase 1 seam between the BLE bridge (producer) and the
 * USB/IP server (consumer). Pure C, no iOS APIs, so it is fully unit-testable on the
 * Windows/WSL dev box against the Phase-0 bench before any Mac/iPad work.
 *
 * Translation (the load-bearing bit): the controller's BLE state report is id 0x45 with a
 * 45-byte TritonMTUNoQuat_t payload; the Phase-0-GREEN USB state report is id 0x42 with a
 * 53-byte payload (54 bytes on the wire). The first 45 bytes of both are the byte-identical
 * TritonMTUNoQuat_t (SDL casts both to it), so translation = rewrite the report id 0x45->0x42,
 * keep the 45-byte struct, and zero-pad the 8 trailing bytes. (Zeros for those 8 bytes were
 * accepted by Steam in the GREEN gate; their exact meaning is unconfirmed pending a live USB
 * capture — see docs/.../triton-usb-descriptor.md.)
 *
 * Concurrency: single-producer (BLE consumer thread) / single-consumer (USB/IP server thread),
 * latest-state-wins. Intermediate states may be coalesced/dropped — correct for a polled HID
 * state report (the host always wants the *current* state, not a backlog).
 */
#ifndef TRITON_INPUT_QUEUE_H
#define TRITON_INPUT_QUEUE_H

#define TRITON_BLE_STATE_ID    0x45   /* ID_TRITON_CONTROLLER_STATE_BLE */
#define TRITON_USB_STATE_ID    0x42   /* ID_TRITON_CONTROLLER_STATE      */
#define TRITON_NOQUAT_LEN      45     /* sizeof packed TritonMTUNoQuat_t  */
#define TRITON_USB_PAYLOAD     53     /* USB 0x42 declared payload bytes  */
#define TRITON_USB_WIRE        54     /* 1 (report id) + 53               */

#ifdef __cplusplus
extern "C" {
#endif

/* Reset to the empty state (no report received yet). Safe to call before use. */
void triton_input_queue_reset(void);

/* Producer (BLE thread): hand a raw BLE report — report[0] = BLE report id,
 * report[1..len-1] = payload. Translates a state report (0x45, or a passthrough 0x42)
 * into the USB 0x42 wire form and stores it as the latest state.
 * Returns 1 if a USB state report was produced, 0 if the report was ignored
 * (e.g. a non-state BLE report id, or len < 1). */
int triton_input_push_ble(const unsigned char *report, int len);

/* Consumer (USB/IP server thread): copy the latest USB-form 0x42 state report into buf.
 * Always writes a valid TRITON_USB_WIRE-byte report — a neutral (centered, all-zero
 * payload) 0x42 report if no BLE state has arrived yet — so the host always gets valid
 * state. Returns bytes written (TRITON_USB_WIRE), or 0 if cap < TRITON_USB_WIRE. */
int triton_input_pop(unsigned char *buf, int cap);

/* Diagnostic: 1 once at least one real BLE state report has been translated/stored. */
int triton_input_have_data(void);

#ifdef __cplusplus
}
#endif

#endif /* TRITON_INPUT_QUEUE_H */
