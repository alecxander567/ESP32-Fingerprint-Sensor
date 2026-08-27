#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <Adafruit_Fingerprint.h>
#include <esp_task_wdt.h>

const char* serverUrl = "https://capstone-project-backend-2-apq4.onrender.com/fingerprints/";
const char* heartbeatUrl = "https://capstone-project-backend-2-apq4.onrender.com/device/heartbeat";

#define MODE_POLL_MS 500
#define HTTP_FALLBACK_MS 3000
#define HEARTBEAT_MS 10000
#define WIFI_DEAD_RESTART 30000
#define WDT_TIMEOUT_S 60

struct WifiNetwork { const char* ssid; const char* password; };
WifiNetwork myNetworks[] = {
   {"PLDTHOMEFIBRdGp8s", "PLDTWIFIZp2Tr"},
   {"kupal123", "kupal123"},
};

WiFiMulti wifiMulti;
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

WiFiClientSecure secureClient;
HTTPClient http;
WebSocketsClient webSocket;

#define BUZZER_PIN 13
#define GREEN_LED_PIN 32
#define RED_LED_PIN 25

#define BUZZER_FREQ 2000
#define BUZZER_RES 8

inline void buzzerOn() { ledcWriteTone(BUZZER_PIN, BUZZER_FREQ); }
inline void buzzerOff() { ledcWriteTone(BUZZER_PIN, 0); }

void beepSuccess() {
    buzzerOn();
    delay(100);
    buzzerOff();
}

void beepError() {
    for (int i = 0; i < 2; i++) {
        buzzerOn(); delay(80);
        buzzerOff(); delay(80);
    }
}

void ledSuccess() { digitalWrite(GREEN_LED_PIN, HIGH); delay(300); digitalWrite(GREEN_LED_PIN, LOW); }
void ledError() { digitalWrite(RED_LED_PIN, HIGH); delay(300); digitalWrite(RED_LED_PIN, LOW); }

int httpGet(const String& url, String* responseBody = nullptr) {
    if (WiFi.status() != WL_CONNECTED) {
        return -1;
    }

    http.begin(secureClient, url);
    http.setTimeout(3000);
    http.setReuse(true);

    int code = http.GET();
    if (responseBody && code == 200) {
        *responseBody = http.getString();
    }
    if (code < 0) {
        Serial.println("[HTTP] Error: " + http.errorToString(code));
    }
    
    return code;
}

// GLOBAL VARIABLES
String currentMode = "";
bool wsConnected = false;
unsigned long lastWsPing = 0;
bool enrollmentInProgress = false;

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            wsConnected = false;
            Serial.println("[WS] Disconnected");
            break;
            
        case WStype_CONNECTED:
            wsConnected = true;
            Serial.println("[WS] Connected");
            break;
            
        case WStype_TEXT: {
            String msg = String((char*)payload);
            Serial.println("[WS] Received: " + msg);
            
            if (msg == "ping") {
                webSocket.sendTXT("pong");
            }
            else if (msg.startsWith("mode:")) {
                String mode = msg.substring(5);
                if (mode == "enroll" || mode == "delete" || 
                    mode == "recognize" || mode == "attendance" || mode == "idle") {
                    currentMode = mode;
                    Serial.println("[WS] Mode updated instantly to: " + currentMode);
                }
            }
            break;
        }
    }
}

static unsigned long wifiLostAt = 0;

void handleWiFiReconnect() {
    static int backoffMs = 1000;

    if (wifiLostAt == 0) wifiLostAt = millis();

    if (millis() - wifiLostAt > WIFI_DEAD_RESTART) {
        Serial.println("[WiFi] Dead for 30s - restarting");
        ESP.restart();
    }

    Serial.println("[WiFi] Disconnected. Retrying in " + String(backoffMs) + " ms");
    delay(backoffMs);
    backoffMs = min(backoffMs * 2, 16000);

    if (wifiMulti.run() == WL_CONNECTED) {
        Serial.println("[WiFi] Reconnected to " + WiFi.SSID());
        backoffMs = 1000;
        wifiLostAt = 0;
    }
}

void updateStatus(int id, const String& status) {
    String url = String(serverUrl) + "update-enrollment?id=" + id + "&status=" + status;
    int code = httpGet(url);
    Serial.println("[Status] id=" + String(id) + " status=" + status + " -> HTTP " + code);
}

void sendHeartbeat() {
    int code = httpGet(heartbeatUrl);
    if (code == 200) {
        // Serial.println("[Heartbeat] OK");
    }
}

String getDeviceMode() {
    String body;
    int code = httpGet(String(serverUrl) + "device-mode", &body);
    if (code != 200) return "idle";

    if (body.indexOf("attendance") != -1) return "attendance";
    if (body.indexOf("enroll") != -1) return "enroll";
    if (body.indexOf("delete") != -1) return "delete";
    if (body.indexOf("recognize") != -1) return "recognize";
    return "idle";
}

