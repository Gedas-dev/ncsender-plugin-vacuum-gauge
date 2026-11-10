## What's Changed

**Beta. The new action moves the machine and has not been run on a real machine yet. Test it in air first.**

### ✨ New
- New choice under "If the vacuum drops": **Stop job + raise Z**. It stops the job, switches the spindle off and raises Z by the distance you set (default 20 mm)
- Z is only raised on a homed machine and never above the top of travel
- The warning shows the line the job stopped at, so you can start again from there

### ℹ️ Notes
- The job is ended, not paused
- The default is still "Show a warning". Nothing changes unless you pick the new action

### 📦 Install
Download `community.vacuum-gauge_v1.2.0-beta.1.zip` below. In ncSender open Plugins → Install from a ZIP file. Do not unzip it.
