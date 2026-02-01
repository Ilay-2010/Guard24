#include <ld2410.h>
#include <WiFi.h>
#include "time.h"
#include "LittleFS.h"

// ============================================================
//                 Guard 24 by Ilay Schoenknecht
// ============================================================


// Send "flash" in the console to read the acivation history.
// Send "del" in the console to erase the activation history
// The serial speed is 115200


// ============================================================
//                          SETTINGS
// ============================================================

const char* WIFI_SSID     = "Your_WIFI_SSID";
const char* WIFI_PASSWORD = "Your_WIFI_Password";


const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 3600;  // Timezone offset (seconds)
const int DST_OFFSET_SEC = 3600;   // Daylight saving (seconds)
const int NIGHT_START_H = 22;      // Start of inactive period (hour)
const int NIGHT_END_H = 7;         // End of inactive period (hour)

const int MAX_DIST_CM = 420;     // Detection distance limit
const int SENSE_THRESHOLD = 40;  // Standard sensitivity (Lower = more sensitive)

const int GRACE_PERIOD_MS = 10000;  // Activation delay
const int ALARM_TRIGGER_MS = 7000;  // Presence time before alarm

#define RADAR_RX_PIN 27
#define RADAR_TX_PIN 26
#define PIEZO_PIN 16
#define BUTTON_PIN 17

// ============================================================
// MAIN PROGRAM
// ============================================================

HardwareSerial RadarSerial(2);
ld2410 radar;

uint32_t presenceStartTime = 0;
uint32_t lastButtonPress = 0;
uint32_t systemActivationTime = 0;
uint32_t lastAlarmTone = 0;
int deactivateClickCount = 0;

bool systemActive = true;
bool alarmTriggered = false;
bool loggedWarning = false;
bool loggedAlarm = false;

void syncTime() {
  Serial.print("WiFi connecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      Serial.println(" Time synced.");
    }
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void goToDeepSleep(int currentHour) {
  int hoursToWait;
  if (currentHour >= NIGHT_START_H) {
    hoursToWait = (24 - currentHour) + NIGHT_END_H;
  } else {
    hoursToWait = NIGHT_END_H - currentHour;
  }

  uint64_t sleepTimeUs = (uint64_t)hoursToWait * 3600 * 1000000;
  Serial.print("Night mode. Sleeping for ");
  Serial.print(hoursToWait);
  Serial.println(" hours.");

  Serial.flush();
  esp_sleep_enable_timer_wakeup(sleepTimeUs);
  esp_deep_sleep_start();
}

bool checkNightAndSleep() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;
  if (timeinfo.tm_hour >= NIGHT_START_H || timeinfo.tm_hour < NIGHT_END_H) {
    goToDeepSleep(timeinfo.tm_hour);
    return true;
  }
  return false;
}

void logEvent(char type) {
  File file = LittleFS.open("/alarm_log.txt", FILE_APPEND);
  if (!file) {
    Serial.println("Error: Could not open log for writing");
    return;
  }

  struct tm timeinfo;
  char buf[40];
  if (getLocalTime(&timeinfo)) {
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  } else {
    snprintf(buf, sizeof(buf), "Time-Not-Synced-%lu", millis());
  }

  file.print(type);
  file.print(": ");
  file.println(buf);
  file.close();
  Serial.print("Logged ");
  Serial.print(type);
  Serial.print(": ");
  Serial.println(buf);
}

void dumpLog() {
  Serial.println("\n--- FLASH LOG ---");
  if (!LittleFS.exists("/alarm_log.txt")) {
    Serial.println("No log file found.");
    return;
  }
  File file = LittleFS.open("/alarm_log.txt", FILE_READ);
  if (!file) {
    Serial.println("Failed to open log file.");
    return;
  }
  while (file.available()) {
    Serial.write(file.read());
  }
  file.close();
  Serial.println("--- END ---");
}

