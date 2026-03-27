/*********
  WaterWatch IoT System - ESP32 Valve Control
  FirebaseClient + WiFiManager + Factory Reset
*********/

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <WiFiManager.h>
#include <Preferences.h>

// ======================================
// CONFIG - Load from local config.h (not in git)
// ======================================
#include "config.h"

// ======================================
// HARDWARE
// ======================================
const int valvePin       = 2;
const int resetButtonPin = 0;
String deviceId          = "";  // generated on first boot

// ======================================
// FIREBASE OBJECTS
// ======================================
void processData(AsyncResult &aResult);
void streamCallback(AsyncResult &aResult);

UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);
FirebaseApp app;
WiFiClientSecure ssl_client;
WiFiClientSecure stream_ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
AsyncClient streamClient(stream_ssl_client);
RealtimeDatabase Database;

// ======================================
// PREFERENCES & WIFIMANAGER
// ======================================
Preferences preferences;
WiFiManager wm;

// ======================================
// STATE
// ======================================
bool   firebaseReady    = false;
bool   streamReady      = false;
String currentStatus    = "";
bool   buttonWasPressed = false;

// ======================================
// TIMINGS
// ======================================
const unsigned long STATUS_INTERVAL = 5000;
const unsigned long BUTTON_POLL_MS  = 50;
const unsigned long LONG_PRESS_TIME = 3000;

unsigned long lastStatusUpdate = 0;
unsigned long lastButtonCheck  = 0;
unsigned long buttonPressStart = 0;

// ======================================
// GENERATE RANDOM HEX STRING
// ======================================
String generateRandomHex(int length) {
  String result = "";
  const char* hexChars = "0123456789abcdef";
  for (int i = 0; i < length; i++) {
    result += hexChars[random(16)];
  }
  return result;
}

// ======================================
// GENERATE OR LOAD DEVICE ID
// ======================================
void initializeDeviceId() {
  preferences.begin("water_watch", false);
  String savedDeviceId = preferences.getString("device_id", "");
  
  if (savedDeviceId.length() == 0) {
    // First boot - generate new device ID
    deviceId = "waterwatch_" + generateRandomHex(8);
    preferences.putString("device_id", deviceId);
    
    Serial.println("\nFIRST BOOT - NEW DEVICE GENERATED");
  } else {
    // Load saved device ID
    deviceId = savedDeviceId;
  }
  
  preferences.end();
  
  Serial.printf("[Device] ID: %s\n", deviceId.c_str());
}

// ======================================
// HELPER: start AP config portal
// ======================================
void startConfigPortal() {
  wm.setSaveConfigCallback([]() {
    Serial.println("[WiFi] Credentials saved via portal");
    preferences.begin("water_watch", false);
    preferences.putString("ssid",      WiFi.SSID());
    preferences.putString("password",  WiFi.psk());
    preferences.putBool("provisioned", true);
    preferences.end();
  });

  // Only show WiFi config button
  std::vector<const char*> wmMenu = {"wifi"};
  wm.setMenu(wmMenu);
  wm.setConfigPortalBlocking(false);

  wm.autoConnect(deviceId.c_str());

  Serial.println("[WiFi] AP Mode started");
  Serial.printf("[WiFi] SSID: %s\n", deviceId.c_str());
  Serial.println("[WiFi] Portal: 192.168.4.1");
  Serial.printf("[WiFi] Device ID: %s\n\n", deviceId.c_str());
}

// ======================================
// FACTORY RESET
// ======================================
void performFactoryReset() {
  Serial.println("\nFACTORY RESET TRIGGERED");
  Serial.println("[FACTORY RESET] Clearing all data...");

  preferences.begin("water_watch", false);
  preferences.clear();
  preferences.end();

  wm.resetSettings();

  Serial.println("[FACTORY RESET] WiFi credentials erased");
  Serial.println("[FACTORY RESET] Device ID will be regenerated on next boot");
  Serial.println("[FACTORY RESET] Restarting in 2 s...");
  delay(2000);
  ESP.restart();
}

// ======================================
// HANDLE RESET BUTTON
// ======================================
void handleResetButton() {
  unsigned long now = millis();
  
  // Faster polling for responsiveness
  if (now - lastButtonCheck < BUTTON_POLL_MS) return;
  lastButtonCheck = now;

  bool pressed = (digitalRead(resetButtonPin) == LOW);

  if (pressed && !buttonWasPressed) {
    // Button just pressed
    buttonWasPressed = true;
    buttonPressStart = now;
    Serial.println("[Button] Pressed — hold for 3s to factory reset");
  } 
  else if (!pressed && buttonWasPressed) {
    // Button released
    buttonWasPressed = false;
    unsigned long holdTime = now - buttonPressStart;
    if (holdTime < LONG_PRESS_TIME) {
      Serial.printf("[Button] Released after %lu ms (need 3000 ms)\n", holdTime);
    }
  } 
  else if (pressed && buttonWasPressed) {
    // Button still held
    unsigned long holdTime = now - buttonPressStart;
    if (holdTime >= LONG_PRESS_TIME && holdTime < (LONG_PRESS_TIME + 100)) {
      // Trigger ONCE when threshold is crossed
      Serial.println("[Button] Long press detected — initiating factory reset!");
      performFactoryReset();
    }
  }
}

