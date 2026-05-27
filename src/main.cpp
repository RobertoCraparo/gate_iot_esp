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
const char* topic_rfid_response = "cancello/rfid/response"; // Input: Risposta server (REGISTRATO/NON_REGISTRATO)

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
                mqttClient.publish(topic_stato, "SISTEMA_SBLOCCATO_ADMIN");
                Serial.println("[MQTT TX] Topic: cancello/stato | Payload: SISTEMA_SBLOCCATO_ADMIN");
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

    // Disabilita validazione certificati per servizi cloud pubblici
    espClient.setInsecure();
}

void reconnect_mqtt() {
    while (!mqttClient.connected()) {
        Serial.print("[MQTT] Tentativo di connessione a HiveMQ...");
        String clientId = "ESP8266Lock-" + String(WiFi.macAddress());

        if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
            Serial.println(" CONNESSO.");
            mqttClient.publish(topic_stato, "SISTEMA_ONLINE");
            Serial.println("[MQTT TX] Topic: cancello/stato | Payload: SISTEMA_ONLINE");

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

// Router asincrono per payload in ingresso
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message = "";
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }

    Serial.print("[MQTT RX] Messaggio ricevuto su Topic [");
    Serial.print(topic);
    Serial.print("] -> Payload: ");
    Serial.println(message);

    if (String(topic) == topic_comando && message == "APRI") {
        Serial.println("[SISTEMA] Comando di sblocco remoto autorizzato.");
        updateDisplay("APERTURA", "DA REMOTO");
        triggerServo();
    }

    if (String(topic) == topic_rfid_response) {
        serverResponseReceived = true;
        if (message == "REGISTRATO") {
            Serial.println("[MQTT] Validazione server confermata (REGISTRATO).");
            serverAccessGranted = true;
        } else {
            Serial.println("[MQTT] Validazione server respinta (NON_REGISTRATO).");
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
    failedAttempts = 0; // Azzera correttamente la soglia errori a ogni apertura
    mqttClient.publish(topic_stato, "CANCELLO_APERTO");
    Serial.println("[MQTT TX] Topic: cancello/stato | Payload: CANCELLO_APERTO");

    myServo.write(0);
    delay(4000);

    Serial.println("[ATTUATORE] Chiusura. PWM a 160 gradi.");
    mqttClient.publish(topic_stato, "CANCELLO_CHIUSO");
    Serial.println("[MQTT TX] Topic: cancello/stato | Payload: CANCELLO_CHIUSO");
    myServo.write(160);

    // --- FIX RECUPERO HARDWARE I2C ---
    // Re-inizializzazione bus I2C e periferiche per compensare i cali di tensione o rumore del servo
    delay(500); // Pausa di stabilizzazione per permettere l'arresto elettrico del rotore
    Wire.begin(SDA_PIN, SCL_PIN);
    keyPad.begin();
    display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);

    updateDisplay("STANDBY", "");
}

void handleFailure() {
    failedAttempts++;
    Serial.print("\n[SECURITY] Tentativo di accesso negato: ");
    Serial.print(failedAttempts);
    Serial.println("/3");

    mqttClient.publish(topic_stato, "TENTATIVO_FALLITO");
    Serial.println("[MQTT TX] Topic: cancello/stato | Payload: TENTATIVO_FALLITO");

    if (failedAttempts >= 3) {
        isLocked = true;
        Serial.println("[SECURITY] SOGLIA SUPERATA. SISTEMA BLOCCATO.");
        mqttClient.publish(topic_stato, "ALLARME_SISTEMA_BLOCCATO");
        Serial.println("[MQTT TX] Topic: cancello/stato | Payload: ALLARME_SISTEMA_BLOCCATO");
        updateDisplay("BLOCCATO", "ACC. NEGATO");
    }
}

void test2FA() {
    Serial.println("\n[SISTEMA] Modalità 2FA CONTINUA attivata. Invia un carattere qualsiasi a console per uscire.");

    // Loop primario modalità continua
    while (true) {
        if (isLocked) {
            Serial.println("\n[SECURITY] Sistema attualmente inibito causa blocco.");
            updateDisplay("BLOCCATO", "ACC. NEGATO");
            return;
        }

        Serial.println("\n--- AVVIO FLUSSO 2FA ---");
        updateDisplay("FASE 1: PIN", "");

        String tempBuffer = "";
        bool pinUnlocked = false;

        while (!pinUnlocked) {
            mqttClient.loop();
            if (Serial.available()) { while(Serial.available()) Serial.read(); return; }

            if (keyPad.isPressed()) {
                char key = keyPad.getChar();
                if (key == '#') {
                    tempBuffer = "";
                    updateDisplay("FASE 1: PIN", tempBuffer);
                }
                else if (key == '*') {
                    if (tempBuffer == currentPassword) {
                        Serial.println("[SISTEMA] PIN corretto. Primo fattore validato.");
                        pinUnlocked = true;
                    } else {
                        Serial.println("[SISTEMA] Errore: PIN inserito non valido.");
                        updateDisplay("ERRORE", "PIN ERRATO");
                        handleFailure();
                        if (isLocked) return;
                        delay(1500);
                        tempBuffer = "";
                        updateDisplay("FASE 1: PIN", tempBuffer);
                    }
                }
                else if (key != 'N') {
                    tempBuffer += key;
                    updateDisplay("FASE 1: PIN", tempBuffer);
                }
                delay(250);
            }
            delay(10);
        }

        Serial.println("[SISTEMA] In attesa di input biometrico o transponder NFC...");
        updateDisplay("FASE 2", "NFC O DITO");

        // Loop secondario per polling asincrono sensori fisici
        while (true) {
            mqttClient.loop();
            if (Serial.available()) { while(Serial.available()) Serial.read(); return; }

            if (finger.getImage() == FINGERPRINT_OK && finger.image2Tz() == FINGERPRINT_OK) {
                Serial.println("[SENS. OTTICO] Impronta rilevata. Ricerca nel database locale...");
                if (finger.fingerSearch() == FINGERPRINT_OK) {
                    Serial.print("[SISTEMA] Matching riuscito! ID Impronta: ");
                    Serial.println(finger.fingerID);

                    String bioPayload = "ACCESSO_IMPRONTA_ID:" + String(finger.fingerID);
                    mqttClient.publish(topic_stato, bioPayload.c_str());
                    Serial.print("[MQTT TX] Topic: cancello/stato | Payload: ");
                    Serial.println(bioPayload);

                    updateDisplay("ACCESSO", "CONSENTITO");
                    triggerServo();

                    break; // Interrompe loop secondario e riavvia da Fase 1
                } else {
                    Serial.println("[SISTEMA] Matching fallito. Impronta sconosciuta.");
                    updateDisplay("ERRORE", "IMPRONTA NO");
                    handleFailure();
                    if (isLocked) return;
                    delay(2000);
                    updateDisplay("FASE 2", "NFC O DITO");
                }
            }

            if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
                String uidStr = "";
                for (byte i = 0; i < rfid.uid.size; i++) {
                    uidStr += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
                    uidStr += String(rfid.uid.uidByte[i], HEX);
                }
                uidStr.toUpperCase();

                Serial.print("[RFID] Lettura transponder. UID estratto: ");
                Serial.println(uidStr);
                Serial.println("[SISTEMA] Avvio procedura di verifica server-side...");
                updateDisplay("ATTESA SVR", "VERIFICA...");

                serverResponseReceived = false;
                serverAccessGranted = false;

                mqttClient.publish(topic_rfid_check, uidStr.c_str());
                Serial.print("[MQTT TX] Topic: cancello/rfid/check | Payload: ");
                Serial.println(uidStr);

                // Generazione finestra timeout logico di 5000ms
                unsigned long startTime = millis();
                while (!serverResponseReceived && millis() - startTime < 5000) {
                    mqttClient.loop();
                    delay(10);
                }

                if (serverResponseReceived) {
                    if (serverAccessGranted) {
                        Serial.println("[SISTEMA] Il server ha approvato l'accesso.");
                        updateDisplay("ACCESSO", "CONSENTITO");
                        triggerServo();
                        rfid.PICC_HaltA();

                        break;
                    } else {
                        Serial.println("[SISTEMA] Il server ha negato l'accesso. Tessera non registrata.");
                        updateDisplay("NEGATO", "NON REGISTR");
                        handleFailure();
                    }
                } else {
                    Serial.println("[ERRORE] Timeout raggiunto. Il server non ha risposto entro 5 secondi.");
                    updateDisplay("ERRORE", "TIMEOUT SVR");
                    handleFailure();
                }

                if (isLocked) { rfid.PICC_HaltA(); return; }
                delay(2000);
                if (!isLocked) updateDisplay("FASE 2", "NFC O DITO");
                rfid.PICC_HaltA();
            }
            delay(50);
        }
    }
}

