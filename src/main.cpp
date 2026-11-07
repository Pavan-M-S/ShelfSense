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
 * 3. STATUS INDICATORS (Buzzer, Red LED & Green LED)
 * -----------------------------------------------------------------------------------------
 *   Component  | Pin / Terminal       | ESP32 Pin  | Configurable? | Notes
 *  ------------+----------------------+------------+---------------+-----------------------
 *   Buzzer     | Positive (+)         | GPIO 27    | YES           | Configurable (BUZZER_PIN)
 *              | Negative (-)         | GND        | No (Fixed)    | Common Ground
 *   Red LED    | Anode (+) [Long leg] | GPIO 26    | YES           | Configurable (RED_LED_PIN)
 *              | Cathode (-)          | GND        | No (Fixed)    | Connect with 220Ω-330Ω resistor
 *   Green LED  | Anode (+) [Long leg] | GPIO 25    | YES           | Configurable (GREEN_LED_PIN)
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

// =========================================================================================
// SECTION 1: HARDWARE PIN ASSIGNMENTS & SETTINGS
// =========================================================================================

// RC522 RFID Reader Pins (SPI)
#define RST_PIN         4       // Reset pin for RC522
#define SS_PIN          5       // Slave Select / SDA pin for RC522

// Feedback Peripherals (Buzzer, Red LED for Errors, Green LED for Success)
#define BUZZER_PIN      27      // Active Buzzer positive pin
#define RED_LED_PIN     26      // Error / Alert Red LED pin
#define GREEN_LED_PIN   25      // Correct / Success Green LED pin

// LCD I2C Configuration
#define LCD_I2C_ADDR    0x27    // Standard I2C address (0x27 or 0x3F)
#define LCD_COLUMNS     16      // 16 characters wide
#define LCD_ROWS        2       // 2 lines high

// Session Timeout Configuration
#define SESSION_TIMEOUT_MS 20000 // Configurable session timeout in ms (20 seconds)

// Baud Rate for USB Serial Debugging
#define SERIAL_BAUD     115200

// =========================================================================================
// SECTION 2: HARDWARE OBJECT INSTANCES
// =========================================================================================

// MFRC522 instance communicating over SPI
MFRC522 mfrc522(SS_PIN, RST_PIN);

// LiquidCrystal_I2C instance communicating over standard I2C (SDA=21, SCL=22)
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLUMNS, LCD_ROWS);

// =========================================================================================
// SECTION 3: SYSTEM STATES & DATA MODELS
// =========================================================================================

// Finite State Machine (FSM) States
enum SystemState {
  WAITING_FOR_STUDENT,  // Ready state: Waiting for student ID card to be scanned
  WAITING_FOR_BOOK      // Authenticated state: Waiting for a book tag to be scanned
};

// Variable holding the current system state
SystemState currentState = WAITING_FOR_STUDENT;

// Session timer tracking (inactivity detection in WAITING_FOR_BOOK)
unsigned long sessionStartTime = 0;

// Structure to model a student in the library system
struct Student {
  const char* name;     // Student's full name (e.g. "Pavan")
  const char* uid;      // RFID tag UID in uppercase HEX without spaces (e.g. "37E09604")
};

// Structure to model a book in the library system
struct Book {
  const char* title;    // Book title (e.g. "Algorithms")
  const char* uid;      // RFID tag UID in uppercase HEX without spaces (e.g. "F10F7E05")
  uint8_t shelf;        // Assigned shelf number
  uint8_t rack;         // Assigned rack number
};

// -----------------------------------------------------------------------------------------
// DATABASE CONFIGURATION: Students & Books
// Easily add, modify, or remove students and books below!
// -----------------------------------------------------------------------------------------

// List of authorized students: { "Student Name", "RFID UID" }
const Student registeredStudents[] = {
  { "Pavan", "37E09604" },
  { "Rehan", "A97AF8B2" },
  { "DVS"  , "0203448B258000"},
  { "Pruthvi", "C94B8554" }  // Placeholder for future students

  // Add new students here in the format: { "Name", "UID" },
};

// Database of registered books: { "Book Title", "RFID UID", Shelf Number, Rack Number }
const Book registeredBooks[] = {
  { "Geronimo Stilton - Journey to the Center of the Earth", "F10F7E05", 2, 1 }, // Shelf 2, Rack 1
  { "Harry Potter", "8DC0FA6B", 4, 3 }  // Shelf 4, Rack 3
  // Add new books here in the format: { "Title", "UID", shelf, rack },
};

// Helper constants for database sizes
const size_t NUM_STUDENTS = sizeof(registeredStudents) / sizeof(registeredStudents[0]);
const size_t NUM_BOOKS    = sizeof(registeredBooks) / sizeof(registeredBooks[0]);

