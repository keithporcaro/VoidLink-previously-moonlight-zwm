/* triton_input_queue.c — see triton_input_queue.h. Pure C + pthreads (POSIX; available on
 * both the WSL dev box and iOS). Latest-state-wins, single-producer/single-consumer. */
#include "triton_input_queue.h"
#include <string.h>
#include <pthread.h>
#ifdef __APPLE__
/* iOS 26 SDK explicit-modules layout: the _DarwinFoundation2 sub-modules holding
 * timespec / clock_gettime aren't auto-exported, so <time.h> alone leaves them
 * "not reachable". Force-include the specific sub-headers clang's fix-it suggests. */
#include <sys/_types/_timespec.h>
#include <_time.h>
#endif
#include <time.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char   g_latest[TRITON_USB_WIRE];   /* latest USB-form 0x42 report */
static int             g_have = 0;                   /* 1 once a real report stored  */

/* --- IMU liveness gate -------------------------------------------------------------------
 * The controller streams gyro/accel only after the host writes GYRO_MODE (reg 0x30); until
 * then the IMU block — including its leading u32 timestamp — is FROZEN at a stale non-zero
 * sample. A frozen non-zero gyro reads to Steam as a *constant* rotation and drives its
 * desktop gyro-mouse (the cursor "fly"). So pass the IMU through only while its timestamp is
 * advancing, and zero it while frozen. Self-correcting: when Steam enables gyro (its own
 * register write, forwarded to the controller over BLE) the timestamp starts ticking and live
 * data flows; when gyro is off the block is zeroed — no build flag to toggle. Confirmed on real
 * hardware over USB 2026-06-08: feature 0x01 [87 03 30 18 00] -> timestamp climbs + live
 * accel/gyro in the same 0x45 report; [87 03 30 00 00] -> re-frozen. Mutated only by the single
 * producer thread (triton_input_push_ble) / reset, so it needs no extra lock. */
#define TRITON_IMU_STALE_LIMIT 4    /* unchanged-timestamp frames before declaring frozen */
static unsigned g_imu_last_ts = 0;
static int      g_imu_have_ts = 0;
static int      g_imu_stale   = 0;

#ifndef TRITON_BLE_LIVE_IMU
static int triton_imu_is_live(const unsigned char *usb)
{
    unsigned ts = (unsigned)usb[TRITON_IMU_OFFSET]
                | ((unsigned)usb[TRITON_IMU_OFFSET + 1] << 8)
                | ((unsigned)usb[TRITON_IMU_OFFSET + 2] << 16)
                | ((unsigned)usb[TRITON_IMU_OFFSET + 3] << 24);
    int live;
    if (!g_imu_have_ts) {
        g_imu_have_ts = 1;
        g_imu_stale   = TRITON_IMU_STALE_LIMIT;   /* unknown until it moves -> treat as frozen */
        live = 0;
    } else if (ts != g_imu_last_ts) {
        g_imu_stale = 0;
        live = 1;
    } else {
        if (g_imu_stale < TRITON_IMU_STALE_LIMIT) g_imu_stale++;
        live = (g_imu_stale < TRITON_IMU_STALE_LIMIT);
    }
    g_imu_last_ts = ts;
    return live;
}
#endif  /* !TRITON_BLE_LIVE_IMU */

void triton_input_queue_reset(void)
{
    pthread_mutex_lock(&g_lock);
    memset(g_latest, 0, sizeof g_latest);
    g_latest[0] = TRITON_USB_STATE_ID;   /* neutral 0x42 report */
    g_have = 0;
    g_imu_have_ts = 0;                    /* re-arm the IMU liveness gate */
    g_imu_stale   = 0;
    g_imu_last_ts = 0;
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

    /* Gate the IMU on the assembled report: live gyro (advancing timestamp) passes through;
     * a frozen (gyro-disabled) sample is zeroed so it can't drive Steam's gyro-mouse. The
     * TRITON_BLE_LIVE_IMU build disables the gate (raw passthrough) for A/B contrast. */
#ifndef TRITON_BLE_LIVE_IMU
    if (!triton_imu_is_live(usb)) {
        memset(usb + TRITON_IMU_OFFSET, 0, TRITON_IMU_LEN);
    }
#endif

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

/* ---- Feature-response round-trip cell ---- */
static pthread_mutex_t g_flock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_fcond  = PTHREAD_COND_INITIALIZER;
static unsigned char   g_freply[256];
static int             g_freply_len   = 0;
static int             g_freply_ready = 0;

void triton_feature_clear(void)
{
    pthread_mutex_lock(&g_flock);
    g_freply_ready = 0;
    g_freply_len   = 0;
    pthread_mutex_unlock(&g_flock);
}

void triton_feature_provide(const unsigned char *data, int len)
{
    if (!data || len < 0) {
        return;
    }
    if (len > (int)sizeof g_freply) {
        len = (int)sizeof g_freply;
    }
    pthread_mutex_lock(&g_flock);
    memcpy(g_freply, data, (size_t)len);
    g_freply_len   = len;
    g_freply_ready = 1;
    pthread_cond_signal(&g_fcond);
    pthread_mutex_unlock(&g_flock);
}

int triton_feature_wait(unsigned char *buf, int cap, int timeout_ms)
{
    if (!buf || cap <= 0) {
        return 0;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&g_flock);
    int rc = 0;
    while (!g_freply_ready && rc == 0) {
        rc = pthread_cond_timedwait(&g_fcond, &g_flock, &ts);   /* ETIMEDOUT breaks the loop */
    }
    int n = 0;
    if (g_freply_ready) {
        n = (g_freply_len <= cap) ? g_freply_len : cap;
        memcpy(buf, g_freply, (size_t)n);
        g_freply_ready = 0;     /* one-shot */
        g_freply_len   = 0;
    }
    pthread_mutex_unlock(&g_flock);
    return n;
}