void checkKeypadInput() {
    if (isLocked) return;

    if (keyPad.isPressed()) {
        char key = keyPad.getChar();
        if (key == '#') {
            inputBuffer = "";
            updateDisplay("STANDBY", inputBuffer);
        }
        else if (key == '*') {
            if (inputBuffer == currentPassword) {
                Serial.println("\n[SISTEMA] PIN digitato da tastierino corretto. Digitare 0 a console per 2FA.");
                updateDisplay("PIN OK", "USA OPZ. 0");
            } else {
                Serial.println("\n[SISTEMA] PIN digitato da tastierino errato.");
                updateDisplay("PIN ERRATO", "NEGATO");
                handleFailure();
            }
            delay(2000);
            inputBuffer = "";
            if (!isLocked) updateDisplay("STANDBY", inputBuffer);
        }
        else if (key != 'N') {
            inputBuffer += key;
            updateDisplay("INSERIRE PIN", inputBuffer);
        }
        delay(250);
    }
}

void changePasswordSerial() {
    Serial.println("\n[ADMIN] Inserimento nuova password numerica:");
    updateDisplay("MODIFICA PIN", "ATTESA PC");
    while (!Serial.available()) { mqttClient.loop(); delay(10); }
    String newPass = Serial.readStringUntil('\n');
    newPass.trim();
    if (newPass.length() > 0) {
        currentPassword = newPass;
        Serial.print("[ADMIN] Password modificata con successo in: ");
        Serial.println(currentPassword);
        updateDisplay("PIN SALVATO", "OK");
    }
    delay(2000); updateDisplay("STANDBY", "");
}

