/* Unit test for triton_input_queue (Phase 1, the BLE->USB seam). Runs on the WSL dev box:
 *   gcc -Wall test_triton_input_queue.c triton_input_queue.c -lpthread -o t && ./t */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "triton_input_queue.h"

/* Provider that deposits a 5-byte feature reply after 40 ms (for the cross-thread tests). */
static void *provider_thread(void *arg)
{
    (void)arg;
    usleep(40 * 1000);
    unsigned char reply[5] = { 0x01, 0x83, 0x02, 0xAB, 0xCD };
    triton_feature_provide(reply, sizeof reply);
    return NULL;
}

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
    for (int i = 0; i < TRITON_IMU_OFFSET - 1; i++)        /* non-IMU fields (struct off 0..28) copied */
        assert(buf[1 + i] == (unsigned char)(0x10 + i));
    for (int i = 0; i < TRITON_IMU_LEN; i++)               /* IMU (wire 30..45) zeroed (frozen-IMU workaround) */
        assert(buf[TRITON_IMU_OFFSET + i] == 0x00);
    for (int i = 0; i < (TRITON_USB_PAYLOAD - TRITON_NOQUAT_LEN); i++)
        assert(buf[1 + TRITON_NOQUAT_LEN + i] == 0x00);    /* 8 USB-only trailing bytes zero */

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

    /* ---- Feature-response round-trip cell ---- */
    unsigned char fbuf[64];

    /* 8. Wait with no provider times out -> 0. */
    triton_feature_clear();
    assert(triton_feature_wait(fbuf, sizeof fbuf, 20) == 0);

    /* 9. Provide then wait returns the reply; it is one-shot (a second wait times out). */
    unsigned char r1[4] = { 0x01, 0x83, 0x10, 0x20 };
    triton_feature_provide(r1, sizeof r1);
    int fn = triton_feature_wait(fbuf, sizeof fbuf, 50);
    assert(fn == 4 && fbuf[1] == 0x83 && fbuf[3] == 0x20);
    assert(triton_feature_wait(fbuf, sizeof fbuf, 20) == 0);

    /* 10. clear() discards a pending reply. */
    triton_feature_provide(r1, sizeof r1);
    triton_feature_clear();
    assert(triton_feature_wait(fbuf, sizeof fbuf, 20) == 0);

    /* 11. Cross-thread: provider deposits at 40 ms; a 200 ms wait receives it. */
    pthread_t th;
    pthread_create(&th, NULL, provider_thread, NULL);
    fn = triton_feature_wait(fbuf, sizeof fbuf, 200);
    pthread_join(th, NULL);
    assert(fn == 5 && fbuf[1] == 0x83 && fbuf[4] == 0xCD);

    /* 12. Cross-thread timeout: provider at 40 ms, but the server only waits 15 ms -> 0. */
    triton_feature_clear();
    pthread_t th2;
    pthread_create(&th2, NULL, provider_thread, NULL);
    assert(triton_feature_wait(fbuf, sizeof fbuf, 15) == 0);
    pthread_join(th2, NULL);          /* provider still fires at 40 ms */
    triton_feature_clear();           /* discard that late reply */

    /* 13. REAL captured controller frame (2026-06-08, live BLE→USB on hardware): a frame whose
     *     sticks/buttons are live but whose IMU tail arrived FROZEN (C3 7A C3 13 …). Confirm the
     *     gamepad fields pass through verbatim and the frozen IMU zeroes out (no gyro-mouse). */
    {
        triton_input_queue_reset();
        unsigned char real[] = {                 /* BLE 0x45 report = [0x45][45-byte payload] */
            0x45,
            0x00,0x00,0x10,0x31,0x00,0x00,0x00,0x00,      /* seq, buttons (pressed) */
            0xFD,0x14,0x41,0xB9,0x84,0xFA,0x01,0x80,      /* triggers + sticks (live, off-center) */
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,  /* pads/left-pressure */
            0xC3,0x7A,0xC3,0x13,0x02,0x10,0xE9,0x0F,0x5C,0x3C,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00 /* FROZEN imu tail */
        };
        assert(triton_input_push_ble(real, sizeof real) == 1);
        n = triton_input_pop(buf, sizeof buf);
        assert(n == TRITON_USB_WIRE && buf[0] == 0x42);
        /* gamepad fields survive verbatim */
        assert(buf[3] == 0x10 && buf[4] == 0x31);                       /* buttons */
        assert(buf[9] == 0xFD && buf[10] == 0x14 && buf[16] == 0x80);   /* sticks  */
        /* the frozen IMU (was C3 7A C3 13 …) is zeroed -> no Steam gyro-mouse cursor-fly */
        for (int i = 0; i < TRITON_IMU_LEN; i++) assert(buf[TRITON_IMU_OFFSET + i] == 0x00);
    }

    printf("PASS\n");
    return 0;
}
