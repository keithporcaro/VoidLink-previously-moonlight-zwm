/* triton_input_queue.c — see triton_input_queue.h. Pure C + pthreads (POSIX; available on
 * both the WSL dev box and iOS). Latest-state-wins, single-producer/single-consumer. */
#include "triton_input_queue.h"
#include <string.h>
#include <pthread.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char   g_latest[TRITON_USB_WIRE];   /* latest USB-form 0x42 report */
static int             g_have = 0;                   /* 1 once a real report stored  */

void triton_input_queue_reset(void)
{
    pthread_mutex_lock(&g_lock);
    memset(g_latest, 0, sizeof g_latest);
    g_latest[0] = TRITON_USB_STATE_ID;   /* neutral 0x42 report */
    g_have = 0;
    pthread_mutex_unlock(&g_lock);
}

int triton_input_push_ble(const unsigned char *report, int len)
{
    if (!report || len < 1) {
        return 0;
    }

    unsigned char usb[TRITON_USB_WIRE];
    memset(usb, 0, sizeof usb);
    usb[0] = TRITON_USB_STATE_ID;        /* 0x42 on the wire regardless of source id */

    unsigned char id = report[0];
    if (id == TRITON_BLE_STATE_ID) {
        /* BLE 0x45: payload is the 45-byte TritonMTUNoQuat_t. Copy it, leave the
         * 8 trailing USB-only bytes zero. */
        int n = len - 1;
        if (n > TRITON_NOQUAT_LEN) n = TRITON_NOQUAT_LEN;
        if (n > 0) memcpy(usb + 1, report + 1, (size_t)n);
    } else if (id == TRITON_USB_STATE_ID) {
        /* Already a USB 0x42 report (e.g. a recorded-USB replay or canned feed): copy
         * the whole thing, including the report id, up to the wire length. */
        int n = len;
        if (n > TRITON_USB_WIRE) n = TRITON_USB_WIRE;
        memcpy(usb, report, (size_t)n);
        usb[0] = TRITON_USB_STATE_ID;
    } else {
        /* Non-state BLE report (battery 0x43, wireless 0x79, timestamp 0x47, ...).
         * Not forwarded by this v1 queue (it carries only the rendered state report).
         * Returning 0 lets the caller decide; a future revision can multiplex these. */
        return 0;
    }

    pthread_mutex_lock(&g_lock);
    memcpy(g_latest, usb, sizeof usb);
    g_have = 1;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int triton_input_pop(unsigned char *buf, int cap)
{
    if (!buf || cap < TRITON_USB_WIRE) {
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    if (g_have) {
        memcpy(buf, g_latest, TRITON_USB_WIRE);
    } else {
        memset(buf, 0, TRITON_USB_WIRE);
        buf[0] = TRITON_USB_STATE_ID;    /* neutral 0x42 report before first BLE report */
    }
    pthread_mutex_unlock(&g_lock);
    return TRITON_USB_WIRE;
}

int triton_input_have_data(void)
{
    pthread_mutex_lock(&g_lock);
    int h = g_have;
    pthread_mutex_unlock(&g_lock);
    return h;
}
