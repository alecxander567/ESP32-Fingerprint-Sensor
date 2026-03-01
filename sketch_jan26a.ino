#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Adafruit_Fingerprint.h>

const char* serverUrl = "https://capstone-project-backend-1-25tm.onrender.com/fingerprints/";
const char* heartbeatUrl = "https://capstone-project-backend-1-25tm.onrender.com/device/heartbeat";

struct WifiNetwork {
    const char* ssid;
    const char* password;
};

WifiNetwork myNetworks[] = {
    {"PLDTHOMEFIBRdGp8s", "PLDTWIFIZp2Tr"},
    {"slowifi!",          "Link.18"},
    {"SPCT WiFi",          ""},
    {"kupal123",          "kupal123"},
    {"ASUS_D0_2G_Guest",    ""},
    {"dd-wrt",    ""},
};

WiFiMulti wifiMulti;
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

#define BUZZER_PIN 13
#define GREEN_LED_PIN 32
#define RED_LED_PIN   25

void beepSuccess() {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(200);
    digitalWrite(BUZZER_PIN, LOW);
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

void beepError() {
    for (int i = 0; i < 2; i++) {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(150);
        digitalWrite(BUZZER_PIN, LOW);
        delay(150);
    }
}

void updateStatus(int id, String status) {
    if (WiFi.status() == WL_CONNECTED) {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        String url = String(serverUrl) + "update-enrollment?id=" + String(id) + "&status=" + status;
        Serial.println("Updating status: " + url);

        http.begin(client, url);
        http.setTimeout(10000);

        int code = http.GET();
        Serial.println("   Response code: " + String(code));

        if (code != 200) {
            Serial.println("   WARNING: Non-200 response from server!");
        }

        http.end();
    } else {
        Serial.println("WiFi not connected, cannot update status!");
    }
}

void enrollFingerprint(int id) {
    Serial.println("\n========================================");
    Serial.println("STARTING ENROLLMENT FOR ID: " + String(id));
    Serial.println("========================================\n");

    int p = -1;

    Serial.println("STEP 1: Waiting for finger placement...");
    updateStatus(id, "place_finger");

    unsigned long startTime = millis();
    int attempts = 0;

    Serial.println("Ensuring sensor is clear...");
    delay(500);

    int clearAttempts = 0;
    while (finger.getImage() == FINGERPRINT_OK) {
        if (clearAttempts == 0) {
            Serial.println("Sensor detects something - please ensure sensor is COMPLETELY CLEAR!");
        }
        clearAttempts++;
        delay(1000);
        if (millis() - startTime > 20000) {
            Serial.println("ERROR: Sensor won't clear after 20 seconds!");
            updateStatus(id, "error");
            beepError();
            ledError();
            return;
        }
    }

    delay(1000);
    Serial.println("Ready - waiting for finger...");
    startTime = millis();

    while (p != FINGERPRINT_OK) {
        p = finger.getImage();
        attempts++;
        if (millis() - startTime > 30000) {
            Serial.println("\nTIMEOUT waiting for finger!");
            updateStatus(id, "error");
            beepError();
            ledError();
            return;
        }
        delay(50);
    }

    Serial.println("\nFinger detected!");
    if (finger.image2Tz(1) != FINGERPRINT_OK) {
        Serial.println("Image conversion failed");
        updateStatus(id, "error");
        beepError();
        ledError();
        return;
    }
    Serial.println("Image 1 converted successfully");
    delay(500);

    Serial.println("\nSTEP 2: Remove your finger...");
    updateStatus(id, "remove_finger");
    p = 0;

    startTime = millis();
    while (p != FINGERPRINT_NOFINGER) {
        p = finger.getImage();
        if (millis() - startTime > 10000) {
            Serial.println("TIMEOUT waiting for finger removal!");
            updateStatus(id, "error");
            beepError();
            ledError();
            return;
        }
        delay(50);
    }
    Serial.println("Finger removed");
    delay(1000);

    Serial.println("\nSTEP 3: Place the SAME finger again...");
    updateStatus(id, "place_again");
    p = -1;

    startTime = millis();
    attempts = 0;
    while (p != FINGERPRINT_OK) {
        p = finger.getImage();
        attempts++;
        if (millis() - startTime > 30000) {
            Serial.println("\nTIMEOUT waiting for second finger placement!");
            updateStatus(id, "error");
            beepError();
            ledError();
            return;
        }
        delay(50);
    }

    Serial.println("\nFinger detected again!");
    if (finger.image2Tz(2) != FINGERPRINT_OK) {
        Serial.println("Second image conversion failed");
        updateStatus(id, "error");
        beepError();
        ledError();
        return;
    }
    Serial.println("Image 2 converted successfully");

    Serial.println("\nSTEP 4: Creating fingerprint template...");
    if (finger.createModel() == FINGERPRINT_OK) {
        Serial.println("Template created - fingerprints match!");
        if (finger.storeModel(id) == FINGERPRINT_OK) {
            Serial.println("\n========================================");
            Serial.println("ENROLLMENT SUCCESS!");
            Serial.println("========================================\n");
            updateStatus(id, "success");
            beepSuccess();
            ledSuccess();
        } else {
            Serial.println("Failed to store fingerprint in sensor memory");
            updateStatus(id, "error");
            beepError();
            ledError();
        }
    } else {
        Serial.println("Fingerprints did not match - please try again");
        updateStatus(id, "error");
        beepError();
        ledError();
    }
}

void deleteFingerprint(int id) {
    Serial.println("\n========================================");
    Serial.println("DELETING FINGERPRINT ID: " + String(id));
    Serial.println("========================================\n");

    uint8_t p = finger.deleteModel(id);

    if (p == FINGERPRINT_OK) {
        Serial.println("Fingerprint deleted successfully from sensor!");
        updateStatus(id, "delete_success");
        beepSuccess();
        ledSuccess();
    } else {
        Serial.println("Failed to delete fingerprint from sensor");
        Serial.println("Error code: " + String(p));
        updateStatus(id, "delete_error");
        beepError();
        ledError();
    }
}

void markAttendance(int fingerId) {
    if (WiFi.status() == WL_CONNECTED) {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        String url = String(serverUrl) + "mark-attendance?finger_id=" + String(fingerId);

        Serial.println("Sending attendance to server...");
        http.begin(client, url);
        http.setTimeout(10000);

        int code = http.GET();
        Serial.println("Server response: " + String(code));

        http.end();
    } else {
        Serial.println("WiFi not connected!");
    }
}

void scanForAttendance() {
    int p = finger.getImage();
    if (p != FINGERPRINT_OK) return;

    Serial.println("\nFINGER DETECTED - Processing...");

    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) {
        Serial.println("Failed to convert image");
        return;
    }

    p = finger.fingerSearch();

    if (p == FINGERPRINT_OK) {
        Serial.println("\n========================================");
        Serial.println("MATCH FOUND!");
        Serial.println("Finger ID: " + String(finger.fingerID));
        Serial.println("Confidence: " + String(finger.confidence));
        Serial.println("========================================\n");
        ledSuccess();
        beepSuccess();
        markAttendance(finger.fingerID);

        // Wait for finger to be lifted before accepting next scan
        delay(500);
        while (finger.getImage() != FINGERPRINT_NOFINGER) {
            delay(50);
        }
        delay(500);

    } else if (p == FINGERPRINT_NOTFOUND) {
        Serial.println("\nFINGERPRINT NOT FOUND");
        beepError();
        ledError();

        // Wait for finger to be lifted before accepting next scan
        while (finger.getImage() != FINGERPRINT_NOFINGER) {
            delay(50);
        }
        delay(300);

    } else {
        Serial.println("\nSENSOR ERROR: " + String(p));
        delay(1000);
    }
}

