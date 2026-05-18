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
