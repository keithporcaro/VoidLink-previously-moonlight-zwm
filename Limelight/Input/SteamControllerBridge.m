//
//  SteamControllerBridge.m
//  Moonlight
//
//  Build notes:
//   - This file is active only when SDL3 headers are reachable (#if __has_include below).
//     To enable it, add SDL3 to the Xcode project (e.g. an SDL3.xcframework built for iOS,
//     or SwiftPM/CocoaPods), link it, and add `NSBluetoothAlwaysUsageDescription` to
//     Info.plist (the SC pairs over BLE). No `bluetooth-central` background mode is needed;
//     the bridge runs only during an active streaming session.
//   - Pin SDL3 to a release commit that includes libsdl-org/SDL PR #15528 (2026 Steam
//     Controller HID driver), not `main`.
//   - Until SDL3 is added, the bridge compiles to a no-op (isAvailable == NO) so the app
//     keeps building.
//

#import "SteamControllerBridge.h"

#if __has_include(<SDL3/SDL.h>)

#import <SDL3/SDL.h>
#include <Limelight.h>

// 2026 Steam Controller USB IDs. VID is constant; accept both wired and dongle PIDs.
static const uint16_t SC_VENDOR_ID = 0x28DE;
static const uint16_t SC_PRODUCT_ID_WIRED = 0x1302;
static const uint16_t SC_PRODUCT_ID_DONGLE = 0x1304;

// SDL trackpad index -> SC pad. NOTE: confirm this mapping during the SDL3 bring-up spike;
// SDL may enumerate the pads in either order for this device.
static const int SC_SDL_TOUCHPAD_LEFT = 0;
static const int SC_SDL_TOUCHPAD_RIGHT = 1;

typedef struct {
    bool wasDown;
    float lastX;
    float lastY;
} sc_pad_state_t;

@interface SteamControllerBridge () {
    uint8_t _playerIndex;
    SDL_Gamepad *_gamepad;       // NULL until an SC is opened
    SDL_JoystickID _instanceId;  // 0 when none open
    dispatch_source_t _pollTimer;
    sc_pad_state_t _padState[2];
    uint16_t _lastScExtButtonFlags;
    uint16_t _lastGripL;
    uint16_t _lastGripR;
    BOOL _arrivalReported;
}
@end

@implementation SteamControllerBridge

+ (BOOL)isAvailable {
    return YES;
}

- (instancetype)initWithPlayerIndex:(uint8_t)playerIndex {
    self = [super init];
    if (self) {
        _playerIndex = playerIndex;
        _steamControllerEmulationEnabled = YES;
        _gamepad = NULL;
        _instanceId = 0;
    }
    return self;
}

- (void)start {
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        NSLog(@"SteamControllerBridge: SDL_Init failed: %s", SDL_GetError());
        return;
    }

    // Pick up any SC that's already connected.
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids) {
        for (int i = 0; i < count; i++) {
            [self tryOpenGamepad:ids[i]];
        }
        SDL_free(ids);
    }

    // Poll at ~60 Hz on a background queue. SDL HIDAPI event processing is driven by
    // SDL_PumpEvents/SDL_UpdateGamepads which we call here, so we don't need SDL's own
    // event loop or a window.
    _pollTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                        dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0));
    dispatch_source_set_timer(_pollTimer, dispatch_time(DISPATCH_TIME_NOW, 0),
                              (uint64_t)(NSEC_PER_SEC / 60), (uint64_t)(NSEC_PER_SEC / 240));
    __weak SteamControllerBridge *weakSelf = self;
    dispatch_source_set_event_handler(_pollTimer, ^{
        [weakSelf pollOnce];
    });
    dispatch_resume(_pollTimer);
}

