#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <Adafruit_Fingerprint.h>
#include <esp_task_wdt.h>

// ── Device Configuration ──────────────────────────────────────────────────────
// CHANGE THIS FOR EACH DEVICE:
// Device 1: "esp32-1"
// Device 2: "esp32-default"
#define DEVICE_ID "esp32-1"  // <-- Change this per device

// ── Server endpoints ──────────────────────────────────────────────────────────
const char* serverUrl = "https://capstone-project-backend-2-apq4.onrender.com/fingerprints/";
const char* heartbeatUrl = "https://capstone-project-backend-2-apq4.onrender.com/device/heartbeat";

// ── Timing constants ──────────────────────────────────────────────────────────
#define MODE_POLL_MS       500
#define HTTP_FALLBACK_MS   3000
#define HEARTBEAT_MS       10000   
#define WIFI_DEAD_RESTART  30000  
#define WDT_TIMEOUT_S      60  
#define ENROLL_POLL_MS     200
#define DELETE_POLL_MS     200

// ── WiFi networks ─────────────────────────────────────────────────────────────
struct WifiNetwork { const char* ssid; const char* password; };
WifiNetwork myNetworks[] = {
   {"PLDTHOMEFIBRdGp8s", "PLDTWIFIZp2Tr"},
   {"kupal123",          "kupal123"},
};

// ── Global objects ────────────────────────────────────────────────────────────
WiFiMulti wifiMulti;
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

WiFiClientSecure secureClient;
HTTPClient http;
WebSocketsClient webSocket;

// ── Pin definitions ───────────────────────────────────────────────────────────
#define BUZZER_PIN      13
#define GREEN_LED_PIN   32
#define RED_LED_PIN     25

// ── Buzzer configuration ──────────────────────────────────────────────────────
#define BUZZER_FREQ     2000
#define BUZZER_RES      8
#define USE_PWM_BUZZER  1

#if USE_PWM_BUZZER
    inline void buzzerOn() { ledcWriteTone(BUZZER_PIN, BUZZER_FREQ); }
    inline void buzzerOff() { ledcWriteTone(BUZZER_PIN, 0); }
#else
    inline void buzzerOn() { digitalWrite(BUZZER_PIN, HIGH); }
    inline void buzzerOff() { digitalWrite(BUZZER_PIN, LOW); }
#endif

// ── LED / Buzzer helpers ────────────────────────────────────────────────────
void beepSuccess() { 
    buzzerOn(); 
    delay(100); 
    buzzerOff(); 
}

void beepError() {
    for (int i = 0; i < 2; i++) {
        buzzerOn(); 
        delay(80);
        buzzerOff(); 
        delay(80);
    }
}

void ledSuccess() { 
    digitalWrite(GREEN_LED_PIN, HIGH); 
    delay(300); 
    digitalWrite(GREEN_LED_PIN, LOW); 
}

void ledError() { 
    digitalWrite(RED_LED_PIN, HIGH); 
    delay(300); 
    digitalWrite(RED_LED_PIN, LOW); 
}

// ── Centralised HTTP GET ────────────────────────────────────────────────────
int httpGet(const String& url, String* responseBody = nullptr) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[HTTP] WiFi not connected, skipping: " + url);
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
        Serial.println("[HTTP] Error on GET " + url + " → " + http.errorToString(code));
    }
    return code;
}

// ── Global variables for WebSocket ──────────────────────────────────────────
String currentMode = "";
bool wsConnected = false;
unsigned long lastWsPing = 0;
bool enrollmentInProgress = false;
bool isRecognizing = false;
long recognitionSessionId = -1;

// ── WebSocket Event Handler ─────────────────────────────────────────────────
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            wsConnected = false;
            Serial.println("[WS] Disconnected");
            break;
            
        case WStype_CONNECTED:
            wsConnected = true;
            Serial.println("[WS] Connected as: " + String(DEVICE_ID));
            break;
            
        case WStype_TEXT: {
            String msg = String((char*)payload);
            Serial.println("[WS] Received: " + msg);
            
            if (msg == "ping") {
                webSocket.sendTXT("pong");
            }
            else if (msg.startsWith("mode:")) {
                // Parse mode with optional session_id
                String rest = msg.substring(5);
                int secondColon = rest.indexOf(':');
                String mode = secondColon >= 0 ? rest.substring(0, secondColon) : rest;

                if (mode == "enroll" || mode == "delete" ||
                    mode == "recognize" || mode == "attendance" || mode == "idle") {
                    currentMode = mode;
                    
                    // Extract session_id if present
                    if (mode == "recognize" && secondColon >= 0) {
                        String sessionStr = rest.substring(secondColon + 1);
                        recognitionSessionId = atol(sessionStr.c_str());
                        Serial.println("[WS] Parsed session ID: " + String(recognitionSessionId));
                    } else {
                        recognitionSessionId = -1;
                    }
                    
                    Serial.println("[WS] Mode updated to: " + currentMode +
                                    " (session=" + String(recognitionSessionId) + ")");
                }
            }
            break;
        }
    }
}

