/* triton_device.c — synthetic 2026 Steam Controller (Triton) USB/IP device.
 * Vendored from the Phase-0-GREEN hid-triton.c (commit b256da5; derived from lcgamboa
 * hid-keyboard.c, GPLv2) and adapted for Phase 1 (the iPad port):
 *   - interrupt-IN reports now come from triton_input_queue (BLE-fed) instead of the
 *     canned sweep + usleep pacing;
 *   - Steam's SET_REPORT / interrupt-OUT writes are forwarded to a registered write sink
 *     (the BLE bridge on iOS), while the synthetic GET_ATTRIBUTES/echo responder that won
 *     the GREEN gate is kept verbatim;
 *   - main() is gone (see triton_main.c for the Tier-1 standalone harness); the server is
 *     started via triton_usbip_start() on a dedicated thread.
 *
 * Presents the GENUINE captured identity + 372-byte HID report descriptor of
 * VID 0x28DE / PID 0x1302 (see docs/superpowers/captures/triton-usb-descriptor.md).
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "usbip.h"
#include "triton_report.h"
#include "triton_report_desc.h"   /* GENERATED: triton_report_desc_bytes[372] */
#include "triton_input_queue.h"
#include "triton_device.h"

/* ---- HID Report Descriptor: the genuine 372-byte Triton descriptor. ---- */
#define triton_report_desc      triton_report_desc_bytes
#define TRITON_REPORT_DESC_SIZE TRITON_REPORT_DESC_LEN

/* Steam -> controller write sink (forwards SET_REPORT / interrupt-OUT to BLE on iOS).
 * NULL in Tier-1 standalone builds — the synthetic responder alone kept Steam GREEN. */
static triton_write_sink_fn g_write_sink = 0;
void triton_set_write_sink(triton_write_sink_fn cb) { g_write_sink = cb; }

/* Live feature round-trips (0 = synthetic/echo only, the GREEN path; 1 = forward to BLE). */
static int g_feature_live = 0;
void triton_set_feature_live(int on) { g_feature_live = on; }
#define TRITON_FEATURE_TIMEOUT_MS 50

/* ---- Device Descriptor: the captured identity Steam matches on. ---- */
const USB_DEVICE_DESCRIPTOR dev_dsc =
{
    0x12,        /* bLength                                                */
    0x01,        /* DEVICE descriptor                                      */
    0x0200,      /* bcdUSB 2.0                                             */
    0xEF,        /* bDeviceClass    = Miscellaneous (IAD/composite)        */
    0x02,        /* bDeviceSubClass = Common Class                         */
    0x01,        /* bDeviceProtocol = Interface Association                */
    0x40,        /* bMaxPacketSize0 (64)                                   */
    0x28DE,      /* idVendor  = Valve                                      */
    0x1302,      /* idProduct = Triton (USB). Rebuild 0x1303 for BLE-PID variant. */
    0x0307,      /* bcdDevice = 3.07 (captured)                            */
    0x01,        /* iManufacturer -> strings[1] "Valve Software"           */
    0x02,        /* iProduct      -> strings[2] "Steam Controller"         */
    0x03,        /* iSerialNumber -> strings[3] (synthetic placeholder)            */
    0x01         /* bNumConfigurations                                     */
};

/* ---- Configuration: 1 HID interface, 2 interrupt endpoints (IN 0x81 + OUT 0x01). ----
 * CONFIG_HID in usbip.h has only one endpoint, so define a 2-endpoint variant.
 * The first two members match CONFIG_GEN, which usbip.c casts `configuration` to. */
typedef struct __attribute__ ((__packed__)) _CONFIG_HID2
{
    USB_CONFIGURATION_DESCRIPTOR dev_conf;
    USB_INTERFACE_DESCRIPTOR     dev_int;
    USB_HID_DESCRIPTOR           dev_hid;
    USB_ENDPOINT_DESCRIPTOR      dev_ep_in;
    USB_ENDPOINT_DESCRIPTOR      dev_ep_out;
} CONFIG_HID2;

