// --- Librairies nécessaires ---
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>

// --- LoRa Libraries and Definitions ---
#define M0 21
#define M1 19
#define AUX 4
#define TXD2 17
#define RXD2 16

#define ARRIVED 0x55
#define EMPTY 0xAA
#define ACKNOWLEDGE 0x25

byte expected_30dBm[6] = { 0xC0, 0x00, 0x01, 0x1A, 0x17, 0x47 };
byte expected_20dBm[6] = { 0xC0, 0x00, 0x01, 0x1A, 0x17, 0x44 };

enum boxStatus {
  emptyStatus,
  fullStatus
} mailBoxStatus = emptyStatus;

Preferences preferences;
#define RESET_BUTTON_PIN 5
String pushoverUser;
String pushoverToken;

// Variables pour la gestion des tentatives
#define MAX_PUSHOVER_RETRIES 3
#define WIFI_CHECK_INTERVAL 30000 // 30 secondes
unsigned long lastWifiCheckTime = 0;

void wait_aux() {
  while (digitalRead(AUX) == LOW) {
    delay(1);
  }
  delay(5);
}

void set_programming() {
  digitalWrite(M0, HIGH);
  digitalWrite(M1, HIGH);
  delay(20);
  wait_aux();
}

void set_transparent() {
  digitalWrite(M0, LOW);
  digitalWrite(M1, LOW);
  delay(20);
  wait_aux();
}

void sendCommand(const String& command) {
  wait_aux();
  Serial2.write(command.c_str(), command.length());
  Serial2.flush();
  delay(30);
}

void sendCommand(const byte* command, size_t len) {
  wait_aux();
  Serial2.write(command, len);
  Serial2.flush();
  delay(30);
}

void sendCommandIfMismatch(const byte* expected, size_t len) {
  sendCommand("\xC1\xC1\xC1");
  while (Serial2.available() < 6) {
    delay(10);
  }
  byte currentConfig[6];
  Serial2.readBytes(currentConfig, 6);
  bool mismatch = false;
  for (size_t i = 0; i < len; i++) {
    if (currentConfig[i] != expected[i]) {
      mismatch = true;
      break;
    }
  }
  if (mismatch) {
    Serial.println("Configuration mismatch detected, programming module...");
    sendCommand(expected, len);
  } else {
    Serial.println("Module configuration is already correct.");
  }
}

bool programming_E32() {
  Serial.println("--------------------Program E32 Module---------------------");
  set_programming();
  sendCommand("\xC3\xC3\xC3");
  byte received[4];
  Serial2.readBytes(received, 4);
  const byte* expectedConfig;
  if (received[3] == 0x1E) {
    Serial.println("30dBm module detected.");
    expectedConfig = expected_30dBm;
  } else {
    Serial.println("20dBm module detected.");
    expectedConfig = expected_20dBm;
  }
  sendCommandIfMismatch(expectedConfig, 6);
  set_transparent();
  return true;
}

void checkResetButton() {
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    Serial.println("→ BOUTON RESET APPUYÉ : suppression clés + config WiFiManager...");

    preferences.begin("pushover", false);
    preferences.remove("user");
    preferences.remove("token");
    preferences.end();

    WiFiManager wm;
    wm.resetSettings();

    delay(1000);
    while (digitalRead(RESET_BUTTON_PIN) == LOW) {
      delay(10);
    }

    ESP.restart();
  }
}

void loadPreferences() {
  preferences.begin("pushover", true);
  pushoverUser = preferences.getString("user", "");
  pushoverToken = preferences.getString("token", "");
  preferences.end();
}

bool ensureWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi déconnecté, tentative de reconnexion...");
    WiFi.disconnect();
    WiFi.reconnect();
    
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
      delay(500);
      Serial.print(".");
    }
    
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("\nÉchec de reconnexion WiFi");
      return false;
    }
    Serial.println("\nWiFi reconnecté !");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }
  return true;
}

