#include <Arduino.h>
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <Servo.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <I2CKeyPad.h>

// --- DEFINIZIONE PIN OTTIMIZZATA ---
// Tastierino I2C allocato sui pin di boot (necessitano di essere HIGH all'avvio)
#define SDA_PIN 2  // D4 (GPIO2)
#define SCL_PIN 0  // D3 (GPIO0)
const uint8_t KEYPAD_ADDRESS = 0x20; // Indirizzo I2C dell'expander PCF8574

// RFID RC522 allocato sulla SPI Hardware
#define SS_PIN 16  // D0 (GPIO16) - Slave Select
#define RST_PIN 255 // Hardwired a 3.3V per liberare pin

// Servomotore allocato sul pin di boot LOW
#define SERVO_PIN 15 // D8 (GPIO15) - Pin con pull-down, perfetto per input passivi

// Sensore Impronte AS608 allocato sui pin stabili
#define ESP_RX_PIN 5 // D1 (Collegato al TX del sensore)
#define ESP_TX_PIN 4 // D2 (Collegato all'RX del sensore)

// --- ISTANZIAZIONE CLASSI ---
SoftwareSerial mySerial(ESP_RX_PIN, ESP_TX_PIN);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Servo myServo;
MFRC522 rfid(SS_PIN, RST_PIN);
I2CKeyPad keyPad(KEYPAD_ADDRESS);

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

    // Setup I2C custom sui pin D3/D4
    Wire.begin(SDA_PIN, SCL_PIN);
    if (!keyPad.begin()) {
        Serial.println("\n[ERRORE] Tastierino I2C non rilevato.");
    } else {
        keyPad.loadKeyMap("123A456B789C*0#D");
    }

    // Setup Servomotore
    myServo.attach(SERVO_PIN);
    myServo.write(0);
    delay(500);

    // Setup SPI e modulo RFID
    SPI.begin();
    rfid.PCD_Init();

    // Setup Sensore Impronte tramite porta seriale virtuale
    finger.begin(57600);
    if (!finger.verifyPassword()) {
        Serial.println("\n[ERRORE] AS608 non rilevato. Verifica i cavi su D1/D2.");
        while (1) { delay(1); }
    }

    Serial.println("\nInizializzazione di tutti i moduli completata.");
    printMenu();
}

