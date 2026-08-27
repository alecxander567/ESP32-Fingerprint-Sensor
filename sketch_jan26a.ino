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

// ── Timing constants ──────────────────────────────────────────────────────────
#define MODE_POLL_MS       500    // WebSocket mode check
#define HTTP_FALLBACK_MS   3000   // HTTP polling every 3 seconds as backup
#define HEARTBEAT_MS      10000   
#define WIFI_DEAD_RESTART  30000  
#define WDT_TIMEOUT_S         60  

struct WifiNetwork { const char* ssid; const char* password; };
WifiNetwork myNetworks[] = {
   {"PLDTHOMEFIBRdGp8s", "PLDTWIFIZp2Tr"},
   {"kupal123",          "kupal123"},
};

WiFiMulti wifiMulti;
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// ── HTTP and WebSocket clients ──────────────────────────────────────────────
WiFiClientSecure secureClient;
HTTPClient http;
WebSocketsClient webSocket;

#define BUZZER_PIN     13
#define GREEN_LED_PIN  32
#define RED_LED_PIN    25

// ── LED / Buzzer helpers ────────────────────────────────────────────────────
void beepSuccess() { 
    digitalWrite(BUZZER_PIN, HIGH); 
    delay(200); 
    digitalWrite(BUZZER_PIN, LOW); 
}

void beepError() {
    for (int i = 0; i < 2; i++) {
        digitalWrite(BUZZER_PIN, HIGH); 
        delay(150);
        digitalWrite(BUZZER_PIN, LOW);  
        delay(150);
    }
}

void ledSuccess() { 
    digitalWrite(GREEN_LED_PIN, HIGH); 
    delay(1000); 
    digitalWrite(GREEN_LED_PIN, LOW); 
}

void ledError() { 
    digitalWrite(RED_LED_PIN, HIGH); 
    delay(1000); 
    digitalWrite(RED_LED_PIN, LOW); 
}

// ── Centralised HTTP GET ────────────────────────────────────────────────────
int httpGet(const String& url, String* responseBody = nullptr) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[HTTP] WiFi not connected, skipping: " + url);
        return -1;
    }

    http.begin(secureClient, url);
    http.setTimeout(5000);                
    http.setReuse(true);                 

    int code = http.GET();
    if (responseBody && code == 200) {
        *responseBody = http.getString();
    }
    if (code < 0) {
        Serial.println("[HTTP] Error on GET " + url + " → " + http.errorToString(code));
    }
    // DO NOT call http.end() - keep connection alive
    // http.end();
    return code;
}

// ── Global variables for WebSocket ──────────────────────────────────────────
String currentMode = "";
bool wsConnected = false;
unsigned long lastWsPing = 0;
bool enrollmentInProgress = false;

// ── WebSocket Event Handler ─────────────────────────────────────────────────
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

// ── Fingerprint enrollment ──────────────────────────────────────────────────
void enrollFingerprint(int id) {
    enrollmentInProgress = true;
    Serial.println("\n=== ENROLL id=" + String(id) + " ===");
    esp_task_wdt_reset();   

    int p = -1;
    updateStatus(id, "place_finger");

    // Wait for sensor to be clear first
    unsigned long t = millis();
    while (finger.getImage() == FINGERPRINT_OK) {
        if (millis() - t > 20000) { 
            updateStatus(id, "error"); 
            beepError(); 
            ledError(); 
            enrollmentInProgress = false;
            return; 
        }
        delay(500);
    }
    delay(500);

    // Capture image 1
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

    // Remove finger
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
    delay(800);

    // Capture image 2
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

    // Create & store model
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
    currentMode = "";  // Reset mode after enrollment
    Serial.println("[Enroll] Complete, mode reset");
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
    currentMode = "";  // Reset mode after delete
}

// ── Attendance scan ─────────────────────────────────────────────────────────
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
        Serial.println("[Attendance] Match! id=" + String(finger.fingerID)
                       + " conf=" + finger.confidence);
        ledSuccess(); 
        beepSuccess();
        markAttendance(finger.fingerID);
        delay(500);
        while (finger.getImage() != FINGERPRINT_NOFINGER) delay(50);
        delay(500);
    } else if (p == FINGERPRINT_NOTFOUND) {
        Serial.println("[Attendance] No match.");
        beepError(); 
        ledError();
        while (finger.getImage() != FINGERPRINT_NOFINGER) delay(50);
        delay(300);
    } else {
        Serial.println("[Attendance] Search error: " + String(p));
        delay(1000);
    }
}

// ── Recognition test mode ──────────────────────────────────────────────────
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
    delay(2000);
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(BUZZER_PIN,    OUTPUT); digitalWrite(BUZZER_PIN,    LOW);
    pinMode(GREEN_LED_PIN, OUTPUT); digitalWrite(GREEN_LED_PIN, LOW);
    pinMode(RED_LED_PIN,   OUTPUT); digitalWrite(RED_LED_PIN,   LOW);

    // ── WATCHDOG ──────────────────────────────────────────────────────────────
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms    = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_config);
    esp_task_wdt_add(NULL);

    delay(1000);
    Serial.println("\n=== ESP32 FINGERPRINT SYSTEM ===");

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

    secureClient.setInsecure();

    // WiFi
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

    // ── WebSocket Connection ──────────────────────────────────────────────────
    webSocket.beginSSL("capstone-project-backend-2-apq4.onrender.com", 443, "/fingerprints/ws/esp32-default");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(3000);

    Serial.println("=== SYSTEM READY ===\n");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    esp_task_wdt_reset();   

    // Process WebSocket messages
    webSocket.loop();

    // Keep WebSocket alive
    if (wsConnected && millis() - lastWsPing > 30000) {
        lastWsPing = millis();
        webSocket.sendTXT("ping");
    }

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

    // ── HTTP FALLBACK POLLING ────────────────────────────────────────────────
    // ALWAYS check mode via HTTP every 3 seconds as backup
    static unsigned long lastHttpCheck = 0;
    if (millis() - lastHttpCheck > HTTP_FALLBACK_MS) {
        lastHttpCheck = millis();
        String mode = getDeviceMode();
        if (mode != currentMode) {
            Serial.println("[HTTP] Mode changed to: " + mode);
            currentMode = mode;
        }
    }

    // ── WebSocket Mode Polling ──────────────────────────────────────────────
    // Only for debug purposes when WebSocket is connected
    static unsigned long lastModeCheck = 0;
    if (wsConnected && millis() - lastModeCheck > MODE_POLL_MS) {
        lastModeCheck = millis();
        // WebSocket handles mode updates via events
    }

    // ── Debug: print current mode every 10 seconds ──────────────────────────
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 10000) {
        lastDebug = millis();
        Serial.print("[DEBUG] Mode: ");
        Serial.print(currentMode);
        Serial.print(" | WS: ");
        Serial.println(wsConnected ? "Connected" : "Disconnected");
    }

    // Skip if enrollment is in progress
    if (enrollmentInProgress) {
        delay(20);
        return;
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