## What's Changed

**Beta. The two new actions move the machine and have not been run on a real machine yet. Test them in air first.**

### ✨ New
- **Pause + park Z**: pauses the job, stops the spindle and parks Z using the controller's parking cycle. Press Resume to continue; the spindle restarts and Z returns. Needs parking enabled in the controller ($41, $56 to $59). If parking is off, the spindle still stops but Z stays
- **Stop job + raise Z**: ends the job, switches the spindle off and raises Z by the distance you set (default 20 mm). Only on a homed machine, never above the top of travel
- A short explanation of the chosen action is shown under the setting

### ℹ️ Notes
- The default is still "Show a warning". Nothing changes unless you pick a new action

### 📦 Install
Download `community.vacuum-gauge_v1.2.0-beta.2.zip` below. In ncSender open Plugins → Install from a ZIP file. Do not unzip it.
