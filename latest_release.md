## What's Changed

### ✨ New
- **Stop job + raise Z now goes to your Safe Z height.** After the stop, Z rises to the Safe Z height set in ncSender (machine coordinates), the same value Park and `$SAFEZ` use. If you change Safe Z in ncSender, the plugin follows within 15 seconds
- New option **Raise Z to**: *Safe Z height* (default) or *Fixed distance* (the old behaviour). The window shows the Safe Z value the plugin will use
- Z is never moved down: if it is already at or above Safe Z it stays where it is. If Safe Z cannot be read, the fixed distance is used

### ⚠️ Note
- If you use "Stop job + raise Z", Z now rises further than before (to Safe Z, not 20 mm). Try it in air once: no bit, Z well above the table

### 📦 Install
Download `community.vacuum-gauge_v1.4.0.zip` below. In ncSender open Plugins → Install from a ZIP file. Do not unzip it.
