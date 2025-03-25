#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <LittleFS.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h> 

#define SS_PIN D8
#define RST_PIN D0
#define BUZZER_PIN D1
#define LED_PIN D3
#define RELAY_PIN D2
#define BUZZER_CHANNEL 0
#define BUZZER_RESOLUTION 8

const char* ssid = "coe";
const char* password = "12345678";
const char* supabaseUrl = "https://cyledjoxclfmrjdfakvs.supabase.co";
const char* supabaseKey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImN5bGVkam94Y2xmbXJqZGZha3ZzIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDIxMzUzNjgsImV4cCI6MjA1NzcxMTM2OH0.t0XbpsgcWzI3JD-1wcjYrcZtXDfxTmNTGEf0ewS8-iI";

MFRC522 rfid(SS_PIN, RST_PIN);
bool registrationMode = false;
const String masterRfidUid = "D36255F5"; // master card UID C9BAF597 white wala
unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 30000; 
enum SyncState {
    SYNC_IDLE,
    SYNC_REGISTRATIONS,
    SYNC_ATTENDANCE,
    SYNC_USERS
};
SyncState currentSyncState = SYNC_IDLE;
bool pendingSync = false;

void setup() {
    Serial.begin(115200);
    SPI.begin();
    rfid.PCD_Init();
    analogWriteRange(255);
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);

    if (!LittleFS.begin()) {
        Serial.println("Failed to mount LittleFS");
        return;
    }

    // Attempt Wi-Fi connection
    WiFi.begin(ssid, password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nConnected to WiFi");
         playWiFiConnected();
        syncAuthorizedUsers(); // Sync authorized users
        playSyncComplete();

    } else {
        Serial.println("\nWi-Fi unavailable. Using local data.");
    }
     playStartupSound();
}

void loop() {
    // Periodically check Wi-Fi
    if (millis() - lastWiFiCheck >= wifiCheckInterval) {
        lastWiFiCheck = millis();
        checkWiFiAndSync();
    }

    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
        return;
    }

    String rfidUid = getRfidUid();
    Serial.println("Scanned RFID: " + rfidUid);

    // Check if master card is tapped
    if (rfidUid == masterRfidUid) {
        bool previousMode = registrationMode; // Store current mode
        registrationMode = !registrationMode; // Toggle registration mode
        playModeChangeSound();
        
        // If we're exiting registration mode (was true, now false)
        if (previousMode && !registrationMode) {
            Serial.println("Exiting registration mode - syncing authorized users");
            if (WiFi.status() == WL_CONNECTED) {
                syncAuthorizedUsers(); // Sync the updated user list
            } else {
                Serial.println("Cannot sync - WiFi not connected");
            }
        }

        Serial.println(registrationMode ? "Registration mode ON" : "Registration mode OFF");
        tone(BUZZER_PIN, 1000, 500);
        digitalWrite(LED_PIN, registrationMode ? HIGH : LOW);
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
        delay(300); // Debounce to prevent multiple reads
        return;
    }

    if (registrationMode) {
        registerNewUser(rfidUid); // Send to users table
    } else {
        if (checkLocalAccess(rfidUid)) {
            grantAccess();
            logAttendance(rfidUid); // Send to attendance table
        } else {
            denyAccess();
        }
    }

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    delay(100); // Small delay between card reads
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