const CONFIG_HID2 configuration_hid = {
    /* Configuration: wTotalLength = sizeof(blob)=41, 1 interface, bus-powered+rwu, 500mA */
    { 0x09, USB_DESCRIPTOR_CONFIGURATION, sizeof(CONFIG_HID2), 1, 1, 0, 0xA0, 0xFA },
    /* Interface 0: HID (class 0x03), 2 endpoints, no boot protocol */
    { 0x09, USB_DESCRIPTOR_INTERFACE, 0, 0, 2, 0x03, 0x00, 0x00, 0 },
    /* HID class descriptor: bcdHID 1.11, 1 report descriptor, length = 372 */
    { 0x09, 0x21, 0x0111, 0x00, 0x01, 0x22, TRITON_REPORT_DESC_SIZE },
    /* Endpoint 0x81 IN, interrupt, 64-byte, bInterval 1 */
    { 0x07, USB_DESCRIPTOR_ENDPOINT, 0x81, 0x03, 0x0040, 0x01 },
    /* Endpoint 0x01 OUT, interrupt, 64-byte, bInterval 1 */
    { 0x07, USB_DESCRIPTOR_ENDPOINT, 0x01, 0x03, 0x0040, 0x01 }
};

const char *configuration = (const char *)&configuration_hid;
const USB_INTERFACE_DESCRIPTOR *interfaces[] = { &configuration_hid.dev_int };
const USB_DEVICE_QUALIFIER_DESCRIPTOR dev_qua = {};

/* ---- String descriptors (UTF-16LE, length-prefixed). Padded so usbip.c's
 *      bounds guard can safely stall any unexpected index (e.g. MS-OS 0xEE). ---- */
static const unsigned char str_lang[] = { 0x04, 0x03, 0x09, 0x04 };          /* LangID 0x0409 */
static const unsigned char str_mfg[]  = { 0x1E, 0x03,
    'V',0,'a',0,'l',0,'v',0,'e',0,' ',0,'S',0,'o',0,'f',0,'t',0,'w',0,'a',0,'r',0,'e',0 };
static const unsigned char str_prod[] = { 0x22, 0x03,
    'S',0,'t',0,'e',0,'a',0,'m',0,' ',0,'C',0,'o',0,'n',0,'t',0,'r',0,'o',0,'l',0,'l',0,'e',0,'r',0 };
static const unsigned char str_ser[]  = { 0x1C, 0x03,   /* synthetic serial — not a real device id */
    'T',0,'R',0,'I',0,'T',0,'O',0,'N',0,'0',0,'0',0,'0',0,'0',0,'0',0,'0',0,'1',0 };
const unsigned char *strings[] = {
    str_lang, str_mfg, str_prod, str_ser,
    0, 0, 0, 0, 0, 0, 0, 0       /* indices 4..11 absent -> usbip.c stalls them */
};

/* ---- Feature-report (Steam Controller command) emulation ----
 * Steam's init writes a command to feature report 0x01 (e.g. 0x83 GET_ATTRIBUTES)
 * then reads feature 0x01 back for the response. We track the last command and
 * answer GET_ATTRIBUTES with a well-formed MsgGetAttributes reply so Steam can
 * finish registering the controller. Wire format (from SDL controller_structs.h,
 * read-only reference): [report_id][type][length][ {tag:u8, value:u32 LE} ... ].
 * Attribute tags per SDL controller_constants.h. */
static unsigned char g_last_feature_cmd = 0;
static unsigned char g_last_feature_data[256];
static int           g_last_feature_len = 0;

#define TRITON_CMD_GET_ATTRIBUTES   0x83
#define ATTRIB_UNIQUE_ID             0
#define ATTRIB_PRODUCT_ID            1
#define ATTRIB_CAPABILITIES          2
#define ATTRIB_FIRMWARE_BUILD_TIME   4
#define ATTRIB_CONNECTION_INTERVAL  11   /* in microseconds */

static int triton_put_attr(unsigned char *p, unsigned char tag, unsigned int val)
{
    p[0] = tag;
    p[1] = (unsigned char)(val & 0xFF);
    p[2] = (unsigned char)((val >> 8) & 0xFF);
    p[3] = (unsigned char)((val >> 16) & 0xFF);
    p[4] = (unsigned char)((val >> 24) & 0xFF);
    return 5;
}

