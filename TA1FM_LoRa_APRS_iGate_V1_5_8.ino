/*
  TA1FM LoRa APRS iGate & Digi V1.5.8 (pil kalibrasyonlu test)
  Heltec Wireless Stick Lite V3 (ESP32-S3 + SX1262), SSD1306 / SH1106 / OFF on Wire1
  RadioLib 7.8.1, Adafruit SSD1306, Adafruit SH110X, Adafruit GFX
  APRS-IS iGate + optional backup RF digipeater. Own position beacon to APRS-IS in iGate mode; RF in backup Digi mode.
  AUTO DIGI OFF BY DEFAULT. TX requires explicit web opt-in.
  Pinout: Ricardo Guzman WSL V3 board_pinout.h (provided by user).
*/
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>
#include <time.h>
#include <esp_system.h>
#include <RadioLib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SH110X.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>

// === EDIT THESE TWO BEFORE FIRST UPLOAD ===
// 8+ characters, change them for your own device.
static const char* DEFAULT_AP_PASSWORD = "1234567890";
// Experimental TOCALL: NOT a registered device identity.
// Replace only after an identifier is formally allocated.
static const char* OWN_TOCALL = "APZTFM";
static const char* PANEL_PASSWORD = "1234567890";
static const char* PANEL_USER = "admin";

static constexpr int PIN_SCK=9, PIN_MISO=11, PIN_MOSI=10, PIN_CS=8;
static constexpr int PIN_RST=12, PIN_DIO1=14, PIN_BUSY=13;
static constexpr int PIN_SDA=41, PIN_SCL=42, PIN_VEXT=36;
static constexpr int BATTERY_PIN=1, ADC_CTRL_PIN=37;
static constexpr int ADC_CTRL_ON_STATE=LOW;
static constexpr float BATTERY_DIVIDER_RATIO=4.9f; // WSL V3: 390k + 100k; multimetre ile dogrulayin.
static constexpr uint32_t BATTERY_SAMPLE_INTERVAL_MS=30000UL;
static constexpr float LORA_FREQ=433.775f, LORA_BW=125.0f;
static constexpr uint8_t LORA_SF=12, LORA_CR=5, LORA_SYNC=0x12;
static constexpr size_t MAX_FRAME=255;

SX1262 radio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY);
Adafruit_SSD1306 oledSsd(128,64,&Wire1,-1);
Adafruit_SH1106G oledSh(128,64,&Wire1,-1);
Adafruit_GFX* oledDisplay=nullptr; // Secili OLED'in ortak cizim arayuzu.
WebServer server(80);
Preferences prefs;
WiFiClient aprs;

struct Config {
  String call, ssid, wifiPassword, host, passcode, apPassword;
  uint16_t port;
  double latitude, longitude;
  uint16_t beaconMinutes;
  bool beaconEnabled;
  String symbolTable, symbolCode;
  String commentText, statusText;
  uint16_t statusMinutes;
  bool statusEnabled;
  bool autoDigi, wide1, wide2, aprsIsEnabled;
  String rfBeaconPath; // DIRECT, WIDE1-1, WIDE2-1, WIDE2-2
  uint8_t probeMinutes;
  uint8_t txPowerDbm;
  uint8_t oledType; // 0=SSD1306, 1=SH1106, 2=Kapali
  bool beaconBattery; // APRS comment sonuna Vbat bilgisi ekle.
  uint16_t oledSleepSeconds; // 0=zamanlama kapali (sürekli acik)
  float batteryCalibration; // Volt cinsinden artis/azalis; varsayilan 0.00V
};
Config cfg;
float batteryVoltage=NAN;
uint32_t lastBatterySample=0;
bool batterySampled=false;
bool radioOk=false, oledOk=false, aprsVerified=false, loginSent=false;
bool oledSleeping=false;
uint32_t lastDisplayRxActivity=0;
String adminSaltHex, adminHashHex; // NVS'de sadece salt + SHA256 dogrulayicisi tutulur.
uint8_t securityFailures=0;
uint32_t securityBlockUntil=0;

String hexEncode(const uint8_t* b,size_t n){
  const char* digits="0123456789abcdef"; String out;out.reserve(n*2);
  for(size_t i=0;i<n;i++){out+=(char)digits[b[i]>>4];out+=(char)digits[b[i]&15];}
  return out;
}
String adminDigest(const String& saltHex,const String& password){
  // Salt'tan sonra parola; standart SHA-256, salt rastgele 128-bit.
  String input=saltHex+":"+password;
  uint8_t hash[32];
  mbedtls_sha256((const unsigned char*)input.c_str(),input.length(),hash,0);
  return hexEncode(hash,sizeof(hash));
}
bool constantEqual(const String& a,const String& b){
  if(a.length()!=b.length())return false;
  uint8_t diff=0;
  for(size_t i=0;i<a.length();i++)diff|=(uint8_t)(a[i]^b[i]);
  return diff==0;
}
bool checkAdminPassword(const String& password){
  if(adminSaltHex.length()==32 && adminHashHex.length()==64)
    return constantEqual(adminHashHex,adminDigest(adminSaltHex,password));
  return constantEqual(password,String(PANEL_PASSWORD)); // Ilk kurulum / eski surum.
}
void storeAdminPassword(const String& password){
  uint8_t salt[16];esp_fill_random(salt,sizeof(salt));
  String sh=hexEncode(salt,sizeof(salt));
  String hh=adminDigest(sh,password);
  prefs.putString("admsalt",sh);
  prefs.putString("admhash",hh);
  adminSaltHex=sh;adminHashHex=hh;
}
// HTTP Basic: sadece yetkili kullanici ve hash dogrulama.
bool validAdminAuthHeader(){
  String h=server.header("Authorization");
  if(!h.startsWith("Basic ") || h.length()>250)return false;
  String enc=h.substring(6);
  uint8_t out[160];size_t olen=0;
  if(mbedtls_base64_decode(out,sizeof(out)-1,&olen,(const unsigned char*)enc.c_str(),enc.length())!=0)return false;
  out[olen]=0;
  String credentials((const char*)out);
  int sep=credentials.indexOf(':');
  if(sep<1)return false;
  String user=credentials.substring(0,sep),pass=credentials.substring(sep+1);
  return constantEqual(user,String(PANEL_USER)) && checkAdminPassword(pass);
}

void displayPower(bool on){
  if(!oledOk || !oledDisplay || oledSleeping==!on)return;
  if(cfg.oledType==0)oledSsd.ssd1306_command(on?0xAF:0xAE);
  else oledSh.oled_command(on?0xAF:0xAE);
  oledSleeping=!on;
}
void onValidRxDisplay(){
  lastDisplayRxActivity=millis();
  if(oledSleeping)displayPower(true);
}
void maintainDisplaySleep(){
  if(!oledOk || cfg.oledType==2)return;
  if(cfg.oledSleepSeconds==0){if(oledSleeping)displayPower(true);return;}
  if((uint32_t)(millis()-lastDisplayRxActivity)>=((uint32_t)cfg.oledSleepSeconds*1000UL))
    displayPower(false);
}


String batteryLabel(){
  if(!isfinite(batteryVoltage)) return "---";
  return String(batteryVoltage,2)+"V";
}

// Batarya bolucusu sadece ADC okumasi suresince acik kalir.
// Olcumde 2.5-4.5 V araligi: Li-ion pil bagli degilse yaniltici veri gondermeyelim.
void sampleBattery(){
  digitalWrite(ADC_CTRL_PIN,ADC_CTRL_ON_STATE);
  delayMicroseconds(3000);
  uint32_t sum=0;
  for(int i=0;i<16;i++) sum+=(uint32_t)analogReadMilliVolts(BATTERY_PIN);
  digitalWrite(ADC_CTRL_PIN,!ADC_CTRL_ON_STATE);
  float raw=(sum/16.0f)*(BATTERY_DIVIDER_RATIO/1000.0f);
  float calibrated=raw+cfg.batteryCalibration;
  batteryVoltage=(isfinite(raw) && raw>=2.5f && raw<=4.5f &&
                  isfinite(calibrated) && calibrated>=2.5f && calibrated<=4.5f)?calibrated:NAN;
  lastBatterySample=millis();
  batterySampled=true;
}
void maintainBattery(){
  if(!batterySampled || (uint32_t)(millis()-lastBatterySample)>=BATTERY_SAMPLE_INTERVAL_MS) sampleBattery();
}

// APRS konum comment limitini korur; status mesaji degismez.
String beaconComment(){
  String comment=cfg.commentText;
  if(!cfg.beaconBattery || !isfinite(batteryVoltage))return comment;
  String suffix="Vbat="+String(batteryVoltage,2)+"V";
  if(suffix.length()>43)return comment;
  const size_t maxBase=43-suffix.length()-1;
  if(comment.length()>maxBase)comment.remove(maxBase);
  comment.trim();
  return comment.isEmpty()?suffix:comment+" "+suffix;
}

volatile bool rxFlag=false;
void IRAM_ATTR onRadioRx() { rxFlag=true; }

bool apEnabled=false;
String lastStation="---", aprsLine="";
uint32_t countRx=0, countValid=0, countSent=0, countDup=0, countDropped=0;
uint32_t lastScreen=0, lastWifiAttempt=0, lastAprsAttempt=0, disconnectedSince=0, lastKeepalive=0;
uint32_t aprsConnectStarted=0, lastBeacon=0;
bool beaconDueOnConnect=true;
uint32_t countBeacon=0, countStatus=0;
uint32_t lastStatus=0;
bool statusDueOnConnect=true;
uint32_t lastRxAt=0, countRejected=0;
float lastRssi=0, lastSnr=0;
// Auto Digi state: APRS-IS service health, NOT just Wi-Fi association.
bool digiMode=false;
uint32_t bootToken=0, restartAt=0;
bool bootRfPending=true;
RTC_DATA_ATTR uint8_t rapidBootCount=0;
static constexpr uint32_t BOOT_RF_WAIT_MS=10000UL;
String manualRfResult="", manualIsResult="";
bool otaFailed=false, otaStarted=false, otaSuccessful=false;
String otaError="";
uint32_t aprsDownFrom=0, lastDigiProbe=0;
uint32_t countDigiTx=0, countDigiBusy=0, countDigiLimit=0, countDigiFailed=0;
uint32_t lastDigiTx=0, txWindowStart=0;
// RF beacon has its own schedule but shares TX cooldown and hourly quota with relay TX.
uint32_t lastRfBeacon=0, lastRfBeaconAttempt=0, countRfBeacon=0, countRfBeaconFail=0;
bool rfBeaconDue=false;
// Web requests only schedule work; radio/TC P operations stay in loop().
bool manualRfPending=false, manualIsPending=false;
uint32_t lastManualRfRequest=0;
uint16_t txWindowCount=0;
// Only one pending relay, avoids uncontrolled packet accumulation when busy.
String digiPending="";
uint32_t digiPendingAt=0;
static constexpr uint32_t DIGI_AFTER_MS=60000UL;
static constexpr uint32_t TX_GAP_MS=30000UL;
static constexpr uint16_t TX_MAX_PER_HOUR=20;
// Web panelinden ayarlanir; varsayilan 10 dBm, bu kart icin ust sinir 21 dBm.


