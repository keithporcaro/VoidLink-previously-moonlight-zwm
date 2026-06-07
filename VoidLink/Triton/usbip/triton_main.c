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

static void *canned_feeder(void *arg)
{
    (void)arg;
    unsigned tick = 0;
    unsigned char buf[64];
    while (!g_feed_stop) {
        int n = triton_fill_canned_report(buf, sizeof buf, TRITON_REPORT_ID_USB, tick++);
        if (n > 0) triton_input_push_ble(buf, n);   /* 0x42 passthrough into the queue */
        usleep(8000);                               /* ~125 Hz */
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        triton_dump_descriptors();
        return 0;
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
