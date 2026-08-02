/*
 * Öntözésvezérlés ESP8266 + Telegram + Blynk
 * 
 * Hardware:
 *   - Wemos D1 Mini (ESP8266)
 *   - 4 csatorna relé board (aktív alacsony = LOW be, HIGH ki)
 *   - SSD1306 OLED (SDA=GPIO12/D6, SCL=GPIO14/D5)
 *   - NTP időszinkron (nincs RTC)
 * 
 * Funkciók:
 *   - 4 zóna relé vezérlés
 *   - Napi időzítés (LittleFS-ben tárolva, reboot-t túlél)
 *   - 2 Telegram bot (Admin + Admin2)
 *   - Blynk dashboard
 *   - Hardware Watchdog
 *   - Auto firmware check (GitHub publikus repó)
 *   - OTA update Telegram-ból
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>

// Blynk template define-ok az include ELŐTT kell
#define BLYNK_TEMPLATE_ID   "TMPL2D8e2u0UL"
#define BLYNK_TEMPLATE_NAME "Ontozes"
#include <BlynkSimpleEsp8266.h>

#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>

// ==================== KONFIGURÁCIÓ ====================

// WiFi
#define WIFI_SSID         "Unifi4"
#define WIFI_PASS         "jkmjkmpp"

// Telegram Bot #1 (Admin)
#define BOT_TOKEN_ADMIN   "8764475265:AAGuen7VFUZ2AEpI5qBhVYhcem4go7ZX7HU"
#define ADMIN_CHAT_ID     "5079519547"

// Telegram Bot #2 (Admin 2 — Ilona)
#define BOT_TOKEN_FAMILY  "8824963625:AAGecULECaM03134n7ejlOAs8xB5ytoR4mk"
#define FAMILY_CHAT_ID    "8853524984"

// Blynk — template define-ok az include-nál (fent)
#define BLYNK_AUTH_TOKEN    "yZ2fyi267XuP3ZHqlHqW76xvmt9bCc5O"

// Relé board (aktív magas — HIGH = be, LOW = ki)
// OLED: SDA=GPIO12/D6, SCL=GPIO14/D5 — relék D1-D4!
#define RELAY1_PIN    D1   // GPIO5 — Zone 1
#define RELAY2_PIN    D2   // GPIO4 — Zone 2
#define RELAY3_PIN    D3   // GPIO0 — Zone 3
#define RELAY4_PIN    D4   // GPIO2 — Zone 4
#define RELAY_ON      HIGH
#define RELAY_OFF     LOW

// OLED (Ideaspark v2.1: SDA=GPIO12/D6, SCL=GPIO14/D5, addr 0x3C)
#define OLED_ADDR     0x3C
#define OLED_W        128
#define OLED_H        64
#define OLED_RST      -1

// Firmware verzió (GitHub publikus repó)
#define FIRMWARE_VERSION  "1.0.8"
#define FIRMWARE_BIN_URL   "https://raw.githubusercontent.com/pitee33/ontozes-vezerlo/main/firmware.bin"
#define FIRMWARE_VER_URL  "https://raw.githubusercontent.com/pitee33/ontozes-vezerlo/main/version.txt"

// Blynk virtual pin-ek (4 zóna + státusz)
#define VPIN_ZONE1    V1
#define VPIN_ZONE2    V2
#define VPIN_ZONE3    V3
#define VPIN_ZONE4    V4
#define VPIN_STATUS   V5
#define VPIN_SCHEDULE V6

// ==================== GLOBÁLIS VÁLTOZÓK ====================

WiFiClientSecure securedClient;   // Admin bot
WiFiClientSecure securedClient2; // 2. bot (külön SSL kapcsolat)
UniversalTelegramBot botAdmin(BOT_TOKEN_ADMIN, securedClient);
UniversalTelegramBot botFamily(BOT_TOKEN_FAMILY, securedClient2);
BlynkTimer blynkTimer;

// OLED
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, OLED_RST);
bool oledPresent = false;
unsigned long lastOledUpdate = 0;
int oledPage = 0;           // 0 = státusz, 1 = ütemezés
unsigned long lastPageSwitch = 0;

#define NUM_ZONES 4

struct Zone {
  int pin;
  bool active;
  unsigned long startTime;
  int durationMinutes;
  // Ütemezés
  bool scheduled;
  int schedHour;
  int schedMin;
  int schedDuration;
};

Zone zones[NUM_ZONES] = {
  { RELAY1_PIN, false, 0, 0, false, 6, 30, 10 },
  { RELAY2_PIN, false, 0, 0, false, 7, 0, 10 },
  { RELAY3_PIN, false, 0, 0, false, 18, 0, 15 },
  { RELAY4_PIN, false, 0, 0, false, 19, 0, 15 }
};

unsigned long lastBotPoll = 0;
unsigned long lastScheduleCheck = 0;
unsigned long lastFirmwareCheck = 0;
unsigned long lastBlynkUpdate = 0;
unsigned long lastNtpSync = 0;

bool wifiConnected = false;
bool ntpSynced = false;
String currentVersion = FIRMWARE_VERSION;

// ==================== FUNKCIÓK ====================

void updateOled() {
  if (!oledPresent) return;
  
  // Oldalváltás 5 másodpercenként
  unsigned long now = millis();
  if (now - lastPageSwitch > 5000) {
    oledPage = (oledPage + 1) % 2;
    lastPageSwitch = now;
  }
  
  display.clearDisplay();
  
  if (oledPage == 0) {
    // === 1. OLDAL: STÁTUSZ (4 zóna kompakt) ===
    // Sárga sáv (y=0-15): "idő" + pontos idő — size 2 (12x16px)
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("ido ");
    
    if (ntpSynced) {
      time_t t = time(nullptr);
      struct tm *tm = localtime(&t);
      char timeBuf[6];
      sprintf(timeBuf, "%02d:%02d", tm->tm_hour, tm->tm_min);
      // "ido " = 4×12 = 48px, idő = 5×12 = 60px, össz = 108px x=48-tól → 108
      display.setCursor(48, 0);
      display.print(timeBuf);
    } else {
      display.setCursor(48, 0);
      display.print("--:--");
    }
    
    // Kék sáv (y=16-63): 4 zóna kompakt
    display.drawLine(0, 16, 128, 16, SSD1306_WHITE);
    display.setTextSize(1);  // vissza size 1 a kék sávba
    
    for (int i = 0; i < NUM_ZONES; i++) {
      int y = 18 + i * 11;  // 18, 29, 40, 51
      display.setCursor(0, y);
      char label[6];
      sprintf(label, "Z%d:", i + 1);
      display.print(label);
      
      if (zones[i].active) {
        display.print("ON ");
        unsigned long elapsed = (millis() - zones[i].startTime) / 1000;
        int remaining = zones[i].durationMinutes * 60 - elapsed;
        if (remaining < 0) remaining = 0;
        char rem[8];
        sprintf(rem, "%d:%02d", remaining / 60, remaining % 60);
        display.print(rem);
      } else {
        display.print("OFF");
      }
      
      // Ütemezés jobb oldalon
      if (zones[i].scheduled) {
        char sched[16];
        sprintf(sched, "%02d:%02d %dp", zones[i].schedHour, zones[i].schedMin, zones[i].schedDuration);
        display.setCursor(60, y);
        display.print(sched);
      } else {
        display.setCursor(60, y);
        display.print("---");
      }
    }
    
  } else {
    // === 2. OLDAL: RENDSZER INFO ===
    // Sárga sáv: WiFi — size 2 (rövidítve)
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("WIFI");
    display.setCursor(72, 0);
    display.print(wifiConnected ? "OK" : "X");
    
    display.drawLine(0, 16, 128, 16, SSD1306_WHITE);
    display.setTextSize(1);  // vissza size 1 a kék sávba
    display.setCursor(0, 20);
    display.print("IP: " + WiFi.localIP().toString());
    
    display.setCursor(0, 30);
    if (ntpSynced) {
      time_t t = time(nullptr);
      struct tm *tm = localtime(&t);
      char dt[20];
      sprintf(dt, "%04d.%02d.%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
      display.print("Datum: " + String(dt));
    } else {
      display.print("NTP: N/A");
    }
    
    display.setCursor(0, 40);
    unsigned long upSec = millis() / 1000;
    display.print("Uptime: " + String(upSec / 3600) + "h " + 
                  String((upSec % 3600) / 60) + "m");
    
    display.setCursor(0, 53);
    display.print("4ch TG+Blynk " + currentVersion);
  }
  
  // Oldalszám jelzés (jobb alsó sarok)
  if (oledPage == 0) {
    display.fillRect(124, 62, 4, 1, SSD1306_WHITE);
  } else {
    display.fillRect(120, 62, 4, 1, SSD1306_WHITE);
    display.fillRect(124, 62, 4, 1, SSD1306_WHITE);
  }
  
  display.display();
}

void sendTelegram(const String &msg, bool adminOnly = false) {
  // Admin bot — ha hibázik, SSL kliens reset
  if (!botAdmin.sendMessage(ADMIN_CHAT_ID, msg, "")) {
    Serial.println("sendTelegram: admin bot HIBA, SSL reset");
    securedClient.stop();
    delay(100);
    securedClient.setInsecure();
    // Próbálja újra
    botAdmin.sendMessage(ADMIN_CHAT_ID, msg, "");
  }
  if (!adminOnly) {
    if (!botFamily.sendMessage(FAMILY_CHAT_ID, msg, "")) {
      Serial.println("sendTelegram: family bot HIBA, SSL reset");
      securedClient2.stop();
      delay(100);
      securedClient2.setInsecure();
      botFamily.sendMessage(FAMILY_CHAT_ID, msg, "");
    }
  }
  ESP.wdtFeed();
}

void updateBlynkStatus() {
  Blynk.virtualWrite(VPIN_ZONE1, zones[0].active ? 1 : 0);
  Blynk.virtualWrite(VPIN_ZONE2, zones[1].active ? 1 : 0);
  Blynk.virtualWrite(VPIN_ZONE3, zones[2].active ? 1 : 0);
  Blynk.virtualWrite(VPIN_ZONE4, zones[3].active ? 1 : 0);
  String status = "Z1:" + String(zones[0].active ? "ON" : "OFF") + 
                  " Z2:" + String(zones[1].active ? "ON" : "OFF") +
                  " Z3:" + String(zones[2].active ? "ON" : "OFF") +
                  " Z4:" + String(zones[3].active ? "ON" : "OFF");
  Blynk.virtualWrite(VPIN_STATUS, status);
  
  String sched = "";
  for (int i = 0; i < NUM_ZONES; i++) {
    if (i > 0) sched += " | ";
    sched += "Z" + String(i + 1) + ":";
    if (zones[i].scheduled) {
      sched += String(zones[i].schedHour) + ":" + 
               (zones[i].schedMin < 10 ? "0" : "") + String(zones[i].schedMin) + 
               " " + String(zones[i].schedDuration) + "p";
    } else {
      sched += "---";
    }
  }
  Blynk.virtualWrite(VPIN_SCHEDULE, sched);
}

void saveSchedule() {
  DynamicJsonDocument doc(1024);
  for (int i = 0; i < NUM_ZONES; i++) {
    JsonObject z = doc.createNestedObject("z" + String(i));
    z["sched"]  = zones[i].scheduled;
    z["hour"]   = zones[i].schedHour;
    z["min"]    = zones[i].schedMin;
    z["dur"]    = zones[i].schedDuration;
  }
  File f = LittleFS.open("/schedule.json", "w");
  serializeJson(doc, f);
  f.close();
}

void loadSchedule() {
  File f = LittleFS.open("/schedule.json", "r");
  if (!f) return;
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;
  for (int i = 0; i < NUM_ZONES; i++) {
    JsonObject z = doc["z" + String(i)];
    zones[i].scheduled    = z["sched"] | false;
    zones[i].schedHour     = z["hour"] | 6;
    zones[i].schedMin      = z["min"] | 30;
    zones[i].schedDuration = z["dur"] | 10;
  }
}

void startZone(int zoneIdx, int minutes) {
  if (zoneIdx < 0 || zoneIdx >= NUM_ZONES) return;
  zones[zoneIdx].active = true;
  zones[zoneIdx].startTime = millis();
  zones[zoneIdx].durationMinutes = minutes;
  digitalWrite(zones[zoneIdx].pin, RELAY_ON);
  
  String msg = "💧 Zone " + String(zoneIdx + 1) + " ELINDITVA (" + 
               String(minutes) + " perc)";
  sendTelegram(msg);
  updateBlynkStatus();
}

void stopZone(int zoneIdx) {
  if (zoneIdx < 0 || zoneIdx >= NUM_ZONES) return;
  zones[zoneIdx].active = false;
  digitalWrite(zones[zoneIdx].pin, RELAY_OFF);
  
  String msg = "✅ Zone " + String(zoneIdx + 1) + " LEALLITVA";
  sendTelegram(msg);
  updateBlynkStatus();
}

void stopAllZones() {
  for (int i = 0; i < NUM_ZONES; i++) {
    if (zones[i].active) stopZone(i);
  }
}

void checkRunningZones() {
  for (int i = 0; i < NUM_ZONES; i++) {
    if (zones[i].active) {
      unsigned long elapsed = (millis() - zones[i].startTime) / 1000 / 60;
      if (elapsed >= zones[i].durationMinutes) {
        stopZone(i);
      }
    }
  }
}

void checkSchedule() {
  if (!ntpSynced) return;
  
  time_t now = time(nullptr);
  struct tm *tm = localtime(&now);
  int curHour = tm->tm_hour;
  int curMin  = tm->tm_min;
  
  // csak egész percenként ellenőriz (ne többször触发)
  static int lastCheckedMinute = -1;
  if (lastCheckedMinute == curMin) return;
  lastCheckedMinute = curMin;
  
  for (int i = 0; i < NUM_ZONES; i++) {
    if (zones[i].scheduled && 
        zones[i].schedHour == curHour && 
        zones[i].schedMin == curMin &&
        !zones[i].active) {
      startZone(i, zones[i].schedDuration);
    }
  }
}

void syncNTP(bool silent = false) {
  Serial.println("NTP szinkron...");
  configTime(2 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  for (int i = 0; i < 10; i++) {
    time_t now = time(nullptr);
    if (now > 100000) {
      ntpSynced = true;
      Serial.println("NTP OK: " + String(ctime(&now)));
      if (!silent) {
        sendTelegram("🕐 NTP szinkron OK: " + String(ctime(&now)), true);
      }
      return;
    }
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nNTP SIKERTELEN!");
  ntpSynced = false;
}

void checkFirmware() {
  Serial.println("FW check indul...");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, FIRMWARE_VER_URL);
  http.setTimeout(15000);
  int code = http.GET();
  Serial.println("FW check HTTP code: " + String(code));
  if (code == HTTP_CODE_OK) {
    String latest = http.getString();
    http.end();
    latest.trim();
    Serial.println("GitHub version: " + latest + " | local: " + currentVersion);
    if (latest != currentVersion && latest.length() > 0) {
      String msg = "📦 Új firmware elérhető!\n";
      msg += "Jelenlegi: " + currentVersion + "\n";
      msg += "Új: " + latest + "\n";
      msg += "Frissítés: /upgrade";
      botAdmin.sendMessage(ADMIN_CHAT_ID, msg, "");
    }
  } else {
    Serial.println("FW check HIBA: " + http.errorToString(code));
    http.end();
  }
}

void doOTAUpdate() {
  Serial.println("=== OTA START ===");
  botAdmin.sendMessage(ADMIN_CHAT_ID, "⏳ OTA indul...", "");
  
  Blynk.disconnect();
  delay(200);
  botAdmin.getUpdates(botAdmin.last_message_received + 1);
  securedClient.stop();
  securedClient2.stop();
  delay(500);
  Serial.println("OTA: Free heap: " + String(ESP.getFreeHeap()));
  
  WiFiClientSecure otaClient;
  otaClient.setInsecure();
  
  ESP.wdtDisable();
  
  HTTPClient http;
  http.begin(otaClient, FIRMWARE_BIN_URL);
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  Serial.println("OTA: HTTP GET...");
  int code = http.GET();
  Serial.println("OTA HTTP code: " + String(code));
  
  if (code != HTTP_CODE_OK) {
    String err = "❌ HTTP " + String(code);
    Serial.println(err);
    http.end();
    ESP.wdtEnable(30000);
    delay(500);
    securedClient.setInsecure();
    botAdmin.sendMessage(ADMIN_CHAT_ID, err, "");
    delay(2000);
    ESP.restart();
    return;
  }
  
  int totalSize = http.getSize();
  Serial.println("OTA size: " + String(totalSize));
  if (totalSize <= 0) {
    http.end();
    ESP.wdtEnable(30000);
    delay(500);
    securedClient.setInsecure();
    botAdmin.sendMessage(ADMIN_CHAT_ID, "❌ Méret 0", "");
    delay(2000);
    ESP.restart();
    return;
  }
  
  if (!Update.begin(totalSize)) {
    http.end();
    ESP.wdtEnable(30000);
    delay(500);
    securedClient.setInsecure();
    botAdmin.sendMessage(ADMIN_CHAT_ID, "❌ Flash hely", "");
    delay(2000);
    ESP.restart();
    return;
  }
  
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t written = 0;
  int lastPct = -1;
  while (written < (size_t)totalSize) {
    size_t avail = stream->available();
    if (avail == 0) { delay(10); continue; }
    size_t rd = stream->readBytes(buf, min((size_t)1024, (size_t)(totalSize - written)));
    if (rd == 0) break;
    written += Update.write(buf, rd);
    int pct = (int)((written * 100) / totalSize);
    if (pct != lastPct && pct % 25 == 0) {
      Serial.println("OTA: " + String(pct) + "%");
      lastPct = pct;
    }
  }
  Serial.println("OTA written: " + String(written) + "/" + String(totalSize));
  
  if (written == (size_t)totalSize && Update.end(true) && Update.isFinished()) {
    Serial.println("OTA SUCCESS! Reboot...");
    http.end();
    delay(500);
    ESP.restart();
  } else {
    String err = "❌ written=" + String(written) + "/" + String(totalSize);
    Serial.println(err);
    http.end();
    ESP.wdtEnable(30000);
    delay(500);
    securedClient.setInsecure();
    botAdmin.sendMessage(ADMIN_CHAT_ID, err, "");
    delay(2000);
    ESP.restart();
  }
}

// ==================== TELEGRAM PARANCSOK ====================

String getStatusText() {
  String txt = "🌿 *Öntözés Státusz (4 zóna)*\n\n";
  for (int i = 0; i < NUM_ZONES; i++) {
    txt += "*Zone " + String(i + 1) + ":* ";
    txt += zones[i].active ? "🟢 ON" : "🔴 OFF";
    if (zones[i].active) {
      unsigned long elapsed = (millis() - zones[i].startTime) / 1000;
      int remaining = zones[i].durationMinutes * 60 - elapsed;
      txt += " (még " + String(remaining / 60) + "p " + 
             String(remaining % 60) + "mp)";
    }
    txt += "\n";
    if (zones[i].scheduled) {
      txt += "  ⏰ " + String(zones[i].schedHour) + ":" + 
             (zones[i].schedMin < 10 ? "0" : "") + String(zones[i].schedMin) + 
             " (" + String(zones[i].schedDuration) + "p)\n";
    }
  }
  time_t now = time(nullptr);
  if (ntpSynced) {
    txt += "\n🕐 Idő: " + String(ctime(&now));
  } else {
    txt += "\n⚠️ NTP nincs szinkronizálva\n";
  }
  txt += "FW: " + currentVersion + "\n";
  txt += "WiFi: " + String(WiFi.RSSI()) + " dBm\n";
  txt += "Heap: " + String(ESP.getFreeHeap() / 1024) + " kB\n";
  unsigned long secs = millis() / 1000;
  txt += "Uptime: " + String(secs / 3600) + "ó " + String((secs % 3600) / 60) + "p";
  return txt;
}

String getHelpText(bool isAdmin) {
  String h = "📋 *Parancsok (4 zóna)*\n\n";
  h += "/zone1 <perc> — Zone 1 indítása\n";
  h += "/zone2 <perc> — Zone 2 indítása\n";
  h += "/zone3 <perc> — Zone 3 indítása\n";
  h += "/zone4 <perc> — Zone 4 indítása\n";
  h += "/stop — Összes leállítása\n";
  h += "/status — Állapot lekérdezése\n";
  h += "/schedule — Ütemezés lekérdezése\n";
  if (isAdmin) {
    h += "\n*Admin:*\n";
    h += "/set <zone> <ora> <perc> <dur> — Ütemezés\n";
    h += "  pl: /set 1 06 30 10\n";
    h += "/clear <zone> — Ütemezés törlése\n";
    h += "/upgrade — Firmware frissítés\n";
    h += "/reboot — Újraindítás\n";
  }
  return h;
}

void handleCommand(UniversalTelegramBot &bot, String text, String chatId, bool isAdmin) {
  Serial.println("CMD: [" + chatId + "] " + text);
  text.trim();
  text.toLowerCase();
  
  if (text == "/help" || text == "/start") {
    bot.sendMessage(chatId, getHelpText(isAdmin), "Markdown");
    return;
  }
  
  if (text == "/status") {
    bot.sendMessage(chatId, getStatusText(), "Markdown");
    return;
  }
  
  if (text.startsWith("/zone")) {
    int zoneNum = text.substring(5, 6).toInt();
    int zoneIdx = zoneNum - 1;
    if (zoneIdx < 0 || zoneIdx >= NUM_ZONES) {
      bot.sendMessage(chatId, "Ismeretlen zóna. /help", "");
      return;
    }
    int mins = 10;
    int sp = text.indexOf(' ');
    if (sp > 0) mins = text.substring(sp + 1).toInt();
    if (mins < 1) mins = 1;
    if (mins > 120) mins = 120;
    startZone(zoneIdx, mins);
    return;
  }
  
  if (text == "/stop") {
    stopAllZones();
    return;
  }
  
  if (text == "/schedule") {
    String s = "⏰ *Ütemezés (4 zóna)*\n\n";
    for (int i = 0; i < NUM_ZONES; i++) {
      s += "*Zone " + String(i + 1) + ":* ";
      if (zones[i].scheduled) {
        s += String(zones[i].schedHour) + ":" + 
              (zones[i].schedMin < 10 ? "0" : "") + String(zones[i].schedMin) + 
              " (" + String(zones[i].schedDuration) + "p)\n";
      } else {
        s += "nincs\n";
      }
    }
    bot.sendMessage(chatId, s, "Markdown");
    return;
  }
  
  if (!isAdmin) {
    bot.sendMessage(chatId, "❌ Ezt a parancsot csak admin használhatja", "");
    return;
  }
  
  if (text.startsWith("/set")) {
    int sp1 = text.indexOf(' ');
    int sp2 = text.indexOf(' ', sp1 + 1);
    int sp3 = text.indexOf(' ', sp2 + 1);
    int sp4 = text.indexOf(' ', sp3 + 1);
    if (sp1 > 0 && sp2 > 0 && sp3 > 0 && sp4 > 0) {
      int z = text.substring(sp1 + 1, sp2).toInt() - 1;
      int h = text.substring(sp2 + 1, sp3).toInt();
      int m = text.substring(sp3 + 1, sp4).toInt();
      int d = text.substring(sp4 + 1).toInt();
      if (z >= 0 && z < NUM_ZONES && h >= 0 && h <= 23 && m >= 0 && m <= 59 && d > 0) {
        zones[z].scheduled = true;
        zones[z].schedHour = h;
        zones[z].schedMin = m;
        zones[z].schedDuration = d;
        saveSchedule();
        String msg = "✅ Zone " + String(z + 1) + " ütemezve: " + 
                     String(h) + ":" + (m < 10 ? "0" : "") + String(m) + 
                     " (" + String(d) + " perc)";
        sendTelegram(msg);
        updateBlynkStatus();
        return;
      }
    }
    bot.sendMessage(chatId, "Használat: /set <zone> <ora> <perc> <dur>\npl: /set 1 06 30 10\nZónák: 1-4", "");
    return;
  }
  
  if (text.startsWith("/clear")) {
    int sp = text.indexOf(' ');
    if (sp > 0) {
      int z = text.substring(sp + 1).toInt() - 1;
      if (z >= 0 && z < NUM_ZONES) {
        zones[z].scheduled = false;
        saveSchedule();
        String msg = "🗑️ Zone " + String(z + 1) + " ütemezés törölve";
        sendTelegram(msg);
        updateBlynkStatus();
        return;
      }
    }
    bot.sendMessage(chatId, "Használat: /clear <zone>\npl: /clear 1\nZónák: 1-4", "");
    return;
  }
  
  if (text == "/upgrade") {
    doOTAUpdate();
    return;
  }
  
  if (text == "/reboot") {
    bot.sendMessage(chatId, "🔄 Újraindítás...", "");
    delay(500);
    ESP.restart();
    return;
  }
  
  bot.sendMessage(chatId, "Ismeretlen parancs. /help", "");
}

void handleBotUpdates(UniversalTelegramBot &bot, WiFiClientSecure &client, bool isAdmin) {
  int numNew = bot.getUpdates(bot.last_message_received + 1);
  if (numNew < 0) {
    // Hiba — SSL kliens reset
    Serial.println("Bot poll HIBA, SSL reset");
    client.stop();
    delay(100);
    client.setInsecure();
    return;
  }
  while (numNew) {
    for (int i = 0; i < numNew; i++) {
      String text = bot.messages[i].text;
      String chatId = bot.messages[i].chat_id;
      handleCommand(bot, text, chatId, isAdmin);
    }
    ESP.wdtFeed();
    numNew = bot.getUpdates(bot.last_message_received + 1);
  }
}

// ==================== BLYNK HANDLERS ====================

BLYNK_WRITE(VPIN_ZONE1) {
  int state = param.asInt();
  if (state) startZone(0, 10);
  else stopZone(0);
}

BLYNK_WRITE(VPIN_ZONE2) {
  int state = param.asInt();
  if (state) startZone(1, 10);
  else stopZone(1);
}

BLYNK_WRITE(VPIN_ZONE3) {
  int state = param.asInt();
  if (state) startZone(2, 10);
  else stopZone(2);
}

BLYNK_WRITE(VPIN_ZONE4) {
  int state = param.asInt();
  if (state) startZone(3, 10);
  else stopZone(3);
}

// ==================== SETUP & LOOP ====================

void setup() {
  Serial.begin(115200);
  delay(100);
  
  // Relé pin-ek — aktív magas, boot-kor LOW (ki)
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(RELAY3_PIN, OUTPUT);
  pinMode(RELAY4_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, RELAY_OFF);
  digitalWrite(RELAY2_PIN, RELAY_OFF);
  digitalWrite(RELAY3_PIN, RELAY_OFF);
  digitalWrite(RELAY4_PIN, RELAY_OFF);
  
  // LittleFS
  LittleFS.begin();
  loadSchedule();
  
  // OLED inicializálás (Ideaspark v2.1: SDA=GPIO12, SCL=GPIO14)
  Wire.begin(12, 14);  // SDA=D6/GPIO12, SCL=D5/GPIO14
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    oledPresent = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Ontozes Vezerles");
    display.println("Booting...");
    display.display();
    Serial.println("OLED OK");
  } else {
    oledPresent = false;
    Serial.println("OLED nem talalhato! (0x3C/0x3D)");
  }
  
  // WiFi
  Serial.println("WiFi csatlakozás...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 30) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi sikertelen! Watchdog újraindítja.");
    // WDT reset lesz
  }
  
  // TLS
  securedClient.setInsecure();
  securedClient2.setInsecure();
  
  // Blynk
  Blynk.config(BLYNK_AUTH_TOKEN);
  // Nem blokkoló — Blynk.run() kezeli a csatlakozást
  
  // NTP
  syncNTP();
  
  // Blynk timer-ek
  blynkTimer.setInterval(5000L, []() {
    updateBlynkStatus();
  });
  
  // Boot üzenet
  if (wifiConnected) {
    // Régi Telegram üzenetek ürítése — offset a legutolsó update utáni pozícióra állít
    // getUpdates(-1) = az összes régi üzenetet üríti, nem dolgozza fel
    int n1 = botAdmin.getUpdates(0);
    while (n1) {
      n1 = botAdmin.getUpdates(botAdmin.last_message_received + 1);
    }
    int n2 = botFamily.getUpdates(0);
    while (n2) {
      n2 = botFamily.getUpdates(botFamily.last_message_received + 1);
    }
    Serial.println("Bot queue ürítve (admin:" + String(botAdmin.last_message_received) + 
                   " family:" + String(botFamily.last_message_received) + ")");
    
    String bootMsg = "🌿 Öntözésvezérlés elindult!\n";
    bootMsg += "IP: " + WiFi.localIP().toString() + "\n";
    bootMsg += "FW: " + currentVersion + "\n";
    bootMsg += "NTP: " + String(ntpSynced ? "OK" : "FAILED");
    sendTelegram(bootMsg, true);
  }
  
  // Első OLED frissítés
  updateOled();
  
  Serial.println("Setup kész!");
}

void loop() {
  ESP.wdtFeed(); // Watchdog etetés
  
  // Blynk futás
  if (wifiConnected) {
    Blynk.run();
  }
  blynkTimer.run();
  
  // Telegram bot poll (1 másodpercenként)
  unsigned long now = millis();
  if (now - lastBotPoll > 1000) {
    lastBotPoll = now;
    if (wifiConnected) {
      handleBotUpdates(botAdmin, securedClient, true);
      handleBotUpdates(botFamily, securedClient2, true);
    }
  }
  
  // Futó zónák ellenőrzése (ms-onként)
  checkRunningZones();
  
  // Ütemezés ellenőrzése (1 másodpercenként elég)
  if (now - lastScheduleCheck > 1000) {
    lastScheduleCheck = now;
    checkSchedule();
  }
  
  // NTP újracsatlakozás (1 óránként, csendesen — csak boot-nál küld Telegramot)
  if (now - lastNtpSync > 3600000) {
    lastNtpSync = now;
    if (wifiConnected) syncNTP(true);
  }
  
  // Firmware check (6 óránként)
  if (now - lastFirmwareCheck > 21600000) {
    lastFirmwareCheck = now;
    if (wifiConnected) checkFirmware();
  }
  
  // OLED frissítés (1 másodpercenként)
  if (now - lastOledUpdate > 1000) {
    lastOledUpdate = now;
    updateOled();
  }
  
  // WiFi reconnect
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    WiFi.reconnect();
    delay(1000);
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
    }
  }
  
  delay(10);
}