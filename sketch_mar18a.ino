#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <LittleFS.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h> 
#include <Ticker.h>

// Pin Definitions
#define SS_PIN D8
#define RST_PIN D0
#define BUZZER_PIN D1
#define LED_PIN D3
#define RELAY_PIN D2
#define BUZZER_CHANNEL 0
#define BUZZER_RESOLUTION 8

// WiFi and Supabase Configuration
const char* ssid = "coe";
const char* password = "12345678";
const char* supabaseUrl = "https://cyledjoxclfmrjdfakvs.supabase.co";
const char* supabaseKey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImN5bGVkam94Y2xmbXJqZGZha3ZzIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDIxMzUzNjgsImV4cCI6MjA1NzcxMTM2OH0.t0XbpsgcWzI3JD-1wcjYrcZtXDfxTmNTGEf0ewS8-iI";

// For ESP8266, we'll use setInsecure() for simplicity in this example
// For production, consider using setFingerprint() or setTrustAnchors()
const char* fingerprint = "A1 B2 C3 D4 E5 F6 07 89 10 11 12 13 14 15 16 17 18 19 20"; // Replace with your actual fingerprint

// RFID and System Variables
MFRC522 rfid(SS_PIN, RST_PIN);
bool registrationMode = false;
const String masterRfidUid = "533E4C1C"; // Master card UID

// Timing and Sync Control
Ticker syncTicker;
bool needsSync = false;
bool isSyncing = false;
unsigned long lastSyncAttempt = 0;
const unsigned long SYNC_RETRY_INTERVAL = 5000; // 5 seconds
unsigned long lastCardCheck = 0;
const unsigned long CARD_CHECK_INTERVAL = 100; // 100ms
bool firstSyncComplete = false; // Track if first sync has occurred
bool initialUserSyncDone = false; // Track if initial user sync is done
uint8_t registrationExitSyncCount = 0; // Count syncs after registration exit
const uint8_t MAX_REGISTRATION_EXIT_SYNCS = 3; // Max syncs after registration exit

void setup() {
    Serial.begin(115200);
    while (!Serial); // Wait for serial to initialize
    Serial.println("\n\nStarting RFID Attendance System...");
    
    SPI.begin();
    rfid.PCD_Init();
    Serial.println("RFID Reader Initialized");
    
    analogWriteRange(255);
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("GPIO Pins Initialized");
    
    if (!LittleFS.begin()) {
        Serial.println("Failed to mount LittleFS");
        return;
    }
    Serial.println("LittleFS Mounted Successfully");
    
    WiFi.begin(ssid, password);
    Serial.println("Connecting to WiFi...");
    
    // Only sync attendance periodically, not users
    syncTicker.attach(5.0, []{ 
        needsSync = true; 
        Serial.println("Sync triggered by ticker (attendance only)");
    });
    playStartupSound();
}


void loop() {
    handleWiFi();
    
    // Process background sync if needed (non-blocking)
    if (needsSync && WiFi.status() == WL_CONNECTED && !isSyncing) {
        if (millis() - lastSyncAttempt > SYNC_RETRY_INTERVAL) {
            Serial.println("Attempting background sync...");
            performBackgroundSync();
        }
    }
    
    // Always check for cards with minimal delay
    checkForCard();
}

void handleWiFi() {
    static bool lastWiFiStatus = false;
    bool currentWiFiStatus = (WiFi.status() == WL_CONNECTED);
    
    if (currentWiFiStatus != lastWiFiStatus) {
        if (currentWiFiStatus) {
            Serial.println("\nWiFi connected!");
            Serial.print("IP Address: ");
            Serial.println(WiFi.localIP());
            playWiFiConnected();
            
            // Sync users on first WiFi connection if not done yet
            if (!initialUserSyncDone) {
                Serial.println("First WiFi connection - syncing authorized users");
                syncAuthorizedUsers();
                initialUserSyncDone = true;
            }
            
            // Always try to sync attendance when WiFi connects
            needsSync = true;
        } else {
            Serial.println("WiFi disconnected!");
            playWiFiDisconnected();
        }
        lastWiFiStatus = currentWiFiStatus;
    }
}