// =========================================================================================
// SECTION 4: SERIAL LOGGING & DIAGNOSTIC HELPERS
// =========================================================================================

// Returns a human-readable name of the current state
const char* getStateName(SystemState state) {
  switch (state) {
    case WAITING_FOR_STUDENT: return "WAITING_FOR_STUDENT";
    case WAITING_FOR_BOOK:    return "WAITING_FOR_BOOK";
    default:                  return "UNKNOWN_STATE";
  }
}

// Log general informational messages
void logInfo(const __FlashStringHelper* message) {
  Serial.print(F("[INFO]    "));
  Serial.println(message);
}
void logInfo(const char* message) {
  Serial.print(F("[INFO]    "));
  Serial.println(message);
}
void logInfo(const String& message) {
  Serial.print(F("[INFO]    "));
  Serial.println(message);
}

// Log success events
void logSuccess(const __FlashStringHelper* message) {
  Serial.print(F("[SUCCESS] "));
  Serial.println(message);
}
void logSuccess(const char* message) {
  Serial.print(F("[SUCCESS] "));
  Serial.println(message);
}
void logSuccess(const String& message) {
  Serial.print(F("[SUCCESS] "));
  Serial.println(message);
}

// Log warnings (e.g. invalid tags)
void logWarn(const __FlashStringHelper* message) {
  Serial.print(F("[WARN]    "));
  Serial.println(message);
}
void logWarn(const char* message) {
  Serial.print(F("[WARN]    "));
  Serial.println(message);
}
void logWarn(const String& message) {
  Serial.print(F("[WARN]    "));
  Serial.println(message);
}

// Log errors (e.g. hardware faults or unauthorized access)
void logError(const __FlashStringHelper* message) {
  Serial.print(F("[ERROR]   "));
  Serial.println(message);
}
void logError(const char* message) {
  Serial.print(F("[ERROR]   "));
  Serial.println(message);
}
void logError(const String& message) {
  Serial.print(F("[ERROR]   "));
  Serial.println(message);
}

// Log state machine transitions
void logStateTransition(SystemState fromState, SystemState toState) {
  Serial.print(F("[STATE]   Transition: "));
  Serial.print(getStateName(fromState));
  Serial.print(F(" -> "));
  Serial.println(getStateName(toState));
}

// =========================================================================================
// SECTION 5: HARDWARE FEEDBACK & LCD HELPERS (SIMULTANEOUS LED & BUZZER)
// =========================================================================================

// Displays a message on the LCD and mirrors it to the Serial Monitor
void showMessage(const char* line1, const char* line2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);

  char buf[17];
  strncpy(buf, line1, 16);
  buf[16] = '\0';
  lcd.print(buf);

  Serial.print(F("[LCD]     Line 1: \""));
  Serial.print(line1);
  Serial.print(F("\""));

  if (line2 != nullptr && line2[0] != '\0') {
    lcd.setCursor(0, 1);
    strncpy(buf, line2, 16);
    buf[16] = '\0';
    lcd.print(buf);
    Serial.print(F(" | Line 2: \""));
    Serial.print(line2);
    Serial.print(F("\""));
  }
  Serial.println();
}

inline void showMessage(const String& line1, const String& line2 = "") {
  showMessage(line1.c_str(), line2.c_str());
}

// Scrolls line 1 smoothly from right to left if it exceeds 16 characters while keeping line 2 static
void showMessageWithScroll(const char* line1, const char* line2 = "", int scrollSpeedMs = 280, int totalDurationMs = 3500) {
  Serial.print(F("[LCD]     Line 1: \""));
  Serial.print(line1);
  Serial.print(F("\""));
  if (line2 != nullptr && line2[0] != '\0') {
    Serial.print(F(" | Line 2: \""));
    Serial.print(line2);
    Serial.print(F("\""));
  }
  Serial.println();

  lcd.clear();

  // Print line 2 (static)
  if (line2 != nullptr && line2[0] != '\0') {
    lcd.setCursor(0, 1);
    char buf[17];
    strncpy(buf, line2, 16);
    buf[16] = '\0';
    lcd.print(buf);
  }

  size_t len1 = strlen(line1);

  // If line 1 fits within 16 chars, simply display without scrolling
  if (len1 <= 16) {
    lcd.setCursor(0, 0);
    lcd.print(line1);
    delay(totalDurationMs);
    return;
  }

  // If line 1 is longer than 16 chars:
  // Show initial view, pause briefly so user can start reading, then scroll smoothly
  char frame[17];
  strncpy(frame, line1, 16);
  frame[16] = '\0';
  lcd.setCursor(0, 0);
  lcd.print(frame);
  delay(800);

  // Smooth scroll using stack buffer without creating heap Strings
  size_t padLen = len1 + 4; // virtual padded length (line1 + "    ")
  unsigned long startTime = millis();

  int offset = 0;
  while (millis() - startTime < (unsigned long)totalDurationMs) {
    for (int i = 0; i < 16; i++) {
      size_t charIdx = (offset + i) % padLen;
      if (charIdx < len1) {
        frame[i] = line1[charIdx];
      } else {
        frame[i] = ' ';
      }
    }
    frame[16] = '\0';
    lcd.setCursor(0, 0);
    lcd.print(frame);
    delay(scrollSpeedMs);
    offset++;
  }
}