void checkWiFiAndSync() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi disconnected. Reconnecting...");
        WiFi.begin(ssid, password);
        delay(2000); // Wait for connection
        if (WiFi.status() == WL_CONNECTED) {
           playWiFiConnected();
            syncAuthorizedUsers();
            syncLocalRegistrations();
            syncLocalAttendance();
             playSyncComplete();
        }
    }
}
void registerNewUser(String rfidUid) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi disconnected. Storing locally.");
        logRegistrationLocally(rfidUid);
        return;
    }

    WiFiClientSecure client;
    client.setInsecure(); // Bypass SSL verification (remove in production)

    HTTPClient http;
    String url = String(supabaseUrl) + "/rest/v1/users";
    
    http.begin(client, url);
    http.addHeader("apikey", supabaseKey);
    http.addHeader("Authorization", "Bearer " + String(supabaseKey));
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Prefer", "return=minimal"); // Supabase optimization
    
    String payload = "{\"rfid_uid\":\"" + rfidUid + "\"}";
    Serial.println("Payload: " + payload); // Debug output

    int httpCode = http.POST(payload);
    String response = http.getString(); // Get server response

    if (httpCode == HTTP_CODE_CREATED) {
        Serial.println("User registered successfully!");
    } else {
        Serial.printf("Failed to register. HTTP Code: %d\n", httpCode);
        Serial.println("Response: " + response);
    }

    http.end();
}
void logRegistrationLocally(String rfidUid) {
    File file = LittleFS.open("/local_registrations.txt", "a");
    if (file) {
        file.println(rfidUid);
        file.close();
        Serial.println("Registration logged locally: " + rfidUid);
    }
}

void syncLocalRegistrations() {
    if (!LittleFS.exists("/local_registrations.txt")) return;
    
    File file = LittleFS.open("/local_registrations.txt", "r");
    while (file.available()) {
        String rfidUid = file.readStringUntil('\n');
        rfidUid.trim();
        if (rfidUid.length() > 0) registerNewUser(rfidUid);
    }
    file.close();
    LittleFS.remove("/local_registrations.txt");
    Serial.println("Local registrations synced.");
     playSyncComplete();
}


bool checkLocalAccess(String rfidUid) {
    if (!LittleFS.exists("/authorized_users.txt")) {
        Serial.println("authorized_users.txt does not exist. Creating it...");
        File file = LittleFS.open("/authorized_users.txt", "w");
        if (!file) {
            Serial.println("Failed to create authorized_users.txt");
            return false;
        }
        file.close();
    }

    File file = LittleFS.open("/authorized_users.txt", "r");
    if (!file) {
        Serial.println("Failed to open authorized_users.txt");
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
    playErrorSound();
}

void logAttendance(String rfidUid) {
    if (WiFi.status() == WL_CONNECTED) {
        WiFiClientSecure client;
        client.setInsecure(); // Use this for testing. For production, add the Supabase SSL certificate.

        HTTPClient http;
        String url = String(supabaseUrl) + "/rest/v1/attendance";

        http.begin(client, url);
        http.addHeader("apikey", supabaseKey);
        http.addHeader("Authorization", "Bearer " + String(supabaseKey));
        http.addHeader("Content-Type", "application/json");

        String payload = "{\"rfid_uid\":\"" + rfidUid + "\"}";

        int httpCode = http.POST(payload);

        Serial.println(httpCode == 201 ? "Attendance logged" : "Failed to log attendance, HTTP Code: " + String(httpCode));
        Serial.println("Response: " + http.getString());

        http.end();
    } else {
        Serial.println("Wi-Fi not connected. Attendance logged locally.");
        logAttendanceLocally(rfidUid);
    }
}

void logAttendanceLocally(String rfidUid) {
    File file = LittleFS.open("/local_attendance.txt", "a");
    if (!file) {
        Serial.println("Failed to open local_attendance.txt");
        return;
    }
    file.println(rfidUid);
    file.close();
    Serial.println("Attendance logged locally for UID: " + rfidUid);
}

void syncLocalAttendance() {
    if (!LittleFS.exists("/local_attendance.txt")) {
        Serial.println("No local attendance data to sync.");
        return;
    }

    File file = LittleFS.open("/local_attendance.txt", "r");
    if (!file) {
        Serial.println("Failed to open local_attendance.txt for reading");
        return;
    }

    while (file.available()) {
        String rfidUid = file.readStringUntil('\n');
        rfidUid.trim();
        if (rfidUid.length() > 0) {
            logAttendance(rfidUid); // Log attendance to the server
        }
    }

    file.close();
    LittleFS.remove("/local_attendance.txt"); // Clear local attendance data after syncing
    Serial.println("Local attendance data synced and cleared.");
     playSyncComplete();
}

void syncAuthorizedUsers() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Network unavailable. Cannot sync users.");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();  // Skip SSL validation (for development)

    HTTPClient https;
    String url = String(supabaseUrl) + "/rest/v1/users";  // Corrected table name

    Serial.println("Connecting to: " + url);
    https.begin(client, url);
    https.addHeader("apikey", supabaseKey);
    https.addHeader("Authorization", "Bearer " + String(supabaseKey));

    int httpCode = https.GET();
    String payload = https.getString();

    Serial.println("HTTP Response Code: " + String(httpCode));
    Serial.println("Response: " + payload);

    if (httpCode == HTTP_CODE_OK) {
        // Parse the JSON response
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            Serial.println("Failed to parse JSON: " + String(error.c_str()));
            https.end();
            return;
        }

        // Clean LittleFS before writing new data
        LittleFS.remove("/authorized_users.txt");

        // Open the authorized_users.txt file for writing
        File file = LittleFS.open("/authorized_users.txt", "w");
        if (!file) {
            Serial.println("Failed to open authorized_users.txt for writing");
            https.end();
            return;
        }

        // Write each RFID UID to the file
        for (JsonObject user : doc.as<JsonArray>()) {
            String rfidUid = user["rfid_uid"].as<String>();
            file.println(rfidUid);
            Serial.println("Synced UID: " + rfidUid);
        }

        file.close();
        Serial.println("Authorized Users Synced Successfully!");
         playSyncComplete();
    } else {
        Serial.println("Failed to sync users. HTTP Code: " + String(httpCode));
    }

    https.end();
}





