#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h> 
#include <HTTPClient.h>
#include <Adafruit_Fingerprint.h>

const char* serverUrl = "http://192.168.1.99:8000/fingerprints/";

struct WifiNetwork {
    const char* ssid;
    const char* password;
};

WifiNetwork myNetworks[] = {
    {"PLDTHOMEFIBRdGp8s", "PLDTWIFIZp2Tr"},
    {"slowifi!",          "Link.18"},
};

WiFiMulti wifiMulti;
HardwareSerial mySerial(2); 
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

#define BUZZER_PIN 13
#define GREEN_LED_PIN 32
#define RED_LED_PIN   33

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
    // Double beep for error
    for (int i = 0; i < 2; i++) {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(150);
        digitalWrite(BUZZER_PIN, LOW);
        delay(150);
    }
}

void updateStatus(int id, String status) {
    if(WiFi.status() == WL_CONNECTED){
        HTTPClient http;
        String url = String(serverUrl) + "update-enrollment?id=" + String(id) + "&status=" + status;
        Serial.println("Updating status: " + url);
        
        http.begin(url);
        http.setTimeout(5000); 
        
        int code = http.GET();
        Serial.println("   Response code: " + String(code));
        
        if (code != 200) {
            Serial.println("   WARNING: Non-200 response from server!");
        }
        
        http.end();
    } else {
        Serial.println(" WiFi not connected, cannot update status!");
    }
}

