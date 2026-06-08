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
        CBUUID *svc = [CBUUID UUIDWithString:kTritonServiceUUID];
        [central scanForPeripheralsWithServices:@[svc] options:nil];
        NSLog(@"[Triton] BLE scanning for Valve service %@", kTritonServiceUUID);
    } else {
        NSLog(@"[Triton] BLE central state=%ld (need poweredOn)", (long)central.state);
    }
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
    NSLog(@"[Triton] disconnected (%@); rescanning", error.localizedDescription);
    self.ready = NO; self.inputChar = nil; self.reportChar = nil;
    if (self.central.state == CBManagerStatePoweredOn) {
        [self.central scanForPeripheralsWithServices:@[[CBUUID UUIDWithString:kTritonServiceUUID]] options:nil];
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

    if ([u caseInsensitiveCompare:kTritonInputUUID] == NSOrderedSame ||
        [u caseInsensitiveCompare:kTritonTimestampUUID] == NSOrderedSame) {
        /* Raw state report ([0x45|0x47][payload]) straight into the BLE->USB seam. */
        triton_input_push_ble(bytes, len);
        if (!self.ready) {
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
    /* Strip the leading USB HID report-id byte (e.g. 0x01); BLE carries the command payload.
     * Copy NOW — the C buffer does not outlive this call — then write on the BLE queue
     * (CoreBluetooth peripheral ops must run on the central's queue). */
    NSMutableData *out = [NSMutableData data];
#if TRITON_BLE_C0_WRAPPER
    unsigned char seg = 0xC0;            /* REPORT_SEGMENT_DATA_FLAG | LAST_FLAG (spec §9.5) */
    [out appendBytes:&seg length:1];
#endif
    [out appendBytes:(data + 1) length:(NSUInteger)(len - 1)];
    (void)kind;   /* feature vs output both ride the report char in this first cut */

    dispatch_async(self.bleQueue, ^{
        if (self.controller && self.reportChar) {
            [self.controller writeValue:out forCharacteristic:self.reportChar
                                   type:CBCharacteristicWriteWithResponse];
        }
    });
}

@end