void performBackgroundSync() {
    static uint8_t syncStage = 0;
    isSyncing = true;
    
    Serial.printf("Starting sync stage %d\n", syncStage);
    
    // Process one sync stage per call (non-blocking)
    switch(syncStage) {
        case 0: // Sync registrations
            if (syncLocalRegistrations()) {
                syncStage = 1;
                Serial.println("Registration sync complete");
            } else {
                Serial.println("Registration sync incomplete, will retry");
            }
            break;
            
        case 1: // Sync attendance
            if (syncLocalAttendance()) {
                syncStage = 0;
                needsSync = false;
                Serial.println("Attendance sync complete");
            } else {
                Serial.println("Attendance sync incomplete, will retry");
            }
            break;
    }
    
    lastSyncAttempt = millis();
    isSyncing = false;
}

void checkForCard() {
    if (millis() - lastCardCheck < CARD_CHECK_INTERVAL) return;
    lastCardCheck = millis();
    
    if (!rfid.PICC_IsNewCardPresent()) {
        return;
    }

    if (!rfid.PICC_ReadCardSerial()) {
        return;
    }

    String rfidUid = getRfidUid();
    Serial.println("Scanned RFID: " + rfidUid);

    // Immediately display the RFID
    Serial.print("Card UID: ");
    for (byte i = 0; i < rfid.uid.size; i++) {
        Serial.print(rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
        Serial.print(rfid.uid.uidByte[i], HEX);
    }
    Serial.println();

    // Handle master card
    if (rfidUid == masterRfidUid) {
        handleMasterCard(rfidUid);
        return;
    }

    // Handle regular cards
    if (registrationMode) {
        Serial.println("Processing card in registration mode");
        handleRegistration(rfidUid);
    } else {
        Serial.println("Processing card in attendance mode");
        handleAttendance(rfidUid);
    }
}

String getRfidUid() {
    String uid = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
        uid += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
        uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();
    return uid;
}

void handleMasterCard(String rfidUid) {
    bool previousMode = registrationMode;
    registrationMode = !registrationMode;
    playModeChangeSound();
    
    if (previousMode && !registrationMode) {
        Serial.println("Exiting registration mode");
        // Always sync users when exiting registration mode
        Serial.println("Syncing authorized users after registration");
        syncAuthorizedUsers();
    }

    Serial.println(registrationMode ? "Registration mode ON" : "Registration mode OFF");
    digitalWrite(LED_PIN, registrationMode ? HIGH : LOW);
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}

void handleRegistration(String rfidUid) {
    Serial.println("Attempting to register card: " + rfidUid);
    if (!registerNewUser(rfidUid)) {
        playErrorSound();
        Serial.println("Registration failed for card: " + rfidUid);
    } else {
        Serial.println("Registration successful for card: " + rfidUid);
    }
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}

bool registerNewUser(String rfidUid) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi disconnected. Storing locally.");
        logRegistrationLocally(rfidUid);
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure(); // For testing - use setFingerprint() in production
    client.setTimeout(10000);
    
    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(10000);
    
    String url = String(supabaseUrl) + "/rest/v1/users";
    Serial.println("Attempting to register at URL: " + url);
    
    if (!http.begin(client, url)) {
        Serial.println("Failed to begin HTTP connection");
        return false;
    }
    
    http.addHeader("apikey", supabaseKey);
    http.addHeader("Authorization", "Bearer " + String(supabaseKey));
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Prefer", "return=minimal");
    
    String payload = "{\"rfid_uid\":\"" + rfidUid + "\"}";
    Serial.println("Sending payload: " + payload);

    int httpCode = http.POST(payload);
    String response = http.getString();
    
    Serial.printf("HTTP Response Code: %d\n", httpCode);
    Serial.println("HTTP Response: " + response);
    
    http.end();

    if (httpCode == HTTP_CODE_CREATED) {
        Serial.println("User registered successfully!");
        playSuccessSound();
        return true;
    } else if (httpCode == 409) { // Conflict - already exists
        Serial.println("User already exists - removing from local storage if present");
        removeFromLocalRegistrations(rfidUid);
        return true; // Consider this a success since the user exists
    } else {
        Serial.printf("Failed to register. HTTP Code: %d\n", httpCode);
        Serial.println("Response: " + response);
        return false;
    }
}

