# TA1FM LoRa APRS iGate & Digi — Kurulum Rehberi

## Desteklenen Donanım

**Heltec Wireless Stick Lite V3 (ESP32-S3 + SX1262)**

Bu sürüm yalnızca yukarıdaki kart için geliştirilmiş ve test edilmiştir. Diğer LoRa kartları henüz desteklenmemektedir.

## 1. Hazır Yazılımı İndirme

GitHub projesinin **Releases** bölümünden V1.5.7.1 sürümünü açın.

https://github.com/TA1FM/TA1FM-LoRa-APRS-iGate-Digi/releases

İlk kurulum için `.ino.merged.bin` uzantılı dosyayı indirin.

**Not:** `.ino.bin` dosyası mevcut yazılımın web panelinden OTA ile güncellenmesi içindir.

## 2. ESP32'ye İlk Yükleme

1. Heltec Wireless Stick Lite V3 kartını USB kablosuyla bilgisayara bağlayın.
2. ESP32 Flash Download Tool veya ESP32-S3 destekleyen uygun bir flash programı açın.
3. ESP32-S3 hedefini seçin.
4. İndirdiğiniz `merged.bin` dosyasını seçin.
5. Flash başlangıç adresini `0x0` olarak ayarlayın.
6. Kartın doğru COM portunu seçin.
7. Flash işlemini başlatın.
8. İşlem başarıyla tamamlandığında cihazı yeniden başlatın.

**Dikkat:** Yükleme işlemi mevcut yazılımı ve bazı kayıtlı ayarları silebilir. Gerekirse önceden yedek alın. İşlem sırasında USB bağlantısını kesmeyin.

## 3. İlk Wi-Fi Bağlantısı

Cihazda kayıtlı Wi-Fi ağı yoksa kurulum erişim noktası oluşturulur.

- Wi-Fi ağ adı: `TA1FM-APRS-SETUP`
- Varsayılan AP şifresi: `1234567890`
- Web paneli: `http://192.168.4.1`

Telefon veya bilgisayarla bu ağa bağlanıp tarayıcıdan paneli açın.

## 4. Yönetici Girişi

- Kullanıcı adı: `admin`
- Varsayılan yönetici şifresi: `123456789`

Yönetici şifresi bu sürümün kaynak kodunda tanımlıdır. Şifreyi değiştirmek için kaynak kodu düzenleyip yeniden derlemek gerekir.

**Güvenlik:** Yayımlanan varsayılan şifreler herkes tarafından bilinir. Kendi cihazınızda değiştirmeniz önerilir. Yönetim panelini doğrudan internete açmayın.

## 5. APRS Yapılandırması

Web panelinden şu bilgileri düzenleyin:

- Kendi amatör telsiz çağrı işaretiniz ve SSID
- Wi-Fi ağınız ve şifreniz
- APRS-IS sunucusu, port ve passcode
- İstasyon koordinatları
- Beacon aralığı, sembol ve açıklama
- RF çıkış gücü
- Digipeater ve WIDE ayarları

**Varsayılan APRS-IS sunucusu:** `euro.aprs2.net:14580`

İnternet bağlantısı ve APRS-IS girişi doğrulandıktan sonra iGate işlevini kontrol edin.

## 6. OTA Yazılım Güncelleme

Daha sonraki sürümlerde cihazı USB bağlantısı olmadan güncellemek mümkündür.

1. Releases sayfasından yeni sürümün `.ino.bin` dosyasını indirin.
2. Web panelinde **OTA Güncelle** sayfasını açın.
3. Dosyayı seçerek güncellemeyi başlatın.
4. İşlem tamamlanınca cihazın yeniden başlamasını bekleyin.
5. Web panelinden sürümü kontrol edin.

**OTA için `merged.bin` kullanmayın.**

## 7. RF ve Digipeater Kullanımı

Saha testlerindeki LoRa parametreleri:

- Frekans: 433.775 MHz
- Spreading Factor: SF12
- Bandwidth: 125 kHz
- Coding Rate: 4/5

Digipeater işlevi varsayılan olarak kapalıdır. RF yayınına izin veren ayarlar yalnızca gerekli yetkilendirme ve uygun anten sistemi bulunduğunda açılmalıdır.

Çıkış gücü, harici PA kullanımı, anten kazancı ve yerel mevzuat birlikte değerlendirilmelidir.

## 8. Sorun Giderme

**Kurulum ağı görünmüyorsa:** Cihazı yeniden başlatın, USB seri monitörden bağlantı durumunu kontrol edin.

**APRS-IS bağlanmıyorsa:** Wi-Fi bağlantısını, çağrı işaretini, passcode'u, sunucu adresini ve portu kontrol edin.

**RX paketleri görünmüyorsa:** Anten bağlantısını, 433.775 MHz frekansını ve LoRa parametrelerini kontrol edin.

**OTA başarısızsa:** Doğru `.ino.bin` dosyasının seçildiğini ve kullanılan bölümleme düzeninin OTA desteklediğini kontrol edin.

---

**Proje:** TA1FM LoRa APRS iGate & Digi

**GitHub:** https://github.com/TA1FM/TA1FM-LoRa-APRS-iGate-Digi

**73 de TA1FM!**