String getDeviceMode() {
    if (WiFi.status() != WL_CONNECTED) return "idle";

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = String(serverUrl) + "device-mode";

    http.begin(client, url);
    http.setTimeout(10000);

    int code = http.GET();

    if (code == 200) {
        String payload = http.getString();
        http.end();

        if (payload.indexOf("attendance") != -1) return "attendance";
        if (payload.indexOf("enroll") != -1) return "enroll";
        if (payload.indexOf("delete") != -1) return "delete";
        if (payload.indexOf("recognize") != -1) return "recognize";
    }

    http.end();
    return "idle";
}

void sendHeartbeat() {
    if (WiFi.status() == WL_CONNECTED) {
        WiFiClientSecure *client = new WiFiClientSecure;
        client->setInsecure();
        HTTPClient http;

        http.begin(*client, heartbeatUrl);
        http.setTimeout(10000);

        int code = http.GET();
        Serial.println("Heartbeat sent. Code: " + String(code));

        http.end();
        delete client;
    }
}

void setup() {
    Serial.begin(115200);

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    pinMode(GREEN_LED_PIN, OUTPUT);
    pinMode(RED_LED_PIN, OUTPUT);
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(RED_LED_PIN, LOW);

    delay(1000);

    Serial.println("\n\n========================================");
    Serial.println("    ESP32 FINGERPRINT SYSTEM");
    Serial.println("========================================");

    Serial.println("\nInitializing fingerprint sensor...");
    mySerial.begin(57600, SERIAL_8N1, 16, 17);
    finger.begin(57600);

    if (finger.verifyPassword()) {
        Serial.println("Fingerprint sensor found at 57600 baud!");
        finger.setSecurityLevel(2);
        finger.getParameters();

        Serial.print("Sensor capacity: ");
        Serial.println(finger.capacity);
    } else {
        Serial.println("Sensor not found at 57600, trying 9600...");
        mySerial.begin(9600, SERIAL_8N1, 16, 17);
        finger.begin(9600);

        if (finger.verifyPassword()) {
            Serial.println("Fingerprint sensor found at 9600 baud!");
            finger.setSecurityLevel(2);
            finger.getParameters();          
            Serial.print("Sensor capacity: ");
            Serial.println(finger.capacity);
        } else {
            Serial.println("\nFINGERPRINT SENSOR NOT FOUND! Halting...");
            while (1) { delay(1); }
        }
    }

    Serial.println("\nSetting up WiFi...");
    int numNetworks = sizeof(myNetworks) / sizeof(myNetworks[0]);
    for (int i = 0; i < numNetworks; i++) {
        wifiMulti.addAP(myNetworks[i].ssid, myNetworks[i].password);
        Serial.println("Added: " + String(myNetworks[i].ssid));
    }

    Serial.println("\nConnecting to WiFi...");
    int attempts = 0;
    while (wifiMulti.run() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        attempts++;
        if (attempts > 60) {
            Serial.println("\nWiFi connection timeout! Restarting...");
            ESP.restart();
        }
    }

    Serial.println("\n\nWiFi Connected!");
    Serial.println("  IP Address: " + WiFi.localIP().toString());
    Serial.println("  Network: " + WiFi.SSID());
    Serial.println("  Signal: " + String(WiFi.RSSI()) + " dBm");
    Serial.println("  Server URL: " + String(serverUrl));
    Serial.println("\n========================================");
    Serial.println("SYSTEM READY");
    Serial.println("========================================\n");
}