void removeFromLocalRegistrations(String rfidUid) {
    if (!LittleFS.exists("/local_registrations.txt")) {
        return;
    }

    File file = LittleFS.open("/local_registrations.txt", "r");
    if (!file) {
        return;
    }

    String remainingData;
    bool found = false;
    
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line != rfidUid && line.length() > 0) {
            remainingData += line + "\n";
        } else if (line == rfidUid) {
            found = true;
        }
    }
    file.close();

    if (found) {
        File newFile = LittleFS.open("/local_registrations.txt", "w");
        if (newFile) {
            newFile.print(remainingData);
            newFile.close();
            Serial.println("Removed duplicate registration from local storage: " + rfidUid);
        }
    }
}

void logRegistrationLocally(String rfidUid) {
    File file = LittleFS.open("/local_registrations.txt", "a");
    if (file) {
        file.println(rfidUid);
        file.close();
        Serial.println("Registration logged locally: " + rfidUid);
    } else {
        Serial.println("Failed to open local registrations file");
    }
}

bool syncLocalRegistrations() {
    if (!LittleFS.exists("/local_registrations.txt")) {
        Serial.println("No local registrations to sync");
        return true;
    }

    File file = LittleFS.open("/local_registrations.txt", "r");
    if (!file) {
        Serial.println("Failed to open local registrations file");
        return false;
    }

    // Process just one item per sync cycle
    if (file.available()) {
        String rfidUid = file.readStringUntil('\n');
        rfidUid.trim();
        
        if (rfidUid.length() > 0) {
            Serial.println("Processing local registration: " + rfidUid);
            bool success = registerNewUser(rfidUid);
            
            if (success) {
                String remainingData;
                while (file.available()) {
                    remainingData += file.readStringUntil('\n');
                    remainingData += "\n";
                }
                file.close();
                
                File newFile = LittleFS.open("/local_registrations.txt", "w");
                if (newFile) {
                    newFile.print(remainingData);
                    newFile.close();
                    Serial.println("Updated local registrations file");
                } else {
                    Serial.println("Failed to update local registrations file");
                }
                return false; // More items to process
            } else {
                file.close();
                return false; // Try again next time
            }
        }
    }
    
    file.close();
    LittleFS.remove("/local_registrations.txt");
    Serial.println("All local registrations processed, file removed");
    return true;
}

void handleAttendance(String rfidUid) {
    Serial.println("Checking access for card: " + rfidUid);
    if (checkLocalAccess(rfidUid)) {
        Serial.println("Access granted for card: " + rfidUid);
        grantAccess();
        queueAttendance(rfidUid);
    } else {
        Serial.println("Access denied for card: " + rfidUid);
        playRejectionSound();
        denyAccess();
    }
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}

bool checkLocalAccess(String rfidUid) {
    if (!LittleFS.exists("/authorized_users.txt")) {
        File file = LittleFS.open("/authorized_users.txt", "w");
        if (file) {
            file.close();
            Serial.println("Created new authorized_users file");
        } else {
            Serial.println("Failed to create authorized_users file");
        }
    }

    File file = LittleFS.open("/authorized_users.txt", "r");
    if (!file) {
        Serial.println("Failed to open authorized_users file");
        return false;
    }

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line == rfidUid) {
            file.close();
            return true;
        }
    }

    file.close();
    return false;
}

