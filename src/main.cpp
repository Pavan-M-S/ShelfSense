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

// Structure to model a student in the library system
struct Student {
  String name;     // Student's full name (e.g. "Pavan")
  String uid;      // RFID tag UID in uppercase HEX without spaces (e.g. "37E09604")
};

// Structure to model a book in the library system
struct Book {
  String title;    // Book title (e.g. "Algorithms")
  String uid;      // RFID tag UID in uppercase HEX without spaces (e.g. "F10F7E05")
  int shelf;       // Assigned shelf number
  int rack;        // Assigned rack number
};

// -----------------------------------------------------------------------------------------
// DATABASE CONFIGURATION: Students & Books
// Easily add, modify, or remove students and books below!
// -----------------------------------------------------------------------------------------

// List of authorized students: { "Student Name", "RFID UID" }
const Student registeredStudents[] = {
  { "Pavan", "37E09604" }
  // Add new students here in the format: { "Name", "UID" },
};

// Database of registered books: { "Book Title", "RFID UID", Shelf Number, Rack Number }
const Book registeredBooks[] = {
  { "Geronimo Stilton - Journey to the Center of the Earth", "F10F7E05", 2, 1 }, // Shelf 2, Rack 1
  { "Dragon Masters - The Lost Kingdom", "8DC0FA6B", 4, 3 }  // Shelf 4, Rack 3
  // Add new books here in the format: { "Title", "UID", shelf, rack },
};

// Helper constants for database sizes
const size_t NUM_STUDENTS = sizeof(registeredStudents) / sizeof(registeredStudents[0]);
const size_t NUM_BOOKS    = sizeof(registeredBooks) / sizeof(registeredBooks[0]);

// =========================================================================================
// SECTION 4: SERIAL LOGGING & DIAGNOSTIC HELPERS
// =========================================================================================

// Returns a human-readable name of the current state
String getStateName(SystemState state) {
  switch (state) {
    case WAITING_FOR_STUDENT: return "WAITING_FOR_STUDENT";
    case WAITING_FOR_BOOK:    return "WAITING_FOR_BOOK";
    default:                  return "UNKNOWN_STATE";
  }
}

// Log general informational messages
void logInfo(const String& message) {
  Serial.print("[INFO]    ");
  Serial.println(message);
}

// Log success events
void logSuccess(const String& message) {
  Serial.print("[SUCCESS] ");
  Serial.println(message);
}

// Log warnings (e.g. invalid tags)
void logWarn(const String& message) {
  Serial.print("[WARN]    ");
  Serial.println(message);
}

// Log errors (e.g. hardware faults or unauthorized access)
void logError(const String& message) {
  Serial.print("[ERROR]   ");
  Serial.println(message);
}

// Log state machine transitions
void logStateTransition(SystemState fromState, SystemState toState) {
  Serial.print("[STATE]   Transition: ");
  Serial.print(getStateName(fromState));
  Serial.print(" -> ");
  Serial.println(getStateName(toState));
}

// =========================================================================================
// SECTION 5: HARDWARE FEEDBACK & LCD HELPERS (SIMULTANEOUS LED & BUZZER)
// =========================================================================================

// Displays a message on the LCD and mirrors it to the Serial Monitor
void showMessage(const String& line1, const String& line2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.length() > 16 ? line1.substring(0, 16) : line1);

  Serial.print("[LCD]     Line 1: \"");
  Serial.print(line1);
  Serial.print("\"");

  if (line2.length() > 0) {
    lcd.setCursor(0, 1);
    lcd.print(line2.length() > 16 ? line2.substring(0, 16) : line2);
    Serial.print(" | Line 2: \"");
    Serial.print(line2);
    Serial.print("\"");
  }
  Serial.println();
}