inline void showMessageWithScroll(const String& line1, const String& line2 = "", int scrollSpeedMs = 280, int totalDurationMs = 3500) {
  showMessageWithScroll(line1.c_str(), line2.c_str(), scrollSpeedMs, totalDurationMs);
}

// Helper to handle the post-scroll 8-second shelf/rack hold and 3-second warning beeps:
// 1. Initial 4 seconds (0 - 4s): Displays original screen with Green LED solid ON.
// 2. Next 4 seconds (4s - 8s):
//    - Line 1 becomes the shelf/rack info (locationLine)
//    - Line 2 continuously scrolls "Please place it in given shelf" at 280ms speed, repeating until the end.
// 3. 3-second warning phase:
//    - 3 medium beeps spaced evenly across 3 seconds (~3000ms), while Green LED blinks simultaneously with buzzer.
//    - Line 2 continues scrolling or displaying during warning beeps.
// 4. Green LED turns OFF and returns to normal standby.
void holdLocationWait(const char* locationLine) {
  // Phase 1: First 4 seconds (0 - 4000ms) with Green LED solid ON
  digitalWrite(GREEN_LED_PIN, HIGH);
  Serial.println(F("[TIMER]   Holding book view (0-4s)..."));
  for (int sec = 1; sec <= 4; sec++) {
    delay(1000);
    Serial.print(F("[TIMER]   Hold elapsed: "));
    Serial.print(sec);
    Serial.println(F("s / 8s [View: Title + Shelf & Rack]"));
  }

  // Phase 2: After 4 seconds, move shelf/rack to Line 1; display prompt on Line 2
  Serial.println(F("[STATUS]  4 seconds elapsed. Moving shelf/rack to Line 1; showing prompt on Line 2..."));
  lcd.clear();
  lcd.setCursor(0, 0);
  char locBuf[17];
  strncpy(locBuf, locationLine, 16);
  locBuf[16] = '\0';
  lcd.print(locBuf);

  // Smooth readable scrolling for "Please place it in given shelf"
  const char promptMsg[] = "Please place it in given shelf    ";
  const int promptLen = sizeof(promptMsg) - 1;
  int promptOffset = 0;

  // Run remaining 4 seconds (4000ms) with relaxed, easy-to-read scroll speed (280ms per character)
  unsigned long startPhase2 = millis();
  while (millis() - startPhase2 < 4000) {
    char frame[17];
    for (int i = 0; i < 16; i++) {
      frame[i] = promptMsg[(promptOffset + i) % promptLen];
    }
    frame[16] = '\0';
    lcd.setCursor(0, 1);
    lcd.print(frame);
    delay(280);
    promptOffset++;
  }
  Serial.println(F("[TIMER]   Hold elapsed: 8s / 8s [Line 2: \"Please place it in given shelf\"]"));

  // Phase 3: 3-second warning phase (3000ms total) -> 3 medium beeps with simultaneous LED blink
  Serial.println(F("[STATUS]  8-second hold completed. Starting 3-second warning alert (3 beeps + LED flashes)..."));
  lcd.setCursor(0, 1);
  lcd.print(F("Session Ending.."));

  for (int b = 0; b < 3; b++) {
    Serial.print(F("[WARN]    Warning beep "));
    Serial.print(b + 1);
    Serial.print(F(" of 3 (Warning second "));
    Serial.print(b + 1);
    Serial.println(F("/3s)"));

    // 500ms BEEP + LED ON
    digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(GREEN_LED_PIN, HIGH);
    delay(500);

    // 500ms SILENCE + LED OFF
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
    delay(500);
  }

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);
  Serial.println(F("[LED]      Warning complete. Green LED OFF. Returning to normal."));
}

inline void holdLocationWait(const String& locationLine) {
  holdLocationWait(locationLine.c_str());
}

