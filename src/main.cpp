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
#define SS_PIN 16     // GPIO16 (D0) - SPI Chip Select
#define RST_PIN 255   // Pin virtuale, reset gestito via 3V3
#define ESP_RX_PIN 5  // GPIO5 (D1) - UART RX (collegare a TX AS608)
#define ESP_TX_PIN 4  // GPIO4 (D2) - UART TX (collegare a RX AS608)
#define SERVO_PIN 15  // GPIO15 (D8) - Output PWM Servomotore

// --- CONFIGURAZIONE HARDWARE I2C ---
const uint8_t KEYPAD_ADDRESS = 0x20; // Indirizzo I/O Expander PCF8574
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1                // Nessun pin di reset hardware
#define SCREEN_ADDRESS 0x3C          // Indirizzo base display SSD1306

// --- ISTANZIAZIONE OGGETTI ---
I2CKeyPad keyPad(KEYPAD_ADDRESS);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
MFRC522 rfid(SS_PIN, RST_PIN);
SoftwareSerial mySerial(ESP_RX_PIN, ESP_TX_PIN);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Servo myServo;

// --- VARIABILI GLOBALI DI STATO ---
String currentPassword = "1234"; // Credenziale numerica in RAM
String inputBuffer = "";         // Buffer acquisizione tastierino
uint8_t failedAttempts = 0;      // Contatore policy anti-bruteforce
bool isLocked = false;           // Flag di blocco sistema

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
    // Inizializzazione interfaccia UART per debug
    Serial.begin(115200);
    delay(100);

    Serial.println("\n\n--- INIZIALIZZAZIONE SISTEMA ---");

    // Avvio bus I2C
    Wire.begin(SDA_PIN, SCL_PIN);

    // Inizializzazione matrice tastierino
    if (!keyPad.begin()) {
        Serial.println("[ERRORE] PCF8574 non rilevato sul bus I2C.");
        while (1) { delay(100); }
    }
    keyPad.loadKeyMap((char*)"123A456B789C*0#D");

    // Inizializzazione controller display
    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println("[ERRORE] SSD1306 non rilevato sul bus I2C.");
        while (1) { delay(100); }
    }

    // Configurazione segnale PWM e posizionamento di riposo (Chiuso)
    myServo.attach(SERVO_PIN);
    myServo.write(180);

    // Inizializzazione bus SPI e modulo lettore RFID
    SPI.begin();
    rfid.PCD_Init();

    // Inizializzazione sensore ottico via UART software (Baudrate 57600)
    finger.begin(57600);
    if (!finger.verifyPassword()) {
        Serial.println("[ERRORE] Sensore AS608 non rilevato. Verificare collegamenti TX/RX.");
        while (1) { delay(100); }
    }

    Serial.println("\n[OK] Setup periferiche completato.");
    updateDisplay("STANDBY", "PRONTO");

    // Stampa menu diagnostico console
    Serial.println("\n--- MENU COMANDI (CONSOLE) ---");
    Serial.println("1 - Modifica credenziale PIN");
    Serial.println("2 - Lettura UID transponder NFC");
    Serial.println("3 - Gestione database biometrico");
    Serial.println("4 - Test identificazione impronta");
    Serial.println("5 - Calibrazione duty cycle servomotore");
    Serial.println("0 - Esecuzione flusso 2FA");
    Serial.println("9 - Override blocco di sicurezza (Reset)");
}

void loop() {
    // Polling interfaccia UART seriale
    if (Serial.available() > 0) {
        char cmd = Serial.read();

        // Svuotamento buffer da terminatori CR/LF
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
                Serial.println("\n[SISTEMA] Sblocco forzato eseguito. Contatori azzerati.");
                updateDisplay("SBLOCCATO", "STANDBY");
                break;
        }
    }

    // Polling asincrono matrice tastierino
    checkKeypadInput();
}

// Scrittura buffer video su memoria display OLED
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

// Esecuzione sequenza PWM per azionamento meccanico serratura
void triggerServo() {
    Serial.println("\n[ATTUATORE] Posizionamento asse a 0 gradi (Apertura).");
    failedAttempts = 0; // Reset policy sicurezza dopo accesso valido

    myServo.write(0);
    delay(4000); // Ritardo per mantenimento stato aperto

    Serial.println("[ATTUATORE] Posizionamento asse a 180 gradi (Chiusura).");
    myServo.write(180);
    updateDisplay("STANDBY", "");
}

// Gestione incrementale eccezioni e blocco logico sistema
void handleFailure() {
    failedAttempts++;
    Serial.print("\n[SECURITY] Autenticazione fallita: ");
    Serial.print(failedAttempts);
    Serial.println("/3");

    if (failedAttempts >= 3) {
        isLocked = true;
        Serial.println("[SECURITY] Soglia tolleranza superata. Blocco input attivato.");
        updateDisplay("BLOCCATO", "ACC. NEGATO");
    }
}