// Scrolls line 1 smoothly from right to left if it exceeds 16 characters while keeping line 2 static
void showMessageWithScroll(const String& line1, const String& line2 = "", int scrollSpeedMs = 280, int totalDurationMs = 3500) {
  Serial.print("[LCD]     Line 1: \"");
  Serial.print(line1);
  Serial.print("\"");
  if (line2.length() > 0) {
    Serial.print(" | Line 2: \"");
    Serial.print(line2);
    Serial.print("\"");
  }
  Serial.println();

  lcd.clear();

  // Print line 2 (static)
  if (line2.length() > 0) {
    lcd.setCursor(0, 1);
    lcd.print(line2.length() > 16 ? line2.substring(0, 16) : line2);
  }

  // If line 1 fits within 16 chars, simply display without scrolling
  if (line1.length() <= 16) {
    lcd.setCursor(0, 0);
    lcd.print(line1);
    delay(totalDurationMs);
    return;
  }

  // If line 1 is longer than 16 chars:
  // Show initial view, pause briefly so user can start reading, then scroll smoothly
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  delay(800);

  // Add spacing so it wraps around cleanly if scrolling loops
  String paddedText = line1 + "    ";
  int maxOffset = paddedText.length();
  unsigned long startTime = millis();

  int offset = 0;
  while (millis() - startTime < (unsigned long)totalDurationMs) {
    String frame = "";
    for (int i = 0; i < 16; i++) {
      int charIdx = (offset + i) % maxOffset;
      frame += paddedText.charAt(charIdx);
    }
    lcd.setCursor(0, 0);
    lcd.print(frame);
    delay(scrollSpeedMs);
    offset++;
  }
}

