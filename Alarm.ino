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

const char* WIFI_SSID     = "TTDSISMS";
const char* WIFI_PASSWORD = "sagichdirnicht";

const char* NTP_SERVER      = "pool.ntp.org";
const long  GMT_OFFSET_SEC  = 3600;  // Timezone offset (seconds)
const int   DST_OFFSET_SEC  = 3600;  // Daylight saving (seconds)
const int   NIGHT_START_H   = 22;    // Start of inactive period (hour)
const int   NIGHT_END_H     = 7;     // End of inactive period (hour)

const int   MAX_DIST_CM     = 380;   // Detection distance limit
const int   SENSE_THRESHOLD = 40;    // Standard sensitivity (Lower = more sensitive)

const int   RADAR_INTERVAL_MS = 8000;  // Delay between scans
const int   GRACE_PERIOD_MS   = 10000; // Activation delay
const int   ALARM_TRIGGER_MS  = 10000; // Presence time before alarm

#define RADAR_RX_PIN 27
#define RADAR_TX_PIN 26
#define PIEZO_PIN    16
#define BUTTON_PIN   17 

// ============================================================
//                          MAIN PROGRAM
// ============================================================

HardwareSerial RadarSerial(2);
ld2410 radar;

uint32_t lastRadarCheck = 0;
uint32_t presenceStartTime = 0;
uint32_t lastButtonPress = 0;
uint32_t systemActivationTime = 0;
int deactivateClickCount = 0;

bool systemActive = true;
bool alarmTriggered = false;
bool loggedThisAlarm = false;

void syncTime() {
    Serial.print("WiFi connecting...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500); Serial.print(".");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
        struct tm timeinfo;
        getLocalTime(&timeinfo);
        Serial.println(" Time synced.");
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

bool isNightTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return false;
    return (timeinfo.tm_hour >= NIGHT_START_H || timeinfo.tm_hour < NIGHT_END_H);
}

void logAlarm() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;
    File file = LittleFS.open("/alarm_log.txt", FILE_APPEND);
    if (!file) return;
    char buf[30];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    file.println(buf);
    file.close();
    Serial.print("Alarm logged: "); Serial.println(buf);
}

void dumpLog() {
    Serial.println("\n--- FLASH LOG ---");
    File file = LittleFS.open("/alarm_log.txt", FILE_READ);
    if (!file) return;
    while (file.available()) Serial.write(file.read());
    file.close();
    Serial.println("--- END ---");
}

void setup() {
    Serial.begin(115200);
    LittleFS.begin(true);
    syncTime();

    RadarSerial.begin(256000, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
    pinMode(PIEZO_PIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    if (radar.begin(RadarSerial)) {
        radar.requestStartEngineeringMode();
        radar.setGateSensitivityThreshold(0, 90, 90); // Case reflection fix
        radar.setGateSensitivityThreshold(1, 60, 60); // Case reflection fix
        for (uint8_t i = 2; i <= 4; i++) radar.setGateSensitivityThreshold(i, SENSE_THRESHOLD, SENSE_THRESHOLD);
        for (uint8_t i = 5; i <= 8; i++) radar.setGateSensitivityThreshold(i, 100, 100);
        radar.requestEndEngineeringMode();
        Serial.println("Radar ready.");
    }
    
    systemActivationTime = millis();
    tone(PIEZO_PIN, 2000, 100); 
}

void loop() {
    radar.read();
    uint32_t now = millis();

    if (Serial.available() > 0) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        if (cmd == "flash") dumpLog();
        if (cmd == "del") { LittleFS.remove("/alarm_log.txt"); Serial.println("Log cleared."); }
    }

    if (digitalRead(BUTTON_PIN) == LOW && (now - lastButtonPress > 400)) {
        lastButtonPress = now;
        
        if (!systemActive) {
            systemActive = true;
            deactivateClickCount = 0;
            tone(PIEZO_PIN, 2000, 100);
            systemActivationTime = now;
            Serial.println("System ON");
        } 
        else {
            deactivateClickCount++;
            if (deactivateClickCount < 3) {
                tone(PIEZO_PIN, 1500, 100);
                Serial.print("Deactivate: "); Serial.println(deactivateClickCount);
            } else {
                systemActive = false;
                deactivateClickCount = 0;
                alarmTriggered = false;
                noTone(PIEZO_PIN);
                tone(PIEZO_PIN, 1000, 100); delay(150);
                tone(PIEZO_PIN, 1000, 100);
                Serial.println("System OFF");
            }
        }
    }

    if (!systemActive || isNightTime()) return;
    if (now - systemActivationTime < GRACE_PERIOD_MS) return;

    if (now - lastRadarCheck > RADAR_INTERVAL_MS) {
        lastRadarCheck = now;
        if (radar.presenceDetected()) {
            int dist = radar.stationaryTargetDistance();
            if (dist > 0 && dist < MAX_DIST_CM) {
                if (presenceStartTime == 0) {
                    presenceStartTime = now;
                    Serial.print("Target: "); Serial.print(dist); Serial.println("cm");
                    for(int i=0; i<3; i++) { tone(PIEZO_PIN, 2000, 100); delay(500); }
                }
                if (now - presenceStartTime > ALARM_TRIGGER_MS) {
                    if (!loggedThisAlarm) { logAlarm(); loggedThisAlarm = true; }
                    alarmTriggered = true;
                }
            }
        } else {
            presenceStartTime = 0;
            alarmTriggered = false;
            loggedThisAlarm = false;
            noTone(PIEZO_PIN);
        }
    }

    if (alarmTriggered) {
        tone(PIEZO_PIN, 1500); delay(300);
        tone(PIEZO_PIN, 1000); delay(300);
    }
}