// ── WiFi reconnect ──────────────────────────────────────────────────────────
static unsigned long wifiLostAt = 0;

void handleWiFiReconnect() {
    static int backoffMs = 1000;

    if (wifiLostAt == 0) wifiLostAt = millis();  

    if (millis() - wifiLostAt > WIFI_DEAD_RESTART) {
        Serial.println("[WiFi] Dead for 30 s — restarting ESP32...");
        ESP.restart();
    }

    Serial.println("[WiFi] Disconnected. Retrying in " + String(backoffMs) + " ms...");
    delay(backoffMs);
    backoffMs = min(backoffMs * 2, 16000);   

    if (wifiMulti.run() == WL_CONNECTED) {
        Serial.println("[WiFi] Reconnected to " + WiFi.SSID());
        backoffMs  = 1000;
        wifiLostAt = 0;
    }
}

// ── Server helpers ──────────────────────────────────────────────────────────
void updateStatus(int id, const String& status) {
    String url = String(serverUrl) + "update-enrollment?id=" + id + "&status=" + status + "&device_id=" + DEVICE_ID;
    int code = httpGet(url);
    Serial.println("[Status] id=" + String(id) + " status=" + status + " → HTTP " + code);
}

void sendHeartbeat() {
    int code = httpGet(heartbeatUrl);
    if (code == 200) {
        // Silent success
    } else {
        Serial.println("[Heartbeat] HTTP " + String(code));
    }
}

String getDeviceMode() {
    String body;
    String url = String(serverUrl) + "device-mode?device_id=" + DEVICE_ID;
    int code = httpGet(url, &body);
    if (code != 200) return "idle";

    if (body.indexOf("attendance") != -1) return "attendance";
    if (body.indexOf("enroll")     != -1) return "enroll";
    if (body.indexOf("delete")     != -1) return "delete";
    if (body.indexOf("recognize")  != -1) return "recognize";
    return "idle";
}

void markAttendance(int fingerId) {
    String url = String(serverUrl) + "mark-attendance?finger_id=" + fingerId + "&device_id=" + DEVICE_ID;
    int code = httpGet(url);
    Serial.println("[Attendance] finger=" + String(fingerId) + " → HTTP " + code);
}

// ── Fingerprint enrollment ──────────────────────────────────────────────────
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
    isRecognizing = false;
    Serial.println("[Enroll] Complete, mode reset to idle");
}

// ── Delete fingerprint ──────────────────────────────────────────────────────
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
    isRecognizing = false;
}

