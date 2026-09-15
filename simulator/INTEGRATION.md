# Native HMI simulator

This target compiles the same EEZ-generated LVGL hierarchy and handwritten
controller used by the Waveshare firmware. CMake pins LVGL `v8.4.0` and
`lv_drivers v8.3.0`; it never reads PlatformIO's package cache. The SDL
framebuffer is fixed at 800x480, RGB565, using `src/display/lv_conf.h`.

## Commands

Run from Windows PowerShell at the repository root:

```powershell
.\scripts\hmi-simulator.ps1 setup
.\scripts\hmi-simulator.ps1 build
.\scripts\hmi-simulator.ps1 run
.\scripts\hmi-simulator.ps1 clean
```

`setup` is the only action that installs software. It detects or installs
MSYS2, updates it, and installs the UCRT64 GCC, CMake, Ninja, pkgconf, Git and
SDL2 packages. Pass `-Msys2Root` for a nonstandard installation.

## Shared and simulated layers

- `src/display/ui/generated/` is the EEZ-owned widget tree.
- `src/display/ui/ui_controller.cpp` is the shared presenter and native-action
  implementation.
- `src/display/ui/ui_backend_embedded.cpp` maps the presenter to the real UART
  state store.
- `simulator/src/ui_backend_sim.cpp` maps it to `MockGateway`.
- `simulator/src/mock_gateway.cpp` supplies sensor drift, queue progression,
  pairing countdown, relay/mode validation and ACK/NACK behavior.

The SDL loop and all mock callbacks run on one thread. The real display still
uses its LVGL recursive mutex and FreeRTOS refresh task.

## Exactness boundary

Widget coordinates, fonts, DPI, color depth, theme, generated code and
presenter logic are shared with firmware. A screenshot of the SDL framebuffer
therefore represents the pixels LVGL asks the Waveshare driver to draw.
Physical-panel gamma, viewing angle and backlight are outside the simulator.
