# Lessons Learned - ESP32-S3 Tailscale Marauder

## HID Timeout - Root Cause & Solution (FIXED)
- **Status**: **SOLVED**.
- **The "Real" Root Cause**: CPU Starvation on Core 0. In ESP-IDF, the WiFi stack and WireGuard (Tailscale) processing run on Core 0. By default, the TinyUSB internal task also runs on Core 0. Under high network load, the TinyUSB task doesn't get enough cycles to respond to the host's USB polling, leading to `tud_hid_ready()` failing and causing `ESP_ERR_TIMEOUT`.
- **The Architecture Solution**:
  1. **Core Isolation**: Pin the HID Executor task AND the TinyUSB Internal Task to **Core 1**. This physically separates the real-time HID logic from the heavy-lifting network/crypto processing.
  2. **Priority Elevation**: Set both tasks (Executor and Driver) to a high priority (20) to ensure deterministic response times.
  3. **Robustness**: Implement a retry mechanism for the `release` command to prevent stuck keys during transient congestion.
- **Verification**: Working perfectly over Tailscale IP even with high latency/relay (DERP).

## API & Documentation
- **Best Practice**: Always document the REST API in a structured table. It's the primary way users interact with the device programmatically.
- **Convention**: Maintain both English and Spanish documentation for wider reach in the ESP32 community.
- **Scheduler macro references**: Store scheduled HID automation as a saved macro ID with `0` meaning "No macro". Validate non-zero IDs when saving scheduler config and trigger once when the rule becomes active; do not tie execution only to the exact start minute because boot/apply-config can happen inside an active window.
- **Periodic HID automation**: Keep background HID pulses independent from macro execution. Persist their key/interval config, serialize physical HID output, and pause the background task while a macro is queued or running so periodic keys cannot interleave with user macros.
- **TinyUSB descriptor logs**: The `tusb_desc` INFO startup table is unsuitable for the compact web log buffer because it is multi-line, Unicode-heavy, and can be interleaved with later logs. Override that tag to `ERROR` instead of filtering logs in the UI.
- **Disabled command wording**: For DuckyScript commands kept only for parser compatibility, document them as intentionally disabled at runtime rather than "currently dry-run" so the README does not imply they are pending work.

## HID Macro Runtime Tests
- **STRING/STRINGLN do not interpolate firmware variables**: `$N` inside text is typed literally and the target shell may expand it as an empty shell variable. Firmware variables must be verified through runtime behavior such as `DELAY $N` or dedicated status messages, not shell echo output.
- **Visual payloads must match shell semantics**: `CTRL-A` in a Linux shell moves to the start of the line; it does not select text. Regression payloads should use shell-correct cleanup primitives such as `CTRL-U` or avoid relying on selection behavior.
- **After interpolation support**: `$VAR`/`#DEFINE` inside `STRING` and `STRINGLN` are expanded by firmware before typing. Use `$$` when a literal `$` should reach the target shell.
- **Real HID delays need device-level verification**: A successful build is not enough for timing semantics. Verify `DELAY` through `/api/usb-hid/status` while the macro is running, because background automation such as Keep Awake depends on accurate executor busy state.
- **Lock-key commands are low-risk real HID**: `CAPSLOCK`, `NUMLOCK`, and `SCROLLLOCK` should share the same simple key path as `PAUSE`/`PRINTSCREEN`; they do not need special runtime state beyond the host LED feedback variables.

## Firmware Artifact Sync
- Compare release artifacts by SHA-256, not timestamps. `firmware/bootloader.bin`, `firmware/partition-table.bin`, `firmware/ota_data_initial.bin`, and `firmware/wifi_repeater.bin` map to `build/bootloader/bootloader.bin`, `build/partition_table/partition-table.bin`, `build/ota_data_initial.bin`, and `build/wifi_repeater.bin`.
