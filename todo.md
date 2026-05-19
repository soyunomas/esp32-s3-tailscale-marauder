# USB HID Macro Plan

## Scheduler UX And USB HID Macro Scheduling

- [x] Remove the preconfigured scheduler shortcut/button labeled `Work 14-16`.
- [x] Do not ship preconfigured scheduler rules; `Add rule` must create a neutral editable rule, not a work-hours preset.
- [x] Preserve all existing scheduler functionality: modes, timezone/NTP sync, AP/STA/Tailscale rule toggles, safety hold, save/load API behavior, and max 8 rules.
- [x] Add scheduled execution for saved USB HID macros without routing execution through LLMs or deterministic model decisions.
- [x] UX requirement: make macro scheduling explicit and reviewable inside each scheduler rule, with no hidden auto-execution defaults.
- [x] Validate that a scheduled macro references an existing saved macro and fails loudly on invalid input.
- [x] Avoid repeated macro execution while a schedule rule remains continuously active; trigger at the scheduled start/activation boundary.
- [x] Keep USB HID executor serialization and stop/panic behavior intact.
- [x] Update README.md and README-ESP.md after implementation with the scheduler macro execution behavior.

## Mandatory Guardrails

- Do not change existing routing, captive portal, WiFi repeater behavior, Tailscale, scheduler behavior, NAPT, port forwarding, DNS, authentication, OTA, or logging internals unless the user explicitly approves a separate task.
- Keep the USB HID feature isolated in new files and clearly prefixed symbols:
  - `usb_hid_*` for C modules, functions, structs, CSS classes, and JS helpers.
  - `/api/usb-hid/...` for new HTTP endpoints.
  - `usb_hid` or `hid_macro` namespace for NVS data.
- Existing files may only receive minimal integration points:
  - Add new source files to `main/CMakeLists.txt`.
  - Register new static/API handlers in `web_server.c`.
  - Link the USB HID tab UI to its own JS/CSS surface.
- All new visible UI text must be in English.
- Build after every implementation slice with `source /home/yo/esp/esp-idf/export.sh && idf.py build`.

## Target UX

- [x] In the `USB HID` tab, provide:
  - [x] A macro text area where the user can paste/edit commands.
  - [x] `Execute` button to run the current macro.
  - [x] `Save` button to store the macro.
  - [x] Macro name input.
  - [x] Saved macro list with `Load`, `Execute`, `Rename`, and `Delete`.
  - [x] Execution status area with current state, last error, and progress.
- [x] Keep the layout mobile-first and usable on narrow screens.
- [x] Use English labels only, for example: `Macro Name`, `Macro Script`, `Execute`, `Save`, `Saved Macros`, `Status`, `Idle`, `Running`, `Stopped`.

## Phase 1: UI Only, No Firmware Side Effects

- [x] Status: implemented as UI. Device-side execution is not enabled yet; NVS persistence is handled in Phase 2.
- [x] Replace the current placeholder inside the `USB HID` tab with:
  - [x] Macro name input.
  - [x] Macro script text area.
  - [x] Action row: `Execute`, `Save`, `Clear`.
  - [x] Saved macros panel.
  - [x] Status panel.
- [x] Keep all new JS in `main/embedded_files/usb_hid.js`.
- [x] Keep all new styles prefixed with `.usb-hid-`.
- [x] Do not call any network, WiFi, Tailscale, or scheduler endpoints.

## Phase 2: Storage API

- [x] Status: implemented for macro list/save/get/delete under `/api/usb-hid/macros`. Macro execution is still not enabled.
- [x] Add isolated macro persistence:
  - [x] `usb_hid_macro_store.c`
  - [x] `usb_hid_macro_store.h`
- [x] Store macros in NVS under a dedicated namespace.
- [x] Add endpoints:
  - [x] `GET /api/usb-hid/macros`
  - [x] `POST /api/usb-hid/macros`
  - [x] `GET /api/usb-hid/macros/<id>`
  - [x] `PUT /api/usb-hid/macros/<id>`
  - [x] `DELETE /api/usb-hid/macros/<id>`
- [x] Enforce conservative limits:
  - [x] Maximum macro count.
  - [x] Maximum macro name length.
  - [x] Maximum macro body size.
  - [x] Printable ASCII for names and macro text unless a command explicitly needs otherwise.
- [x] Do not alter existing config storage structures.

## Phase 3: Parser For `commands.md`

- [x] Status: implemented as validation only under `/api/usb-hid/validate`. HID execution is still not enabled.
- [x] Add an isolated parser module:
  - [x] `usb_hid_macro_parser.c`
  - [x] `usb_hid_macro_parser.h`
