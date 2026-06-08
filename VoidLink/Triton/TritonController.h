/* TritonController — the single facade StreamFrameViewController talks to. Owns the USB/IP
 * server thread and the BLE bridge, wires Steam's writes back to BLE, and suppresses
 * Voidlink's normal gamepad path while the synthetic Steam Controller is active (so the real
 * controller's input is not ALSO sent to the host as a generic Moonlight gamepad).
 *
 * Lifecycle: -startWithControllerSupport: in connectionStarted, -stop in connectionTerminated.
 */
#import <Foundation/Foundation.h>

@class ControllerSupport;

NS_ASSUME_NONNULL_BEGIN

@interface TritonController : NSObject

+ (instancetype)shared;

/* Start the USB/IP server (so the host can attach) and the BLE bridge. Once the real
 * controller is feeding input, suppresses controllerSupport's normal gamepad path. */
- (void)startWithControllerSupport:(nullable ControllerSupport *)controllerSupport;

/* Stop the server + BLE, and release the gamepad-suppression flag. */
- (void)stop;

@property (nonatomic, readonly) BOOL active;

@end

NS_ASSUME_NONNULL_END