// Dedicated animated handler for displaying book title, shelf, and rack:
// 1. Line 2 remains empty while scrolling the book title.
// 2. Adaptive scroll speed: 280ms if <= 30 characters, faster (180ms) if > 30 characters.
// 3. Green LED blinks during scroll: ON for 2 characters, OFF for 1 character.
// 4. Exactly 4 characters before scroll ends, Line 2 appears showing "Shelf:X  Rack:Y" and beeps once immediately.
// 5. Once scrolling completes, holds display for 8 seconds (Green LED solid ON).
// 6. Then 2-second warning alert (3 medium beeps with simultaneous LED blinks), then LED off and return to normal.
void displayBookLocationSequence(const char* title, uint8_t shelf, uint8_t rack) {
  char locationLine[32];
  snprintf(locationLine, sizeof(locationLine), "Shelf:%u Rack:%u", (unsigned int)shelf, (unsigned int)rack);

  Serial.print(F("[LCD]     Book Title: \""));
  Serial.print(title);
  Serial.print(F("\" | Location: \""));
  Serial.print(locationLine);
  Serial.println(F("\""));

  lcd.clear();

  size_t titleLen = strlen(title);

  // Case A: Short title (<= 16 characters) - No scrolling needed
  if (titleLen <= 16) {
    lcd.setCursor(0, 0);
    lcd.print(title);
    lcd.setCursor(0, 1);
    lcd.print(locationLine);

    // Beep once when showing shelf and rack info
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);

    // Wait 8 seconds with solid LED, followed by 3 warning beeps/blinks over 3 seconds
    holdLocationWait(locationLine);
    return;
  }

  // Case B: Long title (> 16 characters)
  // Adaptive scroll speed: 280ms per step if <= 30 chars; faster 180ms if > 30 chars
  int scrollSpeedMs = (titleLen > 30) ? 180 : 280;

  int totalSteps = (int)titleLen - 16;
  int triggerStep = totalSteps > 4 ? (totalSteps - 4) : 0;
  bool locationShown = false;

  // Initial pause on the first 16 characters so user can start reading
  char frame[17];
  strncpy(frame, title, 16);
  frame[16] = '\0';
  lcd.setCursor(0, 0);
  lcd.print(frame);
  digitalWrite(GREEN_LED_PIN, HIGH);
  delay(800);

  // Scroll step by step from offset 0 to totalSteps
  for (int step = 0; step <= totalSteps; step++) {
    // When we reach 4 characters before finishing scroll, reveal Shelf and Rack on line 2!
    if (step >= triggerStep && !locationShown) {
      lcd.setCursor(0, 1);
      lcd.print(locationLine);
      locationShown = true;
      Serial.print(F("[LCD]     Line 2 revealed (4 chars before end): \""));
      Serial.print(locationLine);
      Serial.println(F("\""));

      // Beep once immediately when shelf & rack appears
      digitalWrite(BUZZER_PIN, HIGH);
      delay(100);
      digitalWrite(BUZZER_PIN, LOW);
    }

    // Green LED blinking pattern: ON for 2 chars, OFF for 1 char
    if (step % 3 == 2) {
      digitalWrite(GREEN_LED_PIN, LOW);
    } else {
      digitalWrite(GREEN_LED_PIN, HIGH);
    }

    // Render 16 characters on Line 1 starting at current offset
    strncpy(frame, title + step, 16);
    frame[16] = '\0';
    lcd.setCursor(0, 0);
    lcd.print(frame);
    delay(scrollSpeedMs);
  }

  // Ensure Line 2 is displayed if it was very short
  if (!locationShown) {
    lcd.setCursor(0, 1);
    lcd.print(locationLine);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
  }

  // After scrolling completes fully: wait 8 seconds with solid LED, followed by 3 warning beeps/blinks over 3 seconds
  holdLocationWait(locationLine);
}

inline void displayBookLocationSequence(const String& title, uint8_t shelf, uint8_t rack) {
  displayBookLocationSequence(title.c_str(), shelf, rack);
}

// Resets system back to the default student check-in state (turns off all LEDs)
void resetSystem() {
  SystemState previousState = currentState;
  currentState = WAITING_FOR_STUDENT;
  sessionStartTime = 0; // Reset session inactivity timer
  
  // Ensure both indicator LEDs are OFF in standby
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);

  if (previousState != WAITING_FOR_STUDENT) {
    logStateTransition(previousState, WAITING_FOR_STUDENT);
  } else {
    logInfo(F("System reset to ready state (waiting for student ID)"));
  }

  // Show "ShelfSense Ready" for a short time, then transition to steady "Please Tap Your Student ID Card"
  showMessage("ShelfSense Ready", "  Smart Library   ");
  delay(1800); // Display for 1.8 seconds, then disappear
  showMessage("Please Tap Your ", "Student ID Card ");
}