void loop() {
    // Intercettazione comandi da Monitor Seriale
    if (Serial.available() > 0) {
        char choice = Serial.read();

        // Pulizia del buffer dai ritorni a capo (\n o \r)
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

// Stampa del menu operativo
void printMenu() {
    Serial.println("\n--- SISTEMA DI ACCESSO MULTI-FATTORE ---");
    Serial.println("1 - Registra nuova impronta");
    Serial.println("2 - Cancella impronta");
    Serial.println("3 - Lettura raw UID Tag NFC");
    Serial.println("4 - Avvia Accesso (PIN + Impronta/NFC)");
    Serial.println("5 - Resetta servomotore a 0 gradi");
    Serial.println("Selezione (1-5):");
}

// Acquisizione sicura di un ID numerico
uint8_t getID() {
    uint8_t id = 0;
    while (id == 0) {
        while (!Serial.available()) { delay(10); }
        id = Serial.parseInt();
        while(Serial.available()) { Serial.read(); }

        if (id < 1 || id > 127) {
            Serial.println("Errore: ID fuori range. Reinserire (1-127):");
            id = 0;
        }
    }
    return id;
}

// Sequenza di sblocco serratura
void triggerServo() {
    Serial.println("Sblocco in corso (rotazione 90 gradi).");
    myServo.write(90);
    delay(3000); // Mantiene l'apertura per 3 secondi
    Serial.println("Blocco in corso (rotazione 0 gradi).");
    myServo.write(0);
}

// Registrazione di un nuovo template biometrico in memoria flash
void enrollFingerprint() {
    Serial.println("ID (1-127) da registrare:");
    uint8_t id = getID();
    Serial.print("Avvio procedura per ID: "); Serial.println(id);

    Serial.println("Appoggia il dito sul prisma...");
    while (finger.getImage() != FINGERPRINT_OK) { delay(50); }
    if (finger.image2Tz(1) != FINGERPRINT_OK) return;

    Serial.println("Rimuovi il dito.");
    while (finger.getImage() != FINGERPRINT_NOFINGER) { delay(50); }

    Serial.println("Appoggia di nuovo lo stesso dito...");
    while (finger.getImage() != FINGERPRINT_OK) { delay(50); }
    if (finger.image2Tz(2) != FINGERPRINT_OK) return;

    // Generazione del modello tramite unione delle due acquisizioni
    if (finger.createModel() != FINGERPRINT_OK) {
        Serial.println("Scansioni non coincidenti. Procedura annullata.");
        return;
    }

    if (finger.storeModel(id) == FINGERPRINT_OK) {
        Serial.println("Template biometrico salvato con successo.");
    } else {
        Serial.println("Errore di scrittura nella memoria del sensore.");
    }
}

// Eliminazione di un template biometrico
void deleteFingerprint() {
    Serial.println("ID (1-127) da eliminare:");
    uint8_t id = getID();
    if (finger.deleteModel(id) == FINGERPRINT_OK) {
        Serial.println("Template eliminato.");
    } else {
        Serial.println("Errore: ID inesistente.");
    }
}

// Polling continuo per lettura del MAC Address (UID) del transponder
void readNFC() {
    Serial.println("\nIn attesa di transponder (invia un carattere per annullare)...");

    while (!Serial.available()) {
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            Serial.print("UID Rilevato: ");
            for (byte i = 0; i < rfid.uid.size; i++) {
                Serial.print(rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
                Serial.print(rfid.uid.uidByte[i], HEX);
            }
            Serial.println();

            rfid.PICC_HaltA(); // Inibisce letture multiple immediate
            delay(1000);
        }
        delay(50);
    }
    while(Serial.available()) { Serial.read(); }
    Serial.println("Uscita dalla modalità lettura.");
}

// Azzeramento forzato della posizione del motore
void resetServo() {
    Serial.println("Esecuzione riposizionamento forzato a 0 gradi.");
    myServo.write(0);
    delay(500);
}

// Routine di Autenticazione 2FA (What You Know + What You Have/Are)
void verifyAccess() {
    Serial.println("\n--- FASE 1: IMMISSIONE PIN ---");
    Serial.println("Tastierino: Digita PIN. '*' per confermare, '#' per resettare input.");

    String enteredPIN = "";
    bool pinUnlocked = false;

    // Loop validazione credenziale statica
    while (!pinUnlocked) {
        if (Serial.available()) {
            while(Serial.available()) Serial.read();
            Serial.println("Autenticazione interrotta dall'host.");
            return;
        }

        if (keyPad.isPressed()) {
            char key = keyPad.getChar();

            if (key == '#') {
                enteredPIN = "";
                Serial.println("\nInput resettato. Digita di nuovo:");
            }
            else if (key == '*') {
                // Hardcoded per testing. In produzione valutare hashing.
                if (enteredPIN == "1234") {
                    Serial.println("\n[OK] PIN Validato.");
                    pinUnlocked = true;
                } else {
                    Serial.println("\n[ERRORE] PIN Non Corretto. Riprovare:");
                    enteredPIN = "";
                }
            }
            else if (key != 'N') {
                enteredPIN += key;
                Serial.print("*"); // Offuscamento input su console
            }
            delay(250); // Debouncing software per tastiera a matrice
        }
        delay(10);
    }

    Serial.println("\n--- FASE 2: VERIFICA BIOMETRICA O TOKEN ---");
    Serial.println("Esporre tag RFID autorizzato o impronta digitale...");

    // Polling concorrente su UART (AS608) e SPI (RC522)
    while (true) {
        if (Serial.available()) {
            while(Serial.available()) Serial.read();
            Serial.println("Autenticazione interrotta dall'host.");
            return;
        }

        // Branch 1: Controllo Biometrico
        if (finger.getImage() == FINGERPRINT_OK) {
            if (finger.image2Tz() == FINGERPRINT_OK) {
                if (finger.fingerSearch() == FINGERPRINT_OK) {
                    Serial.print("[OK] Match Biometrico - ID Utente: ");
                    Serial.println(finger.fingerID);
                    triggerServo();
                    break;
                } else {
                    Serial.println("[ERRORE] Impronta non autorizzata.");
                }
            }
            delay(1000);
        }

        // Branch 2: Controllo Token RFID
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            Serial.print("[OK] Token Rilevato - UID: ");
            for (byte i = 0; i < rfid.uid.size; i++) {
                Serial.print(rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
                Serial.print(rfid.uid.uidByte[i], HEX);
            }
            Serial.println();

            triggerServo();
            rfid.PICC_HaltA();
            break;
        }
        delay(50);
    }

    Serial.println("Ritorno in standby.");
}