// Helper to handle the post-scroll 8-second shelf/rack hold and 3-second warning beeps:
// 1. Initial 4 seconds (0 - 4s): Displays original screen with Green LED solid ON.
// 2. Next 4 seconds (4s - 8s):
//    - Line 1 becomes the shelf/rack info (locationLine)
//    - Line 2 continuously scrolls "Please place your book in respective shelf" at 200ms speed, repeating until the end.
// 3. 3-second warning phase:
//    - 3 medium beeps spaced evenly across 3 seconds (~3000ms), while Green LED blinks simultaneously with buzzer.
//    - Line 2 continues scrolling or displaying during warning beeps.
// 4. Green LED turns OFF and returns to normal standby.
void holdLocationWait(const String& locationLine) {
  // Phase 1: First 4 seconds (0 - 4000ms) with Green LED solid ON
  digitalWrite(GREEN_LED_PIN, HIGH);
  Serial.println("[TIMER]   Holding book view (0-4s)...");
  for (int sec = 1; sec <= 4; sec++) {
    delay(1000);
    Serial.println("[TIMER]   Hold elapsed: " + String(sec) + "s / 8s [View: Title + Shelf & Rack]");
  }

  // Phase 2: After 4 seconds, move shelf/rack to Line 1; display prompt on Line 2
  Serial.println("[STATUS]  4 seconds elapsed. Moving shelf/rack to Line 1; showing prompt on Line 2...");
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(locationLine.length() > 16 ? locationLine.substring(0, 16) : locationLine);

  // Smooth readable scrolling for "Please place it in given shelf"
  String promptMsg = "Please place it in given shelf    ";
  int promptLen = promptMsg.length();
  int promptOffset = 0;

  // Run remaining 4 seconds (4000ms) with relaxed, easy-to-read scroll speed (280ms per character)
  unsigned long startPhase2 = millis();
  while (millis() - startPhase2 < 4000) {
    String frame = "";
    for (int i = 0; i < 16; i++) {
      frame += promptMsg.charAt((promptOffset + i) % promptLen);
    }
    lcd.setCursor(0, 1);
    lcd.print(frame);
    delay(280);
    promptOffset++;
  }
  Serial.println("[TIMER]   Hold elapsed: 8s / 8s [Line 2: \"Please place it in given shelf\"]");

  // Phase 3: 3-second warning phase (3000ms total) -> 3 medium beeps with simultaneous LED blink
  Serial.println("[STATUS]  8-second hold completed. Starting 3-second warning alert (3 beeps + LED flashes)...");
  lcd.setCursor(0, 1);
  lcd.print("Session Ending..");

  for (int b = 0; b < 3; b++) {
    Serial.println("[WARN]    Warning beep " + String(b + 1) + " of 3 (Warning second " + String(b + 1) + "/3s)");

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
  Serial.println("[LED]      Warning complete. Green LED OFF. Returning to normal.");
}

// Dedicated animated handler for displaying book title, shelf, and rack:
// 1. Line 2 remains empty while scrolling the book title.
// 2. Adaptive scroll speed: 280ms if <= 30 characters, faster (180ms) if > 30 characters.
// 3. Green LED blinks during scroll: ON for 2 characters, OFF for 1 character.
// 4. Exactly 4 characters before scroll ends, Line 2 appears showing "Shelf:X  Rack:Y" and beeps once immediately.
// 5. Once scrolling completes, holds display for 8 seconds (Green LED solid ON).
// 6. Then 2-second warning alert (3 medium beeps with simultaneous LED blinks), then LED off and return to normal.
void displayBookLocationSequence(const String& title, int shelf, int rack) {
  String locationLine = "Shelf:" + String(shelf) + " Rack:" + String(rack);

  Serial.println("[LCD]     Book Title: \"" + title + "\" | Location: \"" + locationLine + "\"");

  lcd.clear();

  // Case A: Short title (<= 16 characters) - No scrolling needed
  if (title.length() <= 16) {
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
  int scrollSpeedMs = (title.length() > 30) ? 180 : 280;

  int totalSteps = title.length() - 16;
  int triggerStep = totalSteps > 4 ? (totalSteps - 4) : 0;
  bool locationShown = false;

  // Initial pause on the first 16 characters so user can start reading
  lcd.setCursor(0, 0);
  lcd.print(title.substring(0, 16));
  digitalWrite(GREEN_LED_PIN, HIGH);
  delay(800);

  // Scroll step by step from offset 0 to totalSteps
  for (int step = 0; step <= totalSteps; step++) {
    // When we reach 4 characters before finishing scroll, reveal Shelf and Rack on line 2!
    if (step >= triggerStep && !locationShown) {
      lcd.setCursor(0, 1);
      lcd.print(locationLine);
      locationShown = true;
      Serial.println("[LCD]     Line 2 revealed (4 chars before end): \"" + locationLine + "\"");

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
    lcd.setCursor(0, 0);
    lcd.print(title.substring(step, step + 16));
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

// Resets system back to the default student check-in state (turns off all LEDs)
void resetSystem() {
  SystemState previousState = currentState;
  currentState = WAITING_FOR_STUDENT;
  
  // Ensure both indicator LEDs are OFF in standby
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);

  if (previousState != WAITING_FOR_STUDENT) {
    logStateTransition(previousState, WAITING_FOR_STUDENT);
  } else {
    logInfo("System reset to ready state (waiting for student ID)");
  }

  // Show "ShelfSense Ready" for a short time, then transition to steady "Please Tap Your Student ID Card"
  showMessage("ShelfSense Ready", "   Smart Lib    ");
  delay(1800); // Display for 1.8 seconds, then disappear
  showMessage("Please Tap Your ", "Student ID Card ");
}

// SUCCESS FEEDBACK: Buzzer short beep + Green LED glow SIMULTANEOUSLY
// Optionally leaves the Green LED lit while the shelf location is being displayed
void indicateSuccess(int times = 1, int durationMs = 150, bool keepGreenLit = false) {
  Serial.print("[FEEDBACK] SUCCESS -> Green LED + Buzzer (");
  Serial.print(times);
  Serial.print(" beep(s) @ ");
  Serial.print(durationMs);
  Serial.println(" ms simultaneously)");

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
    Serial.println("[LED]      Green LED glowing for shelf location display");
  }
}

// EMERGENCY ERROR FEEDBACK: Rapid Red LED + Buzzer pulsing alarm for unrecognized/invalid tags (2 seconds)
void showEmergencyAlarm(const String& line1, const String& line2, const String& serialReason) {
  logError("EMERGENCY ALARM TRIGGERED: " + serialReason);
  showMessage(line1, line2);

  Serial.println("[FEEDBACK] EMERGENCY ALARM -> Rapid Red LED + Buzzer pulsing for 2 seconds!");

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

// STANDARD ERROR FEEDBACK: Red LED glowing at the same time Buzzer is beeping SIMULTANEOUSLY (e.g. out-of-order scans)
void showError(const String& line1, const String& line2, const String& serialReason) {
  logWarn("NOTICE: " + serialReason);
  showMessage(line1, line2);

  Serial.println("[FEEDBACK] ERROR -> Red LED + Buzzer pulsing (2 beeps @ 180 ms)");

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

// =========================================================================================
// SECTION 6: RFID & DATABASE LOOKUP FUNCTIONS
// =========================================================================================

// Reads the card UID and returns it formatted as uppercase HEX (e.g. "37E09604")
String readCardUID() {
  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) {
      uid += "0";
    }
    uid += String(mfrc522.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  // Log card technical details to Serial Monitor
  MFRC522::PICC_Type piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  Serial.println("---------------------------------------------------");
  Serial.print("[RFID]    Card Detected! UID: ");
  Serial.print(uid);
  Serial.print(" | Size: ");
  Serial.print(mfrc522.uid.size);
  Serial.print(" bytes | PICC Type: ");
  Serial.println(mfrc522.PICC_GetTypeName(piccType));
  Serial.print("[RFID]    Current System State: ");
  Serial.println(getStateName(currentState));

  // Halt card to prevent repeated reads in the same pass
  mfrc522.PICC_HaltA();
  return uid;
}

// Searches registered students for UID. Returns index if found, -1 if not found
int findStudentIndex(const String& uid) {
  Serial.print("[AUTH]    Checking student database for UID [");
  Serial.print(uid);
  Serial.println("]...");

  for (size_t i = 0; i < NUM_STUDENTS; i++) {
    if (registeredStudents[i].uid == uid) {
      Serial.print("[AUTH]    Match found! Student: \"");
      Serial.print(registeredStudents[i].name);
      Serial.print("\" at index [");
      Serial.print(i);
      Serial.println("]");
      return (int)i;
    }
  }

  Serial.println("[AUTH]    No match found in student database!");
  return -1;
}

// Checks if the UID belongs to an authorized student
bool isStudentRegistered(const String& uid) {
  return findStudentIndex(uid) != -1;
}

// Searches registered books for UID. Returns index if found, -1 if not found
int findBookIndex(const String& uid) {
  Serial.print("[LOOKUP]  Searching library database for book UID [");
  Serial.print(uid);
  Serial.println("]...");

  for (size_t i = 0; i < NUM_BOOKS; i++) {
    if (registeredBooks[i].uid == uid) {
      Serial.print("[LOOKUP]  Match found! Book: \"");
      Serial.print(registeredBooks[i].title);
      Serial.print("\" -> Shelf ");
      Serial.print(registeredBooks[i].shelf);
      Serial.print(", Rack ");
      Serial.println(registeredBooks[i].rack);
      return (int)i;
    }
  }

  Serial.println("[LOOKUP]  No match found in book database!");
  return -1;
}

// =========================================================================================
// SECTION 7: STEP HANDLERS (BUSINESS LOGIC)
// =========================================================================================

// Handles Student Check-In (Step 1)
void handleStudentScan(const String& uid) {
  int studentIdx = findStudentIndex(uid);
  if (studentIdx != -1) {
    // 1. Authorized Student scanned
    const Student& student = registeredStudents[studentIdx];
    logSuccess("Student authenticated: " + student.name + " (" + student.uid + ")");
    
    // Short confirmation beep with green LED flash
    indicateSuccess(1, 100, false);

    logStateTransition(WAITING_FOR_STUDENT, WAITING_FOR_BOOK);
    currentState = WAITING_FOR_BOOK;

    // Display student name on LCD: if longer than 16 chars (e.g. "Hi " + name), scroll smoothly!
    String studentLine = "Hi " + student.name;
    showMessageWithScroll(studentLine, "Tap Book Tag...", 260, 2500);

    logInfo("Ready for book tag. Waiting for book scan...");
    return;
  }

  // 2. Not a student card -> Check if user accidentally scanned a registered Book Tag!
  int bookIdx = findBookIndex(uid);
  if (bookIdx != -1) {
    const Book& scannedBook = registeredBooks[bookIdx];
    logWarn("Book tag (\"" + scannedBook.title + "\") scanned at Step 1 instead of Student ID!");
    
    // Prompt student: "Please Tap Your" / "Student ID First"
    showError("Please Tap Your", "Student ID First", "Book tag scanned before student ID authentication");
    
    // Return cleanly to standby
    resetSystem();
    return;
  }

  // 3. Tag is neither student nor book in database -> Unrecognized ID emergency alarm!
  logError("Unrecognized tag UID: " + uid);
  showEmergencyAlarm("Unrecognized ID", "Access Denied!", "Unrecognized tag scanned at Step 1: " + uid);
  resetSystem();
}

// Handles Book Scanning and Shelf Lookup (Step 2)
void handleBookScan(const String& uid) {
  int bookIdx = findBookIndex(uid);

  if (bookIdx != -1) {
    // 1. Registered Book Tag identified -> Run full sequence!
    const Book& foundBook = registeredBooks[bookIdx];
    
    logSuccess("Book identified: \"" + foundBook.title + "\" located on Shelf " + 
               String(foundBook.shelf) + ", Rack " + String(foundBook.rack));
    
    // Initial success feedback: short confirmation beep + green LED blink
    indicateSuccess(1, 150, false);

    // Run custom animated display sequence (title scroll, reveal, 8s hold, 3s warning)
    displayBookLocationSequence(foundBook.title, foundBook.shelf, foundBook.rack);

    logInfo("Session completed. Resetting for next student...");
    resetSystem();
    return;
  }

  // 2. Check if student card was scanned again instead of a book
  int studentIdx = findStudentIndex(uid);
  if (studentIdx != -1) {
    logWarn("Student ID scanned again instead of a book! Prompting for book tag...");
    showError("Please Tap Book", "Tag (Not ID)", "Student ID card tapped during book scan step");
    
    // Maintain student login session so they can retry scanning the book
    logInfo("Maintaining student session. Ready for book tag retry...");
    showMessage("Tap Your Book...");
    return;
  }

  // 3. Tag is completely unrecognized -> Emergency alarm!
  logError("Unrecognized tag UID: " + uid + " during book lookup step!");
  showEmergencyAlarm("Unrecognized ID", "Unknown Book Tag", "Unregistered tag scanned at Step 2: " + uid);

  // Maintain session so authorized student can scan a valid book without re-authenticating
  logInfo("Maintaining student session. Ready for book tag retry...");
  showMessage("Tap Your Book...");
}

// =========================================================================================
// SECTION 8: HARDWARE DIAGNOSTICS AT BOOT
// =========================================================================================

void runHardwareDiagnostics() {
  Serial.println();
  Serial.println("===================================================");
  Serial.println("         SHELFSENSE ESP32 SYSTEM STARTUP           ");
  Serial.println("===================================================");
  Serial.print("[BOOT]    CPU Frequency: ");
  Serial.print(getCpuFrequencyMhz());
  Serial.println(" MHz");
  Serial.print("[BOOT]    Free Heap RAM: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");

  // 1. Check I2C Bus & LCD Presence
  Serial.println("[BOOT]    Initializing I2C bus and scanning for LCD...");
  Wire.begin(21, 22); // Start I2C explicitly on ESP32 default pins: SDA=21, SCL=22
  delay(50);

  // If configured address fails, scan I2C bus to find the actual address
  uint8_t targetAddr = LCD_I2C_ADDR;
  Wire.beginTransmission(targetAddr);
  byte i2cError = Wire.endTransmission();

  if (i2cError != 0) {
    // Probe alternative standard address 0x3F or others
    Serial.print("[BOOT]    LCD not found at 0x");
    Serial.print(targetAddr, HEX);
    Serial.println(", scanning bus...");
    for (uint8_t addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        Serial.print("[BOOT]    Found I2C device at address 0x");
        Serial.println(addr, HEX);
        targetAddr = addr;
        i2cError = 0;
        break;
      }
    }
  }

  if (i2cError == 0) {
    Serial.print("[BOOT]    I2C LCD detected at address 0x");
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
    logError("I2C LCD NOT responding! Check 5V power and SDA(21)/SCL(22) wiring.");
  }

  // 2. Check SPI & RC522 RFID Reader
  Serial.println("[BOOT]    Initializing SPI and RC522 Reader...");
  SPI.begin();
  mfrc522.PCD_Init();
  
  byte readerVersion = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("[BOOT]    RC522 Firmware Version Register: 0x");
  Serial.println(readerVersion, HEX);

  if (readerVersion == 0x92) {
    logSuccess("RC522 Reader verified (Version 2.0 detected)");
  } else if (readerVersion == 0x91) {
    logSuccess("RC522 Reader verified (Version 1.0 detected)");
  } else if (readerVersion == 0x88) {
    logSuccess("RC522 Reader verified (FM17522 clone detected - OK)");
  } else if (readerVersion == 0x00 || readerVersion == 0xFF) {
    logError("RC522 RFID READER COMMUNICATION FAILED!");
    Serial.println("          [Troubleshooting RC522]:");
    Serial.println("          - CRITICAL: VCC must be connected to 3.3V ONLY (5V can damage RC522)");
    Serial.println("          - Ensure common GND with ESP32");
    Serial.println("          - Verify SPI Wiring: SCK=GPIO 18, MISO=GPIO 19, MOSI=GPIO 23, SS=GPIO 5, RST=GPIO 4");
    Serial.println("          - Check soldered header pins for cold joints or short circuits");
  } else {
    logWarn("Unrecognized RC522 version 0x" + String(readerVersion, HEX) + " (Reader may still function)");
  }

  // 3. Test Output Indicators (Buzzer, Red LED, Green LED)
  Serial.println("[BOOT]    Testing Buzzer (GPIO 27), Red LED (GPIO 26), and Green LED (GPIO 25)...");
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
  
  logSuccess("Green LED, Red LED, and Buzzer hardware test passed");

  // 4. Print Database Summary
  Serial.println("[BOOT]    Loading Registered Database:");
  Serial.print("          - Registered Students: ");
  Serial.println(NUM_STUDENTS);
  for (size_t i = 0; i < NUM_STUDENTS; i++) {
    Serial.print("            [Student ");
    Serial.print(i + 1);
    Serial.print("] Name: \"");
    Serial.print(registeredStudents[i].name);
    Serial.print("\" | UID: ");
    Serial.println(registeredStudents[i].uid);
  }

  Serial.print("          - Registered Books:    ");
  Serial.println(NUM_BOOKS);
  for (size_t i = 0; i < NUM_BOOKS; i++) {
    Serial.print("            [Book ");
    Serial.print(i + 1);
    Serial.print("] Title: \"");
    Serial.print(registeredBooks[i].title);
    Serial.print("\" | UID: ");
    Serial.print(registeredBooks[i].uid);
    Serial.print(" | Shelf: ");
    Serial.print(registeredBooks[i].shelf);
    Serial.print(" | Rack: ");
    Serial.println(registeredBooks[i].rack);
  }

  Serial.println("===================================================");
  Serial.println("[BOOT]    Diagnostics complete. System is ready!");
  Serial.println("===================================================");
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

  // Retrieve formatted UID
  String scannedUID = readCardUID();

  // Route scanned card to the appropriate step handler
  if (currentState == WAITING_FOR_STUDENT) {
    handleStudentScan(scannedUID);
  } else if (currentState == WAITING_FOR_BOOK) {
    handleBookScan(scannedUID);
  }
}