void setup() {
  Serial.begin(115200);

  // Mount LittleFS with format-on-fail
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed");
  }

  syncTime();

  if (checkNightAndSleep()) return;

  RadarSerial.begin(256000, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
  pinMode(PIEZO_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  if (radar.begin(RadarSerial)) {
    radar.requestStartEngineeringMode();
    radar.setGateSensitivityThreshold(0, 90, 90);  // Case reflection fix
    radar.setGateSensitivityThreshold(1, 60, 60);  // Case reflection fix
    for (uint8_t i = 2; i <= 4; i++) radar.setGateSensitivityThreshold(i, SENSE_THRESHOLD, SENSE_THRESHOLD);
    for (uint8_t i = 5; i <= 8; i++) radar.setGateSensitivityThreshold(i, 100, 100);
    radar.requestEndEngineeringMode();
    Serial.println("Radar ready.");
  }

  systemActivationTime = millis();
  tone(PIEZO_PIN, 2000, 100);
}

void loop() {
  uint32_t now = millis();

  if (systemActive) {
    radar.read();
  }

  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "flash") dumpLog();
    if (cmd == "del") {
      if (LittleFS.remove("/alarm_log.txt")) {
        Serial.println("Log cleared.");
      } else {
        Serial.println("No log to delete.");
      }
    }
  }

  if (digitalRead(BUTTON_PIN) == LOW && (now - lastButtonPress > 400)) {
    lastButtonPress = now;

    if (!systemActive) {
      systemActive = true;
      deactivateClickCount = 0;
      tone(PIEZO_PIN, 2000, 100);
      systemActivationTime = now;
      radar.requestStartEngineeringMode();
      radar.requestEndEngineeringMode();
      Serial.println("System ON");
    } else {
      deactivateClickCount++;
      if (deactivateClickCount < 3) {
        tone(PIEZO_PIN, 1500, 100);
        Serial.print("Deactivate: ");
        Serial.println(deactivateClickCount);
      } else {
        systemActive = false;
        deactivateClickCount = 0;
        alarmTriggered = false;
        loggedWarning = false;
        loggedAlarm = false;
        presenceStartTime = 0;
        noTone(PIEZO_PIN);
        radar.requestStartEngineeringMode();
        radar.requestEndEngineeringMode();
        tone(PIEZO_PIN, 1000, 100);
        delay(150);
        tone(PIEZO_PIN, 1000, 100);
        Serial.println("System OFF");
      }
    }
  }

  if (!systemActive) return;

  static uint32_t lastNightCheck = 0;
  if (now - lastNightCheck > 60000) {
    lastNightCheck = now;
    if (checkNightAndSleep()) return;
  }

  if (now - systemActivationTime < GRACE_PERIOD_MS) return;

  // Direct Radar Check (Removed interval for instant response)
  if (radar.presenceDetected()) {
    int dist = radar.stationaryTargetDistance();
    if (dist > 0 && dist < MAX_DIST_CM) {
      if (presenceStartTime == 0) {
        presenceStartTime = now;
        Serial.print("Target: ");
        Serial.print(dist);
        Serial.println("cm");

        // Immediate 3 tones
        for (int i = 0; i < 10; i++) {
          tone(PIEZO_PIN, 2000, 100);
          delay(150);
          noTone(PIEZO_PIN);
          delay(50);
        }

        if (!loggedWarning) {
          logEvent('W');
          loggedWarning = true;
        }
      }

      if (now - presenceStartTime > ALARM_TRIGGER_MS) {
        if (!loggedAlarm) {
          logEvent('A');
          loggedAlarm = true;
        }
        alarmTriggered = true;
      }
    }
  } else {
    if (presenceStartTime != 0) {
      presenceStartTime = 0;
      alarmTriggered = false;
      loggedWarning = false;
      loggedAlarm = false;
      noTone(PIEZO_PIN);
    }
  }

  // Non-blocking Alarm Sound
  if (alarmTriggered) {
    if (now - lastAlarmTone > 600) {
      tone(PIEZO_PIN, 1800, 250);
      lastAlarmTone = now;
    } else if (now - lastAlarmTone > 300 && now - lastAlarmTone < 350) {
      tone(PIEZO_PIN, 1200, 250);
    }
  }
}