void enrollFingerprint(int id) {
    Serial.println("\n========================================");
    Serial.println("STARTING ENROLLMENT FOR ID: " + String(id));
    Serial.println("========================================\n");
    
    Serial.println(" DEBUG: Inside enrollFingerprint function");
    Serial.println(" WiFi status: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
    
    int p = -1;

    // --- STEP 1: PLACE FIRST TIME ---
    Serial.println("STEP 1: Waiting for finger placement...");
    updateStatus(id, "place_finger");
    Serial.println(" DEBUG: updateStatus(place_finger) completed");
    
    unsigned long startTime = millis();
    int attempts = 0;
    
    // First, wait for NO finger to ensure clean start
    Serial.println("Ensuring sensor is clear...");
    delay(500);
    
    int clearAttempts = 0;
    while (finger.getImage() == FINGERPRINT_OK) {
        if (clearAttempts == 0) {
            Serial.println(" Sensor detects something - please ensure sensor is COMPLETELY CLEAR!");
        }
        
        clearAttempts++;
        if (clearAttempts % 10 == 0) {
            Serial.print("Still waiting for clear sensor... [");
            Serial.print(clearAttempts);
            Serial.println(" attempts]");
        }
        
        delay(1000);
        
        if (millis() - startTime > 10000) {
            Serial.println(" ERROR: Sensor won't clear after 10 seconds!");
            Serial.println("   Please clean the sensor and try again.");
            updateStatus(id, "error");
            beepError();
            ledError();
            return;
        }
    }
    
    if (clearAttempts > 0) {
        Serial.println(" Sensor is now clear!");
    }
    
    // Small delay to ensure stable reading
    delay(1000);
    
    // Now wait for finger placement
    Serial.println("Ready - waiting for finger...");
    startTime = millis(); 
    
    while (p != FINGERPRINT_OK) { 
        p = finger.getImage();
        
        // Log every 50 attempts (about every 2.5 seconds)
        if (attempts % 50 == 0) {
            Serial.print(".");
            Serial.print(" [Attempt ");
            Serial.print(attempts);
            Serial.print(", Return code: ");
            Serial.print(p);
            Serial.println("]");
        }
        attempts++;

        if (millis() - startTime > 15000) {
            Serial.println("\nTIMEOUT waiting for finger!");
            updateStatus(id, "error");
            beepError();
            ledError();
            return;
        }
        delay(50);
    }
    
    Serial.println("\n Finger detected!");
    Serial.println("Reading fingerprint image...");

    // Convert image 1
    Serial.println("Converting image 1...");
    if (finger.image2Tz(1) != FINGERPRINT_OK) {
        Serial.println("Image conversion failed");
        updateStatus(id, "error");
        beepError();
        ledError();
        return;
    }
    Serial.println(" Image 1 converted successfully");
    
    // Small delay to ensure stability before asking to remove
    delay(500);

    // --- STEP 2: REMOVE ---
    Serial.println("\nSTEP 2: Remove your finger...");
    updateStatus(id, "remove_finger");
    p = 0;
    
    startTime = millis();
    int removeAttempts = 0;
    while (p != FINGERPRINT_NOFINGER) { 
        p = finger.getImage();
        
        // Log every 20 attempts
        if (removeAttempts % 20 == 0 && removeAttempts > 0) {
            Serial.print("Still waiting for finger removal... [");
            Serial.print(removeAttempts);
            Serial.println(" attempts]");
        }
        removeAttempts++;
        
        if (millis() - startTime > 10000) {
            Serial.println("TIMEOUT waiting for finger removal!");
            updateStatus(id, "error");
            beepError();
            ledError();
            return;
        }
        delay(50);
    }
    Serial.println(" Finger removed");
    delay(1000); 

    // --- STEP 3: PLACE AGAIN ---
    Serial.println("\nSTEP 3: Place the SAME finger again...");
    updateStatus(id, "place_again");
    p = -1;
    
    startTime = millis();
    attempts = 0;
    while (p != FINGERPRINT_OK) { 
        p = finger.getImage();
        
        if (attempts % 50 == 0) {
            Serial.print(".");
            Serial.print(" [Attempt ");
            Serial.print(attempts);
            Serial.println("]");
        }
        attempts++;
        
        if (millis() - startTime > 15000) {
            Serial.println("\nTIMEOUT waiting for second finger placement!");
            updateStatus(id, "error");
            beepError();
            ledError();
            return;
        }
        delay(50);
    }
    Serial.println("\n Finger detected again!");
    Serial.println("Reading second fingerprint image...");

    // Convert image 2
    Serial.println("Converting image 2...");
    if (finger.image2Tz(2) != FINGERPRINT_OK) {
        Serial.println("Second image conversion failed");
        updateStatus(id, "error");
        beepError();
        ledError();
        return;
    }
    Serial.println(" Image 2 converted successfully");

    // --- STEP 4: CHECK MATCH AND SAVE ---
    Serial.println("\nSTEP 4: Creating fingerprint template...");
    if (finger.createModel() == FINGERPRINT_OK) {
        Serial.println("Template created - fingerprints match!");
        Serial.println("Storing fingerprint to ID " + String(id) + "...");
        
       if (finger.storeModel(id) == FINGERPRINT_OK) {
            Serial.println("\n========================================");
            Serial.println("ENROLLMENT SUCCESS!");
            Serial.println("========================================\n");
            updateStatus(id, "success");
            beepSuccess();
            ledSuccess();  
        }
        else {
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

void markAttendance(int fingerId) {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String url = String(serverUrl) + "mark-attendance?finger_id=" + String(fingerId);

        Serial.println("Sending attendance to server...");
        Serial.println("URL: " + url);

        http.begin(url);
        http.setTimeout(5000);

        int code = http.GET();
        Serial.println("Server response: " + String(code));

        http.end();
    } else {
        Serial.println("WiFi not connected!");
    }
}

void scanForAttendance() {
    // Remove the "Waiting for fingerprint" spam - only show when finger detected
    
    int p = finger.getImage();
    if (p != FINGERPRINT_OK) return;  

    Serial.println("\n🔍 FINGER DETECTED - Processing...");

    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) {
        Serial.println(" Failed to convert image");
        Serial.print("   Error code: ");
        Serial.println(p);
        return;
    }
    Serial.println(" Image converted successfully");

    Serial.println(" Searching fingerprint database...");
    p = finger.fingerSearch();

    if (p == FINGERPRINT_OK) {
        Serial.println("\n========================================");
        Serial.println(" MATCH FOUND!");
        Serial.println("========================================");
        Serial.print("   Finger ID: ");
        Serial.println(finger.fingerID);
        Serial.print("   Confidence: ");
        Serial.println(finger.confidence);
        Serial.println("========================================\n");
        ledSuccess(); 
        beepSuccess();
        markAttendance(finger.fingerID);
        
        delay(2000);
    } 
    else if (p == FINGERPRINT_NOTFOUND) {
        Serial.println("\n========================================");
        Serial.println(" FINGERPRINT NOT FOUND");
        Serial.println("========================================");
        Serial.println("This fingerprint is not enrolled in the sensor.");
        Serial.println("Checking sensor database...");
        
        // Debug: Check how many fingerprints are stored
        finger.getTemplateCount();
        Serial.print("   Total enrolled fingerprints: ");
        Serial.println(finger.templateCount);
        Serial.println("========================================\n");
        
        // Error sound (double beep)
       beepError();
       ledError();

        delay(2000); 
    } 
    else {
        Serial.println("\n SENSOR ERROR");
        Serial.print("   Error code: ");
        Serial.println(p);
        delay(1000);
    }
}

String getDeviceMode() {
    if (WiFi.status() != WL_CONNECTED) return "idle";

    HTTPClient http;
    String url = String(serverUrl) + "device-mode";

    http.begin(url);
    http.setTimeout(3000);

    int code = http.GET();

    if (code == 200) {
        String payload = http.getString();
        http.end();

        if (payload.indexOf("attendance") != -1) return "attendance";
        if (payload.indexOf("enroll") != -1) return "enroll";
    }

    http.end();
    return "idle";
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
    Serial.println("    ESP32 FINGERPRINT ENROLLMENT");
    Serial.println("========================================");
    
    Serial.println("\nInitializing fingerprint sensor...");
    
    // RX=16, TX=17
    mySerial.begin(57600, SERIAL_8N1, 16, 17);
    finger.begin(57600);
    
    if (finger.verifyPassword()) {
        Serial.println("Fingerprint sensor found at 57600 baud!");
    } else {
        Serial.println("Sensor not found at 57600, trying 9600...");
   
        mySerial.begin(9600, SERIAL_8N1, 16, 17);
        finger.begin(9600);
        
        if (finger.verifyPassword()) {
            Serial.println("Fingerprint sensor found at 9600 baud!");
        } else {
            Serial.println("\nFINGERPRINT SENSOR NOT FOUND!");
            Serial.println("\nTroubleshooting:");
            Serial.println("  1. Check wiring:");
            Serial.println("     - Sensor TX → ESP32 GPIO 16 (RX2)");
            Serial.println("     - Sensor RX → ESP32 GPIO 17 (TX2)");
            Serial.println("     - VCC → 3.3V or 5V (check sensor spec)");
            Serial.println("     - GND → GND");
            Serial.println("  2. Try swapping TX/RX wires");
            Serial.println("  3. Try different voltage (3.3V vs 5V)");
            Serial.println("\nHalting execution...");
            while (1) { delay(1); } 
        }
    }

    Serial.println("\nSetting up WiFi...");
    int numNetworks = sizeof(myNetworks) / sizeof(myNetworks[0]);
    for (int i = 0; i < numNetworks; i++) {
        wifiMulti.addAP(myNetworks[i].ssid, myNetworks[i].password);
        Serial.println(" Added: " + String(myNetworks[i].ssid));
    }

    Serial.println("\nConnecting to WiFi...");
    int attempts = 0;
    while (wifiMulti.run() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        attempts++;
        if (attempts > 60) {
            Serial.println("\nWiFi connection timeout!");
            Serial.println("Please check your WiFi credentials and restart ESP32");
            while(1) { delay(1); }
        }
    }
    
    Serial.println("\n\nWiFi Connected!");
    Serial.println("  IP Address: " + WiFi.localIP().toString());
    Serial.println("  Network: " + WiFi.SSID());
    Serial.println("  Signal: " + String(WiFi.RSSI()) + " dBm");
    Serial.println("  Server URL: " + String(serverUrl));
    Serial.println("\n========================================");
    Serial.println("SYSTEM READY - Polling for enrollments");
    Serial.println("========================================\n");
}

void loop() {

    static String currentMode = "";
    static unsigned long lastModeCheck = 0;

    // Check mode every 2 seconds
    if (millis() - lastModeCheck > 2000) {
        lastModeCheck = millis();
        currentMode = getDeviceMode();

        Serial.println("\nCurrent Mode: " + currentMode);
    }

    if (currentMode == "enroll") {

        // -------- ENROLLMENT LOGIC --------
        static unsigned long lastPoll = 0;

        if (millis() - lastPoll >= 1000) {
            lastPoll = millis();

            HTTPClient http;
            String url = String(serverUrl) + "check-enrollment";

            http.begin(url);
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

    else if (currentMode == "attendance") {

        // -------- ATTENDANCE LOGIC --------
        scanForAttendance();
    }

    else {
        // -------- IDLE --------
        delay(500);
    }

    delay(10);
}