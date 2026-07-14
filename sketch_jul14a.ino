#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Adafruit_Fingerprint.h>
#include <esp_task_wdt.h>  // Watchdog

const char* serverUrl   = "https://capstone-project-backend-production.up.railway.app/fingerprints/";
const char* heartbeatUrl = "https://capstone-project-backend-production.up.railway.app/device/heartbeat";

// ── Timing constants (tweak here) ────────────────────────────────────────────
#define MODE_POLL_MS       2000  
#define HEARTBEAT_MS      10000   
#define WIFI_DEAD_RESTART  30000  
#define WDT_TIMEOUT_S         60  

struct WifiNetwork { const char* ssid; const char* password; };
WifiNetwork myNetworks[] = {
   {"PLDTHOMEFIBRdGp8s", "PLDTWIFIZp2Tr"},
    //{"slowifi!",          "Link.18"},
    //{"SPCT WiFi",         ""},
    //{"kupal123",          "kupal123"},
    //{"ASUS_D0_2G_Guest",  ""},
   // {"dd-wrt",            ""},
};

WiFiMulti wifiMulti;
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// ── Single shared secure client — avoids repeated stack allocation ────────────
WiFiClientSecure secureClient;

#define BUZZER_PIN     13
#define GREEN_LED_PIN  32
#define RED_LED_PIN    25

// ── Buzzer PWM (matches the pin/PWM config confirmed working in hardware testing) ──
// core v3.x ledc API is pin-based, no manual channel management needed
#define BUZZER_FREQ    2000
#define BUZZER_RES     8

inline void buzzerOn()  { ledcWriteTone(BUZZER_PIN, BUZZER_FREQ); }
inline void buzzerOff() { ledcWriteTone(BUZZER_PIN, 0); }

// ── LED / Buzzer helpers ──────────────────────────────────────────────────────
void beepSuccess() {
    buzzerOn();
    delay(200);
    buzzerOff();
}

void beepError() {
    for (int i = 0; i < 2; i++) {
        buzzerOn();  delay(150);
        buzzerOff(); delay(150);
    }
}

void ledSuccess()  { digitalWrite(GREEN_LED_PIN, HIGH); delay(1000); digitalWrite(GREEN_LED_PIN, LOW); }
void ledError()    { digitalWrite(RED_LED_PIN,   HIGH); delay(1000); digitalWrite(RED_LED_PIN,   LOW); }

// ── Centralised HTTP GET — reuses the shared client ──────────────────────────
// Returns HTTP status code, fills `responseBody` if provided.
int httpGet(const String& url, String* responseBody = nullptr) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[HTTP] WiFi not connected, skipping: " + url);
        return -1;
    }

    HTTPClient http;
    http.begin(secureClient, url);
    http.setTimeout(8000);                
    http.setReuse(true);                 

    int code = http.GET();
    if (responseBody && code == 200) {
        *responseBody = http.getString();
    }
    if (code < 0) {
        Serial.println("[HTTP] Error on GET " + url + " → " + http.errorToString(code));
    }
    http.end();
    return code;
}

// ── WiFi reconnect with exponential backoff ───────────────────────────────────
static unsigned long wifiLostAt = 0;

void handleWiFiReconnect() {
    static int backoffMs = 1000;

    if (wifiLostAt == 0) wifiLostAt = millis();  

    // Hard restart if WiFi is gone too long
    if (millis() - wifiLostAt > WIFI_DEAD_RESTART) {
        Serial.println("[WiFi] Dead for 30 s — restarting ESP32...");
        ESP.restart();
    }

    Serial.println("[WiFi] Disconnected. Retrying in " + String(backoffMs) + " ms...");
    delay(backoffMs);
    backoffMs = min(backoffMs * 2, 16000);   

    if (wifiMulti.run() == WL_CONNECTED) {
        Serial.println("[WiFi] Reconnected to " + WiFi.SSID());
        backoffMs  = 1000;   // reset backoff
        wifiLostAt = 0;
    }
}

// ── Server helpers ─────────────────────────────────────────────────────────────
void updateStatus(int id, const String& status) {
    String url = String(serverUrl) + "update-enrollment?id=" + id + "&status=" + status;
    int code = httpGet(url);
    Serial.println("[Status] id=" + String(id) + " status=" + status + " → HTTP " + code);
}

void sendHeartbeat() {
    int code = httpGet(heartbeatUrl);
    Serial.println("[Heartbeat] HTTP " + String(code));
}

String getDeviceMode() {
    String body;
    int code = httpGet(String(serverUrl) + "device-mode", &body);
    if (code != 200) return "idle";

    if (body.indexOf("attendance") != -1) return "attendance";
    if (body.indexOf("enroll")     != -1) return "enroll";
    if (body.indexOf("delete")     != -1) return "delete";
    if (body.indexOf("recognize")  != -1) return "recognize";
    return "idle";
}

