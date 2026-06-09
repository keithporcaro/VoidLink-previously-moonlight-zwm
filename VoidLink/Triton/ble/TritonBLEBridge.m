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

@interface TritonBLEBridge () <CBCentralManagerDelegate, CBPeripheralDelegate>
@property (nonatomic, strong) CBCentralManager *central;
@property (nonatomic, strong) CBPeripheral     *controller;
@property (nonatomic, strong) CBCharacteristic *inputChar;
@property (nonatomic, strong) CBCharacteristic *reportChar;
@property (nonatomic, assign) BOOL ready;
@property (nonatomic, strong) dispatch_queue_t bleQueue;
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
    for (CBCharacteristic *ch in service.characteristics) {
        NSString *u = ch.UUID.UUIDString;
        NSLog(@"[Triton] discovered char %@ props=0x%02lx", u, (unsigned long)ch.properties); /* surface any haptic/output char */
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

    /* Two write kinds reach us from the USB/IP write sink, and they frame the report-id differently:
     *  - TRITON_WRITE_FEATURE ([0x01][0x87 settings…]): the leading byte is the HID *channel*
     *    report-id (0x01); the real command (0x87) follows. The report char carries the command
     *    payload, so STRIP the channel byte. (This is the path the working gyro-enable takes.)
     *  - TRITON_WRITE_OUTPUT ([0x80][type][rumble…]): the leading byte IS the command discriminator
     *    (0x80 = HAPTIC_RUMBLE) — the controller needs it to recognise a rumble, so KEEP it. Windows
     *    also zero-pads outputs to OutputReportByteLength (64), so trim to the report's declared
     *    length (triton_report_desc.h) rather than forwarding 54 trailing zeros. */
    const unsigned char *payload;
    int plen;
    if (kind == TRITON_WRITE_OUTPUT) {
        payload = data;                       /* keep the 0x80 report-id (rumble discriminator) */
        switch (data[0]) {                    /* trim Windows' 64B zero-pad to the declared length */
            case 0x80: plen = 10; break;      /* HAPTIC_RUMBLE */
            case 0x81: plen = 8;  break;
            case 0x82: plen = 4;  break;
            case 0x83: plen = 10; break;
            case 0x84: plen = 9;  break;
            case 0x85: plen = 4;  break;
            case 0x86: plen = 4;  break;
            default:   plen = len; break;      /* 0x87/0x88/0x89 are genuinely 64B */
        }
        if (plen > len) plen = len;
    } else {
        payload = data + 1;                   /* strip the 0x01 channel report-id */
        plen    = len - 1;
    }

    /* Diagnostic (public): which kind, inbound vs trimmed length, and the leading bytes. */
    {
        NSMutableString *h = [NSMutableString string];
        for (int i = 0; i < plen && i < 12; i++) [h appendFormat:@"%02x ", payload[i]];
        NSLog(@"[Triton] host write kind=%d inLen=%d -> %dB to report char: %{public}@", kind, len, plen, h);
    }

    /* Copy NOW — the C buffer does not outlive this call — then write on the BLE queue
     * (CoreBluetooth peripheral ops must run on the central's queue). */
    NSMutableData *out = [NSMutableData data];
#if TRITON_BLE_C0_WRAPPER
    unsigned char seg = 0xC0;                 /* REPORT_SEGMENT_DATA_FLAG | LAST_FLAG (spec §9.5) */
    [out appendBytes:&seg length:1];
#endif
    [out appendBytes:payload length:(NSUInteger)plen];

    dispatch_async(self.bleQueue, ^{
        if (self.controller && self.reportChar) {
            [self.controller writeValue:out forCharacteristic:self.reportChar
                                   type:CBCharacteristicWriteWithResponse];
        }
    });
}

@end