void readNFC() {
    Serial.println("\n[DIAGNOSTICA] Modalità scansione RFID continua attivata. Avvicinare transponder.");
    updateDisplay("LETTURA NFC", "AVVICINA TAG");
    while (!Serial.available()) {
        mqttClient.loop();
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            Serial.print("[RFID] Dump RAW UID (Hex): ");
            for (byte i = 0; i < rfid.uid.size; i++) {
                Serial.print("0x");
                Serial.print(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
                Serial.print(rfid.uid.uidByte[i], HEX);
                if (i < rfid.uid.size -1) Serial.print(", ");
            }
            Serial.println();
            rfid.PICC_HaltA();
            delay(2000);
        }
        delay(50);
    }
    while(Serial.available()) { Serial.read(); }
    Serial.println("[DIAGNOSTICA] Uscita modalità scansione.");
    updateDisplay("STANDBY", "");
}

void fingerprintMenu() {
    Serial.println("\n--- GESTIONE DATABASE BIOMETRICO ---");
    Serial.println("1 - Compila nuovo template e salva");
    Serial.println("2 - Rimuovi indice di memoria");
    while (!Serial.available()) { mqttClient.loop(); delay(10); }
    char subCmd = Serial.read();
    while(Serial.available()) { Serial.read(); }

    if (subCmd == '1') enrollFingerprint();
    else if (subCmd == '2') deleteFingerprint();
    updateDisplay("STANDBY", "");
}

uint8_t getID() {
    uint8_t id = 0;
    while (id == 0) {
        while (!Serial.available()) { mqttClient.loop(); delay(10); }
        id = Serial.parseInt();
        while(Serial.available()) { Serial.read(); }
        if (id < 1 || id > 127) {
            Serial.println("[ERRORE] Indice fuori limite. Inserire un valore da 1 a 127.");
            id = 0;
        }
    }
    return id;
}