- (void)stop {
    if (_pollTimer) {
        dispatch_source_cancel(_pollTimer);
        _pollTimer = nil;
    }
    if (_gamepad) {
        // SDL re-enables Lizard mode (keyboard+mouse fallback) on close.
        SDL_CloseGamepad(_gamepad);
        _gamepad = NULL;
        _instanceId = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

#pragma mark - Discovery

- (BOOL)isSteamController:(SDL_Gamepad *)gamepad {
    uint16_t vid = SDL_GetGamepadVendor(gamepad);
    uint16_t pid = SDL_GetGamepadProduct(gamepad);
    return vid == SC_VENDOR_ID && (pid == SC_PRODUCT_ID_WIRED || pid == SC_PRODUCT_ID_DONGLE);
}

- (void)tryOpenGamepad:(SDL_JoystickID)instanceId {
    if (_gamepad) {
        return;  // Already managing one SC
    }
    SDL_Gamepad *gp = SDL_OpenGamepad(instanceId);
    if (!gp) {
        return;
    }
    if (![self isSteamController:gp]) {
        SDL_CloseGamepad(gp);
        return;
    }

    _gamepad = gp;
    _instanceId = instanceId;
    _arrivalReported = NO;
    memset(_padState, 0, sizeof(_padState));

    // Enable motion sensors if present.
    if (SDL_GamepadHasSensor(gp, SDL_SENSOR_ACCEL)) {
        SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_ACCEL, true);
    }
    if (SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO)) {
        SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_GYRO, true);
    }

    NSLog(@"SteamControllerBridge: opened Steam Controller (instance %u)", (unsigned)instanceId);
}

- (void)reportArrivalIfNeeded {
    if (_arrivalReported) {
        return;
    }

    uint32_t supportedButtonFlags =
        A_FLAG | B_FLAG | X_FLAG | Y_FLAG |
        UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG |
        LB_FLAG | RB_FLAG | LS_CLK_FLAG | RS_CLK_FLAG |
        PLAY_FLAG | BACK_FLAG | SPECIAL_FLAG |
        PADDLE1_FLAG | PADDLE2_FLAG | PADDLE3_FLAG | PADDLE4_FLAG;

    // type stays XBOX: the host's HIDMaestro emulator switches to Steam Controller mode
    // based on the LI_CCAP_SC_HAPTICS capability + arriving SC packets, not on type.
    uint16_t capabilities = LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_ACCEL | LI_CCAP_GYRO |
                            LI_CCAP_RUMBLE | LI_CCAP_SC_HAPTICS;

    if (LiSendControllerArrivalEvent(_playerIndex, (uint16_t)(1 << _playerIndex), LI_CTYPE_XBOX,
                                     supportedButtonFlags, capabilities) == 0) {
        _arrivalReported = YES;
    }
}

#pragma mark - Polling

- (void)pollOnce {
    SDL_PumpEvents();

    // Drain device add/remove events.
    SDL_Event event;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_GAMEPAD_ADDED, SDL_EVENT_GAMEPAD_REMOVED) > 0) {
        if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
            [self tryOpenGamepad:event.gdevice.which];
        }
        else if (event.type == SDL_EVENT_GAMEPAD_REMOVED && event.gdevice.which == _instanceId) {
            if (_gamepad) {
                SDL_CloseGamepad(_gamepad);
                _gamepad = NULL;
                _instanceId = 0;
            }
        }
    }

    if (!_gamepad) {
        return;
    }

    SDL_UpdateGamepads();
    [self reportArrivalIfNeeded];
    if (!_arrivalReported) {
        return;  // Connection not fully established yet; try again next tick.
    }

    [self emitBaseState];
    [self emitMotion];
    [self emitBattery];
    if (_steamControllerEmulationEnabled) {
        [self emitExtendedState];
        [self emitTrackpads];
    }
}

static int16_t invert_axis(int16_t v) {
    // SDL: +Y is down. Limelight: +Y is up. Negate, guarding against INT16_MIN overflow.
    return v == INT16_MIN ? INT16_MAX : (int16_t)(-v);
}

