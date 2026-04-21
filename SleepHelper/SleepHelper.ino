/*
  SleepHelper.ino
  ---------------
  Reads lux (VEML7700), temperature & humidity (SHT31), syncs time via NTP,
  calls Groq AI for a personalized sleep tip, calculates a daily sleep
  environment score, and shows everything on an e-ink display.

  Hardware assumed:
    - Adafruit Feather ESP32-S3 (or any ESP32 Feather with WiFi)
    - Adafruit 2.13" Monochrome eInk FeatherWing (212 x 104)
      → If you have a 2.9" (296x128) use Adafruit_SSD1680 instead
    - VEML7700 lux sensor on I2C
    - SHT31 temp/humidity sensor on I2C (address 0x44)

  Required libraries (install via Arduino Library Manager):
    - Adafruit VEML7700
    - Adafruit SHT31
    - Adafruit EPD
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
#include "Adafruit_SHT31.h"
#include "Adafruit_EPD.h"
#include <Adafruit_GFX.h>

// ── USER CONFIG ──────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_SSID";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";
const char* GROQ_API_KEY  = "YOUR_GROQ_API_KEY";

// UTC offset in seconds: EST=-18000, CST=-21600, MST=-25200, PST=-28800
const long  GMT_OFFSET_SEC  = -18000;
const int   DST_OFFSET_SEC  = 3600;  // set to 0 if your region skips daylight saving

// How often to refresh the display and request a new AI tip (milliseconds)
const unsigned long UPDATE_INTERVAL_MS = 5UL * 60 * 1000;  // 5 minutes

// ── E-INK PINS ───────────────────────────────────────────────────────────────
// These match the Adafruit 2.13" eInk FeatherWing default wiring.
// Adjust if your board or wing differs.
#define EPD_CS    9
#define EPD_DC    10
#define EPD_RESET 5
#define EPD_BUSY  6

// ── SLEEP ENVIRONMENT THRESHOLDS ─────────────────────────────────────────────
const float IDEAL_TEMP_LOW_C  = 18.0;  // °C  (~64°F)
const float IDEAL_TEMP_HIGH_C = 20.0;  // °C  (~68°F)
const float IDEAL_HUM_LOW     = 40.0;  // %RH
const float IDEAL_HUM_HIGH    = 60.0;  // %RH
const float MAX_NIGHT_LUX     = 10.0;  // lux — above this at night hurts score
const int   NIGHT_START_HOUR  = 21;    // 9 PM
const int   NIGHT_END_HOUR    = 7;     // 7 AM

// ── OBJECTS ──────────────────────────────────────────────────────────────────
Adafruit_VEML7700 veml;
Adafruit_SHT31    sht31;

// For 2.9" 296x128 display swap to: Adafruit_SSD1680 display(296, 128, ...)
// Hardware SPI is used by default — no SPI pins needed in the constructor.
Adafruit_SSD1675 display(212, 104, EPD_DC, EPD_RESET, EPD_CS, EPD_BUSY);

// ── STATE ────────────────────────────────────────────────────────────────────
float         dailyScoreSum  = 0;
int           scoreReadings  = 0;
float         lastDailyScore = -1;
int           lastDayLogged  = -1;
String        lastTip        = "";
unsigned long lastUpdateMs   = 0;

// ── PROTOTYPES ───────────────────────────────────────────────────────────────
void   connectWiFi();
void   syncTime();
bool   getLocalTimeInfo(struct tm &t);
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
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  // Splash on e-ink while everything initialises
  display.begin();
  display.clearBuffer();
  display.setTextColor(EPD_BLACK);
  display.setTextWrap(false);
  display.setTextSize(1);
  display.setCursor(4, 4);
  display.print("Sleep Helper");
  display.setCursor(4, 16);
  display.print("Starting...");
  display.display();

  if (!veml.begin()) {
    Serial.println("VEML7700 not found — check I2C wiring");
    while (1) delay(100);
  }
  Serial.println("VEML7700 OK");

  if (!sht31.begin(0x44)) {
    Serial.println("SHT31 not found — check I2C wiring");
    while (1) delay(100);
  }
  Serial.println("SHT31 OK");

  connectWiFi();
  syncTime();
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();
  if (lastUpdateMs != 0 && now - lastUpdateMs < UPDATE_INTERVAL_MS) return;
  lastUpdateMs = now;

  // --- Read sensors ---
  float lux      = veml.readLux();
  float tempC    = sht31.readTemperature();
  float humidity = sht31.readHumidity();

  if (isnan(tempC) || isnan(humidity)) {
    Serial.println("SHT31 read error — skipping cycle");
    return;
  }

  // --- Get time ---
  struct tm timeInfo;
  if (!getLocalTimeInfo(timeInfo)) {
    Serial.println("Time unavailable — skipping cycle");
    return;
  }
  int hour   = timeInfo.tm_hour;
  int minute = timeInfo.tm_min;
  int day    = timeInfo.tm_yday;

  // --- Current environment score ---
  float currentScore = computeScore(tempC, humidity, lux, hour);

  // --- Accumulate daily score; finalize at midnight ---
  dailyScoreSum += currentScore;
  scoreReadings++;

  if (hour == 0 && day != lastDayLogged) {
    lastDailyScore = dailyScoreSum / scoreReadings;
    dailyScoreSum  = 0;
    scoreReadings  = 0;
    lastDayLogged  = day;
    Serial.printf("Daily score finalised: %.0f\n", lastDailyScore);
  }

  // Show today's running average until yesterday's final score is ready
  float displayedDayScore = (lastDailyScore >= 0)
                            ? lastDailyScore
                            : (scoreReadings > 0 ? dailyScoreSum / scoreReadings : 0);

  // --- AI tip ---
  lastTip = fetchGroqTip(tempC, humidity, lux, hour, minute, currentScore);

  // --- Update display ---
  renderDisplay(tempC, humidity, lux, hour, minute,
                currentScore, displayedDayScore, lastTip);

  // --- Serial log ---
  Serial.printf("[%02d:%02d] Lux=%.1f  Temp=%.1fC  Hum=%.0f%%  "
                "Score=%.0f  DayScore=%.0f\n",
                hour, minute, lux, tempC, humidity,
                currentScore, displayedDayScore);
  Serial.println("Tip: " + lastTip);
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

  // Temperature: ±30 pts, 5 pt/°C outside ideal band
  if (tempC < IDEAL_TEMP_LOW_C)
    score -= min(30.0f, (IDEAL_TEMP_LOW_C - tempC) * 5.0f);
  else if (tempC > IDEAL_TEMP_HIGH_C)
    score -= min(30.0f, (tempC - IDEAL_TEMP_HIGH_C) * 5.0f);

  // Humidity: ±20 pts, 0.4 pt/%RH outside ideal band
  if (humidity < IDEAL_HUM_LOW)
    score -= min(20.0f, (IDEAL_HUM_LOW - humidity) * 0.4f);
  else if (humidity > IDEAL_HUM_HIGH)
    score -= min(20.0f, (humidity - IDEAL_HUM_HIGH) * 0.4f);

  // Light: only penalise during sleep window (night hours)
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
    "Sensor data: temperature %.1f°C, humidity %.0f%%, light level %.1f lux. "
    "Current time: %02d:%02d. Sleep environment score: %.0f out of 100. "
    "Give exactly ONE actionable tip in 20 words or fewer to improve sleep quality.",
    tempC, humidity, lux, hour, minute, score);

  // Build JSON request body
  StaticJsonDocument<768> reqDoc;
  reqDoc["model"]       = "llama3-8b-8192";
  reqDoc["max_tokens"]  = 60;
  reqDoc["temperature"] = 0.6;
  JsonArray messages   = reqDoc.createNestedArray("messages");
  JsonObject msg       = messages.createNestedObject();
  msg["role"]          = "user";
  msg["content"]       = userPrompt;

  String requestBody;
  serializeJson(reqDoc, requestBody);

  HTTPClient http;
  http.begin("https://api.groq.com/openai/v1/chat/completions");
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("Authorization", String("Bearer ") + GROQ_API_KEY);
  http.setTimeout(12000);

  String tip = "No tip available.";
  int httpCode = http.POST(requestBody);

  if (httpCode == 200) {
    String responseBody = http.getString();
    StaticJsonDocument<2048> respDoc;
    DeserializationError err = deserializeJson(respDoc, responseBody);
    if (!err) {
      const char* content = respDoc["choices"][0]["message"]["content"];
      if (content) {
        tip = String(content);
        tip.trim();
      }
    }
  } else {
    Serial.printf("Groq API error: HTTP %d\n", httpCode);
    // Fallback rule-based tip so the display is never empty
    bool isNight = (hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR);
    if (isNight && lux > 50)       tip = "Dim or turn off nearby lights.";
    else if (tempC > IDEAL_TEMP_HIGH_C) tip = "Cool the room for better sleep.";
    else if (tempC < IDEAL_TEMP_LOW_C)  tip = "Add a blanket — room is too cold.";
    else if (humidity > IDEAL_HUM_HIGH) tip = "Run a dehumidifier tonight.";
    else if (humidity < IDEAL_HUM_LOW)  tip = "Use a humidifier — air is dry.";
    else                                tip = "Environment looks good. Sleep well!";
  }

  http.end();
  return tip;
}

// ─────────────────────────────────────────────────────────────────────────────
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
  display.printf("Sleep Helper   %02d:%02d", hour, minute);
  display.drawLine(0, 12, display.width(), 12, EPD_BLACK);

  // ── Sensor readings ───────────────────────────────────────────────────────
  display.setCursor(4, 15);
  display.printf("Temp: %.1fC   Hum: %.0f%%", tempC, humidity);
  display.setCursor(4, 25);
  display.printf("Light: %.1f lux", lux);
  display.drawLine(0, 35, display.width(), 35, EPD_BLACK);

  // ── Scores ────────────────────────────────────────────────────────────────
  display.setCursor(4, 38);
  display.printf("Now:   %.0f / 100", currentScore);
  display.setCursor(4, 48);
  display.printf("Today: %.0f / 100", dayScore);
  display.drawLine(0, 58, display.width(), 58, EPD_BLACK);

  // ── AI Tip ────────────────────────────────────────────────────────────────
  display.setCursor(4, 61);
  display.print("Tip:");
  // 30 chars/line fits comfortably at text size 1 on 212px wide display
  printWordWrapped(tip, 4, 71, 30, 10, display.height() - 2);

  display.display();
}

// Simple word-wrap printer for the e-ink canvas
void printWordWrapped(const String &text, int x, int startY,
                      int maxCharsPerLine, int lineHeight, int maxY) {
  String remaining = text;
  remaining.trim();
  int y = startY;

  while (remaining.length() > 0 && y < maxY) {
    String line;
    if ((int)remaining.length() <= maxCharsPerLine) {
      line = remaining;
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
