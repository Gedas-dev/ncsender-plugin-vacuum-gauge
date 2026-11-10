# Vacuum Gauge for ncSender

A WiFi vacuum gauge for a CNC vacuum table, with a plugin that shows the reading inside [ncSender](https://github.com/siganberg/ncSender).

A pressure sensor on the vacuum line feeds an M5Stack ATOM Lite (ESP32). The ATOM serves the reading over the shop network, and the plugin shows it in ncSender as a dial, as a small pinned gauge on the main screen, and as a check that can warn you or feed-hold the machine if the vacuum drops during a job.

![The Vacuum Gauge window in ncSender](docs/gauge-window.png)

The pinned mini gauge comes in five styles:

![Mini gauge styles: Number, Dial, Bar, Slim, Colour block](docs/mini-gauge-styles.png)

## What is in this repository

| Path | What it is |
|---|---|
| [`dist/vacuum-gauge-ncsender-plugin.zip`](dist/vacuum-gauge-ncsender-plugin.zip) | The plugin, ready to install in ncSender |
| [`plugin/`](plugin) | Plugin source (the files inside the zip) |
| [`firmware/VacGauge/VacGauge.ino`](firmware/VacGauge/VacGauge.ino) | Arduino program for the ATOM Lite |
| [`docs/Vacuum-Gauge-Install-Guide.pdf`](docs/Vacuum-Gauge-Install-Guide.pdf) | Illustrated install guide (5 pages) |

## Parts

| # | Part | Qty | Notes |
|---|---|---|---|
| 1 | M5Stack ATOM Lite (ESP32) | 1 | The WiFi board |
| 2 | M5Stack Tube / Air Pressure Unit (U130) | 1 | Analog sensor, -100 to 200 kPa. Grove cable included |
| 3 | USB-C cable + 5 V USB power adapter | 1 | A data cable, not charge-only |
| 4 | Silicone tube, 2.5 mm ID x 4 mm OD | about 5 cm | Stretches over the 3 mm sensor nipple |
| 5 | Push-in reducer, 1/4" to 5/32" (4 mm) | 1 | Joins the small tube to the 1/4" tube |
| 6 | 1/4" OD PU air tube | as needed | Vacuum line to sensor. Keep it short |
| 7 | Push-in tee, 1/4" | 1 | Goes into the vacuum line |

Do not use a 0-40 kPa sensor (the small red HX710B boards). It only reaches about 12 inHg, and a vacuum table runs at 15 to 25 inHg.

## 1. Build the gauge

1. Plug the sensor's Grove cable into the ATOM Lite.
2. Push the silicone tube into the 5/32" side of the reducer, and stretch the other end over the sensor nipple.
3. Push the 1/4" tube into the reducer and run it to a tee in the vacuum line.
4. Open `firmware/VacGauge/VacGauge.ino` in Arduino IDE. Type your WiFi name and password at the top of the file.
5. In Boards Manager install **esp32 by Espressif Systems**. Select board **M5Atom**, pick the port, and upload. No extra libraries are needed.
6. Open `http://vac-gauge.local` in a browser on the same network. With the pump off it reads about 0.

The reading is in inHg and is negative under vacuum (for example -18.4).

The ATOM's light shows the state:

| Light | Meaning |
|---|---|
| Green | Vacuum is good (15 inHg or more) |
| Yellow | Marginal (8 to 15 inHg) |
| Red | Too weak, or the pump is off |
| Blinking blue | No WiFi |
| Blinking purple | Sensor not plugged in |

## 2. Install the plugin

1. Download [`vacuum-gauge-ncsender-plugin.zip`](dist/vacuum-gauge-ncsender-plugin.zip). Do not unzip it.
2. In ncSender open **Plugins**, click **Install from a ZIP file**, choose the zip, and click **Install from File**.
3. Open **Tools -> Vacuum Gauge**.
4. If the dial says NO SIGNAL, type the ATOM's IP address in **Gauge address** and click **Save**.

## 3. Set it up

1. **Zero the sensor.** Pump off, tube open to air, then click **Zero sensor**.
2. **Set the colours.** Hold down a real workpiece, note the reading, and set "Green at or stronger than" and "Red when weaker than".
3. **Pin the mini gauge.** Click **Pin mini gauge** and pick a look from the **Style** list. Drag it where you want it. After restarting ncSender, open the Vacuum Gauge window once to bring it back.

### Check while a job runs

| Setting | What it does |
|---|---|
| Do nothing | Only shows the reading |
| Show a warning (default) | A message appears if the vacuum drops during a job. The job keeps running |
| Feed hold + warning | The machine stops moving and a message appears. Fix the vacuum, then resume |
| Minimum vacuum | The check acts when the vacuum is weaker than this (default 10 inHg) |
| Must stay low for | Seconds the vacuum must stay low first, so a short dip is ignored (default 2) |

## Safety

Feed hold stops the movement, but **the spindle keeps turning**. This gauge helps the operator. It is not a safety device and does not replace watching the machine. If the gauge loses WiFi during a job you get a warning and the job is not paused.

## Requirements

- ncSender 2.0.225 or newer. The job check and background reading need it. On older 2.x versions the window still reads the gauge directly, but the job check is off.
- The CNC computer and the ATOM on the same network (2.4 GHz WiFi for the ATOM).

## How it works

The firmware serves JSON at `http://vac-gauge.local/api/vacuum`:

```json
{"inHg":-18.42,"kPa":-62.38,"volts":0.476,"zeroOffsetKpa":0.00,"rssi":-58,"uptimeSec":1234,"name":"vac-gauge"}
```

`POST /api/zero` stores the current reading as zero. It refuses if the reading is far from atmosphere, so it cannot be zeroed with the pump running.

The plugin's background script polls that address twice a second, keeps the latest value for the window, and watches the vacuum while a job is running. It uses only a background timer and does not touch the G-code or the command pipeline.

An ESPHome build that serves `{"value":-18.4,"state":"-18.4 inHg"}` also works: paste the full sensor URL into **Gauge address**.

## Status

- The firmware compiles for the M5Atom board. It has not yet been run on a real sensor.
- The plugin was tested on ncSender 2.0.225 built from source for Linux, including a native (AOT) build like the packaged app, against a simulated gauge. It has not yet been run on a real machine.

### Note for plugin authors

Version 1.0.0 of this plugin crashed the packaged ncSender app on load. The cause was a call to `pluginContext.log()`: in the native (AOT) build that call aborts the whole server, while a normal development build runs it without trouble. This plugin does not call it anywhere. If you write a plugin, test it on a native build before you ship it.