- [x] Parse and validate every command listed in `commands.md`.
- [x] First supported execution group:
  - [x] `REM`, `END_REM`
  - [x] `STRING`, `STRINGLN`
  - [x] `DELAY`
  - [x] `ENTER`, `TAB`, `ESCAPE`, `SPACE`, `BACKSPACE`, `DELETE`, `INSERT`
  - [x] `HOME`, `END`, `PAGEUP`, `PAGEDOWN`
  - [x] `UPARROW`, `DOWNARROW`, `LEFTARROW`, `RIGHTARROW`
  - [x] `CTRL`, `ALT`, `SHIFT`, `GUI`, `WINDOWS`, `COMMAND`
  - [x] `F1` through `F12`
  - [x] `HOLD`, `RELEASE`
- [x] Return structured validation errors to the UI with line number and message.

## Phase 4: HID Execution Engine

- [x] Add an isolated executor module:
  - [x] `usb_hid_executor.c`
  - [x] `usb_hid_executor.h`
- [x] Execution must be serialized: only one macro can run at a time.
- [x] Add endpoints:
  - [x] `POST /api/usb-hid/execute`
  - [x] `POST /api/usb-hid/stop`
  - [x] `GET /api/usb-hid/status`
- [x] Implement a queue/task dedicated to USB HID macro execution.
- [x] Never run macro execution on web server request context.
- [x] Add hard safety limits:
  - [x] Maximum runtime.
  - [x] Maximum counted `LOOP` validation.
  - [x] Stop flag checked between commands.
  - [x] Button or API stop support before adding infinite `LOOP`.
- [x] Wire the UI Execute/Stop buttons to the isolated executor.
- [x] Keep execution in safe dry-run mode until real TinyUSB HID is explicitly enabled.
- [x] Keep USB HID executor failures non-critical so existing boot, web UI, captive portal, WiFi, and Tailscale can still start.
- [x] Enable real USB HID output after confirming board USB mode and serial/flash workflow safety.

## Phase 5: Full Command Coverage From `commands.md`

- [x] Status: structural parser coverage is implemented for all commands in `commands.md`; real HID output is enabled for the first safe keyboard subset, while runtime-evaluated commands remain blocked before output.
- [x] Add parser validation for variables, constants, operators, and control flow:
  - [x] `DEFINE`
  - [x] `VAR`
  - [x] `IF`, `ELSE`, `END_IF`
  - [x] `WHILE`, `END_WHILE`
  - [x] `LOOP`, `BREAK`, `CONTINUE`
  - [x] `FUNCTION`, `END_FUNCTION`, `RETURN`
  - [x] `RANDOM_INT`
  - [x] `JITTER`
- [x] Add structural validation:
  - [x] Block balancing for `IF`, `WHILE`, and `FUNCTION`.
  - [x] `ELSE` must be inside an `IF` block and only once.
  - [x] `BREAK` and `CONTINUE` must be inside a `WHILE` block.
  - [x] `RETURN` must be inside a `FUNCTION` block.
  - [x] Maximum nesting depth.
  - [x] Counted `LOOP` must stay within a conservative limit.
- [x] Add parser validation for device/control commands:
  - [x] `WAIT_FOR_BUTTON_PRESS`
  - [x] `LED`
  - [x] `CAPSLOCK`, `NUMLOCK`, `SCROLLLOCK`
  - [x] `INJECT_MOD`
  - [x] `STOP_PAYLOAD`
  - [x] `RESTART_PAYLOAD`
- [x] Add USB mode commands only inside the USB HID module:
  - [x] `ATTACKMODE`
  - [x] `SAVE_ATTACKMODE`
  - [x] `RESTORE_ATTACKMODE`
- [x] Add read-only internal variable recognition:
  - [x] `$_CAPSLOCK_ON`
  - [x] `$_NUMLOCK_ON`
  - [x] `$_CURRENT_VID`
  - [x] `$_CURRENT_PID`
  - [x] `$_BUTTON_ENABLED`
  - [x] `$_HOST_CONFIGURATION_REQUEST_COUNT`
- [x] Treat `EXFIL` as a special reviewed command:
  - [x] It must not touch routing, Tailscale, captive portal, or any network path.
  - [x] Parser only accepts local absolute paths.
  - [x] Dry-run execution reports it as skipped and does not open files or network connections.
- [x] Add dry-run handling for Phase 5 commands that must not affect hardware or networking yet.
- [x] Add a dedicated runtime evaluator for variables, expressions, `IF`, and `WHILE` execution.
  - [x] Local build passes after adding real HID runtime jumps for `IF`/`ELSE`/`END_IF`, `WHILE`/`END_WHILE`, `BREAK`, and `CONTINUE`.
  - [x] OTA flash and device-level execution regression after build.
