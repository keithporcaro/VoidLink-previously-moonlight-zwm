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
#define TRITON_IMU_OFFSET      30     /* wire offset of TritonMTUNoQuat_t.imu (struct off 29 + 1) */
#define TRITON_IMU_LEN         16     /* u32 timestamp + 6x s16 (accel xyz, gyro xyz)            */

#ifdef __cplusplus
extern "C" {
#endif

/* Reset to the empty state (no report received yet). Safe to call before use. */
void triton_input_queue_reset(void);

/* Producer (BLE thread): hand a raw BLE report — report[0] = BLE report id,
 * report[1..len-1] = payload. Translates a state report (0x45, or a passthrough 0x42)
 * into the USB 0x42 wire form and stores it as the latest state. The IMU block is gated:
 * passed through only while its u32 timestamp is advancing (gyro enabled), zeroed while frozen
 * (gyro off) so a stale sample can't drive Steam's gyro-mouse — see triton_input_queue.c.
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

/* --- Feature-response round-trip cell (Steam GET_FEATURE -> controller -> Steam) ---
 * Used only when a live controller (BLE) is wired. The Steam Controller protocol reads a
 * feature by WRITING a command (SET_REPORT) then READING the reply (GET_FEATURE); the BLE
 * bridge forwards the write, the controller replies, the bridge deposits it here, and the
 * USB/IP server thread picks it up — bounded by a timeout so the host is never stalled
 * (it falls back to the synthetic/echo responder that won the GREEN gate). Thread-safe. */

/* Clear any pending feature reply. Call when a new command is sent to the controller, so a
 * following GET_FEATURE waits for the NEW reply rather than returning a stale one. */
void triton_feature_clear(void);

/* BLE thread: deposit the controller's feature-report reply and wake a waiting server thread. */
void triton_feature_provide(const unsigned char *data, int len);

/* Server thread: wait up to timeout_ms for a provided reply; copy into buf (<=cap). Returns
 * bytes copied, or 0 on timeout / none. One-shot: consumes the reply. */
int triton_feature_wait(unsigned char *buf, int cap, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* TRITON_INPUT_QUEUE_H */
