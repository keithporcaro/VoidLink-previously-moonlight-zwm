//
//  SteamControllerBridge.h
//  Moonlight
//
//  Reads a 2026 Steam Controller via SDL3 and forwards its inputs to the host using
//  the Steam Controller Moonlight protocol extension (LiSendControllerSteamExtendedEvent /
//  LiSendControllerSteamTrackpadEvent), plus the regular XInput-shaped multi-controller
//  packet for backwards compatibility.
//
//  Apple's GCController framework does not surface the SC's paddles, trackpads, grip
//  sensors, or linear haptics, and the controller defaults to "Lizard mode" (keyboard +
//  mouse). SDL3 (libsdl-org/SDL PR #15528) implements the vendor HID driver for this
//  device — including taking it out of Lizard mode — so we read it through SDL_Gamepad
//  rather than reimplementing the BLE/HID stack here.
//
//  This whole translation unit is compiled out unless SDL3 headers are available, so the
//  app continues to build before SDL3 is added to the Xcode project. See the build notes
//  in SteamControllerBridge.m.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface SteamControllerBridge : NSObject

// YES if this build was compiled with SDL3 support. When NO, all methods are no-ops.
@property (class, nonatomic, readonly) BOOL isAvailable;

// When NO, the bridge still streams the base XInput-shaped state (so the SC works as a
// plain gamepad) but suppresses the SC-specific packets. Backs the "Use as Steam
// Controller" per-host setting. Defaults to YES.
@property (nonatomic) BOOL steamControllerEmulationEnabled;

// playerIndex is the Limelight controller number reserved for the Steam Controller.
- (instancetype)initWithPlayerIndex:(uint8_t)playerIndex;

// Begins SDL gamepad discovery + polling. Call when a streaming session starts.
- (void)start;

// Stops polling and closes any open SC. SDL3 re-enables Lizard mode on close. Call when
// the session ends.
- (void)stop;

@end

NS_ASSUME_NONNULL_END