void markAttendance(int fingerId) {
    String url = String(serverUrl) + "mark-attendance?finger_id=" + fingerId;
    int code = httpGet(url);
    Serial.println("[Attendance] finger=" + String(fingerId) + " -> HTTP " + code);
}

void enrollFingerprint(int id) {
    enrollmentInProgress = true;
    Serial.println("\n=== ENROLL id=" + String(id) + " ===");
    esp_task_wdt_reset();

    int p = -1;
    updateStatus(id, "place_finger");

    unsigned long t = millis();
    while (finger.getImage() == FINGERPRINT_OK) {
        if (millis() - t > 20000) {
            updateStatus(id, "error");
            beepError();
            ledError();
            enrollmentInProgress = false;
            return;
        }
        delay(100);
    }
    delay(100);

    t = millis();
    while ((p = finger.getImage()) != FINGERPRINT_OK) {
        if (millis() - t > 30000) {
            updateStatus(id, "error");
            beepError();
            ledError();
            enrollmentInProgress = false;
            return;
        }
        delay(50);
    }
    if (finger.image2Tz(1) != FINGERPRINT_OK) {
        updateStatus(id, "error");
        beepError();
        ledError();
        enrollmentInProgress = false;
        return;
    }

    updateStatus(id, "remove_finger");
    t = millis();
    while (finger.getImage() != FINGERPRINT_NOFINGER) {
        if (millis() - t > 10000) {
            updateStatus(id, "error");
            beepError();
            ledError();
            enrollmentInProgress = false;
            return;
        }
        delay(50);
    }
    delay(200);

    updateStatus(id, "place_again");
    t = millis();
    while ((p = finger.getImage()) != FINGERPRINT_OK) {
        if (millis() - t > 30000) {
            updateStatus(id, "error");
            beepError();
            ledError();
            enrollmentInProgress = false;
            return;
        }
        delay(50);
    }
    if (finger.image2Tz(2) != FINGERPRINT_OK) {
        updateStatus(id, "error");
        beepError();
        ledError();
        enrollmentInProgress = false;
        return;
    }

    if (finger.createModel() == FINGERPRINT_OK && finger.storeModel(id) == FINGERPRINT_OK) {
        Serial.println("[Enroll] SUCCESS id=" + String(id));
        updateStatus(id, "success");
        beepSuccess();
        ledSuccess();
    } else {
        Serial.println("[Enroll] FAILED id=" + String(id));
        updateStatus(id, "error");
        beepError();
        ledError();
    }
    
    enrollmentInProgress = false;
    currentMode = "";
    Serial.println("[Enroll] Complete, mode reset to idle");
}

void deleteFingerprint(int id) {
    Serial.println("\n=== DELETE id=" + String(id) + " ===");
    if (finger.deleteModel(id) == FINGERPRINT_OK) {
        updateStatus(id, "delete_success");
        beepSuccess();
        ledSuccess();
    } else {
        updateStatus(id, "delete_error");
        beepError();
        ledError();
    }
    currentMode = "";
}

void scanForAttendance() {
    static unsigned long lastScanLog = 0;
    
    // Log every 5 seconds that we're waiting
    if (millis() - lastScanLog > 5000) {
        lastScanLog = millis();
        Serial.println("[Attendance] Waiting for finger...");
    }
    
    int imageResult = finger.getImage();
    if (imageResult != FINGERPRINT_OK) {
        return;
    }
    
    Serial.println("[Attendance] Finger detected!");
    
    if (finger.image2Tz() != FINGERPRINT_OK) {
        Serial.println("[Attendance] Failed to convert image");
        return;
    }

    int p = finger.fingerSearch();
    if (p == FINGERPRINT_OK) {
        Serial.println("[Attendance] Match! id=" + String(finger.fingerID));
        ledSuccess();
        beepSuccess();
        markAttendance(finger.fingerID);
        delay(200);
        while (finger.getImage() != FINGERPRINT_NOFINGER) delay(50);
        delay(200);
    } else if (p == FINGERPRINT_NOTFOUND) {
        Serial.println("[Attendance] No match.");
        beepError();
        ledError();
        while (finger.getImage() != FINGERPRINT_NOFINGER) delay(50);
        delay(200);
    } else {
        Serial.println("[Attendance] Search error: " + String(p));
        delay(200);
    }
}

