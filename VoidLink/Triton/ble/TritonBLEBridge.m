/* TritonBLEBridge.m — see TritonBLEBridge.h. Hand-written CoreBluetooth client; no SDL.
 *
 * ON-DEVICE VERIFICATION NEEDED (cannot be checked on Windows; design spec §9.5):
 *  - Write framing to 100F6C34: we strip the leading USB HID report-id byte and write the
 *    command payload. Some firmware expects a 0xC0 segment wrapper (REPORT_SEGMENT|LAST) —
 *    toggle TRITON_BLE_C0_WRAPPER if lizard-disable doesn't take.
 *  - Feature-read replies: we treat notifications on 100F6C34 as the reply (triton_feature_provide).
 *    If the controller does not NOTIFY on 100F6C34, switch to read-after-write (see writeReport:).
 */
#import "TritonBLEBridge.h"
#import <CoreBluetooth/CoreBluetooth.h>
#include "../usbip/triton_input_queue.h"
#include "../usbip/triton_device.h"

/* Set to 1 to wrap command writes as [0xC0][payload] (segment header REPORT_SEGMENT_DATA|LAST). */
#define TRITON_BLE_C0_WRAPPER 0

static NSString * const kTritonServiceUUID   = @"100F6C32-1735-4313-B402-38567131E5F3";
static NSString * const kTritonInputUUID     = @"100F6C7A-1735-4313-B402-38567131E5F3"; /* notify, 0x45 */
static NSString * const kTritonTimestampUUID = @"100F6C7C-1735-4313-B402-38567131E5F3"; /* notify, 0x47 */
static NSString * const kTritonReportUUID    = @"100F6C34-1735-4313-B402-38567131E5F3"; /* write/read    */

/* Set (lowercase) to the discovered haptic/output characteristic to LOCK rumble there; while nil,
 * OUTPUT writes SWEEP across every writable characteristic (~1s each) so we can feel which one
 * buzzes and read its UUID from the [Triton] RUMBLE SWEEP log line. */
/* OUTPUT reports route per-id to characteristic 100F6C<id+0x35> (computed in handleHostWriteKind);
 * no fixed index/UUID needed. candidateChars remains only as a discovery-order fallback. */

@interface TritonBLEBridge () <CBCentralManagerDelegate, CBPeripheralDelegate>
@property (nonatomic, strong) CBCentralManager *central;
@property (nonatomic, strong) CBPeripheral     *controller;
@property (nonatomic, strong) CBCharacteristic *inputChar;
@property (nonatomic, strong) CBCharacteristic *reportChar;
@property (nonatomic, assign) BOOL ready;
@property (nonatomic, strong) dispatch_queue_t bleQueue;
@property (nonatomic, strong) NSMutableDictionary *allChars;       /* uuid(lowercase) -> CBCharacteristic */
@property (nonatomic, strong) NSMutableArray *candidateChars;      /* writable chars, for the rumble sweep */
@end

@implementation TritonBLEBridge

- (instancetype)init {
    if ((self = [super init])) {
        /* HIGH priority: SDL's hid.m warns BLE packets are silently dropped if the consumer
         * stalls; keep this off the main/render thread. */
        _bleQueue = dispatch_queue_create("org.smallscale.triton.ble",
                       dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    }
    return self;
}

- (void)start {
    if (self.central) return;
    triton_input_queue_reset();
    self.central = [[CBCentralManager alloc] initWithDelegate:self queue:self.bleQueue];
    /* scanning begins once state == poweredOn (centralManagerDidUpdateState:) */
}

- (void)stop {
    if (self.inputChar && self.controller) {
        [self.controller setNotifyValue:NO forCharacteristic:self.inputChar];
    }
    if (self.controller) {
        [self.central cancelPeripheralConnection:self.controller];
    }
    [self.central stopScan];
    self.controller = nil; self.inputChar = nil; self.reportChar = nil;
    self.central = nil; self.ready = NO;
}

#pragma mark - CBCentralManagerDelegate

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    if (central.state == CBManagerStatePoweredOn) {
        [self acquireController];
    } else {
        NSLog(@"[Triton] BLE central state=%ld (need poweredOn)", (long)central.state);
    }
}