// ======================================
// INIT FIREBASE
// ======================================
void initFirebase() {
  ssl_client.setInsecure();
  ssl_client.setHandshakeTimeout(5);
  stream_ssl_client.setInsecure();
  stream_ssl_client.setHandshakeTimeout(5);

  initializeApp(aClient, app, getAuth(user_auth), processData, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  firebaseReady = true;
  Serial.println("[Firebase] Initialization complete");
}

// ======================================
// START FIREBASE STREAM
// ======================================
void startCommandStream() {
  if (streamReady) return;
  if (!app.ready()) return;

  String path = "/devices/" + deviceId + "/command";
  Database.get(streamClient, path.c_str(), streamCallback, true, "commandStream");
  streamReady = true;
  Serial.println("[Firebase] Stream started on /devices/device1/command");
}

// ======================================
// SEND STATUS TO FIREBASE
// ======================================
void sendStatusToFirebase() {
  if (!app.ready()) return;

  String status = (digitalRead(valvePin) == HIGH) ? "OPEN" : "CLOSED";

  if (status != currentStatus) {
    currentStatus = status;
    String path = "/devices/" + deviceId + "/status";
    Database.set<String>(aClient, path.c_str(), status, processData, "RTDB_Status");
    Serial.printf("[Status] Valve: %s -> Firebase\n", status.c_str());
  }
}

// ======================================
// STREAM CALLBACK
// ======================================
void streamCallback(AsyncResult &aResult) {
  if (aResult.isError()) {
    Firebase.printf("[Stream Error] %s | %s (%d)\n",
      aResult.uid().c_str(),
      aResult.error().message().c_str(),
      aResult.error().code());
    streamReady = false;  // will reconnect on next loop
    return;
  }

  if (aResult.available()) {
    String payload = String(aResult.c_str());
    payload.trim();

    Firebase.printf("[Stream] command: %s\n", payload.c_str());

    if (payload.indexOf("OPEN") >= 0) {
      digitalWrite(valvePin, HIGH);
      currentStatus = "";
      Serial.println("[Valve] Opened via Firebase command");
      sendStatusToFirebase();

    } else if (payload.indexOf("CLOSED") >= 0) {
      digitalWrite(valvePin, LOW);
      currentStatus = "";
      Serial.println("[Valve] Closed via Firebase command");
      sendStatusToFirebase();
    }
  }
}

// ======================================
// FIREBASE ASYNC CALLBACK
// ======================================
void processData(AsyncResult &aResult) {
  if (aResult.isDebug())
    Firebase.printf("[FB Debug] %s | %s\n",
      aResult.uid().c_str(),
      aResult.debug().c_str());

  if (aResult.isError())
    Firebase.printf("[FB Error] %s | %s (%d)\n",
      aResult.uid().c_str(),
      aResult.error().message().c_str(),
      aResult.error().code());

  if (aResult.available())
    Firebase.printf("[FB Data]  %s | %s\n",
      aResult.uid().c_str(),
      aResult.c_str());
}

// ======================================
// SETUP
// ======================================
void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n╔════════════════════════════════╗");
  Serial.println("║     WaterWatch IoT System      ║");
  Serial.println("╚════════════════════════════════╝\n");

  pinMode(valvePin, OUTPUT);
  digitalWrite(valvePin, LOW);
  Serial.println("[GPIO] Valve: GPIO2 (default CLOSED)");

  pinMode(resetButtonPin, INPUT_PULLUP);
  Serial.println("[GPIO] Reset: GPIO0 (hold 3 s for factory reset)\n");

  // Check if reset button held at startup
  delay(100);  // debounce
  if (digitalRead(resetButtonPin) == LOW) {
    Serial.println("[BOOT] Reset button detected — hold for factory reset...");
    unsigned long hold_start = millis();
    while (digitalRead(resetButtonPin) == LOW) {
      if (millis() - hold_start >= 3000) {
        performFactoryReset();
        return;  // won't reach here due to restart
      }
      delay(50);
    }
    Serial.println("[BOOT] Button released before 3s — continuing normal boot");
  }

  // Generate or load device ID
  initializeDeviceId();
  Serial.println();

  preferences.begin("water_watch", false);
  bool provisioned = preferences.getBool("provisioned", false);
  String savedSSID  = preferences.getString("ssid", "");
  String savedPass  = preferences.getString("password", "");
  preferences.end();

  if (provisioned && savedSSID.length() > 0) {
    Serial.printf("[WiFi] Connecting to: %s\n", savedSSID.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WiFi] Connected — IP: %s\n\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println("[WiFi] Failed to connect — falling back to AP mode\n");
      startConfigPortal();
    }
  } else {
    Serial.println("[WiFi] Not provisioned — starting AP mode\n");
    startConfigPortal();
  }

  if (WiFi.status() == WL_CONNECTED) {
    initFirebase();
  } else {
    Serial.println("[Firebase] Skipped — waiting for WiFi via portal");
  }

  Serial.println("\n[Setup] Boot complete!");
}

// ======================================
// LOOP
// ======================================
void loop() {
  unsigned long now = millis();

  handleResetButton();
  wm.process();

  if (!firebaseReady && WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Connected via portal — initializing Firebase");
    initFirebase();
  }

  if (firebaseReady) {
    app.loop();

    if (app.ready()) {
      startCommandStream();
    }
  }

  if (firebaseReady && (now - lastStatusUpdate >= STATUS_INTERVAL)) {
    lastStatusUpdate = now;
    sendStatusToFirebase();
  }
}