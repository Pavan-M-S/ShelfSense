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

// --- ESP32 Pin Definitions ---
#define RST_PIN         4       // Safe GPIO for RC522 Reset
#define SS_PIN          5       // Standard ESP32 VSPI Chip Select (SS/SDA)
#define BUZZER_PIN      27      // Safe output GPIO for Buzzer
#define RED_LED_PIN     26      // Safe output GPIO for Red LED

// --- Object Initialization ---
MFRC522 mfrc522(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2); // Verify your I2C address (usually 0x27 or 0x3F)

// --- Database ---
// Registered Student RFID UIDs (NO spaces)
String registeredStudents[] = {
  "37E09604"  // Student 1
};
const int numStudents = 1;

// Registered Book RFID UIDs (NO spaces)
String registeredBooks[] = {
  "F10F7E05", // Book 1 
  "8DC0FA6B"  // Book 2
};
// Which shelf each book belongs to (Book 1 -> Shelf 2, Book 2 -> Shelf 4)
int bookShelfMap[] = {2, 4}; 
const int numBooks = 2;

// --- System State ---
int currentState = 0; // 0: Waiting for Student, 1: Waiting for Book

void setup() {
  Serial.begin(115200); // Set to 115200 to match platformio.ini monitor_speed
  SPI.begin();          // Initializes VSPI: SCK=18, MISO=19, MOSI=23, SS=5
  mfrc522.PCD_Init();
  
  lcd.init();
  lcd.backlight();
  
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  
  resetSystem();
}

void loop() {
  // Check for new RFID card
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    return;
  }
  
  String scannedUID = getUID();
  Serial.println("Scanned UID: " + scannedUID);
  
  if (currentState == 0) {
    // Step 1: Student Check-In
    if (isValidStudent(scannedUID)) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Welcome, Student!");
      lcd.setCursor(0, 1);
      lcd.print("Tap Your Book...");
      beep(1, 100);
      currentState = 1; // Move to book scanning state
      delay(1500);      // Debounce delay
    } else {
      lcd.clear();
      lcd.print("Access Denied");
      digitalWrite(RED_LED_PIN, HIGH);
      beep(2, 200); // Error beeps
      digitalWrite(RED_LED_PIN, LOW);
      delay(2000);
      resetSystem();
    }
    
  } else if (currentState == 1) {
    // Step 2: Book Tag Scanning & Location Display
    int bookIndex = getBookIndex(scannedUID);
    
    if (bookIndex != -1) {
      int targetShelf = bookShelfMap[bookIndex];
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Book Found ->");
      lcd.setCursor(0, 1);
      lcd.print("Go to Shelf " + String(targetShelf));
      
      beep(2, 100); // Success double-beep
      
      // Give the student 4 seconds to read the screen before resetting
      delay(4000); 
      resetSystem();
      
    } else {
      lcd.clear();
      lcd.print("Invalid Book Tag");
      digitalWrite(RED_LED_PIN, HIGH);
      beep(2, 200); // Error beeps
      digitalWrite(RED_LED_PIN, LOW);
      delay(2000);
      
      // Prompt for book again without resetting student login
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Tap Your Book...");
    }
  }
}

// --- Helper Functions ---

// Converts the scanned byte array into a readable HEX string without spaces
String getUID() {
  String uidString = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    uidString += String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
    uidString += String(mfrc522.uid.uidByte[i], HEX);
  }
  uidString.toUpperCase();
  mfrc522.PICC_HaltA(); // Halt PICC to prevent multiple rapid reads
  return uidString;
}

bool isValidStudent(String uid) {
  for (int i = 0; i < numStudents; i++) {
    if (uid == registeredStudents[i]) return true;
  }
  return false;
}

int getBookIndex(String uid) {
  for (int i = 0; i < numBooks; i++) {
    if (uid == registeredBooks[i]) return i;
  }
  return -1; // Not found
}

void resetSystem() {
  currentState = 0;
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("ShelfSense Ready");
  lcd.setCursor(0, 1);
  lcd.print("Tap Student ID");
  digitalWrite(RED_LED_PIN, LOW);
}

void beep(int times, int duration) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(duration);
    digitalWrite(BUZZER_PIN, LOW);
    delay(duration);
  }
}