/* Acquire the Triton. PRIMARY path mirrors Valve's own iOS client (SDL hid.m:356-369): the
 * controller is normally already connected to the system (paired in iOS Settings), so it is NOT
 * advertising — find it via retrieveConnectedPeripheralsWithServices using Device Information
 * (0x180A, which every BLE device exposes) and filter by the "Steam" name prefix. Opening our own
 * handle to the CUSTOM Valve service coexists with iOS's standard-HID (0x1812) binding, so being
 * OS-paired is fine (this is exactly what Steam Link does). FALLBACK: scan for a controller that
 * is advertising in first-time pairing mode. */
- (void)acquireController {
    NSArray<CBPeripheral *> *connected =
        [self.central retrieveConnectedPeripheralsWithServices:@[[CBUUID UUIDWithString:@"180A"]]];
    for (CBPeripheral *p in connected) {
        if ([p.name hasPrefix:@"Steam"]) {
            NSLog(@"[Triton] found OS-connected controller '%@'", p.name);
            self.controller = p;                 /* retain before connecting */
            [self.central connectPeripheral:p options:nil];
            return;
        }
    }
    NSLog(@"[Triton] no OS-connected Steam controller; scanning for an advertising one");
    [self.central scanForPeripheralsWithServices:@[[CBUUID UUIDWithString:kTritonServiceUUID]] options:nil];
}

- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *,id> *)advertisementData RSSI:(NSNumber *)RSSI {
    NSLog(@"[Triton] discovered %@ (RSSI %@)", peripheral.name, RSSI);
    self.controller = peripheral;        /* retain before connecting */
    [central stopScan];
    [central connectPeripheral:peripheral options:nil];
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral {
    NSLog(@"[Triton] connected; discovering services");
    peripheral.delegate = self;
    [peripheral discoverServices:@[[CBUUID UUIDWithString:kTritonServiceUUID]]];
}

- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)peripheral
                 error:(NSError *)error {
    NSLog(@"[Triton] disconnected (%@); re-acquiring", error.localizedDescription);
    self.ready = NO; self.inputChar = nil; self.reportChar = nil;
    if (self.central.state == CBManagerStatePoweredOn) {
        [self acquireController];   /* prefer the OS-connected controller, then scan */
    }
}