void grantAccess() {
    Serial.println("Access granted");
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(RELAY_PIN, HIGH);
    playSuccessSound();
    delay(3000);
    digitalWrite(RELAY_PIN, LOW);
    digitalWrite(LED_PIN, LOW);
}

void denyAccess() {
    Serial.println("Access denied");
    playRejectionSound();
}

void playRejectionSound() {
    Serial.println("Playing rejection sound");
    for (int i = 0; i < 3; i++) {
        analogWrite(BUZZER_PIN, 50);
        delay(100);
        analogWrite(BUZZER_PIN, 0);
        delay(100);
    }
}

void queueAttendance(String rfidUid) {
    // First try to sync immediately if WiFi is available
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi available - attempting immediate attendance sync");
        if (logAttendance(rfidUid)) {
            Serial.println("Attendance synced immediately");
            return;
        }
    }
    
    // If immediate sync failed or no WiFi, queue for later
    File file = LittleFS.open("/attendance_queue.txt", "a");
    if (file) {
        file.println(rfidUid);
        file.close();
        needsSync = true;
        Serial.println("Attendance queued for sync: " + rfidUid);
    } else {
        Serial.println("Failed to queue attendance");
    }
}

bool logAttendance(String rfidUid) {
    WiFiClientSecure client;
    client.setInsecure(); // For testing - use setFingerprint() in production
    client.setTimeout(10000);
    
    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(10000);

    String url = String(supabaseUrl) + "/rest/v1/attendance";
    Serial.println("Logging attendance at URL: " + url);

    if (!http.begin(client, url)) {
        Serial.println("HTTP begin failed");
        return false;
    }

    http.addHeader("apikey", supabaseKey);
    http.addHeader("Authorization", "Bearer " + String(supabaseKey));
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Prefer", "return=minimal");

    String payload = "{\"rfid_uid\":\"" + rfidUid + "\"}";
    Serial.println("Sending payload: " + payload);
    
    int httpCode = http.POST(payload);
    String response = http.getString();
    
    Serial.printf("HTTP Response Code: %d\n", httpCode);
    Serial.println("HTTP Response: " + response);
    
    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_CREATED) {
            http.end();
            Serial.println("Attendance logged successfully");
            return true;
        }
    }

    http.end();
    return false;
}

void logAttendanceLocally(String rfidUid) {
    File file = LittleFS.open("/attendance_queue.txt", "a");
    if (file) {
        file.println(rfidUid);
        file.close();
        Serial.println("Attendance logged locally for UID: " + rfidUid);
    } else {
        Serial.println("Failed to log attendance locally");
    }
}

bool syncLocalAttendance() {
    if (!LittleFS.exists("/attendance_queue.txt")) {
        Serial.println("No attendance records to sync");
        return true;
    }

    File file = LittleFS.open("/attendance_queue.txt", "r");
    if (!file) {
        Serial.println("Failed to open attendance queue file");
        return false;
    }

    // Process just one item per sync cycle
    if (file.available()) {
        String rfidUid = file.readStringUntil('\n');
        rfidUid.trim();
        
        if (rfidUid.length() > 0) {
            Serial.println("Processing queued attendance: " + rfidUid);
            bool success = logAttendance(rfidUid);
            
            if (success) {
                String remainingData;
                while (file.available()) {
                    remainingData += file.readStringUntil('\n');
                    remainingData += "\n";
                }
                file.close();
                
                File newFile = LittleFS.open("/attendance_queue.txt", "w");
                if (newFile) {
                    newFile.print(remainingData);
                    newFile.close();
                    Serial.println("Updated attendance queue file");
                } else {
                    Serial.println("Failed to update attendance queue file");
                }
                return false; // More items to process
            } else {
                file.close();
                return false; // Try again next time
            }
        }
    }
    
    file.close();
    LittleFS.remove("/attendance_queue.txt");
    Serial.println("All queued attendance processed, file removed");
    return true;
}