struct RxRecord { String station, packet, action; float rssi=0,snr=0; uint32_t receivedAt=0; };
RxRecord rxRecords[15];
uint8_t rxHead=0,rxCount=0;
struct EventRecord { String message; uint32_t at=0; };
EventRecord events[24];
uint8_t eventHead=0,eventCount=0;
void addEvent(const String& msg){events[eventHead].message=msg;events[eventHead].at=millis();eventHead=(eventHead+1)%24;if(eventCount<24)eventCount++;Serial.println("[OLAY] "+msg);}
void recordRx(const String& call,const String& packet,float rssi,float snr,const String& action){
  RxRecord &r=rxRecords[rxHead];r.station=call;r.packet=packet;r.rssi=rssi;r.snr=snr;r.action=action;r.receivedAt=millis();
  rxHead=(rxHead+1)%15;if(rxCount<15)rxCount++;
}
void updateRecentAction(const String& packet,const String& action){
  for(int i=0;i<rxCount;i++){
    uint8_t idx=(rxHead+15-1-i)%15;
    if(rxRecords[idx].packet==packet){rxRecords[idx].action=action;break;}
  }
}
String jsonEscape(const String& s){
  String r="\"";
  for(size_t i=0;i<s.length();i++){
    unsigned char c=s[i];
    if(c=='\"'||c=='\\'){r+='\\';r+=(char)c;}
    else if(c=='\n')r+="\\n";
    else if(c=='\r')r+="\\r";
    else if(c=='\t')r+="\\t";
    else if(c<32){char b[7];snprintf(b,sizeof(b),"\\u%04X",c);r+=b;}
    else r+=(char)c;
  }
  return r+"\"";
}
String rxClock(uint32_t at){
  // NTP time is optional; records fall back to device uptime.
  time_t t=time(nullptr);
  if(t>1700000000){t-=(millis()-at)/1000;struct tm tmv;localtime_r(&t,&tmv);char b[24];strftime(b,sizeof(b),"%H:%M:%S",&tmv);return String(b);}
  uint32_t secs=at/1000;char b[24];snprintf(b,sizeof(b),"+%02lu:%02lu:%02lu",(unsigned long)(secs/3600),(unsigned long)((secs/60)%60),(unsigned long)(secs%60));return String(b);
}
struct Pending { char line[300]; };
static Pending queueData[12];
uint8_t queueHead=0, queueTail=0, queueCount=0;
struct Seen { uint32_t hash=0, time=0; };
Seen seen[24];
uint8_t seenPtr=0;

String htmlEscape(const String& s) {
  String r;
  for(size_t i=0;i<s.length();i++) {
    char c=s[i];
    if(c=='&') r+="&amp;";
    else if(c=='<') r+="&lt;";
    else if(c=='>') r+="&gt;";
    else if(c=='\"') r+="&quot;";
    else if(c=='\'') r+="&#39;";
    else r+=c;
  }
  return r;
}

bool validAprsText(const String& value, size_t limit) {
  if(value.length()>limit) return false;
  for(size_t i=0;i<value.length();i++) {
    uint8_t c=(uint8_t)value[i];
    if(c<32 || c>126 || c=='|' || c=='~') return false;
  }
  return true;
}

void loadConfig(){
  prefs.begin("derman-aprs",false);
  cfg.call=prefs.getString("call","TA1XXX-10");
  cfg.apPassword=prefs.getString("apkey",DEFAULT_AP_PASSWORD);
  if(cfg.apPassword.length()<8 || cfg.apPassword.length()>63)cfg.apPassword=DEFAULT_AP_PASSWORD;
  cfg.ssid=prefs.getString("ssid","");
  cfg.wifiPassword=prefs.getString("wifi","");
  cfg.host=prefs.getString("host","euro.aprs2.net");
  cfg.passcode=prefs.getString("pass","");
  cfg.port=prefs.getUShort("port",14580);
  cfg.latitude=prefs.getDouble("lat",0);
  cfg.longitude=prefs.getDouble("lon",0);
  cfg.beaconMinutes=prefs.getUShort("bmin",30);
  if(cfg.beaconMinutes<10 || cfg.beaconMinutes>120)cfg.beaconMinutes=30;
  cfg.beaconEnabled=prefs.getBool("bena",false);
  cfg.symbolTable=prefs.getString("symtab","L");
  cfg.symbolCode=prefs.getString("symcode","&");
  if(cfg.symbolTable.length()!=1 || cfg.symbolCode.length()!=1){cfg.symbolTable="L";cfg.symbolCode="&";}
  cfg.commentText=prefs.getString("comment","TA1FM LoRa iGate");
  cfg.statusText=prefs.getString("status","TA1FM LoRa APRS iGate &Digi");
  cfg.statusEnabled=prefs.getBool("stena",true);
  cfg.statusMinutes=prefs.getUShort("stmin",60);
  if(cfg.statusMinutes<15 || cfg.statusMinutes>240) cfg.statusMinutes=60;
  // Defensive cleanup of any previously stored invalid data.
  if(cfg.commentText.length()>43) cfg.commentText.remove(43);
  if(cfg.statusText.length()>62) cfg.statusText.remove(62);
  cfg.autoDigi=prefs.getBool("autodigi",false);
  cfg.aprsIsEnabled=prefs.getBool("isenabled",true);
  cfg.wide1=prefs.getBool("wide1",true);
  cfg.wide2=prefs.getBool("wide2",false);
  cfg.rfBeaconPath=prefs.getString("rfpath","WIDE1-1");
  if(cfg.rfBeaconPath!="DIRECT" && cfg.rfBeaconPath!="WIDE1-1" && cfg.rfBeaconPath!="WIDE2-1" && cfg.rfBeaconPath!="WIDE2-2") cfg.rfBeaconPath="WIDE1-1";
  cfg.txPowerDbm=prefs.getUChar("txpwr",10);
  if(cfg.txPowerDbm<10 || cfg.txPowerDbm>21) cfg.txPowerDbm=10;
  cfg.oledType=prefs.getUChar("oledtype",0);
  if(cfg.oledType>2)cfg.oledType=0;
  cfg.beaconBattery=prefs.getBool("batbeacon",false);
  cfg.batteryCalibration=prefs.getFloat("batcal",0.0f);
  if(!isfinite(cfg.batteryCalibration) || cfg.batteryCalibration < -1.0f || cfg.batteryCalibration > 1.0f) cfg.batteryCalibration=0.0f;
  cfg.oledSleepSeconds=prefs.getUShort("oledsleep",0);
  if(cfg.oledSleepSeconds!=0 && cfg.oledSleepSeconds!=15 && cfg.oledSleepSeconds!=30 &&
     cfg.oledSleepSeconds!=60 && cfg.oledSleepSeconds!=300 && cfg.oledSleepSeconds!=600)
     cfg.oledSleepSeconds=0;
  adminSaltHex=prefs.getString("admsalt","");
  adminHashHex=prefs.getString("admhash","");
  cfg.probeMinutes=prefs.getUChar("probemin",10);
  if(cfg.probeMinutes!=1 && cfg.probeMinutes!=5 && cfg.probeMinutes!=10 && cfg.probeMinutes!=15)cfg.probeMinutes=10;

}

bool validCall(String call){
  call.trim(); call.toUpperCase();
  int dash=call.indexOf('-');
  String base=dash<0 ? call:call.substring(0,dash);
  if(base.length()<3 || base.length()>6) return false;
  for(size_t i=0;i<base.length();i++) if(!isalnum((unsigned char)base[i])) return false;
  if(dash>=0){
    String ssid=call.substring(dash+1);
    if(ssid.isEmpty() || ssid.length()>2) return false;
    for(size_t i=0;i<ssid.length();i++) if(ssid[i]<'0'||ssid[i]>'9') return false;
    if(ssid.toInt()>15) return false;
  }
  return true;
}

void screenUpdate(){
  if(!oledOk || !oledDisplay || oledSleeping)return;
  Adafruit_GFX &d=*oledDisplay;
  if(cfg.oledType==0)oledSsd.clearDisplay();
  else oledSh.clearDisplay();
  d.setTextColor(1);d.setTextWrap(false);
  // Cagri isareti solda; pil voltaji sagda (2x fontu pil alani icin daraltiyoruz).
  d.setTextSize(1);d.setCursor(0,1);
  d.print(cfg.call.substring(0,12));
  d.setCursor(96,1);d.print(batteryLabel());
  d.drawLine(0,18,127,18,1);
  d.setCursor(0,23);d.print("WiFi: ");d.print(WiFi.status()==WL_CONNECTED?"OK":"YOK");
  d.setCursor(65,23);d.print("IS: ");
  d.print(cfg.aprsIsEnabled && WiFi.status()==WL_CONNECTED && aprs.connected() && aprsVerified?"OK":"YOK");
  d.setCursor(0,35);d.print("IP: ");
  if(WiFi.status()==WL_CONNECTED)d.print(WiFi.localIP().toString());
  else if(apEnabled)d.print(WiFi.softAPIP().toString());
  else d.print("---");
  d.setCursor(0,49);d.print("SON RX: ");d.print(lastStation.substring(0,15));
  if(cfg.oledType==0)oledSsd.display();
  else oledSh.display();
}

void beginDisplay(){
  oledOk=false;oledDisplay=nullptr;oledSleeping=false;lastDisplayRxActivity=millis();
  if(cfg.oledType==2){Serial.println("[OLED] Panelden KAPALI");return;}
  Wire1.begin(PIN_SDA,PIN_SCL);Wire1.setClock(100000);
  if(cfg.oledType==0){
    oledOk=oledSsd.begin(SSD1306_SWITCHCAPVCC,0x3C);
    if(oledOk)oledDisplay=&oledSsd;
  }else{
    oledOk=oledSh.begin(0x3C,true);
    if(oledOk)oledDisplay=&oledSh;
  }
  Serial.printf("[OLED] %s: %s\n",cfg.oledType==0?"SSD1306":"SH1106",oledOk?"HAZIR":"BASLATILAMADI");
}

void startAp(){
  if(apEnabled) return;
  if(WiFi.status()==WL_CONNECTED) return;
  WiFi.mode(WIFI_AP_STA);
  apEnabled=WiFi.softAP("TA1FM-APRS-SETUP",cfg.apPassword.c_str());
  if(apEnabled) Serial.printf("Kurulum AP: TA1FM-APRS-SETUP / IP: %s\n", WiFi.softAPIP().toString().c_str());
}
void stopAp(){
  if(!apEnabled) return;
  WiFi.softAPdisconnect(true); apEnabled=false;
  WiFi.mode(WIFI_STA);
}
void beginWifi(){
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if(cfg.ssid.length()) {
    WiFi.begin(cfg.ssid.c_str(),cfg.wifiPassword.c_str());
    lastWifiAttempt=millis(); disconnectedSince=millis();
  } else startAp();
}
void maintainWifi(){
  if(WiFi.status()==WL_CONNECTED){
    disconnectedSince=0;
    if(apEnabled) stopAp();
    return;
  }
  if(disconnectedSince==0) disconnectedSince=millis();
  if(cfg.ssid.isEmpty()){startAp();return;}
  if(millis()-lastWifiAttempt>=15000){
    lastWifiAttempt=millis();
    WiFi.disconnect(false,false);
    WiFi.begin(cfg.ssid.c_str(),cfg.wifiPassword.c_str());
  }
  if(millis()-disconnectedSince>=90000) startAp();
}

bool accessAllowed(){
  if(securityBlockUntil && (int32_t)(millis()-securityBlockUntil)<0){
    server.send(429,"text/plain; charset=utf-8","Cok fazla hatali giris. Biraz sonra tekrar deneyin.");return false;
  }
  if(validAdminAuthHeader()){securityFailures=0;securityBlockUntil=0;return true;}
  if(++securityFailures>=10){securityFailures=0;securityBlockUntil=millis()+30000UL;}
  server.requestAuthentication();return false;
}

