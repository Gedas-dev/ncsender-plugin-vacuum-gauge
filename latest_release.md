## What's Changed

### 🔧 Fixes
- **Reading dropping to NO SIGNAL every few minutes:** the plugin now talks to the gauge by its IP address once the gauge has reported it, so it no longer depends on the `.local` name being looked up again. This needs the updated firmware below. With older firmware, type the gauge's IP address in Gauge address

### ✨ New
- **Connection line** in the Vacuum Gauge window: dropouts, gauge restarts, WiFi strength and the gauge's IP, with a hint about the likely cause
- Firmware 1.1 (`VacGauge.ino` below): the gauge reports its IP address

### 📦 Install
Download `community.vacuum-gauge_v1.3.1.zip` below. In ncSender open Plugins → Install from a ZIP file. Do not unzip it.
