#include <Arduino.h>
#include <Wire.h>
#include <I2CKeyPad.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <MFRC522.h>
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <Servo.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h> // Nuova libreria per parsing e formattazione JSON

// --- CONFIGURAZIONE RETE E MQTT ---
const char* ssid = "TP-Link-Pellizzari";
const char* password = "IWdope23M.S";

// Credenziali cluster HiveMQ Cloud
const char* mqtt_server = "4a9747976f4d480f8994db0b416ae029.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "Utente";
const char* mqtt_pass = "Password1";

// Definizione Topic MQTT
const char* topic_stato = "cancello/stato";             // Output: Telemetria e ID impronte
const char* topic_comando = "cancello/comando";         // Input: Comandi di apertura remota
const char* topic_rfid_check = "cancello/rfid/check";   // Output: Invio UID al server
const char* topic_rfid_response = "cancello/rfid/response"; // Input: Risposta server

// --- DEFINIZIONE PIN ---
#define SDA_PIN 2     // GPIO2 (D4) - I2C SDA
#define SCL_PIN 0     // GPIO0 (D3) - I2C SCL
#define SS_PIN 16     // GPIO16 (D0) - SPI Chip Select RFID
#define RST_PIN 255   // Reset hardware disabilitato (pin a 3V3)
#define ESP_RX_PIN 5  // GPIO5 (D1) - UART RX (da sensore TX)
#define ESP_TX_PIN 4  // GPIO4 (D2) - UART TX (a sensore RX)
#define SERVO_PIN 15  // GPIO15 (D8) - PWM Servomotore

// --- CONFIGURAZIONE HARDWARE I2C ---
const uint8_t KEYPAD_ADDRESS = 0x20;
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

// --- ISTANZIAZIONE OGGETTI ---
I2CKeyPad keyPad(KEYPAD_ADDRESS);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
MFRC522 rfid(SS_PIN, RST_PIN);
SoftwareSerial mySerial(ESP_RX_PIN, ESP_TX_PIN);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Servo myServo;
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

// --- VARIABILI GLOBALI ---
String currentPassword = "1234";
String inputBuffer = "";
uint8_t failedAttempts = 0;
bool isLocked = false;

// Flag sincronizzazione asincrona MQTT
volatile bool serverResponseReceived = false;
volatile bool serverAccessGranted = false;

// --- PROTOTIPI ---
void changePasswordSerial();
void checkKeypadInput();
void updateDisplay(String header, String content);
void readNFC();
void test2FA();
void fingerprintMenu();
void enrollFingerprint();
void deleteFingerprint();
void testFingerprint();
void setServoDegrees();
void triggerServo();
void handleFailure();
uint8_t getID();
void setup_wifi();
void reconnect_mqtt();
void mqttCallback(char* topic, byte* payload, unsigned int length);

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n--- INIZIALIZZAZIONE SISTEMA ---");

    setup_wifi();
    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setCallback(mqttCallback);

    Wire.begin(SDA_PIN, SCL_PIN);
    if (!keyPad.begin()) {
        Serial.println("[ERRORE] PCF8574 non rilevato sul bus I2C.");
        while (1) { delay(100); }
    }
    keyPad.loadKeyMap((char*)"123A456B789C*0#D");
    Serial.println("[OK] Tastierino I2C caricato.");

    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println("[ERRORE] SSD1306 non rilevato sul bus I2C.");
        while (1) { delay(100); }
    }
    Serial.println("[OK] Display OLED caricato.");

    // Configurazione pin hardware per prevenire blocchi bus SPI
    pinMode(SS_PIN, OUTPUT);
    digitalWrite(SS_PIN, HIGH);

    SPI.begin();
    rfid.PCD_Init();
    Serial.println("[OK] Modulo RFID inizializzato.");

    myServo.attach(SERVO_PIN);
    myServo.write(160);
    Serial.println("[OK] Attuatore in posizione di chiusura.");

    finger.begin(57600);
    if (!finger.verifyPassword()) {
        Serial.println("[ERRORE] Sensore AS608 non rilevato.");
        while (1) { delay(100); }
    }
    Serial.println("[OK] Sensore biometrico caricato.");

    Serial.println("\n[SISTEMA] Setup completato. Dispositivo pronto.");
    updateDisplay("STANDBY", "PRONTO");
}

