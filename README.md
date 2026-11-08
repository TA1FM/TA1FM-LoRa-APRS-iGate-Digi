# TA1FM LoRa APRS iGate & Digi

**ESP32-S3 tabanlı LoRa APRS Internet Gateway ve Digipeater yazılımı**

**English:** ESP32-S3 based LoRa APRS Internet Gateway and Digipeater firmware.

## 🇹🇷 Türkçe

### Proje Hakkında
TA1FM LoRa APRS iGate & Digi, amatör telsizciler için geliştirilmiş, LoRa üzerinden APRS paketlerini alan, APRS-IS ağına aktaran ve internet bağlantısı olmadığında RF Digipeater olarak çalışabilen bir yazılımdır.

İlk sürüm Heltec Wireless Stick Lite V3 (ESP32-S3 / SX1262) donanımında geliştirilmiş ve test edilmiştir.

### Özellikler
- 433.775 MHz LoRa APRS, SF12, BW125
- APRS-IS bağlantısı ve konum beacon gönderimi
- Otomatik RF Digipeater modu
- WIDE1-1 / WIDE2-2 paket tekrarlama desteği
- Web tabanlı ayar ve kontrol paneli
- Web üzerinden OTA yazılım güncelleme
- OLED ekran desteği
- Son 15 RX Beacon listesi
- RF çıkış gücü ve beacon path ayarları
- Ayarları yedekleme ve geri yükleme
- Wi-Fi bağlantısından bağımsız APRS-IS açma/kapatma

### Kullanılan Donanım
- Heltec Wireless Stick Lite V3
- ESP32-S3 mikrodenetleyici
- SX1262 LoRa modülü
- Harici SSD1306 OLED ekran

### Durum
**Saha test sürümü: V1.5.7.1**

Yazılım test edilmeye devam etmektedir. Gelecekte farklı ESP32 LoRa kartlarına destek eklenmesi planlanmaktadır.

### Geliştirici
**TA1FM — Türkiye**

---

## 🇬🇧 English

### About
TA1FM LoRa APRS iGate & Digi is firmware for amateur radio operators. It receives APRS packets over LoRa, forwards them to APRS-IS, and can operate as an RF digipeater when the internet connection is unavailable.

The initial release was developed and tested with the Heltec Wireless Stick Lite V3 (ESP32-S3 / SX1262).

### Features
- 433.775 MHz LoRa APRS, SF12, BW125
- APRS-IS connectivity and position beacons
- Automatic RF digipeater
- WIDE1-1 / WIDE2-2 path support
- Web-based configuration and control panel
- OTA firmware updates through the web interface
- OLED display support
- Last 15 received APRS beacons
- Adjustable RF TX power and beacon path
- Configuration backup and restore
- Independent APRS-IS enable/disable control

### Hardware
- Heltec Wireless Stick Lite V3
- ESP32-S3 microcontroller
- SX1262 LoRa transceiver
- External SSD1306 OLED display

### Status
**Field-test version: V1.5.7.1**

Support for additional ESP32 LoRa boards is planned.

### Developer
**TA1FM — Türkiye**

---

**73 de TA1FM!** 📡
