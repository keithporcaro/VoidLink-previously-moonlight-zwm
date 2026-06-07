/* triton_report.h — canned 2026 Steam Controller (Triton) state report for the
 * Phase 0 bench test.
 *
 * The genuine controller uses NUMBERED HID reports (verified from the captured
 * 372-byte report descriptor: mouse=0x40, keyboard=0x41, vendor state IN=0x42/0x45,
 * feature=0x01/0x02). On the wire a state report is therefore:
 *     data[0]   = report id   (0x42 = ID_TRITON_CONTROLLER_STATE [USB],
 *                              0x45 = ID_TRITON_CONTROLLER_STATE_BLE)
 *     data[1..] = TritonMTUNoQuat_t  (packed; SDL controller_structs.h, #pragma pack(1))
 *
 * Report 0x42 declares 53 payload bytes; 0x45 declares 45 ( == sizeof TritonMTUNoQuat_t ).
 * Within TritonMTUNoQuat_t, sLeftStickX is at struct offset 9, so its absolute wire
 * offset is 1 (report id) + 9 = 10.  (SDL is read-only reference; offset derived, not copied.)
 */
#ifndef TRITON_REPORT_H
#define TRITON_REPORT_H

#include <string.h>

#define TRITON_REPORT_ID_USB  0x42   /* ID_TRITON_CONTROLLER_STATE */
#define TRITON_REPORT_ID_BLE  0x45   /* ID_TRITON_CONTROLLER_STATE_BLE */
#define TRITON_PAYLOAD_USB    53     /* report 0x42: 53 data bytes (descriptor count 0x35) */
#define TRITON_PAYLOAD_BLE    45     /* report 0x45: 45 data bytes (descriptor count 0x2d) */
#define TRITON_WIRE_MAX       64     /* interrupt endpoint max packet size */
#define TRITON_LSX_OFFSET     10     /* 1 (report id) + offsetof(TritonMTUNoQuat_t, sLeftStickX)=9 */

/* Declared payload length (bytes after the report-id) for a given state report id. */
static inline int triton_payload_len(unsigned char report_id)
{
    return (report_id == TRITON_REPORT_ID_BLE) ? TRITON_PAYLOAD_BLE : TRITON_PAYLOAD_USB;
}

/* Fill `buf` with a canned numbered state report whose left-stick X walks with `tick`
 * (so Steam's controller tester shows motion). Everything else is neutral/zero.
 * `report_id` selects the USB (0x42) or BLE (0x45) message id. Returns bytes written
 * on the wire (1 + payload), or 0 if the buffer is too small. */
static inline int triton_fill_canned_report(unsigned char *buf, int cap,
                                            unsigned char report_id, unsigned tick)
{
    int wire = 1 + triton_payload_len(report_id);
    if (cap < wire) {
        return 0;
    }
    memset(buf, 0, wire);
    buf[0] = report_id;                              /* numbered report: data[0] = report id */
    /* Sweep the full int16 range as tick advances. */
    short lsx = (short)((int)((tick & 0xFF) * 256) - 32768);
    buf[TRITON_LSX_OFFSET]     = (unsigned char)(lsx & 0xFF);
    buf[TRITON_LSX_OFFSET + 1] = (unsigned char)((lsx >> 8) & 0xFF);
    return wire;
}

#endif /* TRITON_REPORT_H */
