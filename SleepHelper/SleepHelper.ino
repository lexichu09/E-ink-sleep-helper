/*
  SleepHelper.ino
  ---------------
  Reads lux (VEML7700), temperature & humidity (SHT45), syncs time via NTP,
  calls Groq AI for a personalized sleep tip, calculates a daily sleep
  environment score, and shows everything on an e-ink display.

  Updates 4 times a day: 7 AM, 8 PM, 10 PM, 11:59 PM.
  Between updates the ESP32-S3 is in deep sleep to save power.
  The e-ink display holds its image without power during sleep.

  Hardware assumed:
    - Adafruit Feather ESP32-S3 (or any ESP32 Feather with WiFi)
    - Adafruit 2.13" Mono eInk FeatherWing — GDEY0213B74 (250 x 122)
    - VEML7700 lux sensor on I2C
    - SHT45 temp/humidity sensor on I2C

  Required libraries (install via Arduino Library Manager):
    - Adafruit VEML7700
    - Adafruit SHT4x
    - Adafruit Unified Sensor
    - Adafruit ThinkInk
    - Adafruit GFX
    - ArduinoJson  (≥ v6)
    - WiFi         (built-in for ESP32)
    - HTTPClient   (built-in for ESP32)
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "Adafruit_VEML7700.h"
#include "Adafruit_SHT4x.h"
#include "Adafruit_ThinkInk.h"

#define uS_TO_S_FACTOR 1000000ULL  // microseconds → seconds for sleep timer

// ── USER CONFIG ──────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_SSID";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";
const char* GROQ_API_KEY  = "YOUR_GROQ_API_KEY";

// UTC offset in seconds: EST=-18000, CST=-21600, MST=-25200, PST=-28800
const long  GMT_OFFSET_SEC = -18000;
const int   DST_OFFSET_SEC = 3600;  // set to 0 if your region skips daylight saving

// ── SCHEDULED WAKE TIMES ─────────────────────────────────────────────────────
// Minutes from midnight: 7:00, 20:00, 22:00, 23:59
const int WAKE_TIMES[]  = { 7*60, 20*60, 22*60, 23*60 + 59 };
const int WAKE_COUNT    = sizeof(WAKE_TIMES) / sizeof(WAKE_TIMES[0]);

// ── E-INK PINS (Adafruit 2.13" ThinkInk FeatherWing GDEY0213B74) ─────────────
#define EPD_DC    10
#define EPD_CS     9
#define EPD_BUSY  -1
#define SRAM_CS    6
#define EPD_RESET -1
#define EPD_SPI  &SPI

// ── SLEEP ENVIRONMENT THRESHOLDS ─────────────────────────────────────────────
const float IDEAL_TEMP_LOW_C  = 18.0;  // °C  (~64°F)
const float IDEAL_TEMP_HIGH_C = 20.0;  // °C  (~68°F)
const float IDEAL_HUM_LOW     = 40.0;  // %RH
const float IDEAL_HUM_HIGH    = 60.0;  // %RH
const float MAX_NIGHT_LUX     = 10.0;  // lux — above this at night hurts score
const int   NIGHT_START_HOUR  = 21;    // 9 PM
const int   NIGHT_END_HOUR    = 7;     // 7 AM

// ── RTC MEMORY — survives deep sleep ─────────────────────────────────────────
RTC_DATA_ATTR float dailyScoreSum  = 0;
RTC_DATA_ATTR int   scoreReadings  = 0;
RTC_DATA_ATTR float lastDailyScore = -1;
RTC_DATA_ATTR int   lastDayLogged  = -1;

// ── OBJECTS ──────────────────────────────────────────────────────────────────
Adafruit_VEML7700 veml;
Adafruit_SHT4x    sht4x;

ThinkInk_213_Mono_GDEY0213B74 display(EPD_DC, EPD_RESET, EPD_CS, SRAM_CS, EPD_BUSY, EPD_SPI);

// ── PROTOTYPES ───────────────────────────────────────────────────────────────
void   connectWiFi();
void   syncTime();
bool   getLocalTimeInfo(struct tm &t);
long   secondsUntilNextWake(const struct tm &t);
void   goToSleep(long seconds);
float  computeScore(float tempC, float humidity, float lux, int hour);
String fetchGroqTip(float tempC, float humidity, float lux,
                    int hour, int minute, float score);
void   renderDisplay(float tempC, float humidity, float lux,
                     int hour, int minute,
                     float currentScore, float dayScore,
                     const String &tip);
void   printWordWrapped(const String &text, int x, int startY,
                        int maxCharsPerLine, int lineHeight, int maxY);

// ─────────────────────────────────────────────────────────────────────────────
// Deep sleep restarts from setup() every time — loop() is never reached.
void setup() {
  Serial.begin(115200);

  // Disable NeoPixel power (saves quiescent current during active time too)
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, LOW);

  // Enable I2C / STEMMA QT power for sensors
  pinMode(I2C_POWER, OUTPUT);
  digitalWrite(I2C_POWER, HIGH);
  delay(10);  // let the rail stabilize before talking to sensors

  // ── Display ───────────────────────────────────────────────────────────────
  display.begin(THINKINK_MONO);

  // Only show splash screen on first power-on, not on timer wake
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
    display.clearBuffer();
    display.setTextColor(EPD_BLACK);
    display.setTextSize(2);
    display.setCursor(10, 40);
    display.print("Sleep Helper");
    display.setTextSize(1);
    display.setCursor(10, 70);
    display.print("Starting...");
    display.display();
  }

  // ── Sensors ───────────────────────────────────────────────────────────────
  if (!veml.begin()) {
    Serial.println("VEML7700 not found — check I2C wiring");
    while (1) delay(100);
  }
  if (!sht4x.begin()) {
    Serial.println("SHT45 not found — check I2C wiring");
    while (1) delay(100);
  }
  sht4x.setPrecision(SHT4X_HIGH_PRECISION);

  // ── Network ───────────────────────────────────────────────────────────────
  connectWiFi();
  syncTime();

  // ── Get time — if unavailable, sleep 1 hour and retry ────────────────────
  struct tm timeInfo;
  if (!getLocalTimeInfo(timeInfo)) {
    Serial.println("No time available — sleeping 1 hour to retry");
    goToSleep(3600);
  }

  int hour   = timeInfo.tm_hour;
  int minute = timeInfo.tm_min;
  int day    = timeInfo.tm_yday;

  // ── Read sensors ──────────────────────────────────────────────────────────
  float lux = veml.readLux();

  sensors_event_t humEvent, tempEvent;
  sht4x.getEvent(&humEvent, &tempEvent);
  float tempC    = tempEvent.temperature;
  float humidity = humEvent.relative_humidity;

  if (isnan(tempC) || isnan(humidity)) {
    Serial.println("SHT45 read error — sleeping until next scheduled time");
    goToSleep(secondsUntilNextWake(timeInfo));
  }

  // ── Score ─────────────────────────────────────────────────────────────────
  float currentScore = computeScore(tempC, humidity, lux, hour);

  // Accumulate daily score in RTC memory; finalise at midnight
  dailyScoreSum += currentScore;
  scoreReadings++;
  if (hour == 0 && day != lastDayLogged) {
    lastDailyScore = dailyScoreSum / scoreReadings;
    dailyScoreSum  = 0;
    scoreReadings  = 0;
    lastDayLogged  = day;
    Serial.printf("Daily score finalised: %.0f\n", lastDailyScore);
  }
  float displayedDayScore = (lastDailyScore >= 0)
                            ? lastDailyScore
                            : (scoreReadings > 0 ? dailyScoreSum / scoreReadings : 0);

  // ── AI tip ────────────────────────────────────────────────────────────────
  String tip = fetchGroqTip(tempC, humidity, lux, hour, minute, currentScore);

  // ── Update display ────────────────────────────────────────────────────────
  renderDisplay(tempC, humidity, lux, hour, minute,
                currentScore, displayedDayScore, tip);

  // ── Serial log ────────────────────────────────────────────────────────────
  Serial.printf("[%02d:%02d] Lux=%.1f  Temp=%.1fC  Hum=%.0f%%  "
                "Score=%.0f  DayScore=%.0f\n",
                hour, minute, lux, tempC, humidity,
                currentScore, displayedDayScore);
  Serial.println("Tip: " + tip);

  // ── Sleep until next scheduled time ──────────────────────────────────────
  goToSleep(secondsUntilNextWake(timeInfo));
}

void loop() {
  // Never reached — deep sleep in setup() restarts the program each wake
}

// ─────────────────────────────────────────────────────────────────────────────
// Returns seconds until the next scheduled wake time.
// Any target within 2 minutes is treated as already served (push to tomorrow).
long secondsUntilNextWake(const struct tm &t) {
  int nowMinutes = t.tm_hour * 60 + t.tm_min;

  long best = -1;
  for (int i = 0; i < WAKE_COUNT; i++) {
    long diff = (long)WAKE_TIMES[i] - nowMinutes;
    if (diff < 2) diff += 1440;  // already past (or just triggered) → tomorrow
    if (best < 0 || diff < best) best = diff;
  }
  // Subtract seconds already elapsed in the current minute for precision
  return best * 60L - t.tm_sec;
}

// ─────────────────────────────────────────────────────────────────────────────
void goToSleep(long seconds) {
  Serial.printf("Sleeping for %lds (%.1f hrs)\n", seconds, seconds / 3600.0f);
  Serial.flush();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  digitalWrite(I2C_POWER, LOW);  // cut STEMMA QT power during sleep

  esp_sleep_enable_timer_wakeup((uint64_t)seconds * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

// ─────────────────────────────────────────────────────────────────────────────
void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi failed — running offline (no time or AI tips)");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
void syncTime() {
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  struct tm t;
  int tries = 0;
  while (!getLocalTime(&t) && tries < 20) {
    delay(500);
    tries++;
  }
  Serial.println(tries < 20 ? "Time synced via NTP" : "NTP sync failed");
}

bool getLocalTimeInfo(struct tm &t) {
  return getLocalTime(&t);
}

// ─────────────────────────────────────────────────────────────────────────────
// Score 0–100: higher is a better sleep environment.
float computeScore(float tempC, float humidity, float lux, int hour) {
  float score = 100.0f;

  // Temperature: up to -30 pts, 5 pts/°C outside ideal band
  if (tempC < IDEAL_TEMP_LOW_C)
    score -= min(30.0f, (IDEAL_TEMP_LOW_C - tempC) * 5.0f);
  else if (tempC > IDEAL_TEMP_HIGH_C)
    score -= min(30.0f, (tempC - IDEAL_TEMP_HIGH_C) * 5.0f);

  // Humidity: up to -20 pts, 0.4 pts/%RH outside ideal band
  if (humidity < IDEAL_HUM_LOW)
    score -= min(20.0f, (IDEAL_HUM_LOW - humidity) * 0.4f);
  else if (humidity > IDEAL_HUM_HIGH)
    score -= min(20.0f, (humidity - IDEAL_HUM_HIGH) * 0.4f);

  // Light: only penalise during the sleep window (night hours)
  bool isNight = (hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR);
  if (isNight && lux > MAX_NIGHT_LUX)
    score -= min(30.0f, (lux - MAX_NIGHT_LUX) * 0.5f);

  return max(0.0f, score);
}

// ─────────────────────────────────────────────────────────────────────────────
String fetchGroqTip(float tempC, float humidity, float lux,
                    int hour, int minute, float score) {
  if (WiFi.status() != WL_CONNECTED) return "Connect WiFi for AI tips.";

  char userPrompt[512];
  snprintf(userPrompt, sizeof(userPrompt),
    "You are a concise sleep environment coach. "
    "Sensor data: temperature %.1f degrees C, humidity %.0f%%, light %.1f lux. "
    "Time: %02d:%02d. Sleep environment score: %.0f/100. "
    "Give ONE actionable tip in 20 words or fewer to improve sleep quality.",
    tempC, humidity, lux, hour, minute, score);

  StaticJsonDocument<1024> reqDoc;
  reqDoc["model"]       = "llama-3.1-8b-instant";
  reqDoc["max_tokens"]  = 60;
  reqDoc["temperature"] = 0.6;
  JsonArray messages = reqDoc.createNestedArray("messages");
  JsonObject msg     = messages.createNestedObject();
  msg["role"]        = "user";
  msg["content"]     = userPrompt;

  String requestBody;
  serializeJson(reqDoc, requestBody);

  HTTPClient http;
  http.begin("https://api.groq.com/openai/v1/chat/completions");
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("Authorization", String("Bearer ") + GROQ_API_KEY);
  http.setTimeout(12000);

  String tip      = "No tip available.";
  int    httpCode = http.POST(requestBody);

  if (httpCode == 200) {
    String responseBody = http.getString();
    StaticJsonDocument<2048> respDoc;
    if (!deserializeJson(respDoc, responseBody)) {
      const char* content = respDoc["choices"][0]["message"]["content"];
      if (content) {
        tip = String(content);
        tip.trim();
      }
    }
  } else {
    Serial.printf("Groq API error: HTTP %d\n", httpCode);
    Serial.println(http.getString());
    bool isNight = (hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR);
    if      (isNight && lux > 50)          tip = "Dim or turn off nearby lights.";
    else if (tempC > IDEAL_TEMP_HIGH_C)    tip = "Cool the room for better sleep.";
    else if (tempC < IDEAL_TEMP_LOW_C)     tip = "Add a blanket — room is too cold.";
    else if (humidity > IDEAL_HUM_HIGH)    tip = "Run a dehumidifier tonight.";
    else if (humidity < IDEAL_HUM_LOW)     tip = "Use a humidifier — air is dry.";
    else                                   tip = "Environment looks great. Sleep well!";
  }

  http.end();
  return tip;
}

// ─────────────────────────────────────────────────────────────────────────────
// Display is 250 wide x 122 tall. Text size 1 = 6x8 px per char (~41 chars/line).
void renderDisplay(float tempC, float humidity, float lux,
                   int hour, int minute,
                   float currentScore, float dayScore,
                   const String &tip) {
  display.clearBuffer();
  display.setTextColor(EPD_BLACK);
  display.setTextWrap(false);

  // ── Header ────────────────────────────────────────────────────────────────
  display.setTextSize(1);
  display.setCursor(4, 2);
  display.printf("Sleep Helper        %02d:%02d", hour, minute);
  display.drawLine(0, 11, display.width(), 11, EPD_BLACK);

  // ── Sensor readings ───────────────────────────────────────────────────────
  display.setCursor(4, 14);
  display.printf("Temp: %.1fC    Humidity: %.0f%%", tempC, humidity);
  display.setCursor(4, 24);
  display.printf("Light: %.1f lux", lux);
  display.drawLine(0, 33, display.width(), 33, EPD_BLACK);

  // ── Scores ────────────────────────────────────────────────────────────────
  display.setCursor(4, 36);
  display.printf("Now:   %.0f / 100", currentScore);
  display.setCursor(4, 46);
  display.printf("Today: %.0f / 100", dayScore);
  display.drawLine(0, 55, display.width(), 55, EPD_BLACK);

  // ── AI Tip ────────────────────────────────────────────────────────────────
  display.setCursor(4, 58);
  display.print("Tip:");
  // 41 chars/line fits at text size 1 on the 250px wide display
  printWordWrapped(tip, 4, 68, 41, 10, display.height() - 2);

  display.display();
}

// ─────────────────────────────────────────────────────────────────────────────
void printWordWrapped(const String &text, int x, int startY,
                      int maxCharsPerLine, int lineHeight, int maxY) {
  String remaining = text;
  remaining.trim();
  int y = startY;

  while (remaining.length() > 0 && y < maxY) {
    String line;
    if ((int)remaining.length() <= maxCharsPerLine) {
      line      = remaining;
      remaining = "";
    } else {
      int breakAt = maxCharsPerLine;
      int space   = remaining.lastIndexOf(' ', maxCharsPerLine);
      if (space > 0) breakAt = space;
      line      = remaining.substring(0, breakAt);
      remaining = remaining.substring(breakAt + 1);
    }
    display.setCursor(x, y);
    display.print(line);
    y += lineHeight;
  }
}