// SUCCESS FEEDBACK: Buzzer short beep + Green LED glow SIMULTANEOUSLY
// Optionally leaves the Green LED lit while the shelf location is being displayed
void indicateSuccess(int times = 1, int durationMs = 150, bool keepGreenLit = false) {
  Serial.print(F("[FEEDBACK] SUCCESS -> Green LED + Buzzer ("));
  Serial.print(times);
  Serial.print(F(" beep(s) @ "));
  Serial.print(durationMs);
  Serial.println(F(" ms simultaneously)"));

  for (int i = 0; i < times; i++) {
    // Turn BOTH Green LED and Buzzer ON at the exact same instant
    digitalWrite(GREEN_LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);

    // Turn BOTH OFF together
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);

    if (i < times - 1) {
      delay(100);
    }
  }

  // Keep Green LED glowing for visual confirmation while shelf number is shown
  if (keepGreenLit) {
    digitalWrite(GREEN_LED_PIN, HIGH);
    Serial.println(F("[LED]      Green LED glowing for shelf location display"));
  }
}

// EMERGENCY ERROR FEEDBACK: Rapid Red LED + Buzzer pulsing alarm for unrecognized/invalid tags (2 seconds)
void showEmergencyAlarm(const char* line1, const char* line2, const char* serialReason) {
  Serial.print(F("[ERROR]   EMERGENCY ALARM TRIGGERED: "));
  Serial.println(serialReason);
  showMessage(line1, line2);

  Serial.println(F("[FEEDBACK] EMERGENCY ALARM -> Rapid Red LED + Buzzer pulsing for 2 seconds!"));

  // Rapid alarm: 10 fast pulses (100ms ON, 100ms OFF) = 2000ms (2 seconds)
  for (int i = 0; i < 10; i++) {
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);

    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
  }

  delay(1000); // Allow user time to read the message on LCD
}

inline void showEmergencyAlarm(const String& line1, const String& line2, const String& serialReason) {
  showEmergencyAlarm(line1.c_str(), line2.c_str(), serialReason.c_str());
}

// STANDARD ERROR FEEDBACK: Red LED glowing at the same time Buzzer is beeping SIMULTANEOUSLY (e.g. out-of-order scans)
void showError(const char* line1, const char* line2, const char* serialReason) {
  Serial.print(F("[WARN]    NOTICE: "));
  Serial.println(serialReason);
  showMessage(line1, line2);

  Serial.println(F("[FEEDBACK] ERROR -> Red LED + Buzzer pulsing (2 beeps @ 180 ms)"));

  for (int i = 0; i < 2; i++) {
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(180);

    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < 1) {
      delay(120);
    }
  }

  delay(1500); // Pause so student can see the prompt on LCD
}

inline void showError(const String& line1, const String& line2, const String& serialReason) {
  showError(line1.c_str(), line2.c_str(), serialReason.c_str());
}

// =========================================================================================
// SECTION 6: RFID & DATABASE LOOKUP FUNCTIONS
// =========================================================================================

// Fills outBuffer with formatted uppercase HEX UID (e.g. "37E09604")
void readCardUID(char* outBuffer, size_t maxLen) {
  outBuffer[0] = '\0';
  size_t offset = 0;
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (offset + 2 < maxLen) {
      snprintf(outBuffer + offset, maxLen - offset, "%02X", mfrc522.uid.uidByte[i]);
      offset += 2;
    }
  }

  // Log card technical details to Serial Monitor
  MFRC522::PICC_Type piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  Serial.println(F("---------------------------------------------------"));
  Serial.print(F("[RFID]    Card Detected! UID: "));
  Serial.print(outBuffer);
  Serial.print(F(" | Size: "));
  Serial.print(mfrc522.uid.size);
  Serial.print(F(" bytes | PICC Type: "));
  Serial.println(mfrc522.PICC_GetTypeName(piccType));
  Serial.print(F("[RFID]    Current System State: "));
  Serial.println(getStateName(currentState));

  // Halt card to prevent repeated reads in the same pass
  mfrc522.PICC_HaltA();
}

// Backward-compatible overload returning String
inline String readCardUID() {
  char buf[32];
  readCardUID(buf, sizeof(buf));
  return String(buf);
}

// Searches registered students for UID. Returns index if found, -1 if not found
int findStudentIndex(const char* uid) {
  Serial.print(F("[AUTH]    Checking student database for UID ["));
  Serial.print(uid);
  Serial.println(F("]..."));

  for (size_t i = 0; i < NUM_STUDENTS; i++) {
    if (strcmp(registeredStudents[i].uid, uid) == 0) {
      Serial.print(F("[AUTH]    Match found! Student: \""));
      Serial.print(registeredStudents[i].name);
      Serial.print(F("\" at index ["));
      Serial.print(i);
      Serial.println(F("]"));
      return (int)i;
    }
  }

  Serial.println(F("[AUTH]    No match found in student database!"));
  return -1;
}

inline int findStudentIndex(const String& uid) {
  return findStudentIndex(uid.c_str());
}

// Checks if the UID belongs to an authorized student
bool isStudentRegistered(const char* uid) {
  return findStudentIndex(uid) != -1;
}

inline bool isStudentRegistered(const String& uid) {
  return isStudentRegistered(uid.c_str());
}

