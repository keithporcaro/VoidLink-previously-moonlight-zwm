/* Unit test for triton_input_queue (Phase 1, the BLE->USB seam). Runs on the WSL dev box:
 *   gcc -Wall test_triton_input_queue.c triton_input_queue.c -lpthread -o t && ./t */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "triton_input_queue.h"

int main(void)
{
    unsigned char buf[64];

    /* 1. Empty: pop returns a neutral 54-byte 0x42 report; have_data == 0. */
    triton_input_queue_reset();
    assert(triton_input_have_data() == 0);
    int n = triton_input_pop(buf, sizeof buf);
    assert(n == TRITON_USB_WIRE);            /* 54 */
    assert(buf[0] == 0x42);
    for (int i = 1; i < TRITON_USB_WIRE; i++) assert(buf[i] == 0x00);

    /* 2. BLE 0x45 state report -> USB 0x42 with the 45-byte struct copied + 8 zero pad.
     *    Build a 46-byte BLE report: [0x45][45-byte payload]; mark each payload byte. */
    unsigned char ble[46];
    ble[0] = TRITON_BLE_STATE_ID;            /* 0x45 */
    for (int i = 0; i < TRITON_NOQUAT_LEN; i++) ble[1 + i] = (unsigned char)(0x10 + i);
    assert(triton_input_push_ble(ble, sizeof ble) == 1);
    assert(triton_input_have_data() == 1);

    memset(buf, 0xAA, sizeof buf);
    n = triton_input_pop(buf, sizeof buf);
    assert(n == TRITON_USB_WIRE);
    assert(buf[0] == 0x42);                  /* report id rewritten 0x45 -> 0x42 */
    for (int i = 0; i < TRITON_NOQUAT_LEN; i++)
        assert(buf[1 + i] == (unsigned char)(0x10 + i));   /* struct copied verbatim */
    for (int i = 0; i < (TRITON_USB_PAYLOAD - TRITON_NOQUAT_LEN); i++)
        assert(buf[1 + TRITON_NOQUAT_LEN + i] == 0x00);    /* 8 trailing bytes zero */

    /* 3. sLeftStickX is at TritonMTUNoQuat_t struct offset 9 -> USB wire offset 10.
     *    (== TRITON_LSX_OFFSET in triton_report.h.) Confirm the byte maps through. */
    assert(buf[10] == (unsigned char)(0x10 + 9));   /* ble payload[9] landed at wire[10] */

    /* 4. Latest-wins: a second report replaces the first. */
    unsigned char ble2[46];
    ble2[0] = TRITON_BLE_STATE_ID;
    for (int i = 0; i < TRITON_NOQUAT_LEN; i++) ble2[1 + i] = (unsigned char)(0x80 + i);
    assert(triton_input_push_ble(ble2, sizeof ble2) == 1);
    n = triton_input_pop(buf, sizeof buf);
    assert(buf[1] == 0x80);                  /* now reflects the newer report */

    /* 5. Non-state BLE report id is ignored; latest is unchanged. */
    unsigned char batt[8] = { 0x43, 1, 2, 3, 4, 5, 6, 7 };   /* ID_TRITON_BATTERY_STATUS */
    assert(triton_input_push_ble(batt, sizeof batt) == 0);
    n = triton_input_pop(buf, sizeof buf);
    assert(buf[1] == 0x80);                  /* still the 0x80.. report */

    /* 6. Passthrough of an already-USB 0x42 report (recorded-USB replay / canned feed). */
    unsigned char usb[TRITON_USB_WIRE];
    memset(usb, 0, sizeof usb);
    usb[0] = 0x42; usb[10] = 0x77;
    assert(triton_input_push_ble(usb, sizeof usb) == 1);
    n = triton_input_pop(buf, sizeof buf);
    assert(buf[0] == 0x42 && buf[10] == 0x77);

    /* 7. Too-small consumer buffer is rejected. */
    unsigned char tiny[16];
    assert(triton_input_pop(tiny, sizeof tiny) == 0);

    printf("PASS\n");
    return 0;
}
