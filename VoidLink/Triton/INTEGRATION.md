# Triton (synthetic Steam Controller over USB/IP) — Voidlink integration

Phase 1 of the iPad passthrough. The C core (`usbip/`) is Tier-1-validated on Windows/WSL and
**Steam-GREEN-parity confirmed**; the Obj-C BLE bridge + lifecycle here build only on **Mac + Xcode
+ a physical iPad** (the Simulator has no Bluetooth radio).

## Files

```
Triton/
  usbip/                 # Tier-1, platform-neutral C (also builds on WSL via its Makefile)
    usbip.{c,h}          # USB/IP protocol core (ported: __APPLE__ gates, no exit(), stoppable)
    triton_device.{c,h}  # Triton identity + 372B descriptor + synthetic feature responder
    triton_report.h, triton_report_desc.h
    triton_input_queue.{c,h}   # BLE->USB seam + feature round-trip cell (unit-tested)
    triton_main.c, Makefile, test_triton_input_queue.c   # Tier-1 harness — EXCLUDE from the app target
  ble/
    TritonBLEBridge.{h,m}      # hand-written CoreBluetooth client (Valve GATT)
  TritonController.{h,m}       # lifecycle facade (server thread + bridge + suppression)
  INTEGRATION.md               # this file
```

## Xcode project changes (do in the UI — do NOT hand-edit project.pbxproj)

1. **Add to the app target's Compile Sources:** `usbip/usbip.c`, `usbip/triton_device.c`,
   `usbip/triton_input_queue.c`, `ble/TritonBLEBridge.m`, `TritonController.m`.
   **Do NOT add** `triton_main.c` (has its own `main()`) or `test_triton_input_queue.c`.
2. **Header Search Paths** (recursive): add `$(SRCROOT)/VoidLink/Triton/**`.
3. **Link** `CoreBluetooth.framework`.
4. Ensure `LINUX` is **not** in `GCC_PREPROCESSOR_DEFINITIONS` (the code takes the `__APPLE__`
   branch automatically). Do **not** define `TRITON_VERBOSE` (keeps `usbip.c` silent).
5. **Symbol check:** `nm libs/SDL2/lib/iOS/libSDL2.a | grep -i hid_` — this prebuilt SDL2 has no
   HIDAPI symbols (confirmed), so there is no collision; we bring our own CoreBluetooth client and
   do NOT vendor SDL's `hid.m`.

## Source edits already applied (in this branch)

- `Input/ControllerSupport.h` — added `@property (atomic, assign) BOOL usbipSteamControllerActive;`
- `Input/ControllerSupport.m` — `updateFinished:` guards `LiSendMultiControllerEvent` with
  `if (!self.usbipSteamControllerActive)` (full suppression while the USB/IP controller is active).
- `ViewControllers/StreamFrameViewController.m` — `#import "TritonController.h"`; start in
  `connectionStarted` (after `connectionEstablished`, on a background queue); stop in
  `connectionTerminated:`. (Consider also stopping in `launchFailed:` and on
  `UIApplicationWillResignActiveNotification` to release BLE while backgrounded.)
- `Limelight-Info.plist` — reworded the two Bluetooth usage strings for Steam-Controller clarity
  (`NSBluetoothAlwaysUsageDescription` / `NSBluetoothPeripheralUsageDescription`). The needed keys
  (`NSBluetoothAlwaysUsageDescription`, `NSLocalNetworkUsageDescription`) were already present.

## How it runs

`connectionStarted` → `[[TritonController shared] startWithControllerSupport:]` →
(a) `TritonBLEBridge` acquires the controller (via `retrieveConnectedPeripherals` — works while
OS-paired; scan is a first-pairing fallback), opens the custom Valve service `100F6C32…`, subscribes
input char `100F6C7A` (`0x45`), pushes raw reports into `triton_input_queue`; (b) the USB/IP server runs on a dedicated thread
(`triton_usbip_start`) so the host attaches with usbip-win2; (c) on first input report,
`usbipSteamControllerActive=YES` suppresses the normal gamepad path. Steam's writes flow back:
server write-sink → C trampoline → `TritonBLEBridge handleHostWriteKind:` → GATT write to
`100F6C34`. `connectionTerminated:` → `[[TritonController shared] stop]`.