bool syncAuthorizedUsers() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Network unavailable. Cannot sync users.");
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure(); // For testing - use setFingerprint() in production
    client.setTimeout(15000);
    
    HTTPClient https;
    https.setReuse(false);
    https.setTimeout(15000);

    String url = String(supabaseUrl) + "/rest/v1/users?select=rfid_uid";
    Serial.println("Fetching authorized users from: " + url);

    if (!https.begin(client, url)) {
        Serial.println("Failed to begin HTTP connection");
        return false;
    }

    https.addHeader("apikey", supabaseKey);
    https.addHeader("Authorization", "Bearer " + String(supabaseKey));

    int httpCode = https.GET();
    String payload = https.getString();
    
    Serial.printf("HTTP Response Code: %d\n", httpCode);
    Serial.println("HTTP Response: " + payload);
    
    https.end();

    if (httpCode != HTTP_CODE_OK) {
        Serial.println("Failed to sync users");
        return false;
    }

    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        Serial.println("JSON parse error: " + String(error.c_str()));
        return false;
    }

    // Clear previous data before syncing new data
    LittleFS.remove("/authorized_users.txt");
    File file = LittleFS.open("/authorized_users.txt", "w");
    if (!file) {
        Serial.println("Failed to create authorized users file");
        return false;
    }

    Serial.println("Updating authorized users list:");
    for (JsonObject user : doc.as<JsonArray>()) {
        String rfidUid = user["rfid_uid"].as<String>();
        file.println(rfidUid);
        Serial.println(" - " + rfidUid);
    }

    file.close();
    Serial.println("Authorized Users Synced Successfully!");
    return true;
}


// Sound functions
void playWiFiConnected() {
    Serial.println("Playing WiFi connected sound");
    int melody[] = {262, 330, 392, 523};
    for (int note : melody) {
        analogWriteFreq(note);
        analogWrite(BUZZER_PIN, 50);
        delay(150);
        analogWrite(BUZZER_PIN, 0);
        delay(30);
    }
}

void playWiFiDisconnected() {
    Serial.println("Playing WiFi disconnected sound");
    int melody[] = {523, 392, 330, 262};
    for (int note : melody) {
        analogWriteFreq(note);
        analogWrite(BUZZER_PIN, 40);
        delay(200);
        analogWrite(BUZZER_PIN, 0);
        delay(50);
    }
}

void playSyncComplete() {
    Serial.println("Playing sync complete sound");
    int melody[] = {330, 392, 494};
    for (int note : melody) {
        analogWriteFreq(note);
        analogWrite(BUZZER_PIN, 60);
        delay(120);
        analogWrite(BUZZER_PIN, 0);
        delay(30);
    }
}

void playStartupSound() {
    Serial.println("Playing startup sound");
    int melody[] = {392, 0, 392, 523};
    for (int note : melody) {
        if (note == 0) {
            analogWrite(BUZZER_PIN, 0);
            delay(100);
        } else {
            analogWriteFreq(note);
            analogWrite(BUZZER_PIN, 70);
            delay(200);
            analogWrite(BUZZER_PIN, 0);
            delay(30);
        }
    }
}

void playSuccessSound() {
    Serial.println("Playing success sound");
    for (int i = 200; i < 800; i += 50) {
        analogWrite(BUZZER_PIN, i/4);
        delay(30);
    }
    analogWrite(BUZZER_PIN, 0);
}

void playErrorSound() {
    Serial.println("Playing error sound");
    analogWrite(BUZZER_PIN, 50);
    delay(100);
    analogWrite(BUZZER_PIN, 0);
    delay(50);
    analogWrite(BUZZER_PIN, 50);
    delay(100);
    analogWrite(BUZZER_PIN, 0);
}

void playModeChangeSound() {
    Serial.println("Playing mode change sound");
    for (int i = 200; i <= 400; i += 10) {
        analogWrite(BUZZER_PIN, i/4);
        delay(10);
    }
    analogWrite(BUZZER_PIN, 0);
}