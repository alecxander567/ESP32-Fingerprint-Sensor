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

void beepSuccess() {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(200);             
    digitalWrite(BUZZER_PIN, LOW);
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
        }
        else {
            Serial.println("Failed to store fingerprint in sensor memory");
            updateStatus(id, "error");
        }
    } else {
        Serial.println("Fingerprints did not match - please try again");
        updateStatus(id, "error"); 
    }
}

void setup() {
    Serial.begin(115200);

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW); 
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
    static unsigned long lastPoll = 0;
    static int pollCount = 0;
    
    // Poll every 1 second
    if (millis() - lastPoll >= 1000) {
        lastPoll = millis();
        pollCount++;
        
        if (wifiMulti.run() == WL_CONNECTED) {
            HTTPClient http;
            String url = String(serverUrl) + "check-enrollment";
            
            // Only print detailed logs every 10 polls to reduce spam
            if (pollCount % 10 == 1) {
                Serial.println("\nChecking for pending enrollments...");
                Serial.println("   URL: " + url);
            }
            
            http.begin(url);
            int httpCode = http.GET();
            
            if (pollCount % 10 == 1) {
                Serial.print("   Response code: ");
                Serial.println(httpCode);
            }
            
            if (httpCode == 200) {
                String payload = http.getString();
                
                if (pollCount % 10 == 1) {
                    Serial.println("   Payload: " + payload);
                }
                
                if (payload != "none" && payload.length() > 0) {
                    Serial.println("\nFOUND ENROLLMENT REQUEST!");
                    Serial.println("   Finger ID: " + payload);
                    enrollFingerprint(payload.toInt());
                    pollCount = 0; 
                } else {
                    if (pollCount % 10 == 1) {
                        Serial.println("   → No pending enrollments");
                    }
                }
            } else {
                Serial.println("HTTP error: " + String(httpCode));
                Serial.println("   Check if backend server is running at " + String(serverUrl));
            }
            http.end();
        } else {
            Serial.println(" WiFi disconnected! Attempting to reconnect...");
        }
    }
    
    delay(10); 
}