void markAttendance(int fingerId) {
    String url = String(serverUrl) + "mark-attendance?finger_id=" + fingerId;
    int code = httpGet(url);
    Serial.println("[Attendance] finger=" + String(fingerId) + " → HTTP " + code);
}

// ── Fingerprint enrollment ────────────────────────────────────────────────────
void enrollFingerprint(int id) {
    Serial.println("\n=== ENROLL id=" + String(id) + " ===");
    esp_task_wdt_reset();   

    int p = -1;
    updateStatus(id, "place_finger");

    // Wait for sensor to be clear first
    unsigned long t = millis();
    while (finger.getImage() == FINGERPRINT_OK) {
        if (millis() - t > 20000) { updateStatus(id, "error"); beepError(); ledError(); return; }
        delay(500);
    }
    delay(500);

    // Capture image 1
    t = millis();
    while ((p = finger.getImage()) != FINGERPRINT_OK) {
        if (millis() - t > 30000) { updateStatus(id, "error"); beepError(); ledError(); return; }
        delay(50);
    }
    if (finger.image2Tz(1) != FINGERPRINT_OK) { updateStatus(id, "error"); beepError(); ledError(); return; }

    // Remove finger
    updateStatus(id, "remove_finger");
    t = millis();
    while (finger.getImage() != FINGERPRINT_NOFINGER) {
        if (millis() - t > 10000) { updateStatus(id, "error"); beepError(); ledError(); return; }
        delay(50);
    }
    delay(800);

    // Capture image 2
    updateStatus(id, "place_again");
    t = millis();
    while ((p = finger.getImage()) != FINGERPRINT_OK) {
        if (millis() - t > 30000) { updateStatus(id, "error"); beepError(); ledError(); return; }
        delay(50);
    }
    if (finger.image2Tz(2) != FINGERPRINT_OK) { updateStatus(id, "error"); beepError(); ledError(); return; }

    // Create & store model
    if (finger.createModel() == FINGERPRINT_OK && finger.storeModel(id) == FINGERPRINT_OK) {
        Serial.println("[Enroll] SUCCESS id=" + String(id));
        updateStatus(id, "success"); beepSuccess(); ledSuccess();
    } else {
        Serial.println("[Enroll] FAILED id=" + String(id));
        updateStatus(id, "error"); beepError(); ledError();
    }
}

// ── Delete fingerprint ────────────────────────────────────────────────────────
void deleteFingerprint(int id) {
    Serial.println("\n=== DELETE id=" + String(id) + " ===");
    if (finger.deleteModel(id) == FINGERPRINT_OK) {
        updateStatus(id, "delete_success"); beepSuccess(); ledSuccess();
    } else {
        updateStatus(id, "delete_error"); beepError(); ledError();
    }
}

// ── Attendance scan ───────────────────────────────────────────────────────────
void scanForAttendance() {
    if (finger.getImage() != FINGERPRINT_OK) return;
    if (finger.image2Tz()  != FINGERPRINT_OK) return;

    int p = finger.fingerSearch();
    if (p == FINGERPRINT_OK) {
        Serial.println("[Attendance] Match! id=" + String(finger.fingerID)
                       + " conf=" + finger.confidence);
        ledSuccess(); beepSuccess();
        markAttendance(finger.fingerID);
        delay(500);
        while (finger.getImage() != FINGERPRINT_NOFINGER) delay(50);
        delay(500);
    } else if (p == FINGERPRINT_NOTFOUND) {
        Serial.println("[Attendance] No match.");
        beepError(); ledError();
        while (finger.getImage() != FINGERPRINT_NOFINGER) delay(50);
        delay(300);
    } else {
        Serial.println("[Attendance] Sensor error: " + String(p));
        delay(1000);
    }
}