// Searches registered books for UID. Returns index if found, -1 if not found
int findBookIndex(const char* uid) {
  Serial.print(F("[LOOKUP]  Searching library database for book UID ["));
  Serial.print(uid);
  Serial.println(F("]..."));

  for (size_t i = 0; i < NUM_BOOKS; i++) {
    if (strcmp(registeredBooks[i].uid, uid) == 0) {
      Serial.print(F("[LOOKUP]  Match found! Book: \""));
      Serial.print(registeredBooks[i].title);
      Serial.print(F("\" -> Shelf "));
      Serial.print(registeredBooks[i].shelf);
      Serial.print(F(", Rack "));
      Serial.println(registeredBooks[i].rack);
      return (int)i;
    }
  }

  Serial.println(F("[LOOKUP]  No match found in book database!"));
  return -1;
}

inline int findBookIndex(const String& uid) {
  return findBookIndex(uid.c_str());
}

// =========================================================================================
// SECTION 7: STEP HANDLERS (BUSINESS LOGIC)
// =========================================================================================

// Handles Student Check-In (Step 1)
void handleStudentScan(const char* uid) {
  int studentIdx = findStudentIndex(uid);
  if (studentIdx != -1) {
    // 1. Authorized Student scanned
    const Student& student = registeredStudents[studentIdx];
    Serial.print(F("[SUCCESS] Student authenticated: "));
    Serial.print(student.name);
    Serial.print(F(" ("));
    Serial.print(student.uid);
    Serial.println(F(")"));
    
    // Short confirmation beep with green LED flash
    indicateSuccess(1, 100, false);

    logStateTransition(WAITING_FOR_STUDENT, WAITING_FOR_BOOK);
    currentState = WAITING_FOR_BOOK;

    // Start session timer for non-blocking inactivity tracking
    sessionStartTime = millis();
    Serial.print(F("[INFO]    Session timer started ("));
    Serial.print(SESSION_TIMEOUT_MS / 1000);
    Serial.println(F("s window for book scan)"));

    // Display student name on LCD: if longer than 16 chars (e.g. "Hi " + name), scroll smoothly!
    char studentLine[48];
    snprintf(studentLine, sizeof(studentLine), "Hi %s", student.name);
    showMessageWithScroll(studentLine, "Tap Book Tag...", 260, 2500);

    Serial.println(F("[INFO]    Ready for book tag. Waiting for book scan..."));
    return;
  }

  // 2. Not a student card -> Check if user accidentally scanned a registered Book Tag!
  int bookIdx = findBookIndex(uid);
  if (bookIdx != -1) {
    const Book& scannedBook = registeredBooks[bookIdx];
    Serial.print(F("[WARN]    Book tag (\""));
    Serial.print(scannedBook.title);
    Serial.println(F("\") scanned at Step 1 instead of Student ID!"));
    
    // Prompt student: "Please Tap Your" / "Student ID First"
    showError("Please Tap Your", "Student ID First", "Book tag scanned before student ID authentication");
    
    // Return cleanly to standby
    resetSystem();
    return;
  }

  // 3. Tag is neither student nor book in database -> Unrecognized ID emergency alarm!
  Serial.print(F("[ERROR]   Unrecognized tag UID: "));
  Serial.println(uid);
  char reason[64];
  snprintf(reason, sizeof(reason), "Unrecognized tag scanned at Step 1: %s", uid);
  showEmergencyAlarm("Unrecognized ID", "Access Denied!", reason);
  resetSystem();
}

inline void handleStudentScan(const String& uid) {
  handleStudentScan(uid.c_str());
}

// Handles Book Scanning and Shelf Lookup (Step 2)
void handleBookScan(const char* uid) {
  int bookIdx = findBookIndex(uid);

  if (bookIdx != -1) {
    // 1. Registered Book Tag identified -> Run full sequence!
    const Book& foundBook = registeredBooks[bookIdx];
    
    Serial.print(F("[SUCCESS] Book identified: \""));
    Serial.print(foundBook.title);
    Serial.print(F("\" located on Shelf "));
    Serial.print(foundBook.shelf);
    Serial.print(F(", Rack "));
    Serial.println(foundBook.rack);
    
    // Initial success feedback: short confirmation beep + green LED blink
    indicateSuccess(1, 150, false);

    // Reset session timer as book checkout/location lookup is now executing
    sessionStartTime = 0;

    // Run custom animated display sequence (title scroll, reveal, 8s hold, 3s warning)
    displayBookLocationSequence(foundBook.title, foundBook.shelf, foundBook.rack);

    Serial.println(F("[INFO]    Session completed. Resetting for next student..."));
    resetSystem();
    return;
  }

  // 2. Check if student card was scanned again instead of a book
  int studentIdx = findStudentIndex(uid);
  if (studentIdx != -1) {
    Serial.println(F("[WARN]    Student ID scanned again instead of a book! Prompting for book tag..."));
    showError("Please Tap Book", "Tag (Not ID)", "Student ID card tapped during book scan step");
    
    // Renew session timer to give the student an additional 20 seconds to correct their action
    sessionStartTime = millis();
    Serial.println(F("[INFO]    Session timer renewed (+20s). Ready for book tag retry..."));
    showMessage("Tap Your Book...");
    return;
  }

  // 3. Tag is completely unrecognized -> Emergency alarm!
  Serial.print(F("[ERROR]   Unrecognized tag UID: "));
  Serial.print(uid);
  Serial.println(F(" during book lookup step!"));
  char reason[64];
  snprintf(reason, sizeof(reason), "Unregistered tag scanned at Step 2: %s", uid);
  showEmergencyAlarm("Unrecognized ID", "Unknown Book Tag", reason);

  // Renew session timer to give the student an additional 20 seconds to correct their action
  sessionStartTime = millis();
  Serial.println(F("[INFO]    Session timer renewed (+20s). Ready for book tag retry..."));
  showMessage("Tap Your Book...");
}