// Flusso logico di autenticazione a due fattori (Fattore 1: PIN, Fattore 2: Biometria/NFC)
void test2FA() {
    if (isLocked) {
        Serial.println("\n[SECURITY] Sistema interdetto. Inviare comando override (9).");
        updateDisplay("BLOCCATO", "ACC. NEGATO");
        return;
    }

    Serial.println("\n--- INIZIO FLUSSO 2FA ---");
    updateDisplay("FASE 1: PIN", "");

    String tempBuffer = "";
    bool pinUnlocked = false;

    // Acquisizione e validazione fattore numerico (Fase 1)
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

    Serial.println("In attesa di transponder valido o pattern biometrico (Fase 2)...");
    updateDisplay("FASE 2", "NFC O DITO");

    // Loop polling concorrente sensori fisici (Fase 2)
    while (true) {
        if (Serial.available()) { while(Serial.available()) Serial.read(); return; }

        // Valutazione buffer sensore ottico
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

        // Valutazione buffer stack SPI per MFRC522
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            bool rfidMatch = true;
            for (byte i = 0; i < 4; i++) {
                if (rfid.uid.uidByte[i] != authorizedUID[i]) { rfidMatch = false; }
            }

            if (rfidMatch) {
                Serial.println("[OK] Identificativo transponder verificato.");
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

// Acquisizione ciclica input matrice I2C
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
                Serial.println("\n[SISTEMA] PIN validato. Richiedere esecuzione 2FA (0).");
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

// Aggiornamento registro variabile credenziale utente
void changePasswordSerial() {
    Serial.println("\n[CONFIG] Inserire nuova sequenza numerica e confermare:");
    updateDisplay("MODIFICA PIN", "ATTESA PC");
    while (!Serial.available()) { delay(10); }
    String newPass = Serial.readStringUntil('\n');
    newPass.trim();
    if (newPass.length() > 0) {
        currentPassword = newPass;
        Serial.println("[OK] Buffer credenziale aggiornato.");
        updateDisplay("PIN SALVATO", "OK");
    }
    delay(2000); updateDisplay("STANDBY", "");
}

// Estrazione dump esadecimale da tag MIFARE/NTAG
void readNFC() {
    Serial.println("\n[NFC] Rilevamento campo magnetico in corso...");
    updateDisplay("LETTURA NFC", "AVVICINA TAG");
    while (!Serial.available()) {
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            Serial.print("Vettore UID estratto: ");
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

// Menu contestuale gestione flash memory AS608
void fingerprintMenu() {
    Serial.println("\n--- GESTIONE DATABASE OTTICO ---\n1 - Allocazione nuovo template\n2 - Deallocazione template");
    while (!Serial.available()) { delay(10); }
    char subCmd = Serial.read();
    while(Serial.available()) { Serial.read(); }

    if (subCmd == '1') enrollFingerprint();
    else if (subCmd == '2') deleteFingerprint();

    updateDisplay("STANDBY", "");
}

// Parsing indice per allocazione database biometrico
uint8_t getID() {
    uint8_t id = 0;
    while (id == 0) {
        while (!Serial.available()) { delay(10); }
        id = Serial.parseInt();
        while(Serial.available()) { Serial.read(); }
        if (id < 1 || id > 127) {
            Serial.println("[ERRORE] Indice non valido. Range consentito: 1-127.");
            id = 0;
        }
    }
    return id;
}

// Acquisizione topologica punti caratteristici e generazione modello biometrico
void enrollFingerprint() {
    Serial.println("Definire indirizzo di memoria (ID):");
    uint8_t id = getID();

    Serial.println("Richiesta esposizione sensore ottico (Acquisizione 1)...");
    while (finger.getImage() != FINGERPRINT_OK) { delay(50); }
    if (finger.image2Tz(1) != FINGERPRINT_OK) return;

    Serial.println("Rimuovere superficie dal sensore.");
    delay(2000);
    while (finger.getImage() != FINGERPRINT_NOFINGER) { delay(50); }

    Serial.println("Richiesta esposizione sensore ottico (Acquisizione 2 - Verifica)...");
    while (finger.getImage() != FINGERPRINT_OK) { delay(50); }
    if (finger.image2Tz(2) != FINGERPRINT_OK) return;

    // Fusione dei due buffer e salvataggio permanente in NVRAM
    if (finger.createModel() == FINGERPRINT_OK && finger.storeModel(id) == FINGERPRINT_OK) {
        Serial.println("[OK] Template biometrico validato e memorizzato.");
        updateDisplay("SUCCESSO", "SALVATA!");
    } else {
        Serial.println("[ERRORE] Fallimento calcolo minuzie o memoria insufficiente.");
    }
    delay(2000);
}

// Rimozione modello da indirizzo logico in memoria flash
void deleteFingerprint() {
    Serial.println("Definire indirizzo di memoria (ID) da liberare:");
    uint8_t id = getID();

    if (finger.deleteModel(id) == FINGERPRINT_OK) {
        Serial.println("[OK] Blocco memoria liberato.");
        updateDisplay("CANCELLA", "ELIMINATA");
    } else {
        Serial.println("[ERRORE] Indirizzo logico vuoto o invalido.");
    }
    delay(2000);
}

// Validazione asincrona impronta contro database NVRAM
void testFingerprint() {
    Serial.println("\n[DIAGNOSTICA] Esecuzione algoritmo di matching 1:N...");
    updateDisplay("TEST IMPRONTA", "APPOGGIA DITO");

    while (!Serial.available()) {
        if (finger.getImage() == FINGERPRINT_OK && finger.image2Tz() == FINGERPRINT_OK && finger.fingerSearch() == FINGERPRINT_OK) {
            Serial.print("[MATCH] Pattern corrispondente all'indirizzo logico ID: ");
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

// Modifica diretta valore PWM per calibrazione escursione asse
void setServoDegrees() {
    Serial.println("Specificare valore duty cycle (0-180 gradi):");
    while (!Serial.available()) { delay(10); }
    int degrees = Serial.parseInt();
    while(Serial.available()) { Serial.read(); }

    if (degrees >= 0 && degrees <= 180) {
        Serial.print("Applicazione comando PWM per gradi: ");
        Serial.println(degrees);
        myServo.write(degrees);
    }
    updateDisplay("STANDBY", "");
}