void startWiFiManager() {
  WiFiManager wm;
  wm.setTimeout(380);
  WiFiManagerParameter custom_user("user", "Pushover User Key", pushoverUser.c_str(), 32);
  WiFiManagerParameter custom_token("token", "Pushover API Token", pushoverToken.c_str(), 32);
  wm.addParameter(&custom_user);
  wm.addParameter(&custom_token);
  
  if (!wm.autoConnect("Mailbox_Config")) {
    Serial.println("Échec de connexion. Redémarrage...");
    delay(3000);
    ESP.restart();
  }
  
  pushoverUser = custom_user.getValue();
  pushoverToken = custom_token.getValue();
  preferences.begin("pushover", false);
  preferences.putString("user", pushoverUser);
  preferences.putString("token", pushoverToken);
  preferences.end();
}

bool sendPushoverNotification(const String& message) {
  if (!ensureWiFiConnection()) {
    Serial.println("Impossible d'envoyer la notification Pushover : WiFi déconnecté");
    return false;
  }

  HTTPClient http;
  bool success = false;
  
  for (int attempt = 1; attempt <= MAX_PUSHOVER_RETRIES; attempt++) {
    Serial.printf("Tentative Pushover %d/%d...\n", attempt, MAX_PUSHOVER_RETRIES);
    
    http.begin("https://api.pushover.net/1/messages.json");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String body = "token=" + String(pushoverToken) +
                  "&user=" + String(pushoverUser) +
                  "&message=" + message +
                  "&title=Alerte Boîte aux lettres";

    int httpCode = http.POST(body);
    if (httpCode == 200) {
      Serial.println("Notification envoyée via Pushover.");
      success = true;
      break;
    } else {
      Serial.printf("Erreur Pushover (tentative %d): %d\n", attempt, httpCode);
      if (attempt < MAX_PUSHOVER_RETRIES) {
        delay(1000 * attempt); // Délai exponentiel entre les tentatives
      }
    }
    http.end();
  }
  
  if (!success) {
    Serial.println("Échec de l'envoi de la notification Pushover après plusieurs tentatives");
  }
  
  return success;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Start program");
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  Serial2.flush();
  pinMode(M0, OUTPUT);
  pinMode(M1, OUTPUT);
  pinMode(AUX, INPUT);
  checkResetButton();
  loadPreferences();

  pushoverToken.trim();
  pushoverUser.trim();

  Serial.println("Contenu récupéré de Preferences :");
  Serial.print("→ pushoverUser = ["); Serial.print(pushoverUser); Serial.println("]");
  Serial.print("→ pushoverToken = ["); Serial.print(pushoverToken); Serial.println("]");
  Serial.print("→ longueur user = "); Serial.println(pushoverUser.length());
  Serial.print("→ longueur token = "); Serial.println(pushoverToken.length());

  if (pushoverUser.length() == 0 || pushoverToken.length() == 0) {
    Serial.println("→ Aucune config Pushover, lancement du portail WiFiManager...");
    startWiFiManager();
  } else {
    WiFi.begin();
    if (!ensureWiFiConnection()) {
      Serial.println("\nConnexion échouée, relance du portail WiFiManager...");
      startWiFiManager();
    }
  }

  Serial.println("\nWiFi connecté !");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Pushover User: ");
  Serial.println(pushoverUser);
  Serial.print("Pushover Token: ");
  Serial.println(pushoverToken);
  programming_E32();
  Serial.println("-----------------------------");
  Serial.println("Initialization finished.");
  Serial.println("-----------------------------");
}

void loop() {
  // Vérification périodique de la connexion WiFi
  if (millis() - lastWifiCheckTime > WIFI_CHECK_INTERVAL) {
    ensureWiFiConnection();
    lastWifiCheckTime = millis();
  }

  if (Serial2.available() > 0) {
    byte receivedCode = Serial2.read();
    Serial.print("Received: 0x");
    Serial.println(receivedCode, HEX);
    if (receivedCode == ARRIVED) {
      mailBoxStatus = fullStatus;
      sendCommand(String((char)ACKNOWLEDGE));
      sendPushoverNotification("📬 Courrier détecté !");
    } else if (receivedCode == EMPTY) {
      mailBoxStatus = emptyStatus;
      sendCommand(String((char)ACKNOWLEDGE));
      sendPushoverNotification("📭 Boîte aux lettres vidée");
    }
  }
  
  checkResetButton();
}