void loop() {
    // Reconnect WiFi if disconnected
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi disconnected! Reconnecting...");
        wifiMulti.run();
        delay(1000);
        return;
    }

    // Heartbeat every 5 seconds
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > 5000) {
        lastHeartbeat = millis();
        sendHeartbeat();
    }

    // Poll device mode every 500ms
    static String currentMode = "";
    static unsigned long lastModeCheck = 0;
    if (millis() - lastModeCheck > 500) {
        lastModeCheck = millis();
        currentMode = getDeviceMode();
        Serial.println("Current Mode: " + currentMode);
    }

    if (currentMode == "enroll") {
        static unsigned long lastPoll = 0;
        if (millis() - lastPoll >= 1000) {
            lastPoll = millis();
            WiFiClientSecure client;
            client.setInsecure();
            HTTPClient http;
            String url = String(serverUrl) + "check-enrollment";
            http.begin(client, url);
            http.setTimeout(10000);
            int httpCode = http.GET();
            if (httpCode == 200) {
                String payload = http.getString();
                if (payload != "none" && payload.length() > 0) {
                    Serial.println("Enrollment request found!");
                    enrollFingerprint(payload.toInt());
                }
            }
            http.end();
        }
    }

    else if (currentMode == "delete") {
        static unsigned long lastDeletePoll = 0;
        if (millis() - lastDeletePoll >= 1000) {
            lastDeletePoll = millis();
            WiFiClientSecure client;
            client.setInsecure();
            HTTPClient http;
            String url = String(serverUrl) + "check-delete";
            http.begin(client, url);
            http.setTimeout(10000);
            int httpCode = http.GET();
            if (httpCode == 200) {
                String payload = http.getString();
                if (payload != "none" && payload.length() > 0) {
                    Serial.println("Delete request found!");
                    deleteFingerprint(payload.toInt());
                }
            }
            http.end();
        }
    }

    else if (currentMode == "recognize") {
        Serial.println("\n=== RECOGNITION TEST MODE ===");
        Serial.println("Waiting for finger...");

        int p = -1;
        unsigned long startTime = millis();
        while (p != FINGERPRINT_OK) {
            p = finger.getImage();
            if (millis() - startTime > 15000) {
                Serial.println("TIMEOUT waiting for finger!");
                WiFiClientSecure client;
                client.setInsecure();
                HTTPClient httpTimeout;
                httpTimeout.begin(client, String(serverUrl) + "recognition-result?finger_id=0&matched=false");
                httpTimeout.setTimeout(10000);
                httpTimeout.GET();
                httpTimeout.end();
                currentMode = "idle";
                return;
            }
            delay(50);
        }

        p = finger.image2Tz();
        if (p != FINGERPRINT_OK) {
            currentMode = "idle";
            return;
        }

        p = finger.fingerSearch();

        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient httpResult;
        String resultUrl;

        if (p == FINGERPRINT_OK) {
            Serial.println("Fingerprint recognized! ID: " + String(finger.fingerID));
            ledSuccess();
            beepSuccess();
            resultUrl = String(serverUrl) + "recognition-result?finger_id=" + String(finger.fingerID) + "&matched=true";
        } else {
            Serial.println("Fingerprint NOT recognized!");
            ledError();
            beepError();
            resultUrl = String(serverUrl) + "recognition-result?finger_id=0&matched=false";
        }

        httpResult.begin(client, resultUrl);
        httpResult.setTimeout(10000);
        httpResult.GET();
        httpResult.end();

        currentMode = "idle";
        delay(2000);
    }

    else if (currentMode == "attendance") {
        scanForAttendance();
    }

    else {
        delay(500);
    }

    delay(10);
}