void doRecognition(String& mode) {
    Serial.println("[Recognize] Waiting for finger...");
    int p = -1;
    unsigned long t = millis();

    while ((p = finger.getImage()) != FINGERPRINT_OK) {
        if (millis() - t > 15000) {
            httpGet(String(serverUrl) + "recognition-result?finger_id=0&matched=false");
            mode = "idle";
            return;
        }
        delay(50);
    }

    if (finger.image2Tz() != FINGERPRINT_OK) {
        mode = "idle";
        return;
    }
    p = finger.fingerSearch();

    String resultUrl;
    if (p == FINGERPRINT_OK) {
        Serial.println("[Recognize] Match id=" + String(finger.fingerID));
        ledSuccess();
        beepSuccess();
        resultUrl = String(serverUrl) + "recognition-result?finger_id=" + finger.fingerID + "&matched=true";
    } else {
        Serial.println("[Recognize] No match.");
        ledError();
        beepError();
        resultUrl = String(serverUrl) + "recognition-result?finger_id=0&matched=false";
    }
    httpGet(resultUrl);
    mode = "idle";
    delay(500);
}

void setup() {
    Serial.begin(115200);
    pinMode(GREEN_LED_PIN, OUTPUT);
    digitalWrite(GREEN_LED_PIN, LOW);
    pinMode(RED_LED_PIN, OUTPUT);
    digitalWrite(RED_LED_PIN, LOW);

    ledcAttach(BUZZER_PIN, BUZZER_FREQ, BUZZER_RES);
    buzzerOff();

    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&wdt_config);
    esp_task_wdt_add(NULL);

    delay(1000);
    Serial.println("\n=== ESP32 FINGERPRINT SYSTEM ===");

    Serial.println("[Buzzer] Self-test beep...");
    buzzerOn();
    delay(200);
    buzzerOff();

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

    secureClient.setInsecure();

    for (auto& n : myNetworks) wifiMulti.addAP(n.ssid, n.password);
    Serial.println("Connecting to WiFi...");
    int tries = 0;
    while (wifiMulti.run() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (++tries > 60) {
            Serial.println("\nTimeout! Restarting...");
            ESP.restart();
        }
    }
    Serial.println("\nConnected to " + WiFi.SSID() + " IP: " + WiFi.localIP().toString());

    webSocket.beginSSL("capstone-project-backend-2-apq4.onrender.com", 443, "/fingerprints/ws/esp32-default");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(3000);

    Serial.println("=== SYSTEM READY ===\n");
}

void loop() {
    esp_task_wdt_reset();

    webSocket.loop();

    if (wsConnected && millis() - lastWsPing > 30000) {
        lastWsPing = millis();
        webSocket.sendTXT("ping");
    }

    if (WiFi.status() != WL_CONNECTED) {
        handleWiFiReconnect();
        return;
    }

    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > HEARTBEAT_MS) {
        lastHeartbeat = millis();
        sendHeartbeat();
    }

    // HTTP FALLBACK POLLING - ALWAYS checks mode every 3 seconds
    static unsigned long lastHttpCheck = 0;
    if (millis() - lastHttpCheck > HTTP_FALLBACK_MS) {
        lastHttpCheck = millis();
        String mode = getDeviceMode();
        if (mode != currentMode) {
            Serial.println("[HTTP] Mode changed to: " + mode);
            currentMode = mode;
        }
    }

    // WebSocket mode polling (only if WebSocket is connected)
    static unsigned long lastModeCheck = 0;
    if (wsConnected && millis() - lastModeCheck > MODE_POLL_MS) {
        lastModeCheck = millis();
        // WebSocket handles mode updates via events, so nothing needed here
    }

    // Debug: print current mode every 10 seconds
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 10000) {
        lastDebug = millis();
        Serial.print("[DEBUG] Current mode: ");
        Serial.print(currentMode);
        Serial.print(" | WS connected: ");
        Serial.println(wsConnected ? "YES" : "NO");
    }

    // Skip if enrollment is already in progress
    if (enrollmentInProgress) {
        delay(20);
        return;
    }

    // ===== PROCESS THE MODE =====
    if (currentMode == "enroll") {
        static unsigned long lastPoll = 0;
        if (millis() - lastPoll >= 200) {
            lastPoll = millis();
            Serial.println("[ENROLL] Checking for enrollment task...");
            String payload;
            int code = httpGet(String(serverUrl) + "check-enrollment", &payload);
            if (code == 200 && payload != "none" && payload.length() > 0) {
                Serial.println("[ENROLL] Found finger_id: " + payload);
                enrollFingerprint(payload.toInt());
            } else {
                if (code == 200) {
                    // Serial.println("[ENROLL] No pending enrollment found");
                } else {
                    Serial.println("[ENROLL] HTTP error: " + String(code));
                }
            }
        }
        
    } else if (currentMode == "delete") {
        static unsigned long lastDeletePoll = 0;
        if (millis() - lastDeletePoll >= 200) {
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
        delay(20);
    }

    delay(5);
}