inline void handleBookScan(const String& uid) {
  handleBookScan(uid.c_str());
}

// =========================================================================================
// SECTION 8: HARDWARE DIAGNOSTICS AT BOOT
// =========================================================================================

void runHardwareDiagnostics() {
  Serial.println();
  Serial.println(F("==================================================="));
  Serial.println(F("         SHELFSENSE ESP32 SYSTEM STARTUP           "));
  Serial.println(F("==================================================="));
  Serial.print(F("[BOOT]    CPU Frequency: "));
  Serial.print(getCpuFrequencyMhz());
  Serial.println(F(" MHz"));
  Serial.print(F("[BOOT]    Free Heap RAM: "));
  Serial.print(ESP.getFreeHeap());
  Serial.println(F(" bytes"));

  // 1. Check I2C Bus & LCD Presence
  Serial.println(F("[BOOT]    Initializing I2C bus and scanning for LCD..."));
  Wire.begin(21, 22); // Start I2C explicitly on ESP32 default pins: SDA=21, SCL=22
  delay(50);

  // If configured address fails, scan I2C bus to find the actual address
  uint8_t targetAddr = LCD_I2C_ADDR;
  Wire.beginTransmission(targetAddr);
  byte i2cError = Wire.endTransmission();

  if (i2cError != 0) {
    // Probe alternative standard address 0x3F or others
    Serial.print(F("[BOOT]    LCD not found at 0x"));
    Serial.print(targetAddr, HEX);
    Serial.println(F(", scanning bus..."));
    for (uint8_t addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        Serial.print(F("[BOOT]    Found I2C device at address 0x"));
        Serial.println(addr, HEX);
        targetAddr = addr;
        i2cError = 0;
        break;
      }
    }
  }

  if (i2cError == 0) {
    Serial.print(F("[BOOT]    I2C LCD detected at address 0x"));
    Serial.println(targetAddr, HEX);
    lcd = LiquidCrystal_I2C(targetAddr, LCD_COLUMNS, LCD_ROWS);
    lcd.init();
    lcd.backlight();
    lcd.clear();
    
    // Welcome splash screens
    showMessage("  ABPS Nandyal  ", "    Welcome!    ");
    delay(2000); // Display for 2 seconds
    showMessage("   ShelfSense   ", " Smart Library  ");
    delay(2000); // Display for 2 seconds
    showMessage("ShelfSense Ready", "Tap Student ID");
  } else {
    logError(F("I2C LCD NOT responding! Check 5V power and SDA(21)/SCL(22) wiring."));
  }

  // 2. Check SPI & RC522 RFID Reader
  Serial.println(F("[BOOT]    Initializing SPI and RC522 Reader..."));
  SPI.begin();
  mfrc522.PCD_Init();
  
  byte readerVersion = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print(F("[BOOT]    RC522 Firmware Version Register: 0x"));
  Serial.println(readerVersion, HEX);

  if (readerVersion == 0x92) {
    logSuccess(F("RC522 Reader verified (Version 2.0 detected)"));
  } else if (readerVersion == 0x91) {
    logSuccess(F("RC522 Reader verified (Version 1.0 detected)"));
  } else if (readerVersion == 0x88) {
    logSuccess(F("RC522 Reader verified (FM17522 clone detected - OK)"));
  } else if (readerVersion == 0x00 || readerVersion == 0xFF) {
    logError(F("RC522 RFID READER COMMUNICATION FAILED!"));
    Serial.println(F("          [Troubleshooting RC522]:"));
    Serial.println(F("          - CRITICAL: VCC must be connected to 3.3V ONLY (5V can damage RC522)"));
    Serial.println(F("          - Ensure common GND with ESP32"));
    Serial.println(F("          - Verify SPI Wiring: SCK=GPIO 18, MISO=GPIO 19, MOSI=GPIO 23, SS=GPIO 5, RST=GPIO 4"));
    Serial.println(F("          - Check soldered header pins for cold joints or short circuits"));
  } else {
    Serial.print(F("[WARN]    Unrecognized RC522 version 0x"));
    Serial.print(readerVersion, HEX);
    Serial.println(F(" (Reader may still function)"));
  }

  // 3. Test Output Indicators (Buzzer, Red LED, Green LED)
  Serial.println(F("[BOOT]    Testing Buzzer (GPIO 27), Red LED (GPIO 26), and Green LED (GPIO 25)..."));
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);

  // Quick sequential test: Green LED test, then Red LED + Buzzer simultaneous test
  digitalWrite(GREEN_LED_PIN, HIGH);
  delay(120);
  digitalWrite(GREEN_LED_PIN, LOW);
  delay(80);

  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(120);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  
  logSuccess(F("Green LED, Red LED, and Buzzer hardware test passed"));

  // 4. Print Database Summary
  Serial.println(F("[BOOT]    Loading Registered Database:"));
  Serial.print(F("          - Registered Students: "));
  Serial.println(NUM_STUDENTS);
  for (size_t i = 0; i < NUM_STUDENTS; i++) {
    Serial.print(F("            [Student "));
    Serial.print(i + 1);
    Serial.print(F("] Name: \""));
    Serial.print(registeredStudents[i].name);
    Serial.print(F("\" | UID: "));
    Serial.println(registeredStudents[i].uid);
  }

  Serial.print(F("          - Registered Books:    "));
  Serial.println(NUM_BOOKS);
  for (size_t i = 0; i < NUM_BOOKS; i++) {
    Serial.print(F("            [Book "));
    Serial.print(i + 1);
    Serial.print(F("] Title: \""));
    Serial.print(registeredBooks[i].title);
    Serial.print(F("\" | UID: "));
    Serial.print(registeredBooks[i].uid);
    Serial.print(F(" | Shelf: "));
    Serial.print(registeredBooks[i].shelf);
    Serial.print(F(" | Rack: "));
    Serial.println(registeredBooks[i].rack);
  }

  Serial.println(F("==================================================="));
  Serial.println(F("[BOOT]    Diagnostics complete. System is ready!"));
  Serial.println(F("==================================================="));
  Serial.println();
}