Host side (Vibepollo / manual): `usbip attach -r <ipad-ip> -b 1-1 --once` on stream start;
`usbip detach -a` on stop (Phase 2 wires this via `client_do_cmds`/`undo_cmds`).

## Gyro / IMU — enable-gated; the liveness gate + on-device test

**Proven on real hardware (genuine controller, native USB, Steam open):** the IMU is OFF by
default — the controller streams accel/gyro only after the host writes `GYRO_MODE`. Steam's
*desktop* config never enables it, so the IMU block (the tail of the `0x45`/`0x42` report, wire
bytes 30–45) arrives **frozen** at a stale non-zero sample; a frozen non-zero gyro reads to Steam
as a *constant* rotation and flies the desktop cursor. The enable command (confirmed both ways —
`0x18` on, `0x0000` off, via `tools/triton-usbip/win/HidInputDump.cs` `ProbeGyro`):

```
feature report 0x01:  87 03 30 18 00
= WRITE_REGISTER, len 3, reg 0x30 (GYRO_MODE), value 0x0018 (raw accel | raw gyro)
```

`triton_input_queue` gates the IMU by **liveness** (`triton_imu_is_live`): pass the IMU through
while its u32 timestamp is advancing (gyro enabled), zero it while frozen (gyro off). Self-
correcting, no flag to flip — when Steam enables gyro the live data flows; otherwise Steam gets
zeros and the cursor stays calm. **Do NOT self-enable gyro** (a permanent enable re-flies the
desktop cursor); let Steam's own enable write drive it. (`TRITON_BLE_LIVE_IMU` disables the gate
for the bench A/B server only.)

**On-device test:**
1. *Baseline (desktop):* cursor calm, sticks/buttons live — the gate zeroing the frozen IMU.
2. *Live gyro:* make Steam ask for it — Steam Input → Gyro = "As Mouse"/"As Joystick", **Always
   On**, or launch a game with native gyro. Steam then sends `87 03 30 18`, which our pipe forwards
   over BLE to `100F6C34`; the controller's IMU timestamp starts advancing and the gate passes the
   live data through.
3. *Capture (Console.app, filter `[Triton]`):* after enabling, do the `BLE in #N` lines show the
   IMU tail of the `0x45` frame **changing**? Changing → live gyro end-to-end. Still frozen → the
   enable write isn't taking → set `TRITON_BLE_C0_WRAPPER 1` (below) and retry.

**Optional fast diagnostic (prove gate + BLE forward without a gyro game):** temporarily write
`{0x87,0x03,0x30,0x18,0x00}` to `100F6C34` once on connect (after subscribing). If the IMU wakes in
the BLE logs and, in a gyro→mouse desktop config, the cursor moves *with* the controller, the gate's
live branch is confirmed over BLE. **Remove before shipping.**

## On-device verification / open items (cannot be checked on Windows — design spec §9.5)

- **Write framing to `100F6C34`:** we strip the leading USB report-id and write the command
  payload. If lizard-disable (or the enable-gyro `87 03 30 18` write — see the Gyro / IMU section)
  doesn't take, set `TRITON_BLE_C0_WRAPPER 1` in `TritonBLEBridge.m` (wraps as `[0xC0][payload]`).
  Capture the genuine write with a BLE sniffer to confirm.
- **Feature-read replies:** we treat notifications on `100F6C34` as the reply
  (`triton_feature_provide`). If the controller does not NOTIFY there, switch to read-after-write.
  Then enable live reads by uncommenting `triton_set_feature_live(1)` in `TritonController.m`
  (off by default = the proven synthetic/echo responder that won the GREEN gate).
- **Haptic/output channel:** outputs currently ride `100F6C34`; the real haptic characteristic may
  differ — verify against rumble.
- **OS-paired is FINE (corrected):** the controller can be paired normally in iOS Settings — Steam
  Link relies on exactly that. `TritonBLEBridge` acquires it via `retrieveConnectedPeripheralsWithServices`
  (the OS-connected set, like Valve's `hid.m`), then opens the CUSTOM service alongside iOS's standard
  HID binding; lizard mode is cleared by the gamepad-enable write. (The earlier spec §4.2 "must NOT be
  OS-paired" claim is wrong.) The `scanForPeripherals` path remains only as a first-time-pairing fallback.
- `triton_device.c` still uses `printf` for its low-volume control-transfer trace; route to `NSLog`
  or gate if you want a fully silent release build.