// ── Recognition test mode ─────────────────────────────────────────────────────
void doRecognition(String& currentMode) {
    Serial.println("[Recognize] Waiting for finger...");
    int p = -1;
    unsigned long t = millis();

    while ((p = finger.getImage()) != FINGERPRINT_OK) {
        if (millis() - t > 15000) {
            httpGet(String(serverUrl) + "recognition-result?finger_id=0&matched=false");
            currentMode = "idle";
            return;
        }
        delay(50);
    }

    if (finger.image2Tz() != FINGERPRINT_OK) { currentMode = "idle"; return; }
    p = finger.fingerSearch();

    String resultUrl;
    if (p == FINGERPRINT_OK) {
        Serial.println("[Recognize] Match id=" + String(finger.fingerID));
        ledSuccess(); beepSuccess();
        resultUrl = String(serverUrl) + "recognition-result?finger_id=" + finger.fingerID + "&matched=true";
    } else {
        Serial.println("[Recognize] No match.");
        ledError(); beepError();
        resultUrl = String(serverUrl) + "recognition-result?finger_id=0&matched=false";
    }
    httpGet(resultUrl);
    currentMode = "idle";
    delay(2000);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(GREEN_LED_PIN, OUTPUT); digitalWrite(GREEN_LED_PIN, LOW);
    pinMode(RED_LED_PIN,   OUTPUT); digitalWrite(RED_LED_PIN,   LOW);

    // Buzzer PWM setup (passive buzzer — driven by tone, not a plain HIGH/LOW)
    // core v3.x ledc API: ledcAttach(pin, freq, resolution) — no channel needed
    ledcAttach(BUZZER_PIN, BUZZER_FREQ, BUZZER_RES);
    buzzerOff();

    // ── WATCHDOG FIX for ESP32 Arduino core v3.x ─────────────────────────────
    // The old 2-argument esp_task_wdt_init(seconds, panic) API was removed.
    // Use a config struct with esp_task_wdt_reconfigure() instead.
    // (The Arduino framework already calls esp_task_wdt_init() internally,
    //  so we just reconfigure the existing watchdog rather than re-init it.)
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms    = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_config);
    esp_task_wdt_add(NULL);
    // ─────────────────────────────────────────────────────────────────────────

    delay(1000);
    Serial.println("\n=== ESP32 FINGERPRINT SYSTEM ===");

    // ── Buzzer self-test ──────────────────────────────────────────────────────
    // Beeps once on every boot so you can confirm wiring without waiting
    // for an enroll/attendance event. If you don't hear this, it's a
    // hardware/wiring issue, not the app logic.
    Serial.println("[Buzzer] Self-test beep...");
    buzzerOn();
    delay(300);
    buzzerOff();

    // Fingerprint sensor init
    mySerial.begin(57600, SERIAL_8N1, 16, 17);
    finger.begin(57600);
    if (!finger.verifyPassword()) {
        mySerial.begin(9600, SERIAL_8N1, 16, 17);
        finger.begin(9600);
        if (!finger.verifyPassword()) {
            Serial.println("Sensor not found! Halting.");
            while (1) delay(1);
        }
    }
    finger.setSecurityLevel(2);
    finger.getParameters();
    Serial.println("Sensor ready. Capacity: " + String(finger.capacity));

    // Shared TLS client — set once, reuse forever
    secureClient.setInsecure();

    // WiFi
    for (auto& n : myNetworks) wifiMulti.addAP(n.ssid, n.password);
    Serial.println("Connecting to WiFi...");
    int tries = 0;
    while (wifiMulti.run() != WL_CONNECTED) {
        delay(500); Serial.print(".");
        if (++tries > 60) { Serial.println("\nTimeout! Restarting..."); ESP.restart(); }
    }
    Serial.println("\nConnected to " + WiFi.SSID() + "  IP: " + WiFi.localIP().toString());
    Serial.println("=== SYSTEM READY ===\n");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    esp_task_wdt_reset();   

    // WiFi guard
    if (WiFi.status() != WL_CONNECTED) {
        handleWiFiReconnect();
        return;
    }

    // Heartbeat
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > HEARTBEAT_MS) {
        lastHeartbeat = millis();
        sendHeartbeat();
    }

    // Mode polling (slower = more stable)
    static String  currentMode   = "";
    static unsigned long lastModeCheck = 0;
    if (millis() - lastModeCheck > MODE_POLL_MS) {
        lastModeCheck = millis();
        currentMode   = getDeviceMode();
        Serial.println("[Mode] " + currentMode);
    }

    // ── Dispatch ──────────────────────────────────────────────────────────────
    if (currentMode == "enroll") {
        static unsigned long lastPoll = 0;
        if (millis() - lastPoll >= 1000) {
            lastPoll = millis();
            String payload;
            if (httpGet(String(serverUrl) + "check-enrollment", &payload) == 200
                && payload != "none" && payload.length() > 0) {
                enrollFingerprint(payload.toInt());
            }
        }

    } else if (currentMode == "delete") {
        static unsigned long lastDeletePoll = 0;
        if (millis() - lastDeletePoll >= 1000) {
            lastDeletePoll = millis();
            String payload;
            if (httpGet(String(serverUrl) + "check-delete", &payload) == 200
                && payload != "none" && payload.length() > 0) {
                deleteFingerprint(payload.toInt());
            }
        }

    } else if (currentMode == "recognize") {
        doRecognition(currentMode);

    } else if (currentMode == "attendance") {
        scanForAttendance();

    } else {
        delay(200);
    }

    delay(10);
}