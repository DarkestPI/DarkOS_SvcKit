# Visinextek HAL

VS816 uses the vendor MAL from `board/package/vs-mp`. The generated plugin is
`hal.visinextek.vs816.so`.

The SDK's VII sample contains the authoritative sensor timing/configuration
tables and dynamically loads `libsnsr*.so`, `libae.so` and `libawb.so`. This HAL
compiles that version-matched helper instead of copying the tables into DarkOS.
`DARKOS_VS816_SAMPLE_ROOT` therefore must match `DARKOS_VS816_MEDIA_ROOT`.

SYS and VB are not managed by the sample helper: `common/vs_sys_guard.c` owns
their process-wide reference count so camera and codec cannot tear each other
down. The first client reserves a 4K pool sized for unpacked RAW16, which also
covers NV12 camera, codec and display consumers.

Build example:

```bash
cmake -S . -B build-vs816 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-visinextek-linux-gnu.cmake \
  -DDARKOS_BUILD_HAL_IMPLEMENTATIONS=ON \
  -DDARKOS_VENDOR=visinextek -DDARKOS_SOC=vs816
cmake --build build-vs816 -j
```

The default camera0 sensor follows the supplied SDK configuration
(`SONY_IMX334_MIPI_8M_30FPS_12BIT`). Override
`DARKOS_VS816_SENSOR0_TYPE` when the production board uses another enum from
the same SDK sample.

Current implementation and board-bring-up status are tracked in
[`ADAPTATION_PLAN.md`](ADAPTATION_PLAN.md).
