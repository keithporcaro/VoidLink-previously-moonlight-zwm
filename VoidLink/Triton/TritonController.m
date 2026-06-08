/* TritonController.m — see TritonController.h. */
#import "TritonController.h"
#import "TritonBLEBridge.h"
#import "ControllerSupport.h"
#include "usbip/triton_device.h"

/* C trampoline so the USB/IP server's C write-sink can reach the Obj-C BLE bridge.
 * Called on the server thread; the bridge copies the bytes and dispatches the GATT write. */
static __weak TritonBLEBridge *g_sinkBridge = nil;
static void triton_write_sink_trampoline(int kind, const unsigned char *data, int len) {
    TritonBLEBridge *b = g_sinkBridge;
    if (b) [b handleHostWriteKind:kind data:data len:len];
}

@interface TritonController ()
@property (nonatomic, strong) TritonBLEBridge   *bridge;
@property (nonatomic, strong) NSThread          *serverThread;
@property (nonatomic, weak)   ControllerSupport *controllerSupport;
@property (nonatomic, assign) BOOL active;
@end

@implementation TritonController

+ (instancetype)shared {
    static TritonController *s; static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [[TritonController alloc] init]; });
    return s;
}

- (void)startWithControllerSupport:(ControllerSupport *)cs {
    @synchronized (self) {
        if (self.active) return;
        self.controllerSupport = cs;

        /* 1) Wire Steam -> controller writes (SET_REPORT / interrupt-OUT -> BLE). */
        self.bridge = [[TritonBLEBridge alloc] init];
        g_sinkBridge = self.bridge;
        triton_set_write_sink(triton_write_sink_trampoline);

        /* 2) Once the real controller is feeding input, suppress Voidlink's normal gamepad
         *    path so its input is not ALSO sent to the host as a generic Moonlight gamepad. */
        __weak typeof(self) wself = self;
        self.bridge.onReady = ^{
            __strong typeof(wself) sself = wself;
            sself.controllerSupport.usbipSteamControllerActive = YES;
            /* For full-fidelity live GET_FEATURE round-trips (spec §9.5), enable once the BLE
             * reply framing is verified on-device:  triton_set_feature_live(1);
             * Off by default keeps the proven synthetic/echo responder (GREEN). */
            NSLog(@"[Triton] live — suppressing Voidlink's normal gamepad path");
        };
        [self.bridge start];

        /* 3) Run the USB/IP server (blocking) on a dedicated thread so the host can attach. */
        self.serverThread = [[NSThread alloc] initWithBlock:^{ triton_usbip_start(); }];
        self.serverThread.name = @"triton-usbip";
        self.serverThread.qualityOfService = NSQualityOfServiceUserInitiated;
        [self.serverThread start];

        self.active = YES;
        NSLog(@"[Triton] started (USB/IP server :3240 + BLE bridge)");
    }
}

- (void)stop {
    @synchronized (self) {
        if (!self.active) return;
        self.controllerSupport.usbipSteamControllerActive = NO;
        triton_set_feature_live(0);
        triton_set_write_sink(NULL);
        g_sinkBridge = nil;

        triton_usbip_stop();        /* unblocks triton_usbip_start -> serverThread exits */
        [self.bridge stop];
        self.bridge = nil;
        self.serverThread = nil;
        self.active = NO;
        NSLog(@"[Triton] stopped");
    }
}

@end