void loop() {
    if (!mqttClient.connected()) {
        reconnect_mqtt();
    }
    mqttClient.loop();

    if (Serial.available() > 0) {
        char cmd = Serial.read();
        while(Serial.available() > 0 && (Serial.peek() == '\r' || Serial.peek() == '\n')) {
            Serial.read();
        }

        switch (cmd) {
            case '1': changePasswordSerial(); break;
            case '2': readNFC(); break;
            case '3': fingerprintMenu(); break;
            case '4': testFingerprint(); break;
            case '5': setServoDegrees(); break;
            case '0': test2FA(); break;
            case '9':
                isLocked = false;
                failedAttempts = 0;
                Serial.println("\n[ADMIN] Sblocco forzato eseguito. Contatori azzerati.");

                String payload = "{\"event\":\"system\",\"status\":\"unlocked_admin\"}";
                mqttClient.publish(topic_stato, payload.c_str());
                Serial.print("[MQTT TX] Topic: cancello/stato | Payload JSON: ");
                Serial.println(payload);

                updateDisplay("SBLOCCATO", "STANDBY");
                break;
        }
    }
    checkKeypadInput();
}

void setup_wifi() {
    Serial.print("\n[WIFI] Connessione a: ");
    Serial.println(ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[WIFI] Connesso. IP assegnato: ");
    Serial.println(WiFi.localIP());

    espClient.setInsecure();
}

void reconnect_mqtt() {
    while (!mqttClient.connected()) {
        Serial.print("[MQTT] Tentativo di connessione a HiveMQ...");
        String clientId = "ESP8266Lock-" + String(WiFi.macAddress());

        if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
            Serial.println(" CONNESSO.");

            String payload = "{\"event\":\"system\",\"status\":\"online\"}";
            mqttClient.publish(topic_stato, payload.c_str());
            Serial.print("[MQTT TX] Topic: cancello/stato | Payload JSON: ");
            Serial.println(payload);

            mqttClient.subscribe(topic_comando);
            mqttClient.subscribe(topic_rfid_response);
            Serial.println("[MQTT] Sottoscrizione ai topic di input completata.");
        } else {
            Serial.print(" FALLITO. Stato errore: ");
            Serial.print(mqttClient.state());
            Serial.println(". Riprovo in 5 secondi.");
            delay(5000);
        }
    }
}

// Router asincrono per payload JSON in ingresso
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    // Conversione sicura in stringa null-terminated per il parsing JSON
    char jsonBuffer[length + 1];
    for (unsigned int i = 0; i < length; i++) {
        jsonBuffer[i] = (char)payload[i];
    }
    jsonBuffer[length] = '\0';

    Serial.print("[MQTT RX] Messaggio ricevuto su Topic [");
    Serial.print(topic);
    Serial.print("] -> Payload JSON: ");
    Serial.println(jsonBuffer);

    // Parsing del JSON in ingresso
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, jsonBuffer);

    if (error) {
        Serial.print("[MQTT RX] Errore di decodifica JSON: ");
        Serial.println(error.c_str());
        return;
    }

    // Gestione Comando di Apertura
    if (String(topic) == topic_comando) {
        const char* cmd = doc["command"]; // Cerca la chiave "command"
        if (cmd && String(cmd) == "APRI") {
            Serial.println("[SISTEMA] Comando di sblocco remoto JSON autorizzato.");
            updateDisplay("APERTURA", "DA REMOTO");
            triggerServo();
        }
    }

    // Gestione Risposta Validazione RFID
    if (String(topic) == topic_rfid_response) {
        serverResponseReceived = true;
        const char* status = doc["status"]; // Cerca la chiave "status"

        if (status && String(status) == "REGISTRATO") {
            Serial.println("[MQTT] Validazione server confermata (REGISTRATO).");
            serverAccessGranted = true;
        } else {
            Serial.println("[MQTT] Validazione server respinta.");
            serverAccessGranted = false;
        }
    }
}

void updateDisplay(String header, String content) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 5);
    display.print(header);
    display.setTextSize(2);
    display.setCursor(0, 30);
    display.print(content);
    display.display();
}

void triggerServo() {
    Serial.println("\n[ATTUATORE] Apertura. PWM a 0 gradi.");
    failedAttempts = 0;

    String payloadOpen = "{\"event\":\"gate\",\"status\":\"opened\"}";
    mqttClient.publish(topic_stato, payloadOpen.c_str());
    Serial.print("[MQTT TX] Topic: cancello/stato | Payload JSON: ");
    Serial.println(payloadOpen);

    myServo.write(0);
    delay(4000);

    Serial.println("[ATTUATORE] Chiusura. PWM a