#pragma mark - CBPeripheralDelegate

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error {
    for (CBService *svc in peripheral.services) {
        [peripheral discoverCharacteristics:nil forService:svc];
    }
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverCharacteristicsForService:(CBService *)service
             error:(NSError *)error {
    if (!self.allChars) self.allChars = [NSMutableDictionary dictionary];
    if (!self.candidateChars) self.candidateChars = [NSMutableArray array];
    for (CBCharacteristic *ch in service.characteristics) {
        NSString *u = ch.UUID.UUIDString;
        self.allChars[u.lowercaseString] = ch;
        /* %{public}s — an %@/object arg reads back as <private>/<decode: missing data>; a C string
         * lands in the log. props bits: 0x02 read, 0x04 writeNoResp, 0x08 write, 0x10 notify. */
        BOOL writable = (ch.properties & (CBCharacteristicPropertyWrite | CBCharacteristicPropertyWriteWithoutResponse)) != 0;
        if (writable) [self.candidateChars addObject:ch];
        NSLog(@"[Triton] char %{public}s props=0x%02lx%{public}s", [u UTF8String],
              (unsigned long)ch.properties, writable ? " [writable]" : "");
        if ([u caseInsensitiveCompare:kTritonInputUUID] == NSOrderedSame) {
            self.inputChar = ch;
            [peripheral setNotifyValue:YES forCharacteristic:ch];
            NSLog(@"[Triton] subscribed input char (0x45)");
        } else if ([u caseInsensitiveCompare:kTritonTimestampUUID] == NSOrderedSame) {
            [peripheral setNotifyValue:YES forCharacteristic:ch];   /* 0x47 (queue ignores it for now) */
        } else if ([u caseInsensitiveCompare:kTritonReportUUID] == NSOrderedSame) {
            self.reportChar = ch;
            if (ch.properties & CBCharacteristicPropertyNotify) {
                [peripheral setNotifyValue:YES forCharacteristic:ch];  /* feature replies */
            }
        }
    }
    NSLog(@"[Triton] %lu chars total, %lu writable candidates for the rumble sweep",
          (unsigned long)self.allChars.count, (unsigned long)self.candidateChars.count);
}

- (void)peripheral:(CBPeripheral *)peripheral didUpdateValueForCharacteristic:(CBCharacteristic *)ch
             error:(NSError *)error {
    if (error || ch.value.length == 0) return;
    const unsigned char *bytes = (const unsigned char *)ch.value.bytes;
    int len = (int)ch.value.length;
    NSString *u = ch.UUID.UUIDString;

    BOOL isInput     = ([u caseInsensitiveCompare:kTritonInputUUID]     == NSOrderedSame);
    BOOL isTimestamp = ([u caseInsensitiveCompare:kTritonTimestampUUID] == NSOrderedSame);
    if (isInput || isTimestamp) {
        /* The GATT characteristic VALUE is the raw report payload with NO HID report-id byte
         * (the report-id is implied by the characteristic UUID). The queue's contract is
         * [report-id][payload], so prepend the id here: 0x45 for the input/state char, 0x47 for
         * the timestamp char. Without this the queue saw payload[0] (= seq_num) != 0x45 and
         * dropped every frame, leaving the synthetic stuck on its neutral all-zero report. */
        unsigned char framed[64];
        framed[0] = isInput ? TRITON_BLE_STATE_ID : 0x47;
        int n = len;
        if (n > (int)sizeof(framed) - 1) n = (int)sizeof(framed) - 1;
        memcpy(framed + 1, bytes, (size_t)n);

        /* Diagnostic: log the first few + 1-in-200 RAW payloads (public so the hex is visible)
         * so we can read the real wire format — seq, sticks, and where the live IMU/gyro lands. */
        static int s_in = 0;
        s_in++;
        if (s_in <= 8 || (s_in % 200) == 0) {
            NSMutableString *h = [NSMutableString string];
            for (int i = 0; i < len && i < 32; i++) [h appendFormat:@"%02x ", bytes[i]];
            NSLog(@"[Triton] BLE in #%d char=%@ len=%d raw: %{public}@",
                  s_in, isInput ? @"input" : @"tstamp", len, h);
        }
        triton_input_push_ble(framed, n + 1);
        if (isInput && !self.ready) {
            self.ready = YES;
            NSLog(@"[Triton] first input report (%d bytes) — device ready", len);
            if (self.onReady) self.onReady();
        }
    } else if ([u caseInsensitiveCompare:kTritonReportUUID] == NSOrderedSame) {
        /* Controller's reply to a feature command -> the round-trip cell. The host's
         * GET_FEATURE prepends the report-id itself; we hand over the raw reply payload. */
        triton_feature_provide(bytes, len);
    }
}

#pragma mark - Host -> controller writes

- (void)handleHostWriteKind:(int)kind data:(const unsigned char *)data len:(int)len {
    if (len < 2 || data == NULL) return;

    /* FEATURE writes ([0x01][0x87 settings…]): strip the 0x01 channel report-id, write the command
     * to the report char 100F6C34 (the proven gyro/lizard path). OUTPUT writes ([0xNN][payload…]):
     * Valve routes each report id 0xNN to its OWN characteristic 100F6C<NN+0x35> (id stripped — it
     * only selects the char). 0x80 rumble->B5, 0x81 trackpad pulse->B6, 0x82 haptic cmd->B7, etc. */
    const unsigned char *payload = data + 1;          /* default: strip the leading report-id */
    int plen = len - 1;
    CBCharacteristic *target = self.reportChar;        /* feature commands ride 100F6C34 */

    if (kind == TRITON_WRITE_OUTPUT) {
        unsigned char rid = data[0];
        int slen;                                      /* declared STRIPPED length (wire = +1) */
        switch (rid) {
            case 0x80: slen = 9; break;   /* grip rumble    -> 100F6CB5 (left/right motor fields)  */
            case 0x81: slen = 7; break;   /* trackpad pulse -> 100F6CB6 (side: 01=L 02=R 03=both)  */
            case 0x82: slen = 3; break;   /* haptic command -> 100F6CB7 (Steam ping/test buzz)     */
            case 0x83: slen = 9; break;   /* LFO tone       -> 100F6CB8 */
            case 0x84: slen = 8; break;   /* log sweep      -> 100F6CB9 */
            case 0x85: slen = 3; break;   /* script         -> 100F6CBA */
            case 0x86: slen = 3; break;   /* vendor         -> 100F6CBB */
            case 0x87: case 0x88: case 0x89: slen = 63; break;  /* vendor big -> 100F6CBC/BD/BE */
            default:   slen = len - 1; break;
        }
        if (slen > len - 1) slen = len - 1;            /* clamp to what arrived (Windows pads to 64) */
        plen = slen;                                   /* payload already = data + 1 */

        NSString *uuid = [NSString stringWithFormat:@"100f6c%02x-1735-4313-b402-38567131e5f3",
                          (rid + 0x35) & 0xff];
        target = self.allChars[uuid];
        if (!target && self.candidateChars.count > 0) {  /* firmware != id+0x35: sweep, never drop */
            static int s_out = 0;
            NSUInteger idx = (s_out / 25) % self.candidateChars.count;
            if ((s_out % 25) == 0)
                NSLog(@"[Triton] no per-report char for id=0x%02x — sweeping candidate %lu", rid, (unsigned long)idx);
            s_out++;
            target = self.candidateChars[idx];
        }
    }

    /* Diagnostic (public, C string). Always log OUTPUT (haptics); throttle the kind=1 keepalive. */
    static int s_w = 0; s_w++;
    if (kind == TRITON_WRITE_OUTPUT || s_w <= 6 || (s_w % 20) == 0) {
        char hx[44]; int o = 0;
        for (int i = 0; i < plen && i < 13 && o < (int)sizeof(hx) - 3; i++)
            o += snprintf(hx + o, sizeof(hx) - o, "%02x ", payload[i]);
        NSLog(@"[Triton] host write kind=%d id=0x%02x inLen=%d -> %dB char=...%02x: %{public}s",
              kind, data[0], len, plen, (unsigned)(kind == TRITON_WRITE_OUTPUT ? (data[0] + 0x35) & 0xff : 0x34), hx);
    }

    /* Copy NOW — the C buffer does not outlive this call — then write on the BLE queue. */
    NSMutableData *out = [NSMutableData data];
#if TRITON_BLE_C0_WRAPPER
    unsigned char seg = 0xC0;                 /* REPORT_SEGMENT_DATA_FLAG | LAST_FLAG (spec §9.5) */
    [out appendBytes:&seg length:1];
#endif
    [out appendBytes:payload length:(NSUInteger)plen];
    CBCharacteristic *tch = target;
    dispatch_async(self.bleQueue, ^{
        if (self.controller && tch) {
            CBCharacteristicWriteType wt = (tch.properties & CBCharacteristicPropertyWrite)
                ? CBCharacteristicWriteWithResponse : CBCharacteristicWriteWithoutResponse;
            [self.controller writeValue:out forCharacteristic:tch type:wt];
        }
    });
}

@end
