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

// --- DEFINIZIONE PIN ---
#define SDA_PIN 2     // GPIO2 (D4) - I2C SDA
#define SCL_PIN 0     // GPIO0 (D3) - I2C SCL
#define SS_PIN 16     // GPIO16 (D0) - SPI Chip Select per RFID
#define RST_PIN 255   // Pin virtuale, reset gestito via hardware a 3V3
#define ESP_RX_PIN 5  // GPIO5 (D1) - UART RX
#define ESP_TX_PIN 4  // GPIO4 (D2) - UART TX
#define SERVO_PIN 15  // GPIO15 (D8) - Output PWM Servomotore

// --- CONFIGURAZIONE HARDWARE I2C ---
const uint8_t KEYPAD_ADDRESS = 0x20; // Indirizzo base PCF8574
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C          // Indirizzo base SSD1306

// --- ISTANZIAZIONE OGGETTI ---
I2CKeyPad keyPad(KEYPAD_ADDRESS);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
MFRC522 rfid(SS_PIN, RST_PIN);
SoftwareSerial mySerial(ESP_RX_PIN, ESP_TX_PIN);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Servo myServo;

// --- VARIABILI GLOBALI DI STATO ---
String currentPassword = "1234";
String inputBuffer = "";
uint8_t failedAttempts = 0;
bool isLocked = false;

// UID del transponder autorizzato
byte authorizedUID[4] = {0x00, 0x00, 0x00, 0x00};

// --- PROTOTIPI DELLE FUNZIONI ---
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

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n--- INIZIALIZZAZIONE SISTEMA ---");

    // Inizializzazione I2C (Display e Tastierino)
    Wire.begin(SDA_PIN, SCL_PIN);
    if (!keyPad.begin()) {
        Serial.println("[ERRORE] PCF8574 non rilevato.");
        while (1) { delay(100); }
    }
    keyPad.loadKeyMap((char*)"123A456B789C*0#D");

    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println("[ERRORE] SSD1306 non rilevato.");
        while (1) { delay(100); }
    }

    // 1. PRIMA Inizializziamo il bus SPI e l'RFID
    SPI.begin();
    rfid.PCD_Init();

    // 2. DOPO Inizializziamo il Servo, forzando la riassegnazione del pin D8
    myServo.attach(SERVO_PIN);
    myServo.write(160); // Posizione chiusa di sicurezza (evita lo stallo a 180)

    // Inizializzazione Sensore Impronte
    finger.begin(57600);
    if (!finger.verifyPassword()) {
        Serial.println("[ERRORE] Sensore AS608 non rilevato.");
        while (1) { delay(100); }
    }

    Serial.println("\n[OK] Setup completato. Conflitto SPI-Servo risolto.");
    updateDisplay("STANDBY", "PRONTO");

    Serial.println("\n--- MENU COMANDI ---");
    Serial.println("1 - Modifica credenziale PIN");
    Serial.println("2 - Lettura UID transponder NFC");
    Serial.println("3 - Gestione database biometrico");
    Serial.println("4 - Test identificazione impronta");
    Serial.println("5 - Calibrazione gradi servomotore");
    Serial.println("0 - Esecuzione flusso 2FA");
    Serial.println("9 - Override blocco di sicurezza");
}

void loop() {
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
                Serial.println("\n[SISTEMA] Sblocco eseguito.");
                updateDisplay("SBLOCCATO", "STANDBY");
                break;
        }
    }
    checkKeypadInput();
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

// Azionamento motore con limiti meccanici sicuri (20 e 160 gradi)
void triggerServo() {
    Serial.println("\n[ATTUATORE] Posizionamento asse a 20 gradi (Apertura).");
    failedAttempts = 0;

    myServo.write(20);
    delay(4000);

    Serial.println("[ATTUATORE] Posizionamento asse a 160 gradi (Chiusura).");
    myServo.write(160);
    updateDisplay("STANDBY", "");
}

void handleFailure() {
    failedAttempts++;
    Serial.print("\n[SECURITY] Autenticazione fallita: ");
    Serial.print(failedAttempts);
    Serial.println("/3");

    if (failedAttempts >= 3) {
        isLocked = true;
        Serial.println("[SECURITY] Blocco input attivato.");
        updateDisplay("BLOCCATO", "ACC. NEGATO");
    }
}

