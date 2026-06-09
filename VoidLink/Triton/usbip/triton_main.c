/* triton_main.c — Tier-1 standalone harness for the Phase-1 Triton USB/IP server.
 * NOT part of the iOS app (exclude from VoidLink.xcodeproj): it supplies main() so the
 * ported C core can be built and bench-tested on the Windows/WSL box exactly as in Phase 0.
 *
 *   make                 -> builds ./triton-server (+ runs the queue unit test)
 *   ./triton-server      -> canned-sweep feed into triton_input_queue, then serve :3240
 *                           (attach with usbip-win2 and confirm Steam stays GREEN)
 *   ./triton-server --dump  -> print device/config descriptor bytes for a regression diff
 *
 * The canned feeder pushes the same sweeping 0x42 state report the GREEN gate used, but now
 * THROUGH triton_input_queue — so the IN path exercised here is byte-for-byte the production
 * (BLE-fed) path, only the producer differs.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "usbip.h"
#include "triton_device.h"
#include "triton_report.h"
#include "triton_input_queue.h"

static volatile int g_feed_stop = 0;

/* A real BLE 0x45 frame captured live from the controller on 2026-06-08: off-center sticks +
 * pressed buttons + a FROZEN IMU tail (C3 7A C3 ...). Pushing this through the SAME queue the
 * app uses lets us prove in Steam that the IMU-zero calms the cursor while sticks/buttons land. */
static const unsigned char g_captured_frame[] = {
    0x45,
    0x00,0x00,0x10,0x31,0x00,0x00,0x00,0x00,
    0xFD,0x14,0x41,0xB9,0x84,0xFA,0x01,0x80,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xC3,0x7A,0xC3,0x13,0x02,0x10,0xE9,0x0F,0x5C,0x3C,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00
};
static int g_replay = 0;   /* --replay: feed the captured frozen-IMU frame instead of the sweep */

static void *canned_feeder(void *arg)
{
    (void)arg;
    unsigned tick = 0;
    unsigned char buf[64];
    while (!g_feed_stop) {
        if (g_replay) {
            triton_input_push_ble(g_captured_frame, (int)sizeof g_captured_frame);  /* 0x45 -> IMU-zero -> 0x42 */
        } else {
            int n = triton_fill_canned_report(buf, sizeof buf, TRITON_REPORT_ID_USB, tick++);
            if (n > 0) triton_input_push_ble(buf, n);   /* 0x42 passthrough into the queue */
        }
        usleep(8000);                                   /* ~125 Hz */
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        triton_dump_descriptors();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--replay") == 0) {
        g_replay = 1;
        printf("REPLAY mode: feeding the captured frozen-IMU controller frame\n");
    }

    triton_input_queue_reset();

    pthread_t feeder;
    if (pthread_create(&feeder, NULL, canned_feeder, NULL) != 0) {
        fprintf(stderr, "failed to start canned feeder\n");
        return 1;
    }

    printf("triton-server (Tier-1 harness): canned sweep -> triton_input_queue -> USB/IP :%d\n",
           TCP_SERV_PORT);
    triton_usbip_start();          /* blocks until usbip_stop() / fatal socket error */

    g_feed_stop = 1;
    pthread_join(feeder, NULL);
    return 0;
}