/* Build [report_id][0x83][len][attribute TLVs] into buf. Returns bytes written. */
static int triton_build_attributes(unsigned char *buf, unsigned char report_id)
{
    int n = 0, start;
    buf[n++] = report_id;                 /* 0x01 */
    buf[n++] = TRITON_CMD_GET_ATTRIBUTES; /* echo 0x83 */
    int len_pos = n++;
    start = n;
    n += triton_put_attr(buf + n, ATTRIB_UNIQUE_ID,           0x00000001);   /* synthetic — not a real device id */
    n += triton_put_attr(buf + n, ATTRIB_PRODUCT_ID,          0x00001302);
    n += triton_put_attr(buf + n, ATTRIB_CAPABILITIES,        0x00000000);
    n += triton_put_attr(buf + n, ATTRIB_FIRMWARE_BUILD_TIME, 0x65A00000);
    n += triton_put_attr(buf + n, ATTRIB_CONNECTION_INTERVAL, 8000);
    buf[len_pos] = (unsigned char)(n - start);   /* payload length (25) */
    return n;
}

/* ---- Interrupt endpoints: IN delivers the latest BLE-fed state; OUT forwards to BLE. ---- */
void handle_data(int sockfd, USBIP_RET_SUBMIT *usb_req, int bl)
{
    if (usb_req->direction == 1) {           /* USBIP_DIR_IN: deliver the latest 0x42 state */
        unsigned char buf[TRITON_WIRE_MAX];
        int n = triton_input_pop(buf, sizeof buf);   /* latest BLE state, or neutral 0x42 */
        if (n > 0) {
            send_usb_req(sockfd, usb_req, (char *)buf, n, 0);
        } else {
            send_usb_req(sockfd, usb_req, "", 0, 0);
        }
        /* No usleep: the host paces IN URBs at the endpoint's bInterval; we answer each
         * with the current state (latest-wins), exactly like real hardware. */
    } else {                                 /* USBIP_DIR_OUT: Steam wrote to the interrupt-OUT ep */
        unsigned char out[256];
        int len = bl;
        if (len > (int)sizeof out) len = (int)sizeof out;
        if (len > 0 && recv(sockfd, out, len, 0) != len) {
            printf("  (intr-OUT recv short)\n");
        }
        if (g_write_sink) g_write_sink(TRITON_WRITE_OUTPUT, out, len);  /* -> BLE haptic/output */
        printf("INTR-OUT ep%d len=%d:", usb_req->ep, len);
        for (int i = 0; i < len; i++) printf(" %02x", out[i]);
        printf("   <== Steam interrupt-OUT write (rumble / lizard-off?)\n");
        send_usb_req(sockfd, usb_req, "", 0, 0);
    }
}

