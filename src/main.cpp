#include <Arduino.h>
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <Servo.h>
#include <SPI.h>
#include <MFRC522.h>

// --- DEFINIZIONE PIN ---
// Pin AS608 spostati per liberare la SPI hardware
#define ESP_RX_PIN 2  // D4 (Collegato al TX del sensore impronte)
#define ESP_TX_PIN 16 // D0 (Collegato all'RX del sensore impronte)

// Pin Servo
#define SERVO_PIN 5   // D1

// Pin RFID RC522
#define RST_PIN 255   // 255 indica pin non usato via software (collegato fisicamente a 3V3)
#define SS_PIN 4      // D2 (SDA)

// --- INIZIALIZZAZIONE OGGETTI ---
SoftwareSerial mySerial(ESP_RX_PIN, ESP_TX_PIN);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Servo myServo;
MFRC522 rfid(SS_PIN, RST_PIN);
//ciao
// --- PROTOTIPI ---
void printMenu();
uint8_t getID();
void enrollFingerprint();
void deleteFingerprint();
void readNFC();
void verifyAccess();
void resetServo();
void triggerServo();

void setup() {
    Serial.begin(115200);
    delay(100);

    // Setup Servomotore
    myServo.attach(SERVO_PIN);
    myServo.write(0);
    delay(500);

    // Setup Bus SPI e modulo RFID
    SPI.begin();
    rfid.PCD_Init();
    Serial.println("\nModulo RFID-RC522 inizializzato.");

    // Setup Sensore Impronte
    finger.begin(57600);
    if (finger.verifyPassword()) {
        Serial.println("Sensore biometrico AS608 inizializzato.");
    } else {
        Serial.println("Errore hardware: AS608 non rilevato.");
        while (1) { delay(1); }
    }

    printMenu();
}

void loop() {
    if (Serial.available() > 0) {
        char choice = Serial.read();

        while(Serial.available() > 0 && (Serial.peek() == '\r' || Serial.peek() == '\n')) {
            Serial.read();
        }

        switch (choice) {
            case '1': enrollFingerprint(); printMenu(); break;
            case '2': deleteFingerprint(); printMenu(); break;
            case '3': readNFC(); printMenu(); break;
            case '4': verifyAccess(); printMenu(); break;
            case '5': resetServo(); printMenu(); break;
            default: printMenu(); break;
        }
    }
}

void printMenu() {
    Serial.println("\n--- GESTIONE ACCESSI ---");
    Serial.println("1 - Registra nuova impronta");
    Serial.println("2 - Cancella impronta");
    Serial.println("3 - Leggi e stampa UID Tag NFC");
    Serial.println("4 - Testa accesso (Impronta o NFC)");
    Serial.println("5 - Resetta servo a 0 gradi");
    Serial.println("Seleziona (1-5):");
}

uint8_t getID() {
    uint8_t id = 0;
    while (id == 0) {
        while (!Serial.available()) { delay(10); }
        id = Serial.parseInt();
        while(Serial.available()) { Serial.read(); }

        if (id < 1 || id > 127) {
            Serial.println("ID non valido (1-127):");
            id = 0;
        }
    }
    return id;
}

// Funzione helper per l'azionamento della porta
void triggerServo() {
    Serial.println("ACCESSO CONSENTITO: Apertura (90 gradi).");
    myServo.write(190);
    delay(10000);
    Serial.println("Chiusura: Ritorno a 0 gradi.");
    myServo.write(0);
}

void enrollFingerprint() {
    Serial.println("Inserisci ID (1-127) per la nuova impronta:");
    uint8_t id = getID();
    Serial.print("Avvio registrazione ID: "); Serial.println(id);

    Serial.println("Appoggia il dito...");
    while (finger.getImage() != FINGERPRINT_OK) { delay(50); }
    if (finger.image2Tz(1) != FINGERPRINT_OK) return;

    Serial.println("Rimuovi il dito.");
    while (finger.getImage() != FINGERPRINT_NOFINGER) { delay(50); }

    Serial.println("Appoggia nuovamente...");
    while (finger.getImage() != FINGERPRINT_OK) { delay(50); }
    if (finger.image2Tz(2) != FINGERPRINT_OK) return;

    if (finger.createModel() != FINGERPRINT_OK) {
        Serial.println("Le impronte non corrispondono.");
        return;
    }

    if (finger.storeModel(id) == FINGERPRINT_OK) {
        Serial.println("Impronta salvata.");
    } else {
        Serial.println("Errore di scrittura.");
    }
}

void deleteFingerprint() {
    Serial.println("Inserisci ID (1-127) da cancellare:");
    uint8_t id = getID();
    if (finger.deleteModel(id) == FINGERPRINT_OK) {
        Serial.println("Impronta eliminata.");
    } else {
        Serial.println("Errore: ID non trovato.");
    }
}

// Opzione 3: Legge il MAC (UID) del tag in formato HEX e lo stampa
void readNFC() {
    Serial.println("\nAvvicina un tag NFC (invia un carattere per uscire)...");

    while (!Serial.available()) {
        // Rileva presenza di un nuovo tag e legge l'UID
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            Serial.print("UID Tag rilevato: ");
            for (byte i = 0; i < rfid.uid.size; i++) {
                Serial.print(rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
                Serial.print(rfid.uid.uidByte[i], HEX);
            }
            Serial.println();

            // Istruisce la scheda a fermare la trasmissione per evitare letture doppie immediate
            rfid.PICC_HaltA();
            delay(1000); // Debounce
        }
        delay(50);
    }
    while(Serial.available()) { Serial.read(); }
    Serial.println("Ritorno al menu.");
}

// Opzione 4: Acquisizione ibrida (Impronta o Tag NFC sbloccano il servo)
void verifyAccess() {
    Serial.println("\nIn attesa di Impronta o Tag NFC (invia un carattere per annullare)...");

    while (!Serial.available()) {

        // 1. Controllo Impronta
        if (finger.getImage() == FINGERPRINT_OK) {
            if (finger.image2Tz() == FINGERPRINT_OK) {
                if (finger.fingerSearch() == FINGERPRINT_OK) {
                    Serial.print("Match Impronta ID: ");
                    Serial.println(finger.fingerID);
                    triggerServo();
                } else {
                    Serial.println("Impronta non autorizzata.");
                }
            }
            break; // Torna al menu dopo l'azione
        }

        // 2. Controllo Tag NFC
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            Serial.print("Tag rilevato UID: ");
            for (byte i = 0; i < rfid.uid.size; i++) {
                Serial.print(rfid.uid.uidByte[i], HEX);
                Serial.print(" ");
            }
            Serial.println();

            // Accesso consentito con qualsiasi TAG rilevato
            triggerServo();
            rfid.PICC_HaltA();
            break; // Torna al menu dopo l'azione
        }

        delay(50);
    }

    while(Serial.available()) { Serial.read(); }
    Serial.println("Ritorno al menu principale...");
}

void resetServo() {
    Serial.println("Comando: Reset servo a 0 gradi.");
    myServo.write(0);
    delay(500);
}