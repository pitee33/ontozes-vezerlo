/*
 * Öntözésvezérlés ESP8266 + Telegram + Blynk
 * FW v1.1.0 — OLED sleep, FLASH gomb ébresztés, több ütemezés/zóna
 *
 * Hardware:
 *   - Wemos D1 Mini (ESP8266)
 *   - 4 csatorna relé board (aktív magas = HIGH be, LOW ki)
 *     D1=Z1, D2=Z2, D0=Z3, D4=Z4
 *   - SSD1306 OLED (SDA=GPIO12/D6, SCL=GPIO14/D5)
 *   - DHT11 hőmérséklet/pára (GPIO13/D7)
 *   - FLASH gomb (GPIO0/D3) — OLED ébresztés
 *   - NTP időszinkron (nincs RTC)
 *
 * Funkciók:
 *   - 4 zóna relé vezérlés
 *   - Több ütemezés zónánként (max 4/zóna)
 *   - OLED kikapcsolása 2 perc inaktivitás után
 *   - FLASH gomb ébreszti az OLED-et
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
#include <DHT.h>

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
// FIGYELEM: Z3 áthelyezve D3-ról D0-ra (GPIO0 felszabadítva FLASH gombnak)
#define RELAY1_PIN    D1   // GPIO5 — Zone 1
#define RELAY2_PIN    D2   // GPIO4 — Zone 2
#define RELAY3_PIN    D0   // GPIO16 — Zone 3 (áthelyezve D3-ról)
#define RELAY4_PIN    D4   // GPIO2 — Zone 4
#define RELAY_ON      HIGH
#define RELAY_OFF     LOW

// FLASH gomb (NodeMCU beépített gomb, GPIO0/D3)
#define FLASH_BTN_PIN D3   // GPIO0 — OLED ébresztés

// DHT11 hőmérséklet/pára szenzor (GPIO13/D7)
#define DHT_PIN       D7
#define DHT_TYPE      DHT11

// OLED (Ideaspark v2.1: SDA=GPIO12/D6, SCL=GPIO14/D5, addr 0x3C)
#define OLED_ADDR     0x3C
#define OLED_W        128
#define OLED_H        64
#define OLED_RST      -1

// OLED sleep timeout (2 perc inaktivitás)
#define OLED_SLEEP_MS 120000UL

// Maximum ütemezések zónánként
#define MAX_SCHEDULES 4

// Firmware verzió (GitHub publikus repó)
#define FIRMWARE_VERSION  "1.2.0"
#define FIRMWARE_BIN_URL   "https://raw.githubusercontent.com/pitee33/ontozes-vezerlo/main/firmware.bin"
#define FIRMWARE_VER_URL  "https://raw.githubusercontent.com/pitee33/ontozes-vezerlo/main/version.txt"

// Időjárás (Open-Meteo — HTTP, ingyenes, nem kell API kulcs)
// Törökbálint: 47.28°N, 18.92°E
#define WEATHER_URL "http://api.open-meteo.com/v1/forecast?latitude=47.28&longitude=18.92&daily=precipitation_sum,precipitation_probability_max&timezone=Europe/Budapest&forecast_days=1"
#define WEATHER_CHECK_MS 1800000UL  // 30 perc
#define RAIN_THRESHOLD_MM 1.0      // >1mm eső = esős nap
#define RAIN_THRESHOLD_PROB 50     // >50% valószínűség = esős
#define WATER_TIMEOUT_MS 1800000UL  // 30 perc válasz nélkül → auto öntöz

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
int oledPage = 0;           // 0 = státusz, 1 = rendszer, 2 = ütemezés
unsigned long lastPageSwitch = 0;

// OLED sleep
bool oledSleeping = false;
unsigned long lastActivity = 0;

// FLASH gomb — interrupt alapú, azonnali reakció
volatile bool btnPressedFlag = false;

void ICACHE_RAM_ATTR flashBtnISR() {
  btnPressedFlag = true;
}

#define NUM_ZONES 4

struct Schedule {
  bool enabled;
  int hour;
  int min;
  int duration;
};

struct Zone {
  int pin;
  bool active;
  unsigned long startTime;
  int durationMinutes;
  Schedule schedules[MAX_SCHEDULES];
};

Zone zones[NUM_ZONES] = {
  { RELAY1_PIN, false, 0, 0, {
    { true, 6, 30, 10 }, { false, 0, 0, 0 }, { false, 0, 0, 0 }, { false, 0, 0, 0 }
  }},
  { RELAY2_PIN, false, 0, 0, {
    { true, 7, 0, 10 }, { false, 0, 0, 0 }, { false, 0, 0, 0 }, { false, 0, 0, 0 }
  }},
  { RELAY3_PIN, false, 0, 0, {
    { true, 18, 0, 15 }, { false, 0, 0, 0 }, { false, 0, 0, 0 }, { false, 0, 0, 0 }
  }},
  { RELAY4_PIN, false, 0, 0, {
    { true, 19, 0, 15 }, { false, 0, 0, 0 }, { false, 0, 0, 0 }, { false, 0, 0, 0 }
  }}
};

unsigned long lastBotPoll = 0;
unsigned long lastScheduleCheck = 0;
unsigned long lastFirmwareCheck = 0;
unsigned long lastBlynkUpdate = 0;
unsigned long lastNtpSync = 0;
unsigned long lastWeatherCheck = 0;

bool wifiConnected = false;
bool ntpSynced = false;
String currentVersion = FIRMWARE_VERSION;

// DHT11 szenzor
DHT dht(DHT_PIN, DHT_TYPE);
float dhtTemp = NAN;
float dhtHum = NAN;
unsigned long lastDhtRead = 0;

// ==================== IDŐJÁRÁS ====================

// Időjárás adatok
float todayRainMm = 0.0;       // Mai eső mennyiség (mm)
int todayRainProb = 0;         // Mai eső valószínűség (%)
bool weatherChecked = false;   // Sikerült-e lekérni
bool isRainyDay = false;        // Esős nap-e

// Pending locsolás kérdés (inline gombos)
struct PendingWater {
  bool active;                 // Van folyamatban lévő kérdés?
  int zoneIdx;                 // Melyik zóna
  int duration;                // Hány perc
  int schedSlot;               // Melyik slot indította
  unsigned long askTime;       // Mikor tettük fel a kérdést
};
PendingWater pendingWater = { false, 0, 0, 0, 0 };

// ==================== OLED SLEEP / WAKE ====================

void wakeOled() {
  if (!oledPresent) return;
  if (oledSleeping) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    oledSleeping = false;
    // Azonnal rajzoljon újra — különben üres képernyő
    lastOledUpdate = 0;  // kényszeríti a loop-ban lévő updateOled()-et
    // Kék LED bekapcsolása (GPIO2/D4 — aktív LOW a beépített LED)
    // Csak ha Z4 relé nincs aktív — ne zavarja a relét
    if (!zones[3].active) {
      digitalWrite(RELAY4_PIN, LOW);  // LED be (LOW = ON a NodeMCU LED-nél)
    }
    Serial.println("OLED wake");
  }
  lastActivity = millis();
}

void checkOledSleep() {
  if (!oledPresent || oledSleeping) return;
  if (millis() - lastActivity > OLED_SLEEP_MS) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    oledSleeping = true;
    // Kék LED kikapcsolása (GPIO2/D4 — HIGH = OFF a NodeMCU LED-nél)
    // Csak ha Z4 relé nincs aktív
    if (!zones[3].active) {
      digitalWrite(RELAY4_PIN, HIGH);  // LED ki
    }
    Serial.println("OLED sleep (inaktivitas) + LED ki");
  }
}

// Forward declaration
void askWaterQuestion(int zoneIdx, int duration, int schedSlot);
void handleCallbackQuery(String callbackData, String chatId, String messageId);

// ==================== FLASH GOMB ====================

void checkFlashButton() {
  if (btnPressedFlag) {
    btnPressedFlag = false;
    // Debounce: várjon amíg felengedik, de ne blokkolja sokáig
    delay(30);
    wakeOled();
    Serial.println("FLASH gomb (ISR) -> OLED wake");
  }
}

// ==================== SEGÉDFUNKCIÓK ====================

// Egy zóna ütemezéseinek száma
int countSchedules(int zoneIdx) {
  int cnt = 0;
  for (int s = 0; s < MAX_SCHEDULES; s++) {
    if (zones[zoneIdx].schedules[s].enabled) cnt++;
  }
  return cnt;
}

// Következő ütemezés ideje (HH:MM string) vagy "---"
String nextScheduleTime(int zoneIdx) {
  if (!ntpSynced) {
    // NTP nélkül csak az első ütemezést mutatjuk
    for (int s = 0; s < MAX_SCHEDULES; s++) {
      if (zones[zoneIdx].schedules[s].enabled) {
        char buf[6];
        sprintf(buf, "%02d:%02d", zones[zoneIdx].schedules[s].hour, zones[zoneIdx].schedules[s].min);
        return String(buf);
      }
    }
    return "---";
  }
  
  time_t now = time(nullptr);
  struct tm *tm = localtime(&now);
  int curMin = tm->tm_hour * 60 + tm->tm_min;
  
  int bestMin = 24 * 60;  // holnap
  bool found = false;
  
  for (int s = 0; s < MAX_SCHEDULES; s++) {
    if (!zones[zoneIdx].schedules[s].enabled) continue;
    int schedMin = zones[zoneIdx].schedules[s].hour * 60 + zones[zoneIdx].schedules[s].min;
    if (schedMin >= curMin && schedMin < bestMin) {
      bestMin = schedMin;
      found = true;
    }
  }
  
  // Ha nincs ma már több, holnap az első
  if (!found) {
    for (int s = 0; s < MAX_SCHEDULES; s++) {
      if (!zones[zoneIdx].schedules[s].enabled) continue;
      int schedMin = zones[zoneIdx].schedules[s].hour * 60 + zones[zoneIdx].schedules[s].min;
      if (schedMin < bestMin) {
        bestMin = schedMin;
        found = true;
      }
    }
  }
  
  if (found) {
    char buf[6];
    sprintf(buf, "%02d:%02d", bestMin / 60, bestMin % 60);
    return String(buf);
  }
  return "---";
}

// Uptime formázott string — nap/hónap bontás
String uptimeStr() {
  unsigned long s = millis() / 1000;
  int days = s / 86400;
  int hours = (s % 86400) / 3600;
  int mins = (s % 3600) / 60;
  int months = days / 30;
  int remDays = days % 30;
  
  String out = "";
  if (months > 0) {
    out += String(months) + "ho ";
    if (remDays > 0) out += String(remDays) + "n ";
    if (hours > 0) out += String(hours) + "o";
  } else if (days > 0) {
    out += String(days) + "n " + String(hours) + "o " + String(mins) + "p";
  } else {
    out += String(hours) + "o " + String(mins) + "p";
  }
  return out;
}

// ==================== OLED ====================

void updateOled() {
  if (!oledPresent) return;
  
  // Oldalváltás 5 másodpercenként
  unsigned long now = millis();
  if (now - lastPageSwitch > 5000) {
    oledPage = (oledPage + 1) % 3;
    lastPageSwitch = now;
  }
  
  display.clearDisplay();
  
  if (oledPage == 0) {
    // === 1. OLDAL: STÁTUSZ (4 zóna kompakt) ===
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("ido ");
    
    if (ntpSynced) {
      time_t t = time(nullptr);
      struct tm *tm = localtime(&t);
      char timeBuf[6];
      sprintf(timeBuf, "%02d:%02d", tm->tm_hour, tm->tm_min);
      display.setCursor(48, 0);
      display.print(timeBuf);
    } else {
      display.setCursor(48, 0);
      display.print("--:--");
    }
    
    display.drawLine(0, 16, 128, 16, SSD1306_WHITE);
    display.setTextSize(1);
    
    for (int i = 0; i < NUM_ZONES; i++) {
      int y = 18 + i * 11;
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
      
      // Ütemezés: count + next time
      int schedCnt = countSchedules(i);
      if (schedCnt > 0) {
        char sched[16];
        sprintf(sched, "%dx %s", schedCnt, nextScheduleTime(i).c_str());
        display.setCursor(60, y);
        display.print(sched);
      } else {
        display.setCursor(60, y);
        display.print("---");
      }
    }
    
  } else if (oledPage == 1) {
    // === 2. OLDAL: RENDSZER INFO ===
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("WIFI");
    display.setCursor(72, 0);
    display.print(wifiConnected ? "OK" : "X");
    
    display.drawLine(0, 16, 128, 16, SSD1306_WHITE);
    display.setTextSize(1);
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
    display.print("Up: " + uptimeStr());
    
    display.setCursor(0, 50);
    if (!isnan(dhtTemp)) {
      display.print(String((int)dhtTemp) + "C " + String((int)dhtHum) + "% P");
    }
    display.print(currentVersion);
    
  } else {
    // === 3. OLDAL: ÜTEMEZÉS ÖSSZEGZÉS ===
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("Utem");
    
    display.drawLine(0, 16, 128, 16, SSD1306_WHITE);
    display.setTextSize(1);
    
    for (int i = 0; i < NUM_ZONES; i++) {
      int y = 20 + i * 11;
      display.setCursor(0, y);
      char label[8];
      sprintf(label, "Z%d:", i + 1);
      display.print(label);
      
      int cnt = countSchedules(i);
      if (cnt > 0) {
        // Listázza az időpontokat kompakt
        display.setCursor(20, y);
        String times = "";
        for (int s = 0; s < MAX_SCHEDULES; s++) {
          if (!zones[i].schedules[s].enabled) continue;
          char tbuf[8];
          sprintf(tbuf, "%02d:%02d", zones[i].schedules[s].hour, zones[i].schedules[s].min);
          if (times.length() > 0) times += " ";
          times += tbuf;
        }
        // Csonkítás ha túl hosszú
        if (times.length() > 17) times = times.substring(0, 17);
        display.print(times);
      } else {
        display.setCursor(20, y);
        display.print("nincs");
      }
    }
  }
  
  // Oldalszám jelzés (jobb alsó sarok)
  for (int p = 0; p <= oledPage; p++) {
    display.fillRect(120 + p * 4, 62, 4, 1, SSD1306_WHITE);
  }
  
  display.display();
}

// ==================== TELEGRAM ====================

void sendTelegram(const String &msg, bool adminOnly = false) {
  if (!botAdmin.sendMessage(ADMIN_CHAT_ID, msg, "")) {
    Serial.println("sendTelegram: admin bot HIBA, SSL reset");
    securedClient.stop();
    delay(100);
    securedClient.setInsecure();
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

// ==================== BLYNK ====================

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
    int cnt = countSchedules(i);
    if (cnt > 0) {
      sched += String(cnt) + "x ";
      sched += nextScheduleTime(i);
    } else {
      sched += "---";
    }
  }
  Blynk.virtualWrite(VPIN_SCHEDULE, sched);
}

// ==================== ÜTEMEZÉS MENTÉS/BETÖLTÉS ====================

void saveSchedule() {
  DynamicJsonDocument doc(2048);
  for (int i = 0; i < NUM_ZONES; i++) {
    JsonArray arr = doc.createNestedArray("z" + String(i));
    for (int s = 0; s < MAX_SCHEDULES; s++) {
      JsonObject sch = arr.createNestedObject();
      sch["e"] = zones[i].schedules[s].enabled ? 1 : 0;
      sch["h"] = zones[i].schedules[s].hour;
      sch["m"] = zones[i].schedules[s].min;
      sch["d"] = zones[i].schedules[s].duration;
    }
  }
  File f = LittleFS.open("/schedule.json", "w");
  serializeJson(doc, f);
  f.close();
}

void loadSchedule() {
  File f = LittleFS.open("/schedule.json", "r");
  if (!f) return;
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;
  
  for (int i = 0; i < NUM_ZONES; i++) {
    String key = "z" + String(i);
    JsonVariant val = doc[key];
    
    if (val.is<JsonArray>()) {
      // Új formátum: ütemezések tömbje
      JsonArray arr = val.as<JsonArray>();
      for (int s = 0; s < MAX_SCHEDULES && s < (int)arr.size(); s++) {
        zones[i].schedules[s].enabled  = arr[s]["e"] | 0;
        zones[i].schedules[s].hour     = arr[s]["h"] | 0;
        zones[i].schedules[s].min      = arr[s]["m"] | 0;
        zones[i].schedules[s].duration = arr[s]["d"] | 0;
      }
    } else if (val.is<JsonObject>()) {
      // Régi formátum (v1.0.x): egyetlen ütemezés
      JsonObject obj = val.as<JsonObject>();
      zones[i].schedules[0].enabled  = obj["sched"] | false;
      zones[i].schedules[0].hour     = obj["hour"] | 6;
      zones[i].schedules[0].min      = obj["min"] | 30;
      zones[i].schedules[0].duration = obj["dur"] | 10;
      for (int s = 1; s < MAX_SCHEDULES; s++) {
        zones[i].schedules[s].enabled = false;
        zones[i].schedules[s].hour = 0;
        zones[i].schedules[s].min = 0;
        zones[i].schedules[s].duration = 0;
      }
    }
  }
  Serial.println("Ütemezés betöltve (v1.1.0 formátum)");
}

// ==================== ZÓNA VEZÉRLÉS ====================

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
  wakeOled();
}

void stopZone(int zoneIdx) {
  if (zoneIdx < 0 || zoneIdx >= NUM_ZONES) return;
  zones[zoneIdx].active = false;
  digitalWrite(zones[zoneIdx].pin, RELAY_OFF);
  
  String msg = "✅ Zone " + String(zoneIdx + 1) + " LEALLITVA";
  sendTelegram(msg);
  updateBlynkStatus();
  wakeOled();
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
  
  static int lastCheckedMinute = -1;
  if (lastCheckedMinute == curMin) return;
  lastCheckedMinute = curMin;
  
  for (int i = 0; i < NUM_ZONES; i++) {
    if (zones[i].active) continue;
    for (int s = 0; s < MAX_SCHEDULES; s++) {
      if (zones[i].schedules[s].enabled &&
          zones[i].schedules[s].hour == curHour &&
          zones[i].schedules[s].min == curMin) {
        // Időjárás-alapú locsolás: esős nap → kérdez
        askWaterQuestion(i, zones[i].schedules[s].duration, s);
        break;
      }
    }
  }
}

// ==================== IDŐJÁRÁS (Open-Meteo) ====================

void checkWeather() {
  if (!wifiConnected) return;
  
  Serial.println("Időjárás lekérés (Open-Meteo)...");
  WiFiClient client;
  HTTPClient http;
  http.begin(client, WEATHER_URL);
  http.setTimeout(10000);
  int code = http.GET();
  Serial.println("Weather HTTP code: " + String(code));
  
  if (code != HTTP_CODE_OK) {
    Serial.println("Weather HIBA: " + http.errorToString(code));
    http.end();
    return;
  }
  
  String response = http.getString();
  http.end();
  
  // JSON parse — Open-Meteo válasz:
  // {"daily":{"time":["2024-01-01"],"precipitation_sum":[0.0],"precipitation_probability_max":[20]}}
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    Serial.println("Weather JSON HIBA: " + String(err.c_str()));
    return;
  }
  
  JsonObject daily = doc["daily"];
  if (daily.isNull()) {
    Serial.println("Weather: nincs daily objektum");
    return;
  }
  
  JsonArray precipSum = daily["precipitation_sum"];
  JsonArray precipProb = daily["precipitation_probability_max"];
  
  if (precipSum.isNull() || precipProb.isNull()) {
    Serial.println("Weather: nincs precipitation adat");
    return;
  }
  
  todayRainMm = precipSum[0].as<float>();
  todayRainProb = precipProb[0].as<int>();
  weatherChecked = true;
  
  // Esős nap ha >1mm eső vagy >50% valószínűség
  isRainyDay = (todayRainMm >= RAIN_THRESHOLD_MM || todayRainProb >= RAIN_THRESHOLD_PROB);
  
  Serial.printf("Weather: %.1fmm, %d%% prob -> %s\n", 
                todayRainMm, todayRainProb, isRainyDay ? "ESŐS" : "száraz");
}

// ==================== LOC SOLÁS KÉRDÉS ====================

// Inline keyboard a locsolás kérdéshez
String waterKeyboardJson() {
  String kb = "{\"inline_keyboard\":[[{\"text\":\"💧 Öntöz\",\"callback_data\":\"water_yes\"},{\"text\":\"🌧 Kihagy\",\"callback_data\":\"water_no\"}]]}";
  return kb;
}

// Kérdés felvétele: esős nap + ütemezett locsolás előtt
void askWaterQuestion(int zoneIdx, int duration, int schedSlot) {
  // Friss időjárás lekérés a döntés előtt — ne 30 perces adatra támasszkodjon
  Serial.println("Ütemezett öntözés előtt — friss időjárás lekérés...");
  checkWeather();
  
  if (!weatherChecked) {
    // Időjárás lekérés sikertelen — biztonsági okokból öntöz
    Serial.println("Weather lekérés sikertelen — auto öntöz (biztonság)");
    botAdmin.sendMessage(ADMIN_CHAT_ID, 
      "⚠️ Időjárás lekérés sikertelen, automatikus öntözés (Z" + 
      String(zoneIdx + 1) + ")", "");
    startZone(zoneIdx, duration);
    return;
  }
  
  if (!isRainyDay) {
    // Nem esős — simán öntöz
    Serial.println("Nem esős nap — auto öntöz");
    startZone(zoneIdx, duration);
    return;
  }
  
  // Esős nap — kérdezz
  pendingWater.active = true;
  pendingWater.zoneIdx = zoneIdx;
  pendingWater.duration = duration;
  pendingWater.schedSlot = schedSlot;
  pendingWater.askTime = millis();
  
  String msg = "🌧 *Esős nap van!*\n";
  msg += "Előrejelzés: " + String(todayRainMm, 1) + "mm eső, ";
  msg += String(todayRainProb) + "% valószínűség\n\n";
  msg += "*Zone " + String(zoneIdx + 1) + "* ütemezett öntözés: ";
  msg += String(duration) + " perc\n\n";
  msg += "Szükséges a locsolás?";
  
  // Admin bot küld inline gombbal
  bool ok = botAdmin.sendMessageWithInlineKeyboard(ADMIN_CHAT_ID, msg, "Markdown", waterKeyboardJson());
  if (!ok) {
    Serial.println("Water question küldés HIBA — auto öntöz");
    pendingWater.active = false;
    startZone(zoneIdx, duration);
  }
  Serial.println("Water question elküldve (Z" + String(zoneIdx + 1) + ")");
}

// Pending water timeout ellenőrzés (10 perc → auto öntöz)
void checkPendingWaterTimeout() {
  if (!pendingWater.active) return;
  if (millis() - pendingWater.askTime > WATER_TIMEOUT_MS) {
    Serial.println("Water question timeout — auto öntöz");
    pendingWater.active = false;
    startZone(pendingWater.zoneIdx, pendingWater.duration);
    botAdmin.sendMessage(ADMIN_CHAT_ID, 
      "⏰ Nincs válasz 30 percig — automatikus öntözés elindítva (Z" + 
      String(pendingWater.zoneIdx + 1) + ")", "");
  }
}

// Callback query kezelése (inline gomb válasz)
void handleCallbackQuery(String callbackData, String chatId, String messageId) {
  Serial.println("Callback: " + callbackData + " from " + chatId);
  
  if (!pendingWater.active) {
    botAdmin.sendMessage(chatId, "Ez a kérdés már lejárt.", "");
    return;
  }
  
  if (callbackData == "water_yes") {
    int z = pendingWater.zoneIdx;
    int d = pendingWater.duration;
    pendingWater.active = false;
    startZone(z, d);
    botAdmin.sendMessage(chatId, 
      "💧 Öntözés elindítva! (Z" + String(z + 1) + ", " + String(d) + "p)", "");
  } else if (callbackData == "water_no") {
    int z = pendingWater.zoneIdx;
    pendingWater.active = false;
    botAdmin.sendMessage(chatId, 
      "🌧 Öntözés kihagyva (Z" + String(z + 1) + "). Esős nap van!", "");
  }
}

// ==================== NTP ====================

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

// ==================== FIRMWARE ====================

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
    int cnt = countSchedules(i);
    if (cnt > 0) {
      for (int s = 0; s < MAX_SCHEDULES; s++) {
        if (!zones[i].schedules[s].enabled) continue;
        txt += "  ⏰ *" + String(s + 1) + ":* " + 
               String(zones[i].schedules[s].hour) + ":" + 
               (zones[i].schedules[s].min < 10 ? "0" : "") + String(zones[i].schedules[s].min) + 
               " (" + String(zones[i].schedules[s].duration) + "p)\n";
      }
    } else {
      txt += "  ⏰ nincs ütemezés\n";
    }
  }
  time_t now = time(nullptr);
  if (ntpSynced) {
    txt += "\n🕐 Idő: " + String(ctime(&now));
  } else {
    txt += "\n⚠️ NTP nincs szinkronizálva\n";
  }
  txt += "📦 FW: " + currentVersion + "\n";
  txt += "📶 WiFi: " + String(WiFi.RSSI()) + " dBm";
  int rssi = WiFi.RSSI();
  if (rssi >= -55) txt += " (kiváló)";
  else if (rssi >= -67) txt += " (jó)";
  else if (rssi >= -78) txt += " (közepes)";
  else if (rssi >= -85) txt += " (gyenge)";
  else txt += " (nagyon gyenge)";
  txt += "\n";
  if (!isnan(dhtTemp)) {
    txt += "🌡 Panel: " + String((int)dhtTemp) + "°C  💧" + String((int)dhtHum) + "%\n";
  }
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t totalHeap = 81920;
  txt += "💾 Heap: " + String(freeHeap / 1024) + "/" + String(totalHeap / 1024) + " kB\n";
  txt += "⏱ Uptime: " + uptimeStr() + "\n";
  if (weatherChecked) {
    txt += "🌤 " + String(todayRainMm, 1) + "mm/" + String(todayRainProb) + "% ";
    txt += isRainyDay ? "(esős)" : "(száraz)";
  }
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
    h += "/set <zone> <slot> <ora> <perc> <dur>\n";
    h += "  pl: /set 1 1 06 30 10\n";
    h += "  zone: 1-4, slot: 1-4\n";
    h += "/clear <zone> [slot] — Ütemezés törlése\n";
    h += "  pl: /clear 1 (összes slot)\n";
    h += "  pl: /clear 1 2 (csak slot 2)\n";
    h += "/upgrade — Firmware frissítés\n";
    h += "/reboot — Újraindítás\n";
    h += "/flash — FLASH gomb debug\n";
    h += "/wake — OLED ébresztése\n";
    h += "/weather — Időjárás info\n";
  }
  return h;
}

void handleCommand(UniversalTelegramBot &bot, String text, String chatId, bool isAdmin) {
  Serial.println("CMD: [" + chatId + "] " + text);
  text.trim();
  text.toLowerCase();
  
  wakeOled(); // Parancs érkezett — OLED ébresztés
  
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
    String s = "⏰ *Ütemezés (4 zóna, max 4/zóna)*\n\n";
    for (int i = 0; i < NUM_ZONES; i++) {
      s += "*Zone " + String(i + 1) + ":*\n";
      int cnt = countSchedules(i);
      if (cnt > 0) {
        for (int sc = 0; sc < MAX_SCHEDULES; sc++) {
          if (!zones[i].schedules[sc].enabled) continue;
          s += "  " + String(sc + 1) + ": " + 
               String(zones[i].schedules[sc].hour) + ":" + 
               (zones[i].schedules[sc].min < 10 ? "0" : "") + String(zones[i].schedules[sc].min) + 
               " (" + String(zones[i].schedules[sc].duration) + "p)\n";
        }
      } else {
        s += "  nincs\n";
      }
    }
    bot.sendMessage(chatId, s, "Markdown");
    return;
  }
  
  if (!isAdmin) {
    bot.sendMessage(chatId, "❌ Ezt a parancsot csak admin használhatja", "");
    return;
  }
  
  // /set <zone> <slot> <ora> <perc> <dur>
  if (text.startsWith("/set")) {
    // 5 paraméter: zone slot hour min dur
    int sp[5];
    sp[0] = text.indexOf(' ');                    // after /set
    sp[1] = text.indexOf(' ', sp[0] + 1);         // after zone
    sp[2] = text.indexOf(' ', sp[1] + 1);         // after slot
    sp[3] = text.indexOf(' ', sp[2] + 1);         // after hour
    sp[4] = text.indexOf(' ', sp[3] + 1);         // after min
    
    if (sp[0] > 0 && sp[1] > 0 && sp[2] > 0 && sp[3] > 0 && sp[4] > 0) {
      int z  = text.substring(sp[0] + 1, sp[1]).toInt() - 1;    // zone 0-3
      int sl = text.substring(sp[1] + 1, sp[2]).toInt() - 1;    // slot 0-3
      int h  = text.substring(sp[2] + 1, sp[3]).toInt();        // hour
      int m  = text.substring(sp[3] + 1, sp[4]).toInt();        // min
      int d  = text.substring(sp[4] + 1).toInt();               // duration
      
      if (z >= 0 && z < NUM_ZONES && sl >= 0 && sl < MAX_SCHEDULES && 
          h >= 0 && h <= 23 && m >= 0 && m <= 59 && d > 0) {
        zones[z].schedules[sl].enabled  = true;
        zones[z].schedules[sl].hour     = h;
        zones[z].schedules[sl].min      = m;
        zones[z].schedules[sl].duration = d;
        saveSchedule();
        String msg = "✅ Zone " + String(z + 1) + " slot " + String(sl + 1) + 
                     " ütemezve: " + String(h) + ":" + (m < 10 ? "0" : "") + String(m) + 
                     " (" + String(d) + " perc)";
        sendTelegram(msg);
        updateBlynkStatus();
        return;
      }
    }
    bot.sendMessage(chatId, "Használat: /set <zone> <slot> <ora> <perc> <dur>\n"
                            "pl: /set 1 1 06 30 10\n"
                            "zone: 1-4, slot: 1-4", "");
    return;
  }
  
  // /clear <zone> [slot]
  if (text.startsWith("/clear")) {
    int sp1 = text.indexOf(' ');
    if (sp1 > 0) {
      int sp2 = text.indexOf(' ', sp1 + 1);
      int z = text.substring(sp1 + 1, sp2 > 0 ? sp2 : text.length()).toInt() - 1;
      if (z >= 0 && z < NUM_ZONES) {
        if (sp2 > 0) {
          // Specific slot
          int sl = text.substring(sp2 + 1).toInt() - 1;
          if (sl >= 0 && sl < MAX_SCHEDULES) {
            zones[z].schedules[sl].enabled = false;
            saveSchedule();
            String msg = "🗑️ Zone " + String(z + 1) + " slot " + String(sl + 1) + " törölve";
            sendTelegram(msg);
            updateBlynkStatus();
            return;
          }
        } else {
          // All slots for this zone
          for (int s = 0; s < MAX_SCHEDULES; s++) {
            zones[z].schedules[s].enabled = false;
          }
          saveSchedule();
          String msg = "🗑️ Zone " + String(z + 1) + " összes ütemezés törölve";
          sendTelegram(msg);
          updateBlynkStatus();
          return;
        }
      }
    }
    bot.sendMessage(chatId, "Használat: /clear <zone> [slot]\n"
                            "pl: /clear 1 (összes)\n"
                            "pl: /clear 1 2 (csak slot 2)\n"
                            "zone: 1-4, slot: 1-4", "");
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
  
  // Debug: FLASH gomb pin állapot
  if (text == "/flash" && isAdmin) {
    int pinState = digitalRead(FLASH_BTN_PIN);
    String msg = "🔍 FLASH gomb debug:\n";
    msg += "Pin (D3/GPIO0): " + String(pinState) + "\n";
    msg += "OLED sleeping: " + String(oledSleeping ? "IGEN" : "NEM") + "\n";
    msg += "OLED present: " + String(oledPresent ? "IGEN" : "NEM") + "\n";
    msg += "Inaktivitás: " + String((millis() - lastActivity) / 1000) + " mp\n";
    msg += "Sleep timeout: " + String(OLED_SLEEP_MS / 1000) + " mp\n";
    if (pinState == LOW) {
      msg += "\n⚠️ Pin LOW — gomb lenyomva vagy lehúzva!";
    } else {
      msg += "\n✅ Pin HIGH — gomb felengedve, pull-up OK";
    }
    bot.sendMessage(chatId, msg, "");
    return;
  }
  
  // Debug: OLED wake manuális
  if (text == "/wake" && isAdmin) {
    wakeOled();
    bot.sendMessage(chatId, "💡 OLED ébresztve! Sleeping: " + String(oledSleeping ? "IGEN" : "NEM"), "");
    return;
  }
  
  // Időjárás info
  if (text == "/weather" && isAdmin) {
    // Friss adatok lekérése
    checkWeather();
    if (!weatherChecked) {
      bot.sendMessage(chatId, "❌ Időjárás lekérés sikertelen", "");
      return;
    }
    String msg = "🌤 *Időjárás (Törökbálint)*\n";
    msg += "Eső ma: " + String(todayRainMm, 1) + " mm\n";
    msg += "Eső valószínű: " + String(todayRainProb) + "%\n";
    msg += "Státusz: " + String(isRainyDay ? "🌧 ESŐS nap" : "☀️ Száraz nap") + "\n\n";
    msg += "Locsolás: " + String(isRainyDay ? "kérdezni fog" : "automatikus");
    bot.sendMessage(chatId, msg, "Markdown");
    return;
  }
  
  bot.sendMessage(chatId, "Ismeretlen parancs. /help", "");
}

void handleBotUpdates(UniversalTelegramBot &bot, WiFiClientSecure &client, bool isAdmin) {
  int numNew = bot.getUpdates(bot.last_message_received + 1);
  if (numNew < 0) {
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
      String messageType = bot.messages[i].type;
      
      // Callback query (inline gomb válasz)
      if (messageType == "callback_query") {
        String callbackData = bot.messages[i].text;
        String messageId = String(bot.messages[i].message_id);
        Serial.println("Callback: " + callbackData);
        handleCallbackQuery(callbackData, chatId, messageId);
      } else {
        // Normál üzenet
        handleCommand(bot, text, chatId, isAdmin);
      }
    }
    ESP.wdtFeed();
    numNew = bot.getUpdates(bot.last_message_received + 1);
  }
}

// ==================== BLYNK HANDLERS ====================

BLYNK_WRITE(VPIN_ZONE1) {
  wakeOled();
  int state = param.asInt();
  if (state) startZone(0, 10);
  else stopZone(0);
}

BLYNK_WRITE(VPIN_ZONE2) {
  wakeOled();
  int state = param.asInt();
  if (state) startZone(1, 10);
  else stopZone(1);
}

BLYNK_WRITE(VPIN_ZONE3) {
  wakeOled();
  int state = param.asInt();
  if (state) startZone(2, 10);
  else stopZone(2);
}

BLYNK_WRITE(VPIN_ZONE4) {
  wakeOled();
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
  pinMode(RELAY3_PIN, OUTPUT);  // D0/GPIO16 (áthelyezve D3-ról)
  pinMode(RELAY4_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, RELAY_OFF);
  digitalWrite(RELAY2_PIN, RELAY_OFF);
  digitalWrite(RELAY3_PIN, RELAY_OFF);
  digitalWrite(RELAY4_PIN, RELAY_OFF);
  
  // FLASH gomb (GPIO0/D3) — input, beépített pull-up + interrupt
  pinMode(FLASH_BTN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLASH_BTN_PIN), flashBtnISR, FALLING);
  
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
    display.println("v1.2.0 Booting...");
    display.display();
    Serial.println("OLED OK");
    lastActivity = millis();  // OLED ébren boot után
  } else {
    oledPresent = false;
    Serial.println("OLED nem talalhato! (0x3C/0x3D)");
  }
  
  // DHT11 inicializálás
  dht.begin();
  Serial.println("DHT11 init (GPIO13/D7)");
  
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
  }
  
  // TLS
  securedClient.setInsecure();
  securedClient2.setInsecure();
  
  // Blynk
  Blynk.config(BLYNK_AUTH_TOKEN);
  
  // NTP
  syncNTP();
  
  // Blynk timer-ek
  blynkTimer.setInterval(5000L, []() {
    updateBlynkStatus();
  });
  
  // Boot üzenet
  if (wifiConnected) {
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
    
    // Telegram parancs menü regisztrálása (/ gomb → autocomplete)
    String cmds = "[";
    cmds += "{\"command\":\"help\",\"description\":\"Parancsok listája\"},";
    cmds += "{\"command\":\"status\",\"description\":\"Állapot lekérdezése\"},";
    cmds += "{\"command\":\"schedule\",\"description\":\"Ütemezés lekérdezése\"},";
    cmds += "{\"command\":\"zone1\",\"description\":\"Zone 1 indítása (perc)\"},";
    cmds += "{\"command\":\"zone2\",\"description\":\"Zone 2 indítása (perc)\"},";
    cmds += "{\"command\":\"zone3\",\"description\":\"Zone 3 indítása (perc)\"},";
    cmds += "{\"command\":\"zone4\",\"description\":\"Zone 4 indítása (perc)\"},";
    cmds += "{\"command\":\"stop\",\"description\":\"Összes leállítása\"},";
    cmds += "{\"command\":\"set\",\"description\":\"Ütemezés (admin)\"},";
    cmds += "{\"command\":\"clear\",\"description\":\"Ütemezés törlése (admin)\"},";
    cmds += "{\"command\":\"weather\",\"description\":\"Időjárás info (admin)\"},";
    cmds += "{\"command\":\"upgrade\",\"description\":\"Firmware frissítés (admin)\"},";
    cmds += "{\"command\":\"reboot\",\"description\":\"Újraindítás (admin)\"},";
    cmds += "{\"command\":\"flash\",\"description\":\"FLASH gomb debug (admin)\"},";
    cmds += "{\"command\":\"wake\",\"description\":\"OLED ébresztése (admin)\"}";
    cmds += "]";
    botAdmin.setMyCommands(cmds);
    Serial.println("Telegram parancs menü regisztrálva (admin)");
  }
  
  // Első OLED frissítés
  updateOled();
  
  // Első időjárás lekérés
  if (wifiConnected) {
    checkWeather();
  }
  
  Serial.println("Setup kész! (v1.2.0)");
}

void loop() {
  ESP.wdtFeed();
  
  // Blynk futás
  if (wifiConnected) {
    Blynk.run();
  }
  blynkTimer.run();
  
  // FLASH gomb ellenőrzés (OLED ébresztés)
  checkFlashButton();
  
  // OLED sleep ellenőrzés
  checkOledSleep();
  
  // Telegram bot poll (1 másodpercenként)
  unsigned long now = millis();
  if (now - lastBotPoll > 1000) {
    lastBotPoll = now;
    if (wifiConnected) {
      handleBotUpdates(botAdmin, securedClient, true);
      handleBotUpdates(botFamily, securedClient2, true);
    }
  }
  
  // Futó zónák ellenőrzése
  checkRunningZones();
  
  // Ütemezés ellenőrzése (1 másodpercenként)
  if (now - lastScheduleCheck > 1000) {
    lastScheduleCheck = now;
    checkSchedule();
  }
  
  // NTP újracsatlakozás (1 óránként, csendesen)
  if (now - lastNtpSync > 3600000) {
    lastNtpSync = now;
    if (wifiConnected) syncNTP(true);
  }

  // DHT11 olvasás (30 másodpercenként)
  if (now - lastDhtRead > 30000) {
    lastDhtRead = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      dhtTemp = t;
      dhtHum = h;
      Serial.printf("DHT: %.1f°C, %.0f%%\n", t, h);
    } else {
      Serial.println("DHT olvasás sikertelen");
    }
  }
  
  // Firmware check (6 óránként)
  if (now - lastFirmwareCheck > 21600000) {
    lastFirmwareCheck = now;
    if (wifiConnected) checkFirmware();
  }
  
  // Időjárás ellenőrzés (30 percenként)
  if (now - lastWeatherCheck > WEATHER_CHECK_MS) {
    lastWeatherCheck = now;
    checkWeather();
  }
  
  // Pending water kérdés timeout (10 perc → auto öntöz)
  checkPendingWaterTimeout();
  
  // OLED frissítés (1 másodpercenként) — csak ha nem alszik
  if (oledPresent && !oledSleeping && now - lastOledUpdate > 1000) {
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