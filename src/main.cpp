/*
 * =========================================================================================
 *                         SHELFSENSE - ESP32 PIN CONNECTION GUIDE
 * =========================================================================================
 * 
 * 1. RC522 RFID READER (SPI Interface - VSPI)
 * -----------------------------------------------------------------------------------------
 *   RC522 Pin  | ESP32 Pin      | Configurable in Code? | Notes
 *  ------------+----------------+-----------------------+----------------------------------
 *   VCC (3.3V) | 3.3V           | No (Fixed Power)      | CRITICAL: 3.3V ONLY! 5V will burn RC522!
 *   RST        | GPIO 4         | YES (RST_PIN)         | Configurable in code
 *   GND        | GND            | No (Fixed Ground)     | Common Ground
 *   IRQ        | Not Connected  | -                     | Not needed for this project
 *   MISO       | GPIO 19        | NO (Hardware SPI)     | Default VSPI MISO
 *   MOSI       | GPIO 23        | NO (Hardware SPI)     | Default VSPI MOSI
 *   SCK        | GPIO 18        | NO (Hardware SPI)     | Default VSPI SCK (Clock)
 *   SDA / SS   | GPIO 5         | YES (SS_PIN)          | Configurable in code (Slave Select)
 * 
 * 2. 16x2 LCD DISPLAY with I2C Backpack (I2C Interface)
 * -----------------------------------------------------------------------------------------
 *   LCD Pin    | ESP32 Pin      | Configurable in Code? | Notes
 *  ------------+----------------+-----------------------+----------------------------------
 *   GND        | GND            | No (Fixed Ground)     | Common Ground
 *   VCC        | 5V / VIN       | No (Fixed Power)      | Requires 5V for backlight & contrast
 *   SDA        | GPIO 21        | NO (Hardware I2C)     | Default Hardware I2C SDA
 *   SCL        | GPIO 22        | NO (Hardware I2C)     | Default Hardware I2C SCL
 *   * Note: Default I2C address is usually 0x27 or 0x3F. Turn potentiometer to adjust contrast.
 * 
 * 3. STATUS INDICATORS (Buzzer & Red LED)
 * -----------------------------------------------------------------------------------------
 *   Component  | Pin / Terminal       | ESP32 Pin  | Configurable? | Notes
 *  ------------+----------------------+------------+---------------+-----------------------
 *   Buzzer     | Positive (+)         | GPIO 27    | YES           | Configurable (BUZZER_PIN)
 *              | Negative (-)         | GND        | No (Fixed)    | Common Ground
 *   Red LED    | Anode (+) [Long leg] | GPIO 26    | YES           | Configurable (RED_LED_PIN)
 *              | Cathode (-)          | GND        | No (Fixed)    | Connect with 220Ω-330Ω resistor
 * 
 * -----------------------------------------------------------------------------------------
 * ESP32 SAFETY WARNING:
 *  - DO NOT use GPIO 6-11 on ESP32: they are wired to internal flash memory.
 *  - Serial baud rate is set to 115200 to match platformio.ini (monitor_speed = 115200).
 * =========================================================================================
 */

#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ==========================================
// 1. PIN DEFINITIONS & HARDWARE SETUP
// ==========================================
#define RST_PIN     4       // RC522 Reset
#define SS_PIN      5       // RC522 Chip Select (SDA)
#define BUZZER_PIN  27      // Buzzer (+)
#define RED_LED_PIN 26      // Red LED (+)

MFRC522 mfrc522(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ==========================================
// 2. SYSTEM STATES & DATA MODELS
// ==========================================
enum SystemState {
  WAITING_FOR_STUDENT,
  WAITING_FOR_BOOK
};

SystemState currentState = WAITING_FOR_STUDENT;

// Book structure: keeps RFID tag and shelf number together
struct Book {
  String uid;
  int shelf;
};

// Database: Registered students
const String registeredStudents[] = {
  "37E09604"  // Student 1
};

// Database: Registered books and their shelf locations
const Book registeredBooks[] = {
  { "F10F7E05", 2 }, // Book 1 -> Shelf 2
  { "8DC0FA6B", 4 }  // Book 2 -> Shelf 4
};

// ==========================================
// 3. UI & FEEDBACK HELPERS
// ==========================================

// Display up to 2 lines of text on the LCD
void showMessage(const String& line1, const String& line2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  if (line2.length() > 0) {
    lcd.setCursor(0, 1);
    lcd.print(line2);
  }
}

// Beep the buzzer a given number of times
void beep(int times, int durationMs = 100) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);
    digitalWrite(BUZZER_PIN, LOW);
    delay(durationMs);
  }
}

// Return the system to the initial ready state
void resetSystem() {
  currentState = WAITING_FOR_STUDENT;
  digitalWrite(RED_LED_PIN, LOW);
  showMessage("ShelfSense Ready", "Tap Student ID");
}

// Flash error LED, sound warning beeps, and show error text
void showError(const String& message) {
  showMessage(message);
  digitalWrite(RED_LED_PIN, HIGH);
  beep(2, 200);
  digitalWrite(RED_LED_PIN, LOW);
  delay(2000);
}

// ==========================================
// 4. RFID & DATABASE LOOKUP HELPERS
// ==========================================

// Read the scanned card's UID as an uppercase hex string (e.g. "37E09604")
String readCardUID() {
  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(mfrc522.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  mfrc522.PICC_HaltA(); // Stop reading current card to prevent repeated triggers
  return uid;
}

// Check if a student UID is registered
bool isStudentRegistered(const String& uid) {
  for (const String& student : registeredStudents) {
    if (student == uid) return true;
  }
  return false;
}

// Find a book's shelf. Returns shelf number, or -1 if book is not registered
int findBookShelf(const String& uid) {
  for (const Book& book : registeredBooks) {
    if (book.uid == uid) return book.shelf;
  }
  return -1;
}

// ==========================================
// 5. STEP HANDLERS (BUSINESS LOGIC)
// ==========================================

void handleStudentScan(const String& uid) {
  if (isStudentRegistered(uid)) {
    showMessage("Welcome, Student!", "Tap Your Book...");
    beep(1, 100);
    currentState = WAITING_FOR_BOOK;
    delay(1500); // Debounce delay
  } else {
    showError("Access Denied");
    resetSystem();
  }
}

void handleBookScan(const String& uid) {
  int shelf = findBookShelf(uid);
  
  if (shelf != -1) {
    showMessage("Book Found ->", "Go to Shelf " + String(shelf));
    beep(2, 100); // Success double-beep
    delay(4000);  // Allow time to read shelf number
    resetSystem();
  } else {
    showError("Invalid Book Tag");
    showMessage("Tap Your Book..."); // Prompt again without logging student out
  }
}

// ==========================================
// 6. MAIN ARDUINO SETUP & LOOP
// ==========================================

void setup() {
  Serial.begin(115200);
  SPI.begin();
  mfrc522.PCD_Init();
  
  lcd.init();
  lcd.backlight();
  
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  
  resetSystem();
}

void loop() {
  // Check for an RFID card
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  String scannedUID = readCardUID();
  Serial.println("Scanned UID: " + scannedUID);

  // Handle RFID card based on current step
  if (currentState == WAITING_FOR_STUDENT) {
    handleStudentScan(scannedUID);
  } else if (currentState == WAITING_FOR_BOOK) {
    handleBookScan(scannedUID);
  }
}
