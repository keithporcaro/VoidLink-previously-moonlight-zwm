/* TritonBLEBridge — a minimal CoreBluetooth client for the 2026 Steam Controller (Triton)
 * over Valve's CUSTOM GATT service (NOT standard HID-over-GATT 0x1812, which is why a
 * third-party app may read it raw — see design spec §4.2). Hand-written (no SDL dependency)
 * against the known UUIDs; reads raw input reports into triton_input_queue and writes
 * Steam's feature/output reports back to the controller.
 *
 * GATT (from SDL/src/hidapi/ios/hid.m, reference only):
 *   service  100F6C32-1735-4313-B402-38567131E5F3
 *   input    100F6C7A-...   (notify; report 0x45 state)
 *   timestamp100F6C7C-...   (notify; report 0x47)
 *   report   100F6C34-...   (write/read; feature + commands)
 *
 * Build: iOS only (Mac + Xcode + physical iPad — the Simulator has no BLE radio). Add
 * CoreBluetooth.framework. Requires NSBluetoothAlwaysUsageDescription (already in the plist).
 * OS-paired is FINE: the controller is normally connected via iOS Settings (Steam Link relies on
 * exactly this). We open our own handle to the CUSTOM Valve service, which coexists with iOS's
 * standard-HID (0x1812) binding; "lizard mode" is just the controller's default reporting, cleared
 * by the gamepad-enable write — it is not a barrier to accessing the custom service. So we acquire
 * via retrieveConnectedPeripheralsWithServices (the OS-connected set), and only scan as a fallback.
 */
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface TritonBLEBridge : NSObject

/* Start scanning for the Triton and wire it to triton_input_queue. Idempotent. */
- (void)start;

/* Stop notifications, disconnect, and tear down the central manager. */
- (void)stop;

/* YES once connected and at least one input report has arrived (device.ready). */
@property (nonatomic, readonly) BOOL ready;

/* Fired once, on the BLE queue, when the first input report arrives (ready transitions YES). */
@property (nonatomic, copy, nullable) void (^onReady)(void);

/* Fired on the BLE queue when the controller disconnects (e.g. powered off mid-session). The
 * facade un-suppresses Voidlink's normal gamepad path; the queue is reset so the synthetic stops
 * replaying the last frame. onReady fires again if the controller reconnects. */
@property (nonatomic, copy, nullable) void (^onDisconnect)(void);

/* Host -> controller write, called from the USB/IP server's write sink (via TritonController).
 * kind is TRITON_WRITE_FEATURE or TRITON_WRITE_OUTPUT (triton_device.h). Strips the leading
 * HID report-id and writes the payload to the Valve report characteristic 100F6C34. */
- (void)handleHostWriteKind:(int)kind data:(const unsigned char *)data len:(int)len;

@end

NS_ASSUME_NONNULL_END