// ── Attendance scan ─────────────────────────────────────────────────────────
void scanForAttendance() {
    static unsigned long lastScanLog = 0;
    
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
        Serial.println("[Attendance] Match! id=" + String(finger.fingerID)
                       + " conf=" + finger.confidence);
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

// ── Recognition test mode ──────────────────────────────────────────────────
void doRecognition() {
    if (isRecognizing) {
        Serial.println("[Recognize] Already in progress, skipping...");
        return;
    }
    
    long mySessionId = recognitionSessionId;

    isRecognizing = true;
    Serial.println("[Recognize] Starting recognition on device: " + String(DEVICE_ID) + 
                   " (session=" + String(mySessionId) + ")");
    
    int p = -1;
    unsigned long t = millis();

    while ((p = finger.getImage()) != FINGERPRINT_OK) {
        webSocket.loop();
        esp_task_wdt_reset();

        if (millis() - t > 15000) {
            Serial.println("[Recognize] Timeout waiting for finger");
            // FIX: Send result with or without session_id
            if (mySessionId != -1) {
                httpGet(String(serverUrl) + "recognition-result?finger_id=0&matched=false&device_id=" + DEVICE_ID + "&session_id=" + mySessionId);
            } else {
                httpGet(String(serverUrl) + "recognition-result?finger_id=0&matched=false&device_id=" + DEVICE_ID);
            }
            currentMode = "idle";
            isRecognizing = false;
            return;
        }
        delay(50);
    }

    Serial.println("[Recognize] Finger detected!");
    
    if (finger.image2Tz() != FINGERPRINT_OK) { 
        Serial.println("[Recognize] Failed to convert image");
        isRecognizing = false;
        currentMode = "idle";
        return; 
    }
    
    p = finger.fingerSearch();

    String resultUrl;
    if (p == FINGERPRINT_OK) {
        Serial.println("[Recognize] Match! id=" + String(finger.fingerID));
        ledSuccess(); 
        beepSuccess();
        if (mySessionId != -1) {
            resultUrl = String(serverUrl) + "recognition-result?finger_id=" + finger.fingerID + "&matched=true&device_id=" + DEVICE_ID + "&session_id=" + mySessionId;
        } else {
            resultUrl = String(serverUrl) + "recognition-result?finger_id=" + finger.fingerID + "&matched=true&device_id=" + DEVICE_ID;
        }
    } else {
        Serial.println("[Recognize] No match.");
        ledError(); 
        beepError();
        if (mySessionId != -1) {
            resultUrl = String(serverUrl) + "recognition-result?finger_id=0&matched=false&device_id=" + DEVICE_ID + "&session_id=" + mySessionId;
        } else {
            resultUrl = String(serverUrl) + "recognition-result?finger_id=0&matched=false&device_id=" + DEVICE_ID;
        }
    }
    
    Serial.println("[Recognize] Sending result: " + resultUrl);
    httpGet(resultUrl);
    isRecognizing = false;
    currentMode = "idle";
    Serial.println("[Recognize] Complete, mode reset to idle");
    delay(500);
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    
    pinMode(GREEN_LED_PIN, OUTPUT);
    digitalWrite(GREEN_LED_PIN, LOW);
    pinMode(RED_LED_PIN, OUTPUT);
    digitalWrite(RED_LED_PIN, LOW);

    #if USE_PWM_BUZZER
        ledcAttach(BUZZER_PIN, BUZZER_FREQ, BUZZER_RES);
        buzzerOff();
    #else
        pinMode(BUZZER_PIN, OUTPUT);
        digitalWrite(BUZZER_PIN, LOW);
    #endif

    esp_task_wdt_config_t wdt_config = {
        .timeout_ms    = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_config);
    esp_task_wdt_add(NULL);

    delay(1000);
    Serial.println("\n=== ESP32 FINGERPRINT SYSTEM ===");
    Serial.println("[Device] ID: " + String(DEVICE_ID));

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
    Serial.println("\nConnected to " + WiFi.SSID() + "  IP: " + WiFi.localIP().toString());

    String wsPath = "/fingerprints/ws/" + String(DEVICE_ID);
    Serial.println("[WS] Connecting with device ID: " + String(DEVICE_ID));
    webSocket.beginSSL("capstone-project-backend-2-apq4.onrender.com", 443, wsPath.c_str());
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(3000);

    Serial.println("=== SYSTEM READY ===\n");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
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

    // ── HTTP FALLBACK POLLING ────────────────────────────────────────────────
    static unsigned long lastHttpCheck = 0;
    if (millis() - lastHttpCheck > HTTP_FALLBACK_MS) {
        lastHttpCheck = millis();
        String mode = getDeviceMode();
        
        if (mode != currentMode) {
            Serial.println("[HTTP] Mode changed to: " + mode);
            currentMode = mode;
        }
    }

    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 10000) {
        lastDebug = millis();
        Serial.print("[DEBUG] Device: " + String(DEVICE_ID));
        Serial.print(" | Mode: ");
        Serial.print(currentMode);
        Serial.print(" | WS: ");
        Serial.print(wsConnected ? "Connected" : "Disconnected");
        Serial.print(" | Recognizing: ");
        Serial.print(isRecognizing ? "Yes" : "No");
        Serial.print(" | Session: ");
        Serial.println(recognitionSessionId);
    }

    if (enrollmentInProgress) {
        delay(20);
        return;
    }

    // ── Dispatch ──────────────────────────────────────────────────────────────
    if (currentMode == "enroll") {
        static unsigned long lastPoll = 0;
        if (millis() - lastPoll >= ENROLL_POLL_MS) {
            lastPoll = millis();
            String payload;
            int code = httpGet(String(serverUrl) + "check-enrollment?device_id=" + DEVICE_ID, &payload);
            if (code == 200 && payload != "none" && payload.length() > 0) {
                Serial.println("[ENROLL] Found finger_id: " + payload);
                enrollFingerprint(payload.toInt());
            }
        }

    } else if (currentMode == "delete") {
        static unsigned long lastDeletePoll = 0;
        if (millis() - lastDeletePoll >= DELETE_POLL_MS) {
            lastDeletePoll = millis();
            String payload;
            if (httpGet(String(serverUrl) + "check-delete?device_id=" + DEVICE_ID, &payload) == 200
                && payload != "none" && payload.length() > 0) {
                deleteFingerprint(payload.toInt());
            }
        }

    } else if (currentMode == "recognize") {
        // FIX: Process recognition from BOTH WebSocket AND HTTP fallback
        if (!isRecognizing) {
            doRecognition();
        }

    } else if (currentMode == "attendance") {
        scanForAttendance();

    } else {
        delay(20);
    }

    delay(5);
}