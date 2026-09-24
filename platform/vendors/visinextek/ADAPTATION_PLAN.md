# VS816 adaptation plan

Target: bring VS816 to the same DarkOS HAL surface as the current RV1126B
implementation. A checked item means the code and cross-build are complete;
hardware acceptance remains explicit because it requires the VS816 board,
sensor and pin assignment.

## Capability matrix

| HAL | RV1126B baseline | VS816 implementation | Hardware acceptance |
| --- | --- | --- | --- |
| Camera | NV12, callback/capture, controls, preview, H.264 route | NV12 callback/capture and VII→VENC H.264 route | Pending |
| Codec | H.264 encode/decode | MAL VENC/VDEC H.264 encode/decode | Pending |
| Light | sysfs GPIO | shared sysfs GPIO backend | Pending pin map |
| Serial | termios/RS-485 | shared termios backend | Pending device map |
| Audio | PCM and G.711 | Not started | Pending |
| Display | output and frame queue | Not started | Pending |
| Graphics | OSD regions | Not started | Pending |
| NPU | optional RKNN | Not started; VS SDK NN libraries staged only | Pending model/API decision |

## Execution stages

1. **Baseline and SDK mapping — complete.** Map the stable DarkOS HAL ABI to
   VS MAL modules and keep vendor code below `platform/vendors/visinextek`.
2. **Toolchain, target selection and packaging — complete.** Add the AArch64
   toolchain, `visinextek/vs816` dispatch, plugin naming, RPATH and SDK runtime
   staging.
3. **Process-wide media lifecycle — complete.** Reference-count SYS/VB so one
   HAL instance cannot tear down another; reserve a 4K RAW-capable common pool.
4. **Board I/O — implementation complete.** Reuse the platform GPIO and serial
   backends. Acceptance needs production GPIO numbers, UART nodes and RS-485
   direction wiring.
5. **Camera and direct encoding — implementation complete.** Use the SDK's
   version-matched VII sensor helper, expose NV12 frames, and bind VII to H.264
   VENC. Acceptance needs the actual sensor enum, MIPI lane/clock setup and ISP
   tuning files. Camera exposure/gain controls and local preview are deferred
   to the display/ISP stages.
6. **Standalone codec — implementation complete.** Support H.264 CBR encode,
   decode, flush and bitrate update, including the SDK frame-mode segmented
   bitstream layout.
7. **Audio — estimated 2–4 engineer-days.** Add AI/AO lifecycle, PCM capture /
   playback, G.711 A-law and µ-law codec paths, volume/mute controls, and audio
   clock/underrun tests.
8. **Display and camera preview — estimated 3–5 engineer-days.** Add VO/VPP
   device/layer/channel lifecycle, NV12 submission, camera→VPP→VO binding,
   hot stop/restart and resolution switching.
9. **Graphics/OSD — estimated 2–4 engineer-days.** Map DarkOS surfaces to RGN or
   TDE/GPE, implement region attach/update/remove and validate ARGB/RGBA format
   conversion and cache coherency.
10. **ISP controls — estimated 2–4 engineer-days.** Map exposure, analogue gain,
    white balance and image controls to the AE/AWB/ISP APIs and define manual /
    automatic mode transitions.
11. **Optional NPU — estimated 3–7 engineer-days after model choice.** Select
    the VS NN API, define tensor layout/quantization compatibility and port one
    production model. This is not implied by merely staging the NN runtime.
12. **Product integration and soak — estimated 4–7 engineer-days.** Add the
    VS816 application preset, device permissions and runtime configuration;
    run camera/codec/audio/display together, repeated open/close, thermal and
    24-hour stability tests.

## Acceptance gates

- Cross-build has no unresolved link symbols and produces an AArch64 shared
  object exporting `HMI_camera`, `HMI_codec`, `HMI_light` and `HMI_serial`.
- Every non-system `DT_NEEDED` library is present in the staged output.
- On the target, verify sensor probe and ISP tuning before judging camera code.
- Run repeated start/stop in mixed camera and codec order to validate global
  SYS/VB ownership.
- Do not call full parity complete until stages 7–10 and the 24-hour combined
  pipeline soak pass on production hardware.

Remaining effort is roughly **13–24 engineer-days**, excluding NPU and delays
for missing schematics, tuning binaries or vendor SDK defects. The completed
foundation represents roughly **5–8 engineer-days** of the original port.
