# TA1FM LoRa APRS iGate & Digi — Arduino IDE Derleme Rehberi

## 1. Desteklenen Donanım

**Heltec Wireless Stick Lite V3 (ESP32-S3 + SX1262)**

Bu yazılımın V1.5.7.1 sürümü yalnızca bu donanım için hazırlanmıştır. Harici SSD1306 OLED ekran kullanılır.

## 2. Gerekli Yazılımlar

- Arduino IDE 2.x
- Espressif ESP32 Arduino Core — test edilen sürüm: **3.3.8**
- RadioLib — test edilen sürüm: **7.8.1**
- Adafruit SSD1306
- Adafruit GFX Library

Gerekli bağımlılıklar Arduino IDE Kütüphane Yöneticisi üzerinden kurulabilir.

WiFi, WebServer, Preferences, Update, SPI ve Wire kütüphaneleri ESP32 Arduino ortamıyla birlikte sağlanır.

## 3. Arduino IDE Kart Ayarları

**Tools → Board:** ESP32S3 Dev Module

| Ayar | Değer |
|---|---|
| Board | ESP32S3 Dev Module |
| Flash Size | 4MB |
| Partition Scheme | Default 4MB with SPIFFS |
| Upload Port | Kartın bağlı olduğu COM portu |

OTA desteği için OTA bölümleri içeren bir bölümleme düzeni kullanılmalıdır.

## 4. Kaynak Kodun Açılması

1. GitHub üzerinden kaynak kodu indirin.
2. `TA1FM_LoRa_APRS_iGate_V1_5_7_1.ino` dosyasını Arduino IDE'de açın.
3. Kütüphanelerin kurulu olduğunu kontrol edin.
4. Gerekiyorsa kendi yönetici şifrenizi kaynak kodda değiştirin.
5. **Verify / Derle** düğmesine basın.
6. Derleme başarılı olursa **Upload / Yükle** ile karta aktarın.

## 5. Hazır BIN Dosyası Oluşturma

Arduino IDE menüsünden:

**Sketch → Export Compiled Binary**

Bu işlem sonunda aşağıdaki dosyalar oluşturulabilir:

- `.ino.bin` — OTA güncellemesi için uygulama dosyası
- `.ino.merged.bin` — USB üzerinden ilk yükleme için birleşik firmware
- `.ino.bootloader.bin` — Bootloader
- `.ino.partitions.bin` — Bölümleme tablosu

Hazır dosyalar ayrıca GitHub Releases bölümünde yayımlanmaktadır.

## 6. LoRa ve OLED Bağlantıları

Yazılımın kullandığı LoRa SX1262 pinleri:

| SX1262 | ESP32-S3 GPIO |
|---|---|
| SCK | 9 |
| MISO | 11 |
| MOSI | 10 |
| NSS / CS | 8 |
| RESET | 12 |
| DIO1 | 14 |
| BUSY | 13 |

Harici SSD1306 OLED bağlantısı:

| OLED | ESP32-S3 GPIO |
|---|---|
| SDA | 41 |
| SCL | 42 |

**Önemli:** Bu pinler yazılımın ilgili kart için yapılandırıldığı bağlantılardır. Kartın donanım şemasına göre bağlantı yapın.

## 7. Güvenlik ve Kullanım

Yazılımı kendi cihazınız için derlerken yönetici şifresini değiştirin.

APRS çağrı işaretinizi, APRS-IS bilgilerinizi ve istasyon koordinatlarınızı doğru yapılandırın.

RF yayını yapmadan önce anten bağlantısını kontrol edin ve geçerli amatör telsiz mevzuatına uyun.

## 8. Kaynak Kod ve Lisans

Proje GNU GPL v3.0 lisansıyla yayımlanmıştır.

Orijinal LoRa APRS iGate projesi ve donanım yapılandırmaları için Ricardo Guzman'a teşekkür ederiz.

https://github.com/richonguzman/LoRa_APRS_iGate

---

**TA1FM LoRa APRS iGate & Digi**

73 de TA1FM!
