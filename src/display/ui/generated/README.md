# Generated Shunya HMI

This directory is owned by the EEZ Studio project at
`hmi/shunya-hmi.eez-project`. The checked-in C files are the firmware-facing
generated artifact and must remain pure LVGL 8.4 C.

## Regeneration contract

- Generate from EEZ Studio into `src/display/ui/generated`.
- Keep widget names and native action names stable; the handwritten presenter
  and action layer may depend on every symbol declared in `ui.h`.
- Do not add Arduino, FreeRTOS, transport, state-store, or other backend calls
  here. Native actions are declared here and implemented by the handwritten
  controller; a missing action must fail the link rather than become a no-op.
- Review generated diffs before committing. Preserve the public
  `shunya_ui_create`/`shunya_ui_init` entry points and the LVGL 8.4 API.
- The visual project is the editable layout source of truth. Runtime values,
  command handling, confirmation policy, and presenter formatting belong
  outside this directory.