- (void)emitBaseState {
    SDL_Gamepad *gp = _gamepad;

    uint32_t buttonFlags = 0;
    #define MAPB(sdlButton, liFlag) if (SDL_GetGamepadButton(gp, sdlButton)) buttonFlags |= (liFlag)
    MAPB(SDL_GAMEPAD_BUTTON_SOUTH, A_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_EAST, B_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_WEST, X_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_NORTH, Y_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_DPAD_UP, UP_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_DPAD_DOWN, DOWN_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_DPAD_LEFT, LEFT_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, RIGHT_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, LB_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, RB_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_LEFT_STICK, LS_CLK_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_RIGHT_STICK, RS_CLK_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_START, PLAY_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_BACK, BACK_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_GUIDE, SPECIAL_FLAG);
    // Paddles also go in the legacy bitmap so non-SC-aware hosts still see them.
    MAPB(SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, PADDLE1_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, PADDLE2_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, PADDLE3_FLAG);
    MAPB(SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, PADDLE4_FLAG);
    #undef MAPB

    uint8_t leftTrigger = (uint8_t)(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * 255 / 32767);
    uint8_t rightTrigger = (uint8_t)(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 255 / 32767);
    int16_t lsX = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX);
    int16_t lsY = invert_axis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY));
    int16_t rsX = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX);
    int16_t rsY = invert_axis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));

    LiSendMultiControllerEvent(_playerIndex, (uint16_t)(1 << _playerIndex), buttonFlags,
                               leftTrigger, rightTrigger, lsX, lsY, rsX, rsY);
}

- (void)emitExtendedState {
    SDL_Gamepad *gp = _gamepad;

    uint16_t flags = 0;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1))  flags |= SCEX_PADDLE_L4;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2))  flags |= SCEX_PADDLE_L5;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1)) flags |= SCEX_PADDLE_R4;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2)) flags |= SCEX_PADDLE_R5;
    if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_GUIDE))         flags |= SCEX_STEAM_BUTTON;

    // Trackpad capacitive-touch + click, derived from SDL touchpad finger state.
    bool lDown = false, rDown = false; float lp = 0, rp = 0, tmp;
    if (SDL_GetNumGamepadTouchpads(gp) > SC_SDL_TOUCHPAD_LEFT) {
        SDL_GetGamepadTouchpadFinger(gp, SC_SDL_TOUCHPAD_LEFT, 0, &lDown, &tmp, &tmp, &lp);
    }
    if (SDL_GetNumGamepadTouchpads(gp) > SC_SDL_TOUCHPAD_RIGHT) {
        SDL_GetGamepadTouchpadFinger(gp, SC_SDL_TOUCHPAD_RIGHT, 0, &rDown, &tmp, &tmp, &rp);
    }
    if (lDown) flags |= SCEX_TP_LEFT_CAP_TOUCH;
    if (rDown) flags |= SCEX_TP_RIGHT_CAP_TOUCH;
    if (lp >= 1.0f) flags |= SCEX_TP_LEFT_CLICK;   // SDL reports pressure 1.0 on a hard click
    if (rp >= 1.0f) flags |= SCEX_TP_RIGHT_CLICK;

    // TODO(spike): capacitive stick touch (SCEX_LS_CAP_TOUCH / SCEX_RS_CAP_TOUCH) and analog
    // grip-cap (gripCapLeft/Right) once the SDL3 exposure for those on this device is pinned
    // down. For now grip is reported as binary 0/65535 if SDL surfaces a grip button.
    uint16_t gripL = 0;
    uint16_t gripR = 0;

    // Suppress redundant packets to keep the control channel quiet.
    if (flags == _lastScExtButtonFlags && gripL == _lastGripL && gripR == _lastGripR) {
        return;
    }
    _lastScExtButtonFlags = flags;
    _lastGripL = gripL;
    _lastGripR = gripR;

    LiSendControllerSteamExtendedEvent(_playerIndex, flags, gripL, gripR, 0);
}

- (void)emitTrackpads {
    [self emitTrackpad:SC_SDL_TOUCHPAD_LEFT padIndex:SC_TRACKPAD_LEFT];
    [self emitTrackpad:SC_SDL_TOUCHPAD_RIGHT padIndex:SC_TRACKPAD_RIGHT];
}