/* ---- Control transfers: serve the report descriptor, log everything Steam does. ---- */
void handle_unknown_control(int sockfd, StandardDeviceRequest *control_req, USBIP_RET_SUBMIT *usb_req)
{
    printf("CTRL type=0x%02x req=0x%02x wValue=0x%02x%02x wIndex=0x%02x%02x wLen=%u\n",
           control_req->bmRequestType, control_req->bRequest,
           control_req->wValue1, control_req->wValue0,
           control_req->wIndex1, control_req->wIndex0, control_req->wLength);

    /* GET_DESCRIPTOR(Report) on the interface (bmRequestType 0x81, type 0x22) */
    if (control_req->bmRequestType == 0x81 && control_req->bRequest == 0x06 &&
        control_req->wValue1 == 0x22) {
        unsigned int n = TRITON_REPORT_DESC_SIZE;
        if (control_req->wLength && control_req->wLength < n) n = control_req->wLength;
        printf("  -> GET_DESCRIPTOR(report): sending %u of %u Triton report-descriptor bytes\n",
               n, (unsigned)TRITON_REPORT_DESC_SIZE);
        send_usb_req(sockfd, usb_req, (char *)triton_report_desc, n, 0);
        return;
    }

    /* Host -> device class requests */
    if (control_req->bmRequestType == 0x21) {
        if (control_req->bRequest == 0x0a) {              /* SET_IDLE */
            send_usb_req(sockfd, usb_req, "", 0, 0);
            return;
        }
        if (control_req->bRequest == 0x09) {              /* SET_REPORT (output/feature) */
            unsigned char data[256];
            int len = control_req->wLength;
            if (len > (int)sizeof data) len = (int)sizeof data;
            if (len > 0 && recv(sockfd, data, len, 0) != len) {
                printf("  (recv short)\n");
            }
            /* Remember the command (data[1]) and the full payload so a following
             * GET_FEATURE on this report can answer it (synthesize for 0x83,
             * echo the written payload back for confirm-style commands). */
            if (len >= 2) g_last_feature_cmd = data[1];
            g_last_feature_len = (len <= (int)sizeof g_last_feature_data) ? len : (int)sizeof g_last_feature_data;
            memcpy(g_last_feature_data, data, g_last_feature_len);
            if (g_write_sink) g_write_sink(TRITON_WRITE_FEATURE, data, len);  /* -> BLE report char */
            if (g_feature_live) triton_feature_clear();  /* a GET_FEATURE now awaits the NEW reply */
            printf("  -> SET_REPORT wValue=0x%02x%02x cmd=0x%02x:",
                   control_req->wValue1, control_req->wValue0, g_last_feature_cmd);
            for (int i = 0; i < len; i++) printf(" %02x", data[i]);
            printf("   <== Steam host->device write (lizard-off / IMU-enable / rumble)\n");
            send_usb_req(sockfd, usb_req, "", 0, 0);
            return;
        }
    }

    /* Device -> host class requests: GET_REPORT / GET_FEATURE — the decisive path.
     * Answer based on the last command Steam wrote to this feature report. */
    if (control_req->bmRequestType == 0xA1) {
        unsigned char rsp[256];
        int len = control_req->wLength;
        if (len > 64) len = 64;                 /* feature report 0x01 is 64 bytes (1 id + 63) */
        if (len > (int)sizeof rsp) len = (int)sizeof rsp;
        memset(rsp, 0, len);
        unsigned char rid = control_req->wValue0;   /* report id (0x01) */
        int n = 0;
        if (g_last_feature_cmd == TRITON_CMD_GET_ATTRIBUTES) {
            n = triton_build_attributes(rsp, rid);          /* [01][83][len][TLVs] — synthetic (GREEN) */
        } else {
            if (g_feature_live) {
                /* Live: the command was forwarded to BLE; await the controller's reply. */
                n = triton_feature_wait(rsp, len, TRITON_FEATURE_TIMEOUT_MS);
                if (n > 0) rsp[0] = rid;
            }
            if (n == 0 && g_last_feature_len > 0) {         /* fallback: mirror the written payload */
                n = (g_last_feature_len <= len) ? g_last_feature_len : len;
                memcpy(rsp, g_last_feature_data, n);
                rsp[0] = rid;
            }
        }
        printf("  -> GET_FEATURE wValue=0x%02x%02x wLen=%u: answering last cmd=0x%02x (%d structured bytes, sending %d)\n",
               control_req->wValue1, control_req->wValue0, control_req->wLength, g_last_feature_cmd, n, len);
        send_usb_req(sockfd, usb_req, (char *)rsp, len, 0);
        return;
    }

    send_usb_req(sockfd, usb_req, "", 0, 0);              /* default: ack empty */
}

/* Start the USB/IP server (blocking). Run on a dedicated thread; returns when stopped. */
void triton_usbip_start(void)
{
    printf("triton: synthetic Steam Controller (28DE:1302, bcd 3.07) USB/IP server on :%d\n", TCP_SERV_PORT);
    printf("  report descriptor: %u bytes; state report id 0x%02x; input via triton_input_queue\n",
           (unsigned)TRITON_REPORT_DESC_SIZE, TRITON_REPORT_ID_USB);
    usbip_run(&dev_dsc);
}

/* Stop the server from another thread (unblocks accept). */
void triton_usbip_stop(void)
{
    usbip_stop();
}

/* Gate host attaches on BLE readiness: 1 = accept OP_REQ_IMPORT, 0 = refuse (host attach retries). */
void triton_usbip_set_ready(int on)
{
    usbip_set_ready(on);
}

/* Drop the active host attachment (BLE disconnect) -> usbip-win2 surprise-removal -> Steam disconnect. */
void triton_usbip_drop_client(void)
{
    usbip_drop_client();
}

/* Diagnostic used by the Tier-1 harness (triton_main.c --dump): print the device and
 * configuration descriptor bytes so they can be diffed against the genuine capture. */
void triton_dump_descriptors(void)
{
    const unsigned char *d = (const unsigned char *)&dev_dsc;
    unsigned i;
    printf("DEVICE(%u):", (unsigned)sizeof dev_dsc);
    for (i = 0; i < sizeof dev_dsc; i++) printf(" %02x", d[i]);
    const unsigned char *c = (const unsigned char *)&configuration_hid;
    printf("\nCONFIG(%u):", (unsigned)sizeof(CONFIG_HID2));
    for (i = 0; i < sizeof(CONFIG_HID2); i++) printf(" %02x", c[i]);
    printf("\nREPORTDESC(%u)\n", (unsigned)TRITON_REPORT_DESC_SIZE);
}
