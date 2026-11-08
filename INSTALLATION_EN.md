# TA1FM LoRa APRS iGate & Digi — Installation Guide

## Supported Hardware

**Heltec Wireless Stick Lite V3 (ESP32-S3 + SX1262)**

Version V1.5.7.1 is designed and tested for this board only. Other LoRa boards are not currently supported.

## 1. Download the Firmware

Visit the project's GitHub Releases page:

https://github.com/TA1FM/TA1FM-LoRa-APRS-iGate-Digi/releases

Download the files for version V1.5.7.1:

- **`.ino.merged.bin`** — Complete firmware for initial USB flashing.
- **`.ino.bin`** — Application firmware for OTA updates.

## 2. Initial Firmware Installation

1. Connect your Heltec Wireless Stick Lite V3 to your computer using a USB data cable.
2. Open an ESP32-S3-compatible flashing application, such as Espressif Flash Download Tool.
3. Select **ESP32-S3** as the target chip.
4. Select the downloaded `.ino.merged.bin` file.
5. Set the flash address to **0x0**.
6. Select the correct COM port.
7. Start flashing.
8. Restart the device after successful installation.

**Warning:** Flashing can erase existing settings. Back up your configuration when possible.

## 3. First Wi-Fi Setup

When no Wi-Fi network is configured, the device starts its setup access point.

- **Wi-Fi SSID:** `TA1FM-APRS-SETUP`
- **Default Wi-Fi password:** `1234567890`
- **Setup address:** `http://192.168.4.1`

Connect your phone or computer to this Wi-Fi network and open the setup address in your browser.

## 4. Web Administration

Default login credentials:

- **Username:** `admin`
- **Password:** `PanelDegistir1234`

**Security notice:** These are public default credentials. The administrator password is defined in the firmware source code and must be changed there before recompiling and flashing your customized firmware. Do not expose the administration panel directly to the internet.

## 5. APRS Configuration

Configure the following settings through the web interface:

- Your amateur radio callsign and SSID
- Wi-Fi network credentials
- APRS-IS server and passcode
- Station latitude and longitude
- Beacon interval and comment
- LoRa RF transmission settings
- Digipeater options

Use your own valid callsign and APRS-IS credentials.

## 6. OTA Firmware Updates

To update an existing compatible installation:

1. Download the new `.ino.bin` firmware from GitHub Releases.
2. Open the device's web administration panel.
3. Navigate to the OTA firmware update page.
4. Select the `.ino.bin` file and start the update.
5. Allow the device to restart.
6. Verify the installed firmware version.

**Never upload the merged firmware image through OTA.**

## 7. LoRa Settings

The tested configuration is:

| Parameter | Value |
|---|---|
| Frequency | 433.775 MHz |
| Spreading Factor | SF12 |
| Bandwidth | 125 kHz |
| Coding Rate | 4/5 |

The automatic RF Digipeater function is disabled by default.

Transmit only with appropriate authorization and in accordance with local amateur radio regulations.

## 8. Troubleshooting

**Setup Wi-Fi not visible:** Restart the device and check the serial monitor for connection status.

**APRS-IS not connecting:** Verify your Wi-Fi connection, callsign, passcode, server and port.

**No LoRa packets received:** Check your antenna, operating frequency and LoRa configuration.

**OTA update fails:** Verify that you selected the application `.ino.bin` file and that the installed partition scheme supports OTA.

## Source Code and License

Project repository:

https://github.com/TA1FM/TA1FM-LoRa-APRS-iGate-Digi

License: GNU GPL v3.0

Acknowledgements: Ricardo Guzman's LoRa APRS iGate project.

https://github.com/richonguzman/LoRa_APRS_iGate

---

**TA1FM LoRa APRS iGate & Digi**

**73 de TA1FM!**