void enrollFingerprint() {
    Serial.println("[ADMIN] Inserire indice di memoria (1-127) per salvataggio impronta:");
    uint8_t id = getID();
    Serial.print("[ADMIN] Indice selezionato: "); Serial.println(id);

    Serial.println("[SENS. OTTICO] Posizionare il dito sul sensore...");
    while (finger.getImage() != FINGERPRINT_OK) { mqttClient.loop(); delay(50); }
    if (finger.image2Tz(1) != FINGERPRINT_OK) {
        Serial.println("[ERRORE] Impossibile estrarre le minuzie (Passaggio 1). Riprovare.");
        return;
    }

    Serial.println("[SENS. OTTICO] Scansione 1 OK. Sollevare il dito.");
    delay(2000);
    while (finger.getImage() != FINGERPRINT_NOFINGER) { mqttClient.loop(); delay(50); }

    Serial.println("[SENS. OTTICO] Riposizionare lo stesso dito per il consolidamento del pattern...");
    while (finger.getImage() != FINGERPRINT_OK) { mqttClient.loop(); delay(50); }
    if (finger.image2Tz(2) != FINGERPRINT_OK) {
        Serial.println("[ERRORE] Impossibile estrarre le minuzie (Passaggio 2). Riprovare.");
        return;
    }

    Serial.println("[SENS. OTTICO] Elaborazione e fusione dei template in corso...");
    if (finger.createModel() == FINGERPRINT_OK && finger.storeModel(id) == FINGERPRINT_OK) {
        Serial.println("[SUCCESSO] Impronta archiviata permanentemente in memoria NVRAM.");
        updateDisplay("SUCCESSO", "SALVATA!");
    } else {
        Serial.println("[ERRORE] I due campioni non corrispondono o memoria insufficiente.");
    }
    delay(2000);
}

void deleteFingerprint() {
    Serial.println("[ADMIN] Inserire l'indice di memoria da formattare:");
    uint8_t id = getID();

    if (finger.deleteModel(id) == FINGERPRINT_OK) {
        Serial.print("[SUCCESSO] Indice di memoria "); Serial.print(id); Serial.println(" sovrascritto e liberato.");
        updateDisplay("CANCELLA", "ELIMINATA");
    } else {
        Serial.println("[ERRORE] Indice vuoto o operazione fallita.");
    }
    delay(2000);
}

void testFingerprint() {
    Serial.println("\n[DIAGNOSTICA] Loop di test matching biometrico attivato. Posizionare un dito.");
    updateDisplay("TEST IMPRONTA", "APPOGGIA DITO");

    while (!Serial.available()) {
        mqttClient.loop();
        if (finger.getImage() == FINGERPRINT_OK && finger.image2Tz() == FINGERPRINT_OK) {
            if(finger.fingerSearch() == FINGERPRINT_OK) {
                Serial.print("[SUCCESSO] Dito riconosciuto. Indice in memoria: ");
                Serial.println(finger.fingerID);
                updateDisplay("MATCH OK", "ID: " + String(finger.fingerID));
            } else {
                Serial.println("[FALLIMENTO] Impronta non presente nel database locale.");
                updateDisplay("ERRORE", "NON TROVATA");
            }
            delay(2000);
            updateDisplay("TEST IMPRONTA", "APPOGGIA DITO");
        }
        delay(50);
    }
    while(Serial.available()) { Serial.read(); }
    Serial.println("[DIAGNOSTICA] Uscita modalità test.");
    updateDisplay("STANDBY", "");
}

void setServoDegrees() {
    Serial.println("[DIAGNOSTICA] Inserire valore PWM assoluto in gradi (0-180):");
    while (!Serial.available()) { mqttClient.loop(); delay(10); }
    int degrees = Serial.parseInt();
    while(Serial.available()) { Serial.read(); }

    if (degrees >= 0 && degrees <= 180) {
        Serial.print("[ATTUATORE] Segnale forzato a gradi: ");
        Serial.println(degrees);
        myServo.write(degrees);
    } else {
        Serial.println("[ERRORE] Valore fuori limite fisico.");
    }
    updateDisplay("STANDBY", "");
}