- (void)emitTrackpad:(int)sdlTouchpad padIndex:(uint8_t)padIndex {
    SDL_Gamepad *gp = _gamepad;
    if (SDL_GetNumGamepadTouchpads(gp) <= sdlTouchpad) {
        return;
    }

    bool down = false;
    float x = 0, y = 0, pressure = 0;
    SDL_GetGamepadTouchpadFinger(gp, sdlTouchpad, 0, &down, &x, &y, &pressure);

    sc_pad_state_t *st = &_padState[padIndex];
    uint8_t pressedFlags = 0;
    if (down) pressedFlags |= SCTP_FLAG_CAP_TOUCH;
    if (pressure >= 1.0f) pressedFlags |= SCTP_FLAG_CLICK;

    if (down && !st->wasDown) {
        LiSendControllerSteamTrackpadEvent(_playerIndex, padIndex, LI_TOUCH_EVENT_DOWN, pressedFlags, x, y, pressure);
    }
    else if (!down && st->wasDown) {
        LiSendControllerSteamTrackpadEvent(_playerIndex, padIndex, LI_TOUCH_EVENT_UP, pressedFlags, st->lastX, st->lastY, 0.0f);
    }
    else if (down && (x != st->lastX || y != st->lastY)) {
        LiSendControllerSteamTrackpadEvent(_playerIndex, padIndex, LI_TOUCH_EVENT_MOVE, pressedFlags, x, y, pressure);
    }

    st->wasDown = down;
    st->lastX = x;
    st->lastY = y;
}

- (void)emitMotion {
    SDL_Gamepad *gp = _gamepad;
    float data[3];
    if (SDL_GamepadSensorEnabled(gp, SDL_SENSOR_ACCEL) &&
        SDL_GetGamepadSensorData(gp, SDL_SENSOR_ACCEL, data, 3)) {
        LiSendControllerMotionEvent(_playerIndex, LI_MOTION_TYPE_ACCEL, data[0], data[1], data[2]);
    }
    if (SDL_GamepadSensorEnabled(gp, SDL_SENSOR_GYRO) &&
        SDL_GetGamepadSensorData(gp, SDL_SENSOR_GYRO, data, 3)) {
        // SDL reports gyro in rad/s; Limelight expects deg/s.
        LiSendControllerMotionEvent(_playerIndex, LI_MOTION_TYPE_GYRO,
                                    data[0] * 57.2957795f, data[1] * 57.2957795f, data[2] * 57.2957795f);
    }
}

- (void)emitBattery {
    int percent = -1;
    SDL_PowerState state = SDL_GetGamepadPowerInfo(_gamepad, &percent);
    uint8_t liState;
    switch (state) {
        case SDL_POWERSTATE_CHARGING: liState = LI_BATTERY_STATE_CHARGING; break;
        case SDL_POWERSTATE_CHARGED: liState = LI_BATTERY_STATE_FULL; break;
        case SDL_POWERSTATE_ON_BATTERY: liState = LI_BATTERY_STATE_DISCHARGING; break;
        case SDL_POWERSTATE_NO_BATTERY: liState = LI_BATTERY_STATE_NOT_PRESENT; break;
        default: liState = LI_BATTERY_STATE_UNKNOWN; break;
    }
    uint8_t liPercent = percent < 0 ? LI_BATTERY_PERCENTAGE_UNKNOWN : (uint8_t)percent;
    LiSendControllerBatteryEvent(_playerIndex, liState, liPercent);
}

@end

#else  // SDL3 not available — inert no-op implementation so the app still builds.

@implementation SteamControllerBridge

+ (BOOL)isAvailable {
    return NO;
}

- (instancetype)initWithPlayerIndex:(uint8_t)playerIndex {
    self = [super init];
    if (self) {
        _steamControllerEmulationEnabled = YES;
    }
    return self;
}

- (void)start {}
- (void)stop {}

@end

#endif