- [x] Add firmware variable interpolation in `STRING` and `STRINGLN`.
- [x] Add string variable assignment for `VAR $NAME = value`.
- [x] Add runtime support for `LOOP`, `FUNCTION`, `END_FUNCTION`, and `RETURN`.
- [x] Add real USB HID output after TinyUSB/serial safety is confirmed.

## Phase 6: Example Macros

- [x] Add built-in examples that remain editable and not auto-executed:
  - [x] Windows shutdown macro.
  - [x] Windows restart macro.
  - [x] Open Run dialog macro.
  - [x] Home server maintenance macro template.
  - [x] Linux shutdown macro.
  - [x] Linux restart macro.
  - [x] Linux open terminal macro.
  - [x] Linux maintenance macro template.
- [x] All examples must use English UI titles and descriptions.
- [x] Do not embed secrets, IPs, passwords, tokens, or host-specific commands.

## Phase 7: Validation

- [x] Build validation:
  - [x] `source /home/yo/esp/esp-idf/export.sh && idf.py build`
- [ ] UI validation:
  - [ ] Mobile width: no clipped text, no horizontal overflow in the USB HID tab.
  - [x] Dashboard still shows connected clients at the bottom.
  - [x] Existing static assets still open.
- [x] Regression checks:
  - [x] Web UI auth works with curl.
  - [x] `/api/status` still works.
  - [x] `/api/clients` still works.
  - [x] No changes to Tailscale endpoints.
  - [x] No changes to WiFi config endpoints.
  - [x] No changes to captive portal redirect behavior.
  - [x] USB HID status and example validation endpoints work.
  - [x] Real HID mode reports `dry_run:false` after OTA.
  - [x] Runtime-only commands are blocked by executor preflight before HID output when they still lack real runtime support.
  - [x] Runtime control-flow commands execute on-device after OTA.
  - [x] Interpolation, string variables, LOOP, and FUNCTION execute on-device after OTA.

## Open Design Questions

- Decide whether macros should survive factory reset or be erased with normal NVS settings.
- Decide whether execution requires a second confirmation for saved macros such as shutdown/restart.
- Decide whether remote execution should require the user to be authenticated only, or authenticated plus an additional local safety flag.

## Maintenance Checkpoints

- [x] 2026-05-19: Synced `firmware/wifi_repeater.bin` from `build/wifi_repeater.bin` after SHA-256 mismatch; all four firmware artifacts now match their build outputs.
- [x] 2026-05-19: Fixed scheduled USB HID trigger semantics: macros now run once when a rule becomes active, not only on exact `start_min`; UI warns when rules are configured while mode is `Always on`.
- [x] 2026-05-19: Documented Scheduler usage in README.md and README-ESP.md, including `Scheduled` mode, weekly rules, and one-shot-per-window USB HID macro execution.
- [x] 2026-05-19: Synced release firmware artifacts before GitHub upload; `firmware/wifi_repeater.bin` SHA-256 now matches `build/wifi_repeater.bin` (`e6b5c047e54e04c48afa9b899fc41542e3261c2b8e540fe3f9e0919e0076042e`).
- [x] 2026-05-19: Added optional USB HID Keep Awake pulse with persistent key/interval config, `/api/usb-hid/keepalive`, UI controls, and executor-side pausing while macros are queued or running.
- [x] 2026-05-19: Fixed real HID `DELAY` waiting to use tick-based waits and verified on-device that Keep Awake pauses during a running macro, then restored Keep Awake disabled.
- [x] 2026-05-20: Suppressed the noisy `tusb_desc` TinyUSB descriptor table in runtime logs while keeping TinyUSB/HID errors visible.
- [x] 2026-05-20: Synced `firmware/wifi_repeater.bin` after log cleanup; SHA-256 matches `build/wifi_repeater.bin` (`ffe894a53911dafdbd580150eff58fe1c83ca0b7b90c814c4c6ef2db3ce7ba6f`).
- [x] 2026-05-20: Promoted DuckyScript `CAPSLOCK`, `NUMLOCK`, and `SCROLLLOCK` from validation-only to real HID execution.
- [x] 2026-05-20: Synced `firmware/wifi_repeater.bin` after lock-key HID execution; SHA-256 matches `build/wifi_repeater.bin` (`f49bebb2cdc52a960f54f72755a86e58acb8de678ba4239342e5340f75d6e293`).
- [x] 2026-05-20: Clarified README wording for parser-compatible commands that are intentionally disabled at runtime.