String pageHeader(const String& title){
  String p=R"HTML(<!doctype html><html lang="tr"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>TA1FM APRS</title><style>
  body{font:16px system-ui,Arial;background:#101924;color:#f4f6f9;margin:0;padding:12px 18px 25px}
  main{max-width:720px;margin:auto}.card{background:#1d2a38;border-radius:12px;padding:18px;margin:12px 0}
  h1{font-size:23px}h2{font-size:18px}a{color:#7dd3fc}label{display:block;margin:12px 0 5px}
  input{box-sizing:border-box;width:100%;padding:11px;font-size:16px;border:1px solid #667789;border-radius:7px;background:#0d1722;color:#fff}
  button{cursor:pointer;background:#1d90b7;border:0;border-radius:8px;color:#fff;padding:13px 16px;margin-top:16px;font-weight:600}
  .muted{color:#acbacb}.ok{color:#74e4a2}.bad{color:#ffbb7a}small{display:block;margin:8px 0}
  .topbar{position:sticky;top:0;z-index:15;background:#101924f5;backdrop-filter:blur(8px);padding:12px 0 10px;border-bottom:1px solid #324b5d;margin-bottom:13px}
  .brand{font-weight:750;font-size:20px;margin-bottom:10px;color:#f3fbff}
  .navitems{display:flex;gap:7px;flex-wrap:wrap}
  .navitem{display:inline-flex;align-items:center;justify-content:center;gap:5px;text-decoration:none;color:#dbe9f4;background:#1d2a38;border:1px solid #3e5669;border-radius:9px;padding:10px 12px;font-size:13px;font-weight:650;white-space:nowrap}
  .navitem:hover{background:#2b4154;border-color:#63bfe8}.navitem.active{background:#087ba0;border-color:#53cafa;color:#fff}
  .actions{display:flex;flex-wrap:wrap;align-items:center;gap:9px;margin-top:14px}
  .actions button{margin-top:0;flex:1 1 150px}.actions button.secondary{background:#274761}
  .actions button:disabled{background:#37434b;color:#a8b5c1;opacity:.65;cursor:not-allowed}
  @media(max-width:500px){.navitem{flex:1 1 110px}.brand{font-size:18px}}
  .links a{display:inline-block;margin:5px 12px 5px 0}.notice{position:fixed;right:18px;top:18px;z-index:20;max-width:350px;border:1px solid #32566b;border-radius:10px;padding:14px;background:#183249;color:#fff;box-shadow:0 8px 25px #0009;display:none}
  .notice.error{background:#5b2830}.notice.success{background:#194a3b}.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
  select{box-sizing:border-box;width:100%;padding:11px;background:#0d1722;color:#fff;border-radius:7px}
  table{border-collapse:collapse;width:100%;font-size:13px}td,th{padding:9px 6px;border-bottom:1px solid #385064;text-align:left}
  td.info{word-break:break-all}.subtle{font-size:13px;color:#a8bfd0}
  </style></head><body><div id='notice' class='notice' role='status' aria-live='polite'></div><main>)HTML";
  p+=R"JS(<script>
let toastTimer;
function toast(msg,type='success',ms=4500){const x=document.getElementById('notice');x.textContent=msg;x.className='notice '+type;x.style.display='block';clearTimeout(toastTimer);if(ms)toastTimer=setTimeout(()=>{x.style.display='none'},ms);}
async function postAction(url){
  if(url==='/beacon/rf'&&!confirm('Gercek RF yayini yapilacak. Onayliyor musunuz?'))return;
  try{const r=await fetch(url,{method:'POST',cache:'no-store'});const t=await r.text();
    toast(t,r.ok?'success':'error',r.ok?0:9000);if(!r.ok)return;
    const key=url.endsWith('/rf')?'rf':'is';
    for(let i=0;i<35;i++){
      await new Promise(resolve=>setTimeout(resolve,1000));
      const a=await fetch('/api/action-result?'+Date.now(),{cache:'no-store'}),x=await a.json();
      if(x[key]==='OK'){toast('Beacon basariyla gonderildi.','success');return;}
      if(x[key]==='HATA'){toast('Beacon gonderilemedi. RF kanal veya baglanti kontrol edin.','error',9000);return;}
    }
    toast('Beacon hala bekliyor veya zaman asimi. Seri monitor / olaylara bakin.','error',9000);
  }catch(e){toast('Cihaza erisilemiyor: '+e,'error',9000)}
}
async function checkReboot(oldToken){
  toast('Cihaz yeniden baslatiliyor / baglanti kontrol ediliyor...','success',0);
  for(let i=0;i<90;i++){
    await new Promise(r=>setTimeout(r,1000));
    try{const r=await fetch('/health?'+Date.now(),{cache:'no-store'});
      if(!r.ok)continue;
      const x=await r.json();
      if(x.boot!==oldToken){sessionStorage.setItem('ta1fm_notice','Yeniden baglandi - sistem hazir.');location.reload();return;}
    }catch(e){}
  }
  toast('Yeniden baglanti saglanamadi. Wi-Fi / IP degismisse yeni adresten acin.','error',0);
}
window.addEventListener('DOMContentLoaded',()=>{const m=sessionStorage.getItem('ta1fm_notice');if(m){sessionStorage.removeItem('ta1fm_notice');toast(m)}});
</script>)JS";
  String active="/";
  if(title.startsWith("Ayarlar")) active="/settings";
  else if(title.startsWith("Son 15 RX")) active="/rx";
  else if(title.startsWith("Son olaylar")) active="/events";
  else if(title.startsWith("OTA")) active="/ota";
  else if(title.startsWith("Guvenlik")) active="/security";
  p+="<header class='topbar'><div class='brand'>&#128225; TA1FM LoRa APRS iGate & Digi</div><nav class='navitems' aria-label='Sayfa menusu'>";
  for(const char* path : {"/","/rx","/settings","/ota","/events","/security"}){
    String dest=path;
    String label=dest=="/"?"&#8962; Ana Panel":dest=="/rx"?"&#128225; Son 15 RX":dest=="/settings"?"&#9881; Ayarlar":dest=="/ota"?"&#8679; OTA Guncelle":dest=="/security"?"&#128274; Guvenlik":"&#9776; Olaylar";
    p+="<a class='navitem"+String(active==dest?" active":"")+"' href='"+dest+"'"+String(active==dest?" aria-current='page'":"")+">"+label+"</a>";
  }
  p+="</nav></header><p class='muted'>"+htmlEscape(title)+"</p>";
  return p;
}
void handleHome(){
  String p=pageHeader("V1.5.8 — 433.775 MHz / SF12 / BW125 / RX");
  p+="<div class='card'><h2>Canli Durum</h2>";
  p+="<p>Istasyon: <b>"+htmlEscape(cfg.call)+"</b></p>";
  p+="<p>Wi-Fi: <b>"+String(WiFi.status()==WL_CONNECTED?"Bagli":"Bagli degil")+"</b></p>";
  p+="<p>Pil voltaji: <b>"+batteryLabel()+"</b> | Kalibrasyon: <b>"+String(cfg.batteryCalibration,2)+" V</b> | OLED: <b>"+String(cfg.oledType==0?"SSD1306":cfg.oledType==1?"SH1106":"KAPALI")+"</b></p>";
  p+="<p>Beacon'da Vbat: <b>"+String(cfg.beaconBattery?"ACIK":"KAPALI")+"</b> | OLED zamanlama: "+String(cfg.oledSleepSeconds?String(cfg.oledSleepSeconds)+" sn":"Kapali (surekli acik)")+"</p>";
  p+="<p>IP: <b>"+String(WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():(apEnabled?WiFi.softAPIP().toString():"---"))+"</b></p>";
  p+="<p>APRS-IS: <b id='isState'>"+String(!cfg.aprsIsEnabled?"MANUEL KAPALI":(aprsVerified?"DOGRULANDI":"BAGLI DEGIL"))+"</b></p>";
  p+="<div class='actions' aria-label='Haberlesme kontrolleri'>";
  p+="<button id='isToggleBtn' type='button' onclick='toggleIs()'>APRS-IS Ac / Kapat</button>";
  p+="<button id='isBeaconBtn' class='secondary' type='button' onclick=\"postAction('/beacon/is')\" "+String(cfg.aprsIsEnabled && aprsVerified?"":"disabled title='APRS-IS bagli degil'")+">APRS-IS Beacon Gonder</button>";
  p+="<button class='secondary' type='button' onclick=\"postAction('/beacon/rf')\">RF Beacon Gonder (TX)</button>";
  p+="</div><small>Manuel RF gonderimde kanal ve TX limitleri korunur. APRS-IS kapaliysa internet beacon butonu pasiftir.</small>";
  p+=R"JS(<script>
async function refreshIsControl(){try{const r=await fetch('/api/is-state?'+Date.now(),{cache:'no-store'});if(!r.ok)return;const s=await r.json();document.getElementById('isBeaconBtn').disabled=!(s.enabled&&s.verified);document.getElementById('isState').textContent=!s.enabled?'MANUEL KAPALI':s.verified?'DOGRULANDI':'BAGLI DEGIL';}catch(e){}}
setInterval(refreshIsControl,5000);
async function toggleIs(){
 if(!confirm('APRS-IS durumunu degistirmek istiyor musunuz? Wi-Fi bagli kalacak.'))return;
 try{const r=await fetch('/api/toggle-is',{method:'POST'});const t=await r.text();toast(t,r.ok?'success':'error',6500);if(r.ok){document.getElementById('isState').textContent=t;document.getElementById('isBeaconBtn').disabled=true;}}catch(e){toast(e.message,'error',10000)}
}
</script>)JS";
  p+="<p>Calisma suresi: "+String(millis()/60000UL)+" dk | WiFi RSSI: "+String(WiFi.status()==WL_CONNECTED?String(WiFi.RSSI())+" dBm":"---")+" | Son restart: "+String((int)esp_reset_reason())+"</p>";
  p+="<p>Son duyulan: <b>"+htmlEscape(lastStation)+"</b></p>";
  p+="<p>Son RF alimi: <b>"+String(lastRxAt ? String((millis()-lastRxAt)/1000)+" sn once" : "Henuz yok")+"</b>";
  if(lastRxAt) p+=" | RSSI: "+String(lastRssi,1)+" dBm | SNR: "+String(lastSnr,1)+" dB";
  p+="</p>";
  p+="<p>LoRa paket: "+String(countRx)+" | APRS: "+String(countValid)+" | Aktarilan: "+String(countSent)+"</p>";
  p+="<p>Tekrar: "+String(countDup)+" | APRS disi/gecersiz: "+String(countRejected)+" | Kuyruk kaybi: "+String(countDropped)+"</p>";
  p+="<p>Konum beacon: <b>"+String(cfg.beaconEnabled?"ACIK":"KAPALI")+"</b> | Gonderilen: "+String(countBeacon)+" | Aralik: "+String(cfg.beaconMinutes)+" dk | Sembol: <b>"+htmlEscape(cfg.symbolTable+cfg.symbolCode)+"</b></p>";
  p+="<p>Konum yorumu (gonderilen): "+htmlEscape(beaconComment())+"</p>";
  p+="<p>Status: <b>"+String(cfg.statusEnabled?"ACIK":"KAPALI")+"</b> | Gonderilen: "+String(countStatus)+" | Aralik: "+String(cfg.statusMinutes)+" dk</p>";
  p+="<p>Durum mesaji: "+htmlEscape(cfg.statusText)+"</p>";
  p+="<p>Auto Digipeater: <b>"+String(cfg.autoDigi?"ACIK":"KAPALI")+"</b> | Aktif mod: <b>"+String(digiMode?"DIGIPEATER":"iGATE/RX")+"</b></p>";
  p+="<p>TX gucu: <b>"+String(cfg.txPowerDbm)+" dBm</b> (RF Beacon ve Digipeater)</p>";
  p+="<p>WIDE1: "+String(cfg.wide1?"ACIK":"KAPALI")+" | WIDE2: "+String(cfg.wide2?"ACIK":"KAPALI")+" | RF Beacon Path: <b>"+htmlEscape(cfg.rfBeaconPath)+"</b> | Kontrol: "+String(cfg.probeMinutes)+" dk</p>";
  p+="<p>Digi TX: "+String(countDigiTx)+" | RF Beacon TX: "+String(countRfBeacon)+" | RF Beacon hata: "+String(countRfBeaconFail)+"</p>";
  p+="<p>RF Mesgul: "+String(countDigiBusy)+" | Saatlik limit: "+String(countDigiLimit)+" | Digi TX hata: "+String(countDigiFailed)+"</p>";
  p+="<p class='muted'>Otomatik Digi TX APRS-IS 60 sn kesikse veya manuel kapaliysa calisir. Manuel RF TX onay ve kanal kontrolune tabidir.</p>";
  p+="</div><div class='card'><h2>Sistem Yonetimi</h2>";
  p+="<button type='button' onclick='restartNow()'>Cihazi Yeniden Baslat (Restart)</button>";
  p+="<small>Yonetici yetkisi gerektirir. Kayitli ayarlar silinmez; aktif baglantilar kisa sure kesilir.</small></div>";
  p+=R"JS(<script>
async function restartNow(){
 if(!confirm('TA1FM cihazi yeniden baslatilacak. Emin misiniz?'))return;
 try{const a=await fetch('/health',{cache:'no-store'});if(!a.ok)throw Error('Yetki veya baglanti hatasi');const old=(await a.json()).boot;
 const r=await fetch('/restart',{method:'POST',cache:'no-store'});const t=await r.text();if(!r.ok)throw Error(t);
 checkReboot(old);}catch(e){toast('Restart hatasi: '+e.message,'error',9000)}
}
</script>)JS";
  p+="<small>Durumu guncellemek icin sayfayi yenileyin.</small></main></body></html>";
  server.send(200,"text/html; charset=utf-8",p);
}
void handleSettings(){
  if(!accessAllowed())return;
  String p=pageHeader("Ayarlar — kaydettikten sonra cihaz yeniden baslar");
  p+="<div class='card'><form id='settingsForm' method='POST' action='/save' onsubmit='return saveSettings(event)'>";
  p+="<h2>OLED ekran ve pil voltaji</h2>";
  p+="<label>OLED ekran tipi</label><select name='oledtype'>";
  for(int i=0;i<3;i++){
    String v=i==0?"SSD1306":i==1?"SH1106":"OLED Kapali";
    p+="<option value='"+String(i)+"' "+String(cfg.oledType==i?"selected":"")+">"+v+"</option>";
  }
  p+="</select><small>OLED secimi kaydedildikten sonra yeniden baslatilarak uygulanir. Ekran yoksa iGate calismaya devam eder.</small>";
  p+="<label>OLED otomatik uyku - son gecerli RX paketinden sonra</label><select name='oledsleep'>";
  for(int secs : {0,15,30,60,300,600}){
    String caption=secs==0?"Zamanlama Kapali (ekran surekli acik)":secs==300?"5 dakika":secs==600?"10 dakika":String(secs)+" saniye";
    p+="<option value='"+String(secs)+"' "+String(cfg.oledSleepSeconds==secs?"selected":"")+">"+caption+"</option>";
  }
  p+="</select><small>Sure dolunca OLED uyur; yeni gecerli APRS RX paketinde uyanir. iGate/Digi devam eder. OLED Kapali ayri secenektir.</small>";
  p+="<p>Pil voltaji: <b>"+batteryLabel()+"</b> (30 saniyede bir olculur)</p>";
  p+="<label>Pil Voltaji Kalibrasyonu (+/- Volt)</label>";
  p+="<input name='batcal' type='number' step='0.01' min='-1.00' max='1.00' required value='"+String(cfg.batteryCalibration,2)+"'>";
  p+="<small>Multimetrede 4.13V, ekranda 3.93V ise +0.20 girin. Eksi deger voltaji dusurur. Kaydedilince OLED / Web / APRS Vbat ortak duzeltilir.</small>";
  p+="<label>iGate cagri isareti (SSID dahil)</label><input name='call' maxlength='9' required value='"+htmlEscape(cfg.call)+"'>";
  p+="<label>Wi-Fi ag adi (SSID)</label><input name='ssid' maxlength='32' value='"+htmlEscape(cfg.ssid)+"'>";
  p+="<label>Wi-Fi sifresi</label><input name='wifi' type='text' maxlength='63' value='"+htmlEscape(cfg.wifiPassword)+"'>";
  p+="<label>APRS-IS sunucusu</label><input name='host' maxlength='80' required value='"+htmlEscape(cfg.host)+"'>";
  p+="<label>APRS-IS port</label><input name='port' type='number' min='1' max='65535' value='"+String(cfg.port)+"'>";
  p+="<label>APRS-IS passcode</label><input name='pass' type='text' maxlength='8' value='"+htmlEscape(cfg.passcode)+"'>";
  p+="<h2>APRS-IS Kontrol</h2>";
  p+="<label><input style='width:auto' type='checkbox' name='isenabled' value='1' "+String(cfg.aprsIsEnabled?"checked":"")+"> APRS-IS baglantisi acik (Wi-Fi bagimsiz)</label>";
  p+="<small>Kapali secilirse Wi-Fi/web kalir, Auto Digi etkinse hemen DIGI moduna gecer. Acilinca APRS-IS'e yeniden baglanir.</small>";
  p+="<h2>iGate Konumu (APRS.fi)</h2>";
  p+="<label>Enlem (ondalik derece, kuzey + / guney -)</label><input name='lat' type='number' step='any' min='-90' max='90' value='"+String(cfg.latitude,6)+"'>";
  p+="<label>Boylam (ondalik derece, dogu + / bati -)</label><input name='lon' type='number' step='any' min='-180' max='180' value='"+String(cfg.longitude,6)+"'>";
  p+="<h2>Harita sembolu</h2>";
  p+="<label>Hazir semboller</label><select id='preset' style='width:100%;padding:11px;background:#0d1722;color:#fff' onchange='pickSymbol()'>";
  p+="<option value=''>Ozel / mevcut kod</option><option value='L&'>LoRa iGate (L&amp;)</option>";
  p+="<option value='R&'>RX-only iGate (R&amp;)</option><option value='L#'>LoRa Digipeater (L#)</option>";
  p+="</select>";
  p+="<label>Symbol Table / Overlay (tek karakter)</label><input id='stab' name='stab' maxlength='1' required value='"+htmlEscape(cfg.symbolTable)+"'>";
  p+="<label>Symbol Code (tek karakter)</label><input id='scode' name='scode' maxlength='1' required value='"+htmlEscape(cfg.symbolCode)+"'>";
  p+="<small>LoRa iGate icin varsayilan L&amp;. Sembolun haritadaki rengi aprs.fi tarafindan belirlenir.</small>";
  p+="<script>function pickSymbol(){let v=document.getElementById('preset').value;if(v.length===2){document.getElementById('stab').value=v[0];document.getElementById('scode').value=v[1]}}</script>";
  p+="<label>Beacon araligi (dakika, 10-120)</label><input name='bmin' type='number' min='10' max='120' value='"+String(cfg.beaconMinutes)+"'>";
  p+="<label><input style='width:auto' type='checkbox' name='bena' value='1' "+String(cfg.beaconEnabled?"checked":"")+"> Konumumu APRS-IS'e gonder</label>";
  p+="<small>GPS gerekmez. iGate modunda APRS-IS konum beacon; Auto Digi ACIK ve Digi modu aktifken ayni beacon RF ile yayina verilir. Koordinatlar bos/0,0 ise etkinlestirilemez.</small>";
  p+="<small>APRS-IS aktarimi icin dogrulanmis amatör telsiz cagri isareti ve uygun passcode gereklidir.</small>";
  p+="<h2>APRS Comment &amp; Status</h2>";
  p+="<label>Comment Text (konum paketine eklenir, en fazla 43 karakter)</label>";
  p+="<input name='comment' maxlength='43' value='"+htmlEscape(cfg.commentText)+"'>";
  p+="<label><input style='width:auto' type='checkbox' name='batbeacon' value='1' "+String(cfg.beaconBattery?"checked":"")+"> Beacon comment alanina pil voltaji ekle (Vbat=3.92V)</label>";
  p+="<small>Acikken yorumun sonuna olculen voltaj eklenir. Toplam 43 karakteri asmamak icin yorum gerekirse kisaltilir. Olcum gecersizse Vbat eklenmez. RF ve APRS-IS konum Beacon'lari icin gecerlidir.</small>";
  p+="<label>Status Message (ayri APRS paketi, en fazla 62 karakter)</label>";
  p+="<input name='status' maxlength='62' value='"+htmlEscape(cfg.statusText)+"'>";
  p+="<label><input style='width:auto' type='checkbox' name='stena' value='1' "+String(cfg.statusEnabled?"checked":"")+"> Status mesajini APRS-IS'e gonder</label>";
  p+="<label>Status gonderim araligi (dakika, 15-240)</label>";
  p+="<input name='stmin' type='number' min='15' max='240' value='"+String(cfg.statusMinutes)+"'>";
  p+="<small>Metinlerde APRS uyumlulugu icin ASCII karakter kullanin (Turkce ozel karakter kullanmayin).</small>";

  p+="<h2>Auto Digipeater (yedek calisma)</h2>";
  p+="<label>LoRa TX gucu (10-21 dBm)</label><select name='txpwr' style='width:100%;padding:11px;background:#0d1722;color:white'>";
  for(int val : {10,14,17,20,21}) p+="<option value='"+String(val)+"' "+String(cfg.txPowerDbm==val?"selected":"")+">"+String(val)+" dBm</option>";
  p+="</select><small>Bu ayar RF Beacon ve Digipeater yayinlarina uygulanir. 21 dBm en yuksek secenektir; yasal EIRP sinirlarini, anten kazancini ve kablo kaybini kontrol edin.</small>";
  p+="<label><input style='width:auto' type='checkbox' name='autodigi' value='1' "+String(cfg.autoDigi?"checked":"")+"> Auto Digipeater ACIK (RF TX izni)</label>";
  p+="<label><input style='width:auto' type='checkbox' name='wide1' value='1' "+String(cfg.wide1?"checked":"")+"> WIDE1-1 paketlerini tekrarla</label>";
  p+="<label><input style='width:auto' type='checkbox' name='wide2' value='1' "+String(cfg.wide2?"checked":"")+"> WIDE2-1 / WIDE2-2 paketlerini tekrarla</label>";
  p+="<label>Kendi RF Beacon Path</label><select name='rfpath' style='width:100%;padding:11px;background:#0d1722;color:white'>";
  for(const char* choice : {"DIRECT","WIDE1-1","WIDE2-1","WIDE2-2"}){
    String v=choice;
    p+="<option value='"+v+"' "+String(cfg.rfBeaconPath==v?"selected":"")+">"+v+"</option>";
  }
  p+="</select><small>Yalniz kendi RF konum beacon'ina uygulanir; APRS-IS beacon ve Digipeater paketleri etkilenmez. WIDE2-2 en fazla iki tekrar istegidir, garanti degildir.</small>";
  p+="<label>Digipeater modundayken APRS-IS kontrol araligi</label><select name='probemin' style='width:100%;padding:11px;background:#0d1722;color:white'>";
  for(int val : {1,5,10,15}) p+="<option value='"+String(val)+"' "+String(cfg.probeMinutes==val?"selected":"")+">"+String(val)+" dakika</option>";
  p+="</select>";
  p+="<small>Internet/APRS-IS 60 sn kesilirse veya APRS-IS manuel kapatilirsa Digi modu. Digi RF beacon, konum beacon ayari aciksa etkin olur. Tekrarlama + RF beacon: en az 30 sn ara, toplam en fazla 20 TX/saat. TX gucu yukaridan ayarlanir. APRS-IS dogrulaninca iGate'e doner.</small>";
  p+="<small>Varsayilan KAPALI. RF yayin izniniz, anteniniz ve yerel kurallar uygun degilse acmayin. Web paneli testleri tek basina TX uyumlulugunu kanitlamaz.</small>";
  p+="<button type='submit'>Ayarlari Kaydet</button> ";
  p+="<button type='button' onclick='backupSettings()'>Ayarlari Indir (JSON)</button> ";
  p+="<button type='button' onclick=\"document.getElementById('restoreFile').click()\">Ayarlari Yukle</button>";
  p+="<input type='file' id='restoreFile' accept='.json,application/json' style='display:none' onchange='restoreSettings(this)'>";
  p+="<small>Yedek JSON dosyasi Wi-Fi ve APRS sifrelerini ACIK METIN olarak icerir; guvenli yerde saklayin. Yukleme sonrasi kaydetmeden once alanlar dogrulanir.</small>";
  p+="</form></div>";
  p+=R"JS(<script>
function settingsObject(){const f=document.getElementById('settingsForm'),out={_format:'TA1FM-V1.5.8-settings'};for(const e of f.elements){if(!e.name)continue;if(e.type==='checkbox')out[e.name]=e.checked;else out[e.name]=e.value;}return out;}
function backupSettings(){const b=new Blob([JSON.stringify(settingsObject(),null,2)],{type:'application/json'}),a=document.createElement('a');a.href=URL.createObjectURL(b);a.download='TA1FM_ayarlar_yedek.json';a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000);toast('Ayarlar JSON olarak indirildi. Dosyada sifreler acik metindir.');}
async function restoreSettings(inp){const f=document.getElementById('settingsForm');try{const file=inp.files[0];if(!file||file.size>20000)throw Error('Dosya cok buyuk veya bulunamadi');const d=JSON.parse(await file.text());if(!['TA1FM-V1.5.7-settings','TA1FM-V1.5.8-settings'].includes(d._format))throw Error('TA1FM ayar dosyasi degil');for(const e of f.elements){if(!e.name)continue;if(!(e.name in d)){if(['oledtype','batbeacon','oledsleep','batcal'].includes(e.name))continue;throw Error('Eksik alan: '+e.name);}if(e.type==='checkbox'){if(typeof d[e.name]!=='boolean')throw Error('Hatali alan: '+e.name);e.checked=d[e.name]}else{if(typeof d[e.name]!=='string')throw Error('Hatali alan: '+e.name);e.value=d[e.name];}}toast('Yedek forma yuklendi. Kontrol edip Kaydet butonuna basin.', 'success',9000);}catch(e){toast('Yedek acilamadi: '+e.message,'error',9000);}finally{inp.value='';}}
async function saveSettings(event){event.preventDefault();const f=document.getElementById('settingsForm');if(!f.reportValidity())return false;let old=0;try{let h=await fetch('/health',{cache:'no-store'});old=(await h.json()).boot;const r=await fetch('/save',{method:'POST',body:new FormData(f)});const msg=await r.text();if(!r.ok){toast(msg,'error',10000);return false;}toast(msg,'success',0);checkReboot(old);}catch(e){toast('Kayit hatasi: '+e.message,'error',10000);}return false;}
</script>)JS";
  p+="</main></body></html>";
  server.send(200,"text/html; charset=utf-8",p);
}
void handleSave(){
  if(!accessAllowed())return;
  String call=server.arg("call"); call.trim();call.toUpperCase();
  String ssid=server.arg("ssid"),host=server.arg("host");host.trim();
  String portText=server.arg("port"),newPass=server.arg("pass"),newWifi=server.arg("wifi");
  int port=portText.toInt();
  String latText=server.arg("lat"),lonText=server.arg("lon");
  double lat=latText.toDouble(),lon=lonText.toDouble();
  int bmin=server.arg("bmin").toInt();
  bool bena=server.hasArg("bena");
  String stab=server.arg("stab"),scode=server.arg("scode");
  String comment=server.arg("comment"),status=server.arg("status");
  int stmin=server.arg("stmin").toInt();
  bool stena=server.hasArg("stena");
  int oledtype=server.arg("oledtype").toInt();
  int oledsleep=server.arg("oledsleep").toInt();
  bool batbeacon=server.hasArg("batbeacon");
  String calText=server.arg("batcal"); calText.trim();
  char* calEnd=nullptr;
  float batcal=strtof(calText.c_str(),&calEnd);
  if(!server.hasArg("batcal") || calText.isEmpty() || !calEnd || *calEnd!='\0' ||
     !isfinite(batcal) || batcal < -1.0f || batcal > 1.0f){
    server.send(400,"text/plain; charset=utf-8","Pil kalibrasyonu -1.00 ile +1.00 V arasinda olmali.");return;
  }
  if(!server.hasArg("oledtype") || oledtype<0 || oledtype>2){
    server.send(400,"text/plain; charset=utf-8","OLED tipi gecersiz.");return;
  }
  if(!server.hasArg("oledsleep") ||
     (oledsleep!=0 && oledsleep!=15 && oledsleep!=30 && oledsleep!=60 && oledsleep!=300 && oledsleep!=600)){
    server.send(400,"text/plain; charset=utf-8","OLED uyku suresi gecersiz.");return;
  }
  bool autodigi=server.hasArg("autodigi");
  bool isenabled=server.hasArg("isenabled");
  bool wide1=server.hasArg("wide1"),wide2=server.hasArg("wide2");
  String rfpath=server.arg("rfpath");
  if(rfpath!="DIRECT" && rfpath!="WIDE1-1" && rfpath!="WIDE2-1" && rfpath!="WIDE2-2"){
    server.send(400,"text/plain; charset=utf-8","RF Beacon path gecersiz.");return;
  }
  int txpwr=server.arg("txpwr").toInt();
  if(txpwr!=10 && txpwr!=14 && txpwr!=17 && txpwr!=20 && txpwr!=21){
    server.send(400,"text/plain; charset=utf-8","TX gucu secimi gecersiz.");return;
  }
  int probemin=server.arg("probemin").toInt();
  if(probemin!=1 && probemin!=5 && probemin!=10 && probemin!=15){
    server.send(400,"text/plain; charset=utf-8","Baglanti kontrol araligi gecersiz.");return;
  }
  if(!validAprsText(comment,43) || !validAprsText(status,62) ||
     stmin<15 || stmin>240 || (stena && status.isEmpty())) {
    server.send(400,"text/plain; charset=utf-8","Comment/Status gecersiz. ASCII karakter kullanin; | ve ~ kullanmayin.");return;
  }
  // '/' primary, '\\' alternate or A-Z/0-9 overlay.
  bool validTable=(stab.length()==1 && (stab=="/" || stab=="\\" || (stab[0]>='A' && stab[0]<='Z') || (stab[0]>='0' && stab[0]<='9')));
  bool validSymbol=(scode.length()==1 && scode[0]>=33 && scode[0]<=126 && scode[0]!='<' && scode[0]!='>');
  if(!validTable || !validSymbol){server.send(400,"text/plain; charset=utf-8","APRS sembol kodu gecersiz.");return;}

  if(bmin<10 || bmin>120 || !isfinite(lat) || !isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180 || (bena && ((lat==0 && lon==0) || latText.isEmpty() || lonText.isEmpty()))){
    server.send(400,"text/plain; charset=utf-8","Konum veya beacon araligi gecersiz.");return;
  }
  if(!validCall(call) || ssid.length()>32 || host.isEmpty() || host.length()>80 || host.indexOf(' ')>=0 || port<1 || port>65535 || newWifi.length()>63 || newPass.length()>8){
    server.send(400,"text/plain; charset=utf-8","Gecersiz ayar! Cagri isareti / sunucu / port bilgilerini kontrol edin.");return;
  }
  prefs.putString("call",call); prefs.putString("ssid",ssid);
  prefs.putString("host",host); prefs.putUShort("port",port);
  prefs.putDouble("lat",lat);prefs.putDouble("lon",lon);
  prefs.putUShort("bmin",(uint16_t)bmin);prefs.putBool("bena",bena);
  prefs.putString("symtab",stab);prefs.putString("symcode",scode);
  prefs.putString("comment",comment);prefs.putString("status",status);
  prefs.putUChar("oledtype",(uint8_t)oledtype);
  prefs.putUShort("oledsleep",(uint16_t)oledsleep);
  prefs.putBool("batbeacon",batbeacon);
  prefs.putFloat("batcal",batcal);
  prefs.putBool("stena",stena);prefs.putUShort("stmin",(uint16_t)stmin);
  prefs.putBool("isenabled",isenabled);
  prefs.putBool("autodigi",autodigi);prefs.putBool("wide1",wide1);
  prefs.putBool("wide2",wide2);prefs.putString("rfpath",rfpath);prefs.putUChar("probemin",(uint8_t)probemin);
  prefs.putUChar("txpwr",(uint8_t)txpwr);
  if(newWifi.length()) prefs.putString("wifi",newWifi);
  if(newPass.length()) prefs.putString("pass",newPass);
  server.send(200,"text/plain; charset=utf-8","Ayarlar kaydedildi. Cihaz yeniden baslatiliyor...");
  restartAt=millis()+1200;
}

// /security: PIN ve admin parola islemleri ana ayarlar sayfasindan ayridir.
void handleSecurity(){
  if(!accessAllowed())return;
  String p=pageHeader("Guvenlik - Yonetici ve Kurulum AP Sifreleri");
  p+="<div class='card'><h2>Yonetici Sifresini Degistir</h2>";
  p+="<p class='muted'>Kullanici adi: <b>admin</b> (degistirilemez)</p>";
  p+="<form method='POST' action='/security/admin' onsubmit='return securityPost(event,this)'>";
  p+="<label>Mevcut yonetici sifresi</label><input type='password' name='oldpass' autocomplete='current-password' required>";
  p+="<label>Yeni sifre (en az 10 karakter)</label><input type='password' name='newpass' minlength='10' maxlength='64' autocomplete='new-password' required>";
  p+="<label>Yeni sifre tekrar</label><input type='password' name='newpass2' minlength='10' maxlength='64' autocomplete='new-password' required>";
  p+="<button type='submit'>Yonetici Sifresini Degistir</button>";
  p+="<small>Sifre NVS icinde tuzlanmis SHA-256 dogrulayicisi olarak saklanir. JSON yedegine eklenmez. HTTP Basic sifreyi agda sifrelemez; paneli internete acmayin.</small></form></div>";
  p+="<div class='card'><h2>Kurulum AP Sifresi</h2>";
  p+="<form method='POST' action='/security/ap' onsubmit='return securityPost(event,this)'>";
  p+="<label>Yeni AP sifresi (8-63 karakter)</label><input type='password' name='apkey' minlength='8' maxlength='63' autocomplete='new-password' required>";
  p+="<label>Yeni AP sifresi tekrar</label><input type='password' name='apkey2' minlength='8' maxlength='63' autocomplete='new-password' required>";
  p+="<button type='submit'>AP Sifresini Degistir</button>";
  p+="<small>AP: TA1FM-APRS-SETUP. Kayittan sonra cihaz yeniden baslar. AP sifresi ayar yedegine eklenmez.</small></form></div>";
  p+=R"JS(<script>
async function securityPost(ev,form){
 ev.preventDefault();if(!form.reportValidity())return false;
 if(!confirm('Guvenlik ayari degistirilecek. Devam edilsin mi?'))return false;
 try{const r=await fetch(form.action,{method:'POST',body:new FormData(form),cache:'no-store'});
 const m=await r.text();toast(m,r.ok?'success':'error',9000);
 if(r.ok){form.reset();if(form.action.endsWith('/admin'))toast('Sifre degisti. Tarayicida yeni sifreyle giris yapin.', 'success',0);}
 }catch(e){toast('Islem basarisiz: '+e.message,'error',9000)}return false;
}
</script>)JS";
  p+="</main></body></html>";
  server.sendHeader("Cache-Control","no-store");
  server.send(200,"text/html; charset=utf-8",p);
}
void handleAdminPasswordChange(){
  if(!accessAllowed())return;
  String oldp=server.arg("oldpass"),p1=server.arg("newpass"),p2=server.arg("newpass2");
  if(!checkAdminPassword(oldp)){server.send(403,"text/plain; charset=utf-8","Mevcut sifre yanlis.");return;}
  if(p1!=p2 || p1.length()<10 || p1.length()>64 || p1==oldp){
    server.send(400,"text/plain; charset=utf-8","Yeni sifreler ayni olmali, 10-64 karakter ve oncekinden farkli olmali.");return;
  }
  storeAdminPassword(p1);
  addEvent("Yonetici sifresi degistirildi");
  server.send(200,"text/plain; charset=utf-8","Yonetici sifresi degistirildi. Yeni sifre ile giris yapin.");
}
void handleApPasswordChange(){
  if(!accessAllowed())return;
  String a=server.arg("apkey"),b=server.arg("apkey2");
  if(a!=b || a.length()<8 || a.length()>63){
    server.send(400,"text/plain; charset=utf-8","AP sifresi 8-63 karakter ve iki alanda ayni olmali.");return;
  }
  prefs.putString("apkey",a);
  addEvent("Kurulum AP sifresi degistirildi");
  server.send(200,"text/plain; charset=utf-8","AP sifresi kaydedildi. Cihaz yeniden baslatiliyor.");
  restartAt=millis()+1800;
}
void handleRestart(){
  if(!accessAllowed())return;
  addEvent("Web panelinden manuel restart istendi");
  server.send(200,"text/plain; charset=utf-8","Cihaz yeniden baslatiliyor.");
  restartAt=millis()+1500;
}

bool beaconCoordinatesValid(){
  return cfg.beaconEnabled && !(cfg.latitude==0 && cfg.longitude==0) &&
         validCall(cfg.call) && validAprsText(cfg.commentText,43);
}
void handleManualIs(){
  if(!accessAllowed())return;
  if(!beaconCoordinatesValid()){
    server.send(409,"text/plain; charset=utf-8","Konum beacon kapali veya koordinat gecersiz.");return;
  }
  if(!cfg.aprsIsEnabled || !aprsVerified || !aprs.connected() || WiFi.status()!=WL_CONNECTED){
    server.send(503,"text/plain; charset=utf-8","APRS-IS baglantisi dogrulanmamis.");return;
  }
  if(manualIsPending){server.send(409,"text/plain; charset=utf-8","APRS-IS beacon istegi zaten bekliyor.");return;}
  manualIsPending=true;
  manualIsResult="SIRADA";
  server.send(202,"text/plain; charset=utf-8","APRS-IS beacon istegi siraya alindi.");
}
void handleManualRf(){
  if(!accessAllowed())return;
  if(!cfg.autoDigi || !radioOk){
    server.send(403,"text/plain; charset=utf-8","RF TX icin Auto Digipeater ACIK ve radyo hazir olmali.");return;
  }
  if(!beaconCoordinatesValid()){
    server.send(409,"text/plain; charset=utf-8","Konum beacon kapali veya koordinat gecersiz.");return;
  }
  if(manualRfPending){server.send(409,"text/plain; charset=utf-8","RF beacon istegi zaten bekliyor.");return;}
  if(lastManualRfRequest && (uint32_t)(millis()-lastManualRfRequest)<TX_GAP_MS){
    server.send(429,"text/plain; charset=utf-8","Yeni manuel RF istegi icin en az 30 sn bekleyin.");return;
  }
  lastManualRfRequest=millis();
  manualRfPending=true;
  manualRfResult="SIRADA";
  server.send(202,"text/plain; charset=utf-8","RF beacon istegi siraya alindi. TX sonucu durum ekraninda kontrol edilebilir.");
}

uint32_t fnv1a(const String& s){
  uint32_t h=2166136261u;
  for(size_t i=0;i<s.length();i++){h^=(uint8_t)s[i];h*=16777619u;}
  return h;
}
bool seenRecently(const String& packet){
  uint32_t h=fnv1a(packet), now=millis();
  for(auto &item:seen) if(item.hash==h && now-item.time<30000u) return true;
  seen[seenPtr]={h,now};seenPtr=(seenPtr+1)%24;
  return false;
}
void enqueue(const String& line){
  if(queueCount>=12){countDropped++;return;}
  line.toCharArray(queueData[queueTail].line,sizeof(queueData[queueTail].line));
  queueTail=(queueTail+1)%12;queueCount++;
}

// LoRa APRS: 3c ff 01 + TNC2 text: SRC>DEST[,PATH]:INFO
// Strictly reject invalid content for safe APRS-IS forwarding.
bool decodeAprs(const uint8_t* data,size_t len,String& source,String& packet){
  if(len<10 || data[0]!=0x3c || data[1]!=0xff || data[2]!=0x01) return false;
  packet="";
  for(size_t i=3;i<len;i++){
    uint8_t c=data[i];
    if(c=='\r' || c=='\n') break;
    if(c<32 || c>126) return false;
    packet+=(char)c;
  }
  int sep=packet.indexOf('>'), colon=packet.indexOf(':');
  if(sep<3 || colon<=sep+1 || colon>=packet.length()-1) return false;
  source=packet.substring(0,sep);source.toUpperCase();
  if(!validCall(source))return false;
  String header=packet.substring(sep+1,colon);
  if(header.indexOf(' ')>=0 || header.length()>100) return false;
  // Do not gate content already marked as internet-originated, or q constructs.
  String uc=header; uc.toUpperCase();
  if(uc.indexOf("TCPIP")>=0 || uc.indexOf("TCPXX")>=0 || uc.indexOf(",Q")>=0 || uc.startsWith("Q")) return false;
  return true;
}

void checkDigiEligibility(const String& packet);
void manageDigiTx();
void updateHybridMode();

void readRadio(){
  if(!rxFlag || !radioOk) return;
  rxFlag=false;
  size_t n=radio.getPacketLength();
  uint8_t data[MAX_FRAME+1]={0};
  int state=(n>0 && n<=MAX_FRAME)? radio.readData(data,n) : -1;
  if(state==RADIOLIB_ERR_NONE){
    countRx++;lastRxAt=millis();
    float rssi=radio.getRSSI(),snr=radio.getSNR();lastRssi=rssi;lastSnr=snr;
    String source,packet;
    Serial.printf("[RX] %u byte RSSI %.1f SNR %.1f\n",(unsigned)n,rssi,snr);
    if(decodeAprs(data,n,source,packet)){
      countValid++;lastStation=source;
      onValidRxDisplay();
      recordRx(source,packet,rssi,snr,"RX");
      Serial.println("[APRS] "+packet);
      if(seenRecently(packet)){countDup++;updateRecentAction(packet,"TEKRAR");Serial.println("[APRS] Duplicate (30 sn)");}
      else if(digiMode && cfg.autoDigi){
        checkDigiEligibility(packet);
      } else if(WiFi.status()==WL_CONNECTED && aprsVerified){
        int colon=packet.indexOf(':');
        // RX-only gate uses qAO, do not falsely advertise RF transmit capability.
        String out=packet.substring(0,colon)+",qAO,"+cfg.call+packet.substring(colon);
        if(out.length()<sizeof(queueData[0].line)-2){enqueue(out);updateRecentAction(packet,"IS SIRADA");}
        else countDropped++;
      } else countDropped++;
    } else {
      countRejected++;
      Serial.println("[APRS] Gecersiz veya desteklenmeyen paket; HEX:");
      for(size_t i=0;i<n;i++){Serial.printf("%02X ",data[i]);if(i%24==23)Serial.println();}
      Serial.println();
    }
  } else Serial.printf("[RX] Paket hatasi: %d\n",state);
  int rc=radio.startReceive();
  if(rc!=RADIOLIB_ERR_NONE) Serial.printf("[RX] Tekrar baslatma hatasi: %d\n",rc);
}


// APRS path manipulation: only first still-unused WIDE alias, no third-party/Internet paths.
// WIDE1-1 -> OURCALL*, WIDE2-2 -> OURCALL*,WIDE2-1, WIDE2-1 -> OURCALL*.
// Never relay own source or a packet that has already traversed our station.
void checkDigiEligibility(const String& packet){
  if(!cfg.autoDigi || !digiMode || !digiPending.isEmpty()) return;
  int gt=packet.indexOf('>'), colon=packet.indexOf(':');
  if(gt<3 || colon<gt+2 || colon+1>=packet.length())return;
  String source=packet.substring(0,gt);source.toUpperCase();
  if(source==cfg.call || packet[colon+1]=='}')return;
  String addr=packet.substring(gt+1,colon);
  String u=addr;u.toUpperCase();
  if(u.indexOf("TCPIP")>=0 || u.indexOf("TCPXX")>=0 || u.indexOf(",Q")>=0)return;
  int firstComma=addr.indexOf(',');
  if(firstComma<0)return;
  String dest=addr.substring(0,firstComma);
  if(dest.isEmpty())return;
  String prefix=source+">"+dest;
  String suffix="";
  bool matched=false;
  bool foundUnused=false;
  int i=firstComma+1;
  while(i<=addr.length()){
    int j=addr.indexOf(',',i);
    if(j<0)j=addr.length();
    String token=addr.substring(i,j);
    String upper=token;upper.toUpperCase();
    if(token.isEmpty() || token.length()>13)return;
    if(upper==cfg.call || upper==cfg.call+"*")return;
    if(!matched && upper.endsWith("*")){
      // Already repeated by another station: preserve used hops.
      suffix+=","+token;
    } else if(!matched && !foundUnused){
      foundUnused=true;
      if(cfg.wide1 && upper=="WIDE1-1"){
        suffix+=","+cfg.call+"*";
        matched=true;
      } else if(cfg.wide2 && (upper=="WIDE2-1" || upper=="WIDE2-2")){
        suffix+=","+cfg.call+"*";
        if(upper=="WIDE2-2")suffix+=",WIDE2-1";
        matched=true;
      } else {
        // Only the first UNUSED hop may be digipeated; no skipping unsupported paths.
        return;
      }
    } else suffix+=","+token;
    if(j==addr.length())break;
    i=j+1;
  }
  if(!matched)return;
  String output=prefix+suffix+packet.substring(colon);
  // 3-byte LoRa APRS header + max 252 bytes TNC2 text.
  if(output.length()>252)return;
  updateRecentAction(packet,"DIGI SIRADA");
  digiPending=output;
  digiPendingAt=millis();
  Serial.println("[DIGI] Aday: "+digiPending);
}

// TX DIAGNOSTIC: apply the same power/OCP order used by Ricardo's working SX1262 setup.
// This does not measure radiated RF power; compare with the wattmeter.
bool prepareTxPower(const char* operation){
  int16_t p=radio.setOutputPower((int8_t)cfg.txPowerDbm);
  int16_t c=radio.setCurrentLimit(140.0f);
  Serial.printf("[TX KONTROL] %s: istenen=%u dBm, POWER=%d, OCP=%d, limit=%.1f mA\n",
                operation,(unsigned)cfg.txPowerDbm,p,c,radio.getCurrentLimit());
  return p==RADIOLIB_ERR_NONE && c==RADIOLIB_ERR_NONE;
}

void manageDigiTx(){
  if(!radioOk || !cfg.autoDigi || !digiMode || digiPending.isEmpty())return;
  uint32_t now=millis();
  if((uint32_t)(now-digiPendingAt)>15000u){digiPending="";return;}
  if(txWindowStart==0 || (uint32_t)(now-txWindowStart)>=3600000u){txWindowStart=now;txWindowCount=0;}
  if(txWindowCount>=TX_MAX_PER_HOUR){countDigiLimit++;digiPending="";return;}
  if(lastDigiTx && (uint32_t)(now-lastDigiTx)<TX_GAP_MS)return;
  // Wait a short, bounded window after hearing packet; re-check RF before TX.
  if((uint32_t)(now-digiPendingAt)<1200u)return;
  if(!prepareTxPower("DIGI")){
    countDigiFailed++;digiPending="";
    radio.startReceive();
    return;
  }
  noInterrupts(); rxFlag=false; interrupts();
  int16_t ch=radio.scanChannel(); // LoRa CAD: check for a detected preamble.
  if(ch!=RADIOLIB_CHANNEL_FREE){
    countDigiBusy++;
    if(ch!=RADIOLIB_LORA_DETECTED)Serial.printf("[DIGI] CAD hata: %d\n",ch);
    // The channel may be occupied. Drop this packet rather than congest RF.
    digiPending="";
    radio.startReceive();
    return;
  }
  uint8_t data[255];
  data[0]=0x3C;data[1]=0xFF;data[2]=0x01;
  memcpy(data+3,digiPending.c_str(),digiPending.length());
  size_t n=digiPending.length()+3;
  // Blocking RF TX: RX is unavailable for the packet airtime.
  int16_t result=radio.transmit(data,n);
  lastDigiTx=millis();txWindowCount++;
  if(result==RADIOLIB_ERR_NONE){countDigiTx++;Serial.println("[DIGI] RF TX OK: "+digiPending);addEvent("DIGI TX OK: "+digiPending.substring(0,70));
    int gt=digiPending.indexOf('>'), colon=digiPending.indexOf(':');
    String src=digiPending.substring(0,gt), info=colon>=0?digiPending.substring(colon):"";
    for(int i=0;i<rxCount;i++){int idx=(rxHead+15-1-i)%15;if(rxRecords[idx].station==src && rxRecords[idx].packet.endsWith(info)){rxRecords[idx].action="DIGI OK";break;}}
  }
  else {countDigiFailed++;addEvent("DIGI TX HATA: "+String(result));Serial.printf("[DIGI] TX HATA: %d\n",result);}
  digiPending="";
  noInterrupts();rxFlag=false;interrupts();
  int16_t r=radio.startReceive();
  if(r!=RADIOLIB_ERR_NONE)Serial.printf("[DIGI] RX yeniden baslatma hata: %d\n",r);
}

void updateHybridMode(){
  uint32_t now=millis();
  bool healthy=(cfg.aprsIsEnabled && WiFi.status()==WL_CONNECTED && aprs.connected() && aprsVerified);
  if(healthy){
    aprsDownFrom=0;
    if(digiMode){
      digiMode=false;digiPending="";rfBeaconDue=false;
      Serial.println("[HIBRIT] APRS-IS dogrulandi -> iGate moduna donuldu");
    }
    return;
  }
  if(aprsDownFrom==0)aprsDownFrom=now?now:1;
  if(!cfg.autoDigi){digiMode=false;digiPending="";rfBeaconDue=false;return;}
  if(!digiMode && (!cfg.aprsIsEnabled || (uint32_t)(now-aprsDownFrom)>=DIGI_AFTER_MS)){
    digiMode=true;digiPending="";
    rfBeaconDue=true;lastRfBeaconAttempt=0;
    lastDigiProbe=now;
    // Drop stale queued APRS-IS frames at transition.
    queueCount=queueHead=queueTail=0;
    addEvent(cfg.aprsIsEnabled?"APRS-IS 60 sn yok -> DIGI aktif":"APRS-IS manuel kapali -> DIGI aktif");
  }
}

void disconnectAprs(){if(manualIsPending)manualIsResult="HATA";manualIsPending=false;aprs.stop();aprsVerified=false;loginSent=false;aprsLine="";aprsConnectStarted=0;}

// Convert signed decimal degree to the uncompressed APRS ddmm.mmN / dddmm.mmE notation.
String coordAprs(double value,bool latitude){
  char hemi=(value>=0 ? (latitude?'N':'E') : (latitude?'S':'W'));
  double a=fabs(value);
  int deg=(int)a;
  long minuteHundredths=lround((a-deg)*6000.0);
  if(minuteHundredths>=6000){deg++;minuteHundredths=0;}
  char buf[16];
  if(latitude) snprintf(buf,sizeof(buf),"%02d%02ld.%02ld%c",deg,minuteHundredths/100,minuteHundredths%100,hemi);
  else snprintf(buf,sizeof(buf),"%03d%02ld.%02ld%c",deg,minuteHundredths/100,minuteHundredths%100,hemi);
  return String(buf);
}

// Locally-originated beacon: TCPIP* only, never RF digipeater path/q construct.
void sendOwnBeacon(){
  if(!cfg.aprsIsEnabled || !cfg.beaconEnabled || !aprsVerified || !aprs.connected())return;
  if((cfg.latitude==0 && cfg.longitude==0) || !validCall(cfg.call))return;
  uint32_t now=millis();
  if(!manualIsPending && !beaconDueOnConnect && (uint32_t)(now-lastBeacon)<(uint32_t)cfg.beaconMinutes*60000UL)return;
  String line=cfg.call+">"+OWN_TOCALL+",TCPIP*:!"+coordAprs(cfg.latitude,true)+cfg.symbolTable+coordAprs(cfg.longitude,false)+cfg.symbolCode+beaconComment();
  size_t n=aprs.print(line+"\r\n");
  if(n==line.length()+2){
    countBeacon++;lastBeacon=now;beaconDueOnConnect=false;manualIsPending=false;manualIsResult="OK";
    Serial.println("[BEACON] APRS-IS TCP gonderildi: "+line);
  }else {manualIsResult="HATA";disconnectAprs();}
}

// RF position beacon only in active backup Digi mode. Never send TCPIP*/qAO on RF.
// The frame uses the same LoRa-APRS 3-byte header as received/digipeated frames.
void manageRfBeacon(){
  if(!radioOk || !cfg.autoDigi || (!digiMode && !manualRfPending && !bootRfPending) || !cfg.beaconEnabled) return;
  if((cfg.latitude==0 && cfg.longitude==0) || !validCall(cfg.call)) return;
  if(!validAprsText(cfg.commentText,43)) return;
  if(bootRfPending && rapidBootCount>=3)bootRfPending=false;
  // Prioritize a pending tracker relay over our own beacon.
  if(!digiPending.isEmpty()) return;
  uint32_t now=millis();
  uint32_t interval=(uint32_t)cfg.beaconMinutes*60000UL;
  if(bootRfPending && now<BOOT_RF_WAIT_MS) return;
  if(!manualRfPending && !rfBeaconDue && !bootRfPending && (uint32_t)(now-lastRfBeacon)<interval) return;
  // Retry slowly after busy channel or failed TX; avoid tight repeated CAD attempts.
  if(lastRfBeaconAttempt && (uint32_t)(now-lastRfBeaconAttempt)<30000UL) return;
  if(lastDigiTx && (uint32_t)(now-lastDigiTx)<TX_GAP_MS) return;
  if(txWindowStart==0 || (uint32_t)(now-txWindowStart)>=3600000UL){
    txWindowStart=now;txWindowCount=0;
  }
  if(txWindowCount>=TX_MAX_PER_HOUR){
    if(!lastRfBeaconAttempt || (uint32_t)(now-lastRfBeaconAttempt)>=30000UL){
      countDigiLimit++;lastRfBeaconAttempt=now;
    }
    return;
  }
  String line=cfg.call+">"+OWN_TOCALL+(cfg.rfBeaconPath=="DIRECT" ? "" : ","+cfg.rfBeaconPath)+":!"+coordAprs(cfg.latitude,true)+cfg.symbolTable+
              coordAprs(cfg.longitude,false)+cfg.symbolCode+beaconComment();
  if(line.length()>252){countRfBeaconFail++;lastRfBeaconAttempt=now;manualRfPending=false;return;}
  lastRfBeaconAttempt=now;
  if(!prepareTxPower("RF BEACON")){
    countRfBeaconFail++;manualRfPending=false;manualRfResult="HATA";
    radio.startReceive();
    return;
  }
  noInterrupts();rxFlag=false;interrupts();
  int16_t cad=radio.scanChannel();
  if(cad!=RADIOLIB_CHANNEL_FREE){
    countDigiBusy++;
    Serial.printf("[RF BEACON] Kanal mesgul / CAD: %d\n",cad);
    radio.startReceive();
    return;
  }
  uint8_t frame[255];
  frame[0]=0x3C;frame[1]=0xFF;frame[2]=0x01;
  memcpy(frame+3,line.c_str(),line.length());
  int16_t rc=radio.transmit(frame,line.length()+3);
  lastDigiTx=millis();
  txWindowCount++;
  if(rc==RADIOLIB_ERR_NONE){
    countRfBeacon++;lastRfBeacon=lastDigiTx;rfBeaconDue=false;manualRfPending=false;bootRfPending=false;manualRfResult="OK";
    Serial.println("[RF BEACON] TX OK: "+line);
  }else{
    countRfBeaconFail++;manualRfPending=false;manualRfResult="HATA";
    Serial.printf("[RF BEACON] TX HATA: %d\n",rc);
  }
  noInterrupts();rxFlag=false;interrupts();
  int16_t rx=radio.startReceive();
  if(rx!=RADIOLIB_ERR_NONE) Serial.printf("[RF BEACON] RX yeniden baslatma hata: %d\n",rx);
}

// A status report is a separate APRS-IS packet, not part of the position beacon.
void sendOwnStatus(){
  if(!cfg.statusEnabled || cfg.statusText.isEmpty() || !aprsVerified || !aprs.connected()) return;
  if(!validAprsText(cfg.statusText,62) || !validCall(cfg.call)) return;
  uint32_t now=millis();
  if(!statusDueOnConnect && (uint32_t)(now-lastStatus)<(uint32_t)cfg.statusMinutes*60000UL) return;
  String line=cfg.call+">"+OWN_TOCALL+",TCPIP*:>"+cfg.statusText;
  size_t n=aprs.print(line+"\r\n");
  if(n==line.length()+2){
    countStatus++;lastStatus=now;statusDueOnConnect=false;
    Serial.println("[STATUS] APRS-IS TCP gonderildi: "+line);
  } else disconnectAprs();
}

void parseAprsResponse(const String& line){
  Serial.println("[APRS-IS] "+line);
  String low=line;low.toLowerCase();
  if(low.startsWith("# logresp ")){
    if(low.indexOf("unverified")>=0){Serial.println("[APRS-IS] Passcode kabul edilmedi");disconnectAprs();return;}
    if(low.indexOf("verified")>=0){aprsVerified=true;beaconDueOnConnect=true;statusDueOnConnect=true;Serial.println("[APRS-IS] GIRIS DOGRULANDI");}
  }
}
void maintainAprs(){
  if(!cfg.aprsIsEnabled || WiFi.status()!=WL_CONNECTED || cfg.passcode.isEmpty() || !validCall(cfg.call)){
    if(aprs.connected()) disconnectAprs();
    return;
  }
  if(!aprs.connected()){
    aprsVerified=false;loginSent=false;
    if(digiMode){
      if((uint32_t)(millis()-lastDigiProbe)<(uint32_t)cfg.probeMinutes*60000UL)return;
      lastDigiProbe=millis();
    } else if((uint32_t)(millis()-lastAprsAttempt)<15000u) return;
    lastAprsAttempt=millis();
    Serial.printf("[APRS-IS] %s:%d baglaniyor...\n",cfg.host.c_str(),cfg.port);
    // TCP connection can block briefly; 1200 ms budget to limit RX interference.
    if(!aprs.connect(cfg.host.c_str(),cfg.port,1200)){
      Serial.println("[APRS-IS] Baglanti basarisiz");return;
    }
    aprs.setNoDelay(true);
    aprsConnectStarted=millis();
    aprsLine="";
    String login="user "+cfg.call+" pass "+cfg.passcode+" vers TA1FM-LoRa-iGate 1.5.8\r\n";
    aprs.print(login);loginSent=true;
    Serial.println("[APRS-IS] Giris istendi");
  }
  // Read only a bounded amount per loop; never block waiting for a line.
  int budget=256;
  while(aprs.available() && budget-- >0){
    char c=aprs.read();
    if(c=='\n'){
      aprsLine.trim();
      if(aprsLine.startsWith("#")) parseAprsResponse(aprsLine);
      aprsLine="";
    } else if(c!='\r') {
      if(aprsLine.length()<255) aprsLine+=c;
      else aprsLine="";
    }
  }
  if(!aprsVerified){
    if(aprsConnectStarted && millis()-aprsConnectStarted>10000){
      Serial.println("[APRS-IS] Login dogrulama zaman asimi");disconnectAprs();
    }
    return;
  }
  sendOwnBeacon();
  if(aprsVerified && aprs.connected()) sendOwnStatus();
  // Send at most one queued frame per loop; TCP write success is not server acceptance.
  if(!digiMode && queueCount && aprs.connected()){
    String sendLine=queueData[queueHead].line;
    size_t wrote=aprs.print(sendLine+"\r\n");
    if(wrote==sendLine.length()+2){
      queueHead=(queueHead+1)%12;queueCount--;countSent++;
      for(int i=0;i<rxCount;i++){int idx=(rxHead+15-1-i)%15;
        if(sendLine.startsWith(rxRecords[idx].station+">") && sendLine.endsWith(rxRecords[idx].packet.substring(rxRecords[idx].packet.indexOf(':')))){
          rxRecords[idx].action="IS OK";break;
        }
      }
      Serial.println("[APRS-IS] TCP gonderildi: "+sendLine);
    } else {disconnectAprs();}
  }
}


void handleActionResult(){
  if(!accessAllowed())return;
  server.sendHeader("Cache-Control","no-store");
  server.send(200,"application/json","{\"rf\":"+jsonEscape(manualRfResult)+",\"is\":"+jsonEscape(manualIsResult)+"}");
}
void handleIsToggle(){
  if(!accessAllowed())return;
  cfg.aprsIsEnabled=!cfg.aprsIsEnabled;
  prefs.putBool("isenabled",cfg.aprsIsEnabled);
  if(!cfg.aprsIsEnabled){
    disconnectAprs();
    aprsDownFrom=millis();
    updateHybridMode(); // Immediate Digi when enabled, independent of Wi-Fi.
    addEvent("APRS-IS manuel KAPATILDI - Wi-Fi devam");
  }else{
    aprsDownFrom=millis();lastAprsAttempt=0;lastDigiProbe=0;
    // Stop digipeating while reconnecting; prevent RF/IS double mode.
    digiMode=false;digiPending="";rfBeaconDue=false;
    addEvent("APRS-IS manuel ACILDI - baglanti deneniyor");
  }
  server.send(200,"text/plain; charset=utf-8",cfg.aprsIsEnabled?"APRS-IS ACILIYOR":"APRS-IS MANUEL KAPALI");
}

// Read-only JSON endpoint. The page polls it every 15 seconds without reloading.
void handleRxJson(){
  if(!accessAllowed())return;
  String p="{\"count\":"+String(countRx)+",\"rows\":[";
  for(uint8_t i=0;i<rxCount;i++){
    int idx=(rxHead+15-1-i)%15;
    const RxRecord &r=rxRecords[idx];
    if(i)p+=",";
    p+="{\"time\":"+jsonEscape(rxClock(r.receivedAt))+
       ",\"call\":"+jsonEscape(r.station)+",\"rssi\":"+String(r.rssi,1)+
       ",\"snr\":"+String(r.snr,1)+",\"action\":"+jsonEscape(r.action)+
       ",\"packet\":"+jsonEscape(r.packet)+"}";
  }
  p+="]}";
  server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",p);
}
void handleRxPage(){
  if(!accessAllowed())return;
  String p=pageHeader("Son 15 RX Beacon - 15 saniyede bir guncellenir");
  p+="<div class='card'>";
  p+="<p id='summary'>Yukleniyor...</p><div style='overflow-x:auto'><table><thead><tr><th>Saat</th><th>Cagri</th><th>RSSI</th><th>SNR</th><th>Islem</th></tr></thead><tbody id='rows'></tbody></table></div>";
  p+="<p class='subtle'>Satira tiklayinca tam APRS paketi gorulur. Sayfa sabit kalir, veri 15 sn'de bir yenilenir.</p>";
  p+="<pre id='fullPacket' style='white-space:pre-wrap;word-break:break-all'></pre></div>";
  p+=R"JS(<script>
async function refreshRx(){try{
 const r=await fetch('/api/rx?'+Date.now(),{cache:'no-store'});if(!r.ok)throw Error(r.status);
 const d=await r.json(),b=document.getElementById('rows');b.replaceChildren();
 for(const item of d.rows){const tr=document.createElement('tr');for(const k of ['time','call','rssi','snr','action']){const td=document.createElement('td');td.textContent=item[k];tr.appendChild(td)}tr.style.cursor='pointer';tr.onclick=()=>document.getElementById('fullPacket').textContent=item.packet;b.appendChild(tr)}
 document.getElementById('summary').textContent='Toplam RX: '+d.count+' | Listelenen: '+d.rows.length+' | Guncelleme: '+new Date().toLocaleTimeString('tr-TR');
}catch(e){document.getElementById('summary').textContent='Baglanti hatasi: '+e.message}}
refreshRx();setInterval(refreshRx,15000);
</script>)JS";
  p+="</main></body></html>";server.send(200,"text/html; charset=utf-8",p);
}
void handleEvents(){
  if(!accessAllowed())return;
  if(server.hasArg("download")){
    String body="TA1FM V1.5.8 Olay Kayitlari (son 24 / RAM)\n";
    for(uint8_t i=0;i<eventCount;i++){int idx=(eventHead+24-eventCount+i)%24;body+=String(events[idx].at/1000)+"s | "+events[idx].message+"\n";}
    server.sendHeader("Content-Disposition","attachment; filename=TA1FM_olaylar.txt");server.send(200,"text/plain; charset=utf-8",body);return;
  }
  String p=pageHeader("Son olaylar - RAM kayitlari");
  p+="<div class='card'><p><a href='/events?download=1'>Olaylari Indir</a></p>";
  for(uint8_t i=0;i<eventCount;i++){int idx=(eventHead+24-1-i)%24;p+="<p><b>"+String(events[idx].at/1000)+"s</b> "+htmlEscape(events[idx].message)+"</p>";}
  p+="<small>Yeniden baslatilinca kayitlar silinir.</small></div></main></body></html>";
  server.send(200,"text/html; charset=utf-8",p);
}
void handleOtaPage(){
  if(!accessAllowed())return;
  String p=pageHeader("OTA Firmware Guncelleme - 4 MB Default / OTA bolumu gerekir");
  p+="<div class='card'>";
  p+="<p><b>UYARI:</b> Yalniz bu ESP32-S3 cihazina ait derlenmis .bin dosyasini yukleyin. ";
  p+="Mevcut partition OTA desteklemiyorsa islem baslatilamaz. USB ile kurtarma erisiminizi koruyun.</p>";
  p+="<form id='otaForm' onsubmit='return doOta(event)'><input type='file' id='bin' name='firmware' accept='.bin,application/octet-stream' required>";
  p+="<button type='submit'>BIN Dosyasini Yukle</button></form><p id='otaMsg'></p></div>";
  p+=R"JS(<script>
async function doOta(e){e.preventDefault();const f=document.getElementById('otaForm'),file=document.getElementById('bin').files[0];if(!file||!file.name.toLowerCase().endsWith('.bin')){toast('Yalniz .bin kabul edilir','error');return false;}
 if(!confirm('OTA baslasin mi? Islem sirasinda cihazin enerjisini kesmeyin.'))return false;
 let boot=0;try{boot=(await (await fetch('/health')).json()).boot}catch(e){}
 const b=document.querySelector('#otaForm button');b.disabled=true;toast('Yukleniyor, cihazin enerjisini kesmeyin...', 'success',0);
 try{const r=await fetch('/ota',{method:'POST',body:new FormData(f)});const msg=await r.text();
 if(!r.ok){toast(msg,'error',0);b.disabled=false;return false;}toast(msg,'success',0);checkReboot(boot);
 }catch(err){toast('OTA baglanti hatasi: '+err,'error',0);b.disabled=false;}return false;}
</script>)JS";
  p+="</main></body></html>";server.send(200,"text/html; charset=utf-8",p);
}
void handleOtaUpload(){
  if(!validAdminAuthHeader())return;
  HTTPUpload &u=server.upload();
  if(u.status==UPLOAD_FILE_START){
    otaFailed=false;otaStarted=false;otaSuccessful=false;otaError="";
    if(!u.filename.endsWith(".bin")){otaFailed=true;otaError="Dosya .bin olmali";return;}
    if(!Update.begin(UPDATE_SIZE_UNKNOWN,U_FLASH)){
      otaFailed=true;otaError="OTA bolumu yetersiz veya yok (partition kontrolu). Hata "+String(Update.getError());return;
    }
    otaStarted=true;
    addEvent("OTA yukleme basladi: "+u.filename);
  }else if(u.status==UPLOAD_FILE_WRITE){
    if(otaStarted&&!otaFailed){if(Update.write(u.buf,u.currentSize)!=u.currentSize){otaFailed=true;otaError="Flash yazma hatasi "+String(Update.getError());Update.abort();}}
  }else if(u.status==UPLOAD_FILE_END){
    if(otaStarted&&!otaFailed){if(!Update.end(true)){otaFailed=true;otaError="OTA bitirme hatasi "+String(Update.getError());}else otaSuccessful=true;}
    otaStarted=false;
  }else if(u.status==UPLOAD_FILE_ABORTED){
    if(otaStarted)Update.abort();otaStarted=false;otaFailed=true;otaError="OTA iptal edildi";
  }
}
void handleOtaFinish(){
  if(!accessAllowed())return;
  if(otaFailed || !otaSuccessful){
    server.send(400,"text/plain; charset=utf-8","OTA basarisiz: "+otaError+". Calisan yazilim korunuyor.");
    addEvent("OTA basarisiz: "+otaError);return;
  }
  addEvent("OTA tamamlandi; yeniden baslatiliyor");
  server.send(200,"text/plain; charset=utf-8","OTA tamamlandi. Cihaz yeniden baslatiliyor...");
  restartAt=millis()+1600;
}

void setup(){
  Serial.begin(115200);delay(400);
  Serial.println("\n=== TA1FM LoRa APRS iGate &Digi V1.5.8 ===");
  loadConfig();
  bootToken=esp_random();
  // RTC survives ESP.restart but not a real power-off. Limit startup TX in reboot loops.
  if(esp_reset_reason()==ESP_RST_POWERON)rapidBootCount=0;
  if(rapidBootCount<255)rapidBootCount++;
  if(rapidBootCount>=3){bootRfPending=false;Serial.println("[BOOT] Tekrarlayan restart: baslangic RF beacon engellendi");}
  addEvent("Basladi, reset nedeni="+String((int)esp_reset_reason()));
  pinMode(PIN_VEXT,OUTPUT); digitalWrite(PIN_VEXT,LOW);delay(100);
  // ADC'de sadece pil gerilimini okurken olcum bolucusunu etkinlestir.
  pinMode(ADC_CTRL_PIN,OUTPUT);
  digitalWrite(ADC_CTRL_PIN,!ADC_CTRL_ON_STATE);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_PIN,ADC_11db);
  sampleBattery();
  beginDisplay();
  SPI.begin(PIN_SCK,PIN_MISO,PIN_MOSI,PIN_CS);
  int rc=radio.begin(LORA_FREQ,LORA_BW,LORA_SF,LORA_CR,LORA_SYNC,10,8,1.8);
  if(rc==RADIOLIB_ERR_NONE){
    // Heltec SX1262: use Ricardo's verified 140 mA OCP setting.
    // This protects against premature PA limiting at elevated TX power;
    // it does NOT guarantee over-the-air packet reception by another station.
    // Ricardo: output power first, then OCP; keep DIO2 antenna control enabled.
    int16_t power=radio.setOutputPower((int8_t)cfg.txPowerDbm);
    int16_t ocp=radio.setCurrentLimit(140.0f);
    int16_t sw=radio.setDio2AsRfSwitch();
    int16_t crc=radio.setCRC(2);
    Serial.printf("[LoRa] OCP: %d (%.1f mA), RF switch: %d, CRC: %d\n",
                  ocp, radio.getCurrentLimit(),sw,crc);
    Serial.printf("[LoRa] TX gucu ayari: %u dBm (sonuc %d)\n",cfg.txPowerDbm,power);
    if(ocp!=RADIOLIB_ERR_NONE || sw!=RADIOLIB_ERR_NONE || crc!=RADIOLIB_ERR_NONE || power!=RADIOLIB_ERR_NONE){
      Serial.printf("[LoRa] RF setup error OCP/SW/CRC/PWR: %d/%d/%d/%d; TX kapali\n",ocp,sw,crc,power);
      cfg.autoDigi=false; // Runtime safety; saved preference unchanged.
    }
    radio.setDio1Action(onRadioRx);
    rc=radio.startReceive();
    radioOk=(rc==RADIOLIB_ERR_NONE);
  }
  Serial.printf("[LoRa] %s (kod %d)\n",radioOk?"SX1262 HAZIR":"HATA",rc);
  beginWifi();
  // Asynchronous SNTP; normal RF receive does not wait for a time server.
  configTime(3*3600,0,"pool.ntp.org","time.google.com");
  aprsDownFrom=millis();
  server.on("/",HTTP_GET,handleHome);
  server.on("/settings",HTTP_GET,handleSettings);
  server.on("/save",HTTP_POST,handleSave);
  server.on("/security",HTTP_GET,handleSecurity);
  server.on("/security/admin",HTTP_POST,handleAdminPasswordChange);
  server.on("/security/ap",HTTP_POST,handleApPasswordChange);
  server.on("/restart",HTTP_POST,handleRestart);
  server.on("/beacon/is",HTTP_POST,handleManualIs);
  server.on("/beacon/rf",HTTP_POST,handleManualRf);
  server.on("/health",HTTP_GET,[](){if(!accessAllowed())return;server.sendHeader("Cache-Control","no-store");server.send(200,"application/json","{\"boot\":"+String(bootToken)+",\"uptime\":"+String(millis()/1000)+"}");});
  server.on("/api/is-state",HTTP_GET,[](){if(!accessAllowed())return;server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",String("{\"enabled\":")+(cfg.aprsIsEnabled?"true":"false")+",\"verified\":"+(aprsVerified?"true":"false")+"}");});
  server.on("/api/toggle-is",HTTP_POST,handleIsToggle);
  server.on("/api/action-result",HTTP_GET,handleActionResult);
  server.on("/api/rx",HTTP_GET,handleRxJson);
  server.on("/rx",HTTP_GET,handleRxPage);
  server.on("/events",HTTP_GET,handleEvents);
  server.on("/ota",HTTP_GET,handleOtaPage);
  server.on("/ota",HTTP_POST,handleOtaFinish,handleOtaUpload);
  const char* collectedHeaders[]={"Authorization"};
  server.collectHeaders(collectedHeaders,1);
  server.onNotFound([](){server.send(404,"text/plain","Sayfa bulunamadi");});
  server.begin();
  screenUpdate();
}

void loop(){
  // Priority to radio packets.
  readRadio();
  maintainBattery();
  maintainWifi();
  server.handleClient();
  if(restartAt && (int32_t)(millis()-restartAt)>=0)ESP.restart();
  if(millis()>120000UL && rapidBootCount!=0)rapidBootCount=0;
  readRadio();
  maintainAprs();
  updateHybridMode();
  readRadio();
  manageDigiTx();
  manageRfBeacon();
  maintainDisplaySleep();
  if(millis()-lastScreen>=1000){lastScreen=millis();screenUpdate();}
  delay(1);
}