// =========================================================================================
// SECTION 9: MAIN ARDUINO SETUP & LOOP
// =========================================================================================

void setup() {
  // Start Serial Monitor at 115200 baud
  Serial.begin(SERIAL_BAUD);
  delay(500); // Allow USB serial link to stabilize

  // Perform full hardware diagnostics & initialization
  runHardwareDiagnostics();

  // Reset to initial ready state
  resetSystem();
}

void loop() {
  // Non-blocking inactivity detection for student check-in session
  if (currentState == WAITING_FOR_BOOK && (millis() - sessionStartTime >= SESSION_TIMEOUT_MS)) {
    Serial.print(F("[WARN]    Session timed out: No book scan received within "));
    Serial.print(SESSION_TIMEOUT_MS / 1000);
    Serial.println(F(" seconds."));

    // Auditory and visual timeout notification on LCD and status peripherals
    showMessage("Session Expired ", "Please Try Again");

    // Fire 2 short warning beeps on the buzzer and Red LED flashes
    for (int i = 0; i < 2; i++) {
      digitalWrite(RED_LED_PIN, HIGH);
      digitalWrite(BUZZER_PIN, HIGH);
      delay(150);
      digitalWrite(RED_LED_PIN, LOW);
      digitalWrite(BUZZER_PIN, LOW);
      if (i < 1) {
        delay(150);
      }
    }
    delay(1000); // Brief pause so student can read "Session Expired / Please Try Again"

    // Revert currentState to WAITING_FOR_STUDENT and restore default standby display
    resetSystem();
    return;
  }

  // Check if a new RFID card is present on the reader
  if (!mfrc522.PICC_IsNewCardPresent()) {
    delay(50);
    return;
  }

  // Read card serial number
  if (!mfrc522.PICC_ReadCardSerial()) {
    // Suppress repeated spam if it's transient RF noise or card bounce
    delay(50);
    return;
  }

  // Retrieve formatted UID into stack buffer (prevents dynamic heap allocation)
  char scannedUID[32];
  readCardUID(scannedUID, sizeof(scannedUID));

  // Route scanned card to the appropriate step handler
  if (currentState == WAITING_FOR_STUDENT) {
    handleStudentScan(scannedUID);
  } else if (currentState == WAITING_FOR_BOOK) {
    handleBookScan(scannedUID);
  }
}
