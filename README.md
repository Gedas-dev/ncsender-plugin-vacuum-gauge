# Vacuum Gauge for ncSender

**Version 1.1.1** · [Download the latest release](https://github.com/Gedas-dev/ncsender-plugin-vacuum-gauge/releases/latest)

A WiFi vacuum gauge for a CNC vacuum table. An M5Stack ATOM Lite reads a pressure sensor on the vacuum line, and a plugin shows the reading inside [ncSender](https://github.com/siganberg/ncSender). It can warn you or feed-hold the machine if the vacuum drops during a job.

![The Vacuum Gauge window in ncSender](docs/gauge-window.png)

![Mini gauge styles: Number, Dial, Bar, Slim, Colour block](docs/mini-gauge-styles.png)

## Parts

- M5Stack ATOM Lite (ESP32)
- M5Stack Tube / Air Pressure Unit (U130), -100 to 200 kPa
- USB-C data cable and 5 V USB power
- Silicone tube 2.5 x 4 mm, a 1/4" to 5/32" push-in reducer, 1/4" air tube, and a 1/4" tee

## Install

1. **Gauge:** plug the sensor into the ATOM's Grove port. Open [`firmware/VacGauge/VacGauge.ino`](firmware/VacGauge/VacGauge.ino) in Arduino IDE, type your WiFi name and password at the top, select board **M5Atom**, and upload.
2. **Plugin:** download [`vacuum-gauge-ncsender-plugin.zip`](dist/vacuum-gauge-ncsender-plugin.zip). In ncSender open **Plugins -> Install from a ZIP file**. Do not unzip it.
3. **Set up:** open **Tools -> Vacuum Gauge**. With the pump off, click **Zero sensor**. Set your green and red levels and click **Save**.

If the dial says NO SIGNAL, type the ATOM's IP address in **Gauge address**.

Full steps with pictures: [Install Guide (PDF)](docs/Vacuum-Gauge-Install-Guide.pdf)

## Notes

- Needs ncSender 2.0.225 or newer.
- Feed hold stops the movement, but **the spindle keeps turning**. This is an operator aid, not a safety device.
- Tested against a simulated gauge. Not yet run on a real machine.