void playWiFiConnected() {
    // Ascending chime for connection established
    int melody[] = {262, 330, 392, 523}; // C4, E4, G4, C5
    for (int note : melody) {
        analogWriteFreq(note);
        analogWrite(BUZZER_PIN, 50); // 50/255 volume
        delay(150);
        analogWrite(BUZZER_PIN, 0);
        delay(30);
    }
}

void playWiFiDisconnected() {
    // Descending chime for disconnection
    int melody[] = {523, 392, 330, 262}; // C5, G4, E4, C4
    for (int note : melody) {
        analogWriteFreq(note);
        analogWrite(BUZZER_PIN, 40);
        delay(200);
        analogWrite(BUZZER_PIN, 0);
        delay(50);
    }
}

void playSyncComplete() {
    // Happy three-tone sequence
    int melody[] = {330, 392, 494}; // E4, G4, B4
    for (int note : melody) {
        analogWriteFreq(note);
        analogWrite(BUZZER_PIN, 60);
        delay(120);
        analogWrite(BUZZER_PIN, 0);
        delay(30);
    }
}

void playStartupSound() {
    // Welcome sequence
    int melody[] = {392, 0, 392, 523}; // G4, pause, G4, C5
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

void playSoftBeep() {
    for (int i = 0; i < 2; i++) {
        analogWrite(BUZZER_PIN, 60);
        delay(50);
        analogWrite(BUZZER_PIN, 0);
        delay(100);
    }
}

void playWindChime() {
    int notes[] = {300, 400, 500, 400, 300};
    for (int note : notes) {
        analogWrite(BUZZER_PIN, note/4);
        delay(80);
        analogWrite(BUZZER_PIN, 0);
        delay(20);
    }
}


void playSuccessSound() {
    for (int i = 200; i < 800; i += 50) {
        analogWrite(BUZZER_PIN, i/4);
        delay(30);
    }
    analogWrite(BUZZER_PIN, 0);
}

void playErrorSound() {
    analogWrite(BUZZER_PIN, 50);
    delay(100);
    analogWrite(BUZZER_PIN, 0);
    delay(50);
    analogWrite(BUZZER_PIN, 50);
    delay(100);
    analogWrite(BUZZER_PIN, 0);
}

void playModeChangeSound() {
    for (int i = 200; i <= 400; i += 10) {
        analogWrite(BUZZER_PIN, i/4);
        delay(10);
    }
    analogWrite(BUZZER_PIN, 0);
}