void test2FA() {
    if (isLocked) {
        Serial.println("\n[SECURITY] Sistema interdetto (Usa opzione 9).");
        updateDisplay("BLOCCATO", "ACC. NEGATO");
        return;
    }

    Serial.println("\n--- INIZIO FLUSSO 2FA ---");
    updateDisplay("FASE 1: PIN", "");

    String tempBuffer = "";
    bool pinUnlocked = false;

    while (!pinUnlocked) {
        if (Serial.available()) { while(Serial.available()) Serial.read(); return; }

        if (keyPad.isPressed()) {
            char key = keyPad.getChar();
            if (key == '#') {
                tempBuffer = "";
                updateDisplay("FASE 1: PIN", tempBuffer);
            }
            else if (key == '*') {
                if (tempBuffer == currentPassword) {
                    Serial.println("[OK] Verifica PIN superata.");
                    pinUnlocked = true;
                } else {
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

    Serial.println("In attesa di transponder o impronta (Fase 2)...");
    updateDisplay("FASE 2", "NFC O DITO");

    while (true) {
        if (Serial.available()) { while(Serial.available()) Serial.read(); return; }

        if (finger.getImage() == FINGERPRINT_OK && finger.image2Tz() == FINGERPRINT_OK) {
            if (finger.fingerSearch() == FINGERPRINT_OK) {
                Serial.println("[OK] Pattern biometrico verificato.");
                updateDisplay("ACCESSO", "CONSENTITO");
                triggerServo();
                break;
            } else {
                updateDisplay("ERRORE", "IMPRONTA NO");
                handleFailure();
                if (isLocked) return;
                delay(2000);
                updateDisplay("FASE 2", "NFC O DITO");
            }
        }

        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            bool rfidMatch = true;
            for (byte i = 0; i < 4; i++) {
                if (rfid.uid.uidByte[i] != authorizedUID[i]) { rfidMatch = false; }
            }

            if (rfidMatch) {
                Serial.println("[OK] Transponder verificato.");
                updateDisplay("ACCESSO", "CONSENTITO");
                triggerServo();
            } else {
                updateDisplay("ERRORE", "NFC ERRATO");
                handleFailure();
                if (isLocked) { rfid.PICC_HaltA(); return; }
                delay(2000);
                updateDisplay("FASE 2", "NFC O DITO");
            }
            rfid.PICC_HaltA();
            break;
        }
        delay(50);
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
                Serial.println("\n[SISTEMA] PIN validato. Digita 0 per 2FA.");
                updateDisplay("PIN OK", "USA OPZ. 0");
            } else {
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
    Serial.println("\n[CONFIG] Inserire nuova sequenza numerica e confermare:");
    updateDisplay("MODIFICA PIN", "ATTESA PC");
    while (!Serial.available()) { delay(10); }
    String newPass = Serial.readStringUntil('\n');
    newPass.trim();
    if (newPass.length() > 0) {
        currentPassword = newPass;
        Serial.println("[OK] Credenziale aggiornata.");
        updateDisplay("PIN SALVATO", "OK");
    }
    delay(2000); updateDisplay("STANDBY", "");
}

void readNFC() {
    Serial.println("\n[NFC] Rilevamento in corso...");
    updateDisplay("LETTURA NFC", "AVVICINA TAG");
    while (!Serial.available()) {
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            Serial.print("UID estratto: ");
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
    updateDisplay("STANDBY", "");
}

void fingerprintMenu() {
    Serial.println("\n--- DATABASE OTTICO ---\n1 - Nuovo template\n2 - Rimuovi template");
    while (!Serial.available()) { delay(10); }
    char subCmd = Serial.read();
    while(Serial.available()) { Serial.read(); }

    if (subCmd == '1') enrollFingerprint();
    else if (subCmd == '2') deleteFingerprint();
    updateDisplay("STANDBY", "");
}

uint8_t getID() {
    uint8_t id = 0;
    while (id == 0) {
        while (!Serial.available()) { delay(10); }
        id = Serial.parseInt();
        while(Serial.available()) { Serial.read(); }
        if (id < 1 || id > 127) {
            Serial.println("[ERRORE] Indice invalido (1-127).");
            id = 0;
        }
    }
    return id;
}

void enrollFingerprint() {
    Serial.println("Definire ID memoria:");
    uint8_t id = getID();

    Serial.println("Appoggiare dito...");
    while (finger.getImage() != FINGERPRINT_OK) { delay(50); }
    if (finger.image2Tz(1) != FINGERPRINT_OK) return;

    Serial.println("Rimuovere dito.");
    delay(2000);
    while (finger.getImage() != FINGERPRINT_NOFINGER) { delay(50); }

    Serial.println("Appoggiare di nuovo...");
    while (finger.getImage() != FINGERPRINT_OK) { delay(50); }
    if (finger.image2Tz(2) != FINGERPRINT_OK) return;

    if (finger.createModel() == FINGERPRINT_OK && finger.storeModel(id) == FINGERPRINT_OK) {
        Serial.println("[OK] Impronta salvata.");
        updateDisplay("SUCCESSO", "SALVATA!");
    } else {
        Serial.println("[ERRORE] Salvataggio fallito.");
    }
    delay(2000);
}

void deleteFingerprint() {
    Serial.println("Definire ID da eliminare:");
    uint8_t id = getID();

    if (finger.deleteModel(id) == FINGERPRINT_OK) {
        Serial.println("[OK] Impronta eliminata.");
        updateDisplay("CANCELLA", "ELIMINATA");
    } else {
        Serial.println("[ERRORE] ID non trovato.");
    }
    delay(2000);
}

void testFingerprint() {
    Serial.println("\n[DIAGNOSTICA] Esecuzione matching...");
    updateDisplay("TEST IMPRONTA", "APPOGGIA DITO");

    while (!Serial.available()) {
        if (finger.getImage() == FINGERPRINT_OK && finger.image2Tz() == FINGERPRINT_OK && finger.fingerSearch() == FINGERPRINT_OK) {
            Serial.print("[MATCH] Trovato ID: ");
            Serial.println(finger.fingerID);
            updateDisplay("MATCH OK", "ID: " + String(finger.fingerID));
            delay(2000);
            updateDisplay("TEST IMPRONTA", "APPOGGIA DITO");
        }
        delay(50);
    }
    while(Serial.available()) { Serial.read(); }
    updateDisplay("STANDBY", "");
}

void setServoDegrees() {
    Serial.println("Valore duty cycle (0-180 gradi):");
    while (!Serial.available()) { delay(10); }
    int degrees = Serial.parseInt();
    while(Serial.available()) { Serial.read(); }

    if (degrees >= 0 && degrees <= 180) {
        Serial.print("Applicazione offset: ");
        Serial.println(degrees);
        myServo.write(degrees);
    }
    updateDisplay("STANDBY", "");
}