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
 *              | Cathode (-)          | GND        | No (Fixed)    | Connect with 220Ω resistor
 *   Green LED  | Anode (+) [Long leg] | GPIO 25    | YES           | Configurable (GREEN_LED_PIN)
 *              | Cathode (-)          | GND        | No (Fixed)    | Connect with 220Ω resistor
 * 
 * -----------------------------------------------------------------------------------------
 * CRITICAL ARCHITECTURAL RULES:
 *  - SILENT BACKGROUND CLOUD SYNC: All Google Cloud Firestore synchronizations occur
 *    completely in the background via a dedicated FreeRTOS worker task on Core 0.
 *  - ZERO CLOUD/WIFI ON LCD: The LCD is strictly dedicated to physical user guidance.
 *    No Wi-Fi, cloud, or sync status is ever printed to the LCD screen.
 *  - SERIAL DIAGNOSTICS: Every network state change, NTP synchronization, RFID read,
 *    state transition, and Firestore document upload is logged with clear tags:
 *    [BOOT], [WIFI], [TIME], [RFID], [AUTH], [LOOKUP], [STATE], [FEEDBACK], [LCD], [FIREBASE].
 *  - ACCURATE TIMESTAMPING: Local time synchronized via NTP (pool.ntp.org) with IST offset
 *    (GMT+5:30 / 19800 seconds). Injected as "YYYY-MM-DD HH:MM:SS" in every Firestore document.
 * =========================================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// =========================================================================================
// SECTION 1: HARDWARE PINS & SYSTEM CONFIGURATION
// =========================================================================================

// RC522 RFID Reader Pins (SPI)
#define RST_PIN             4       // Reset pin for RC522
#define SS_PIN              5       // Slave Select / SDA pin for RC522

// Feedback Peripherals
#define BUZZER_PIN          27      // Active Buzzer positive pin
#define RED_LED_PIN         26      // Error / Alert Red LED pin (220Ω resistor)
#define GREEN_LED_PIN       25      // Success / Location Green LED pin (220Ω resistor)

// LCD I2C Configuration
#define LCD_I2C_ADDR        0x27    // Standard I2C address (0x27 or 0x3F)
#define LCD_COLUMNS         16      // 16 characters wide
#define LCD_ROWS            2       // 2 lines high

// Session Timeout Configuration
#define SESSION_TIMEOUT_MS  20000   // 20-second non-blocking inactivity window for book scanning

// Baud Rate for USB Serial Debugging
#define SERIAL_BAUD         115200

// Wi-Fi Configuration
#define WIFI_SSID           "Wokwi-GUEST"   // Replace with your local Wi-Fi SSID
#define WIFI_PASSWORD       ""              // Replace with your Wi-Fi password

// Firebase & Google Cloud Firestore Configuration
#define FIREBASE_PROJECT_ID "shelf-sense-f86ba"
#define FIREBASE_API_KEY    "AIzaSyDHboFG0dyoMTLCH0bkZ85OkWk2yxBXVRQ"
#define FIRESTORE_COLLECTION "library_logs"

// NTP Time Synchronization Configuration (IST: UTC + 5:30 = 19800 seconds)
#define NTP_SERVER_1        "pool.ntp.org"
#define NTP_SERVER_2        "time.google.com"
#define GMT_OFFSET_SEC      19800   // IST: 5 hours 30 mins = 19800 seconds
#define DAYLIGHT_OFFSET_SEC 0       // No daylight saving time in IST

// =========================================================================================
// SECTION 2: HARDWARE & CLOUD OBJECT INSTANCES
// =========================================================================================

// MFRC522 instance communicating over SPI (SCK=18, MISO=19, MOSI=23, SS=5, RST=4)
MFRC522 mfrc522(SS_PIN, RST_PIN);

// LiquidCrystal_I2C instance communicating over standard I2C (SDA=21, SCL=22)
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLUMNS, LCD_ROWS);

// Mobizt Firebase Data Objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// FreeRTOS Queue & Task handles for non-blocking background Firestore sync
QueueHandle_t firestoreQueue = NULL;
TaskHandle_t firestoreTaskHandle = NULL;

// =========================================================================================
// SECTION 3: SYSTEM STATES & IN-MEMORY DATABASES
// =========================================================================================

// Finite State Machine (FSM) States
enum SystemState {
  WAITING_FOR_STUDENT,  // Ready state: Waiting for student ID card to be scanned
  WAITING_FOR_BOOK      // Authenticated state: Waiting for book tag to be scanned
};

// Variable holding the current system state
SystemState currentState = WAITING_FOR_STUDENT;

// Session timer tracking (inactivity detection in WAITING_FOR_BOOK)
unsigned long sessionStartTime = 0;

// Authenticated student session variables
char activeStudentName[32] = "";
char activeStudentUID[32]  = "";

// Structure modeling a registered student
struct Student {
  const char* name;     // Student's full name (e.g. "Pavan")
  const char* uid;      // RFID tag UID in uppercase HEX without spaces (e.g. "37E09604")
};

// Structure modeling a registered library book
struct Book {
  const char* title;    // Book title
  const char* uid;      // RFID tag UID in uppercase HEX without spaces
  uint8_t shelf;        // Assigned shelf number
  uint8_t rack;         // Assigned rack number
};

// Structure modeling a Firestore log event for background asynchronous queueing
struct FirestoreLogEvent {
  char eventType[20];   // STUDENT_LOGIN, BOOK_PROCESSED, SESSION_TIMEOUT, SECURITY_ALERT
  char studentName[32]; // Authenticated student name
  char studentUID[32];  // Authenticated student UID
  char bookTitle[64];   // Scanned book title (or empty)
  char bookUID[32];     // Scanned book UID (or empty)
  uint8_t shelf;        // Physical shelf location
  uint8_t rack;         // Physical rack location
  char timestamp[25];   // "YYYY-MM-DD HH:MM:SS"
  char statusNote[96];  // Diagnostic status note
};

// -----------------------------------------------------------------------------------------
// DATABASE ENTRIES: Students & Books
// -----------------------------------------------------------------------------------------

// List of authorized students: { "Student Name", "RFID UID" }
const Student registeredStudents[] = {
  { "Pavan",   "37E09604"       },
  { "Rehan",   "A97AF8B2"       },
  { "DVS",     "0203448B258000" },
  { "Pruthvi", "C94B8554"       }
};

// Database of registered books: { "Book Title", "RFID UID", Shelf Number, Rack Number }
const Book registeredBooks[] = {
  { "Geronimo Stilton - Journey to the Center of the Earth", "F10F7E05", 2, 1 }, // Shelf 2, Rack 1
  { "Harry Potter",                                          "8DC0FA6B", 4, 3 }  // Shelf 4, Rack 3
};

// Constants for database sizing
const size_t NUM_STUDENTS = sizeof(registeredStudents) / sizeof(registeredStudents[0]);
const size_t NUM_BOOKS    = sizeof(registeredBooks) / sizeof(registeredBooks[0]);

// =========================================================================================
// SECTION 4: SERIAL LOGGING & DIAGNOSTIC HELPERS
// =========================================================================================

// Returns human-readable state string
const char* getStateName(SystemState state) {
  switch (state) {
    case WAITING_FOR_STUDENT: return "WAITING_FOR_STUDENT";
    case WAITING_FOR_BOOK:    return "WAITING_FOR_BOOK";
    default:                  return "UNKNOWN_STATE";
  }
}

// Log general informational messages
void logInfo(const __FlashStringHelper* message) {
  Serial.print(F("[INFO]     "));
  Serial.println(message);
}
void logInfo(const char* message) {
  Serial.print(F("[INFO]     "));
  Serial.println(message);
}

// Log success events
void logSuccess(const __FlashStringHelper* message) {
  Serial.print(F("[SUCCESS]  "));
  Serial.println(message);
}
void logSuccess(const char* message) {
  Serial.print(F("[SUCCESS]  "));
  Serial.println(message);
}

// Log warning messages
void logWarn(const __FlashStringHelper* message) {
  Serial.print(F("[WARN]     "));
  Serial.println(message);
}
void logWarn(const char* message) {
  Serial.print(F("[WARN]     "));
  Serial.println(message);
}

// Log error events
void logError(const __FlashStringHelper* message) {
  Serial.print(F("[ERROR]    "));
  Serial.println(message);
}
void logError(const char* message) {
  Serial.print(F("[ERROR]    "));
  Serial.println(message);
}

// Log state machine transitions
void logStateTransition(SystemState fromState, SystemState toState) {
  Serial.print(F("[STATE]    Transition: "));
  Serial.print(getStateName(fromState));
  Serial.print(F(" -> "));
  Serial.println(getStateName(toState));
}

// =========================================================================================
// SECTION 5: ACCURATE TIMESTAMPING VIA NTP
// =========================================================================================

// Formats the current local time into "YYYY-MM-DD HH:MM:SS"
// If NTP synchronization is pending or unavailable, provides graceful fallback without stalling
void getCurrentTimestamp(char* outBuf, size_t maxLen) {
  struct tm timeinfo;
  // Non-blocking query with 20ms timeout so execution is never stalled
  if (getLocalTime(&timeinfo, 20)) {
    strftime(outBuf, maxLen, "%Y-%m-%d %H:%M:%S", &timeinfo);
  } else {
    // Graceful fallback timestamp based on ESP32 runtime uptime
    unsigned long sec = millis() / 1000;
    unsigned long h   = (sec / 3600) % 24;
    unsigned long m   = (sec / 60) % 60;
    unsigned long s   = sec % 60;
    snprintf(outBuf, maxLen, "1970-01-01 %02lu:%02lu:%02lu", h, m, s);
    Serial.println(F("[TIME]     NTP sync pending; formatted fallback uptime timestamp"));
  }
}

// =========================================================================================
// SECTION 6: BACKGROUND FIRESTORE SYNCHRONIZATION (FREERTOS WORKER TASK)
// =========================================================================================

// Dispatches a Firestore log event to the FreeRTOS background queue (Zero-blocking execution)
void queueFirestoreLog(const char* eventType,
                       const char* studentName,
                       const char* studentUID,
                       const char* bookTitle,
                       const char* bookUID,
                       uint8_t shelf,
                       uint8_t rack,
                       const char* statusNote) {
  if (firestoreQueue == NULL) {
    Serial.println(F("[FIREBASE] ERROR: Firestore background queue not initialized!"));
    return;
  }

  FirestoreLogEvent logEvent;
  memset(&logEvent, 0, sizeof(logEvent));

  strncpy(logEvent.eventType, eventType, sizeof(logEvent.eventType) - 1);
  strncpy(logEvent.studentName, studentName ? studentName : "", sizeof(logEvent.studentName) - 1);
  strncpy(logEvent.studentUID,  studentUID  ? studentUID  : "", sizeof(logEvent.studentUID)  - 1);
  strncpy(logEvent.bookTitle,   bookTitle   ? bookTitle   : "", sizeof(logEvent.bookTitle)   - 1);
  strncpy(logEvent.bookUID,     bookUID     ? bookUID     : "", sizeof(logEvent.bookUID)     - 1);
  logEvent.shelf = shelf;
  logEvent.rack  = rack;
  getCurrentTimestamp(logEvent.timestamp, sizeof(logEvent.timestamp));
  strncpy(logEvent.statusNote,  statusNote  ? statusNote  : "", sizeof(logEvent.statusNote)  - 1);

  Serial.print(F("[FIREBASE] Dispatching '"));
  Serial.print(logEvent.eventType);
  Serial.println(F("' event to background sync queue..."));

  // Send to queue non-blockingly (timeout = 0 ticks)
  if (xQueueSend(firestoreQueue, &logEvent, 0) != pdTRUE) {
    Serial.println(F("[FIREBASE] WARN: Background queue full! Dropping oldest log event."));
  }
}

// FreeRTOS Background Task running on Core 0 to handle Wi-Fi & Firestore synchronization
void firestoreSyncTask(void* pvParameters) {
  Serial.println(F("[FIREBASE] Background Firestore sync worker task started on Core 0"));

  // Configure Firebase Credentials & Connection Parameters
  config.api_key = FIREBASE_API_KEY;
  config.token_status_callback = tokenStatusCallback;
  Firebase.reconnectNetwork(true);
  fbdo.setBSSLBufferSize(4096 /* Rx buffer */, 1024 /* Tx buffer */);
  fbdo.setResponseSize(2048);

  // Wait for initial Wi-Fi connection
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  Serial.println(F("[FIREBASE] Wi-Fi link ready. Initializing Firebase Authentication..."));
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println(F("[FIREBASE] Anonymous Authentication initialized successfully"));
  } else {
    Serial.print(F("[FIREBASE] Authentication status note: "));
    Serial.println(config.signer.signupError.message.c_str());
  }

  Firebase.begin(&config, &auth);
  Serial.println(F("[FIREBASE] Client library initialized. Ready for background document sync."));

  FirestoreLogEvent event;

  // Infinite processing loop on Core 0
  for (;;) {
    // Wait up to 100ms for incoming queued log events
    if (xQueueReceive(firestoreQueue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
      Serial.println(F("---------------------------------------------------"));
      Serial.print(F("[FIREBASE] Processing queued event: "));
      Serial.println(event.eventType);

      // Verify Wi-Fi connectivity
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[WIFI]     Connection lost! Waiting for background link restore..."));
        unsigned long reconnectStart = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - reconnectStart < 6000)) {
          vTaskDelay(pdMS_TO_TICKS(250));
        }
      }

      if (WiFi.status() == WL_CONNECTED) {
        // Ensure Firebase token is ready
        unsigned long tokenStart = millis();
        while (!Firebase.ready() && (millis() - tokenStart < 5000)) {
          vTaskDelay(pdMS_TO_TICKS(150));
        }

        if (Firebase.ready()) {
          // Construct Firestore Document JSON according to required schema
          FirebaseJson content;
          content.set("fields/eventType/stringValue", event.eventType);
          content.set("fields/studentName/stringValue", event.studentName);
          content.set("fields/studentUID/stringValue", event.studentUID);
          content.set("fields/bookTitle/stringValue", event.bookTitle);
          content.set("fields/bookUID/stringValue", event.bookUID);
          content.set("fields/shelf/integerValue", String(event.shelf));
          content.set("fields/rack/integerValue", String(event.rack));
          content.set("fields/timestamp/stringValue", event.timestamp);
          content.set("fields/statusNote/stringValue", event.statusNote);

          Serial.print(F("[FIREBASE] Target Collection: '"));
          Serial.print(FIRESTORE_COLLECTION);
          Serial.println(F("'"));
          Serial.print(F("[FIREBASE] Document JSON Payload: "));
          Serial.println(content.raw());

          // Automatic document creation in Google Cloud Firestore
          if (Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "", FIRESTORE_COLLECTION, content.raw())) {
            Serial.print(F("[FIREBASE] Document sync SUCCESS! HTTP Code: "));
            Serial.println(fbdo.httpCode());
            Serial.print(F("[FIREBASE] Server Response: "));
            Serial.println(fbdo.payload());
          } else {
            Serial.print(F("[FIREBASE] Document sync FAILED! HTTP Code: "));
            Serial.print(fbdo.httpCode());
            Serial.print(F(" | Reason: "));
            Serial.println(fbdo.errorReason());
          }
        } else {
          Serial.println(F("[FIREBASE] ERROR: Firebase token not ready, document sync deferred."));
        }
      } else {
        Serial.println(F("[FIREBASE] ERROR: Wi-Fi unavailable, unable to sync document."));
      }
      Serial.println(F("---------------------------------------------------"));
    }

    // Yield control to FreeRTOS watchdog & background token refresh
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Wi-Fi Event Handler for detailed Serial diagnostic logging
void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println(F("[WIFI]     Station mode started"));
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.print(F("[WIFI]     Connected to AP: "));
      Serial.println(WIFI_SSID);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print(F("[WIFI]     IP Address obtained: "));
      Serial.print(WiFi.localIP());
      Serial.print(F(" | RSSI: "));
      Serial.print(WiFi.RSSI());
      Serial.println(F(" dBm"));

      // Configure SNTP time synchronization once IP is obtained
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
      Serial.println(F("[TIME]     NTP synchronization started (pool.ntp.org, IST GMT+5:30)"));
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println(F("[WIFI]     Disconnected from Wi-Fi! Auto-reconnecting in background..."));
      WiFi.reconnect();
      break;
    default:
      break;
  }
}

// =========================================================================================
// SECTION 7: HARDWARE FEEDBACK & LCD DISPLAY HELPERS
// =========================================================================================

// Displays a message on the LCD and mirrors it to the Serial Monitor
void showMessage(const char* line1, const char* line2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);

  char buf[17];
  strncpy(buf, line1, 16);
  buf[16] = '\0';
  lcd.print(buf);

  Serial.print(F("[LCD]      Line 1: \""));
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

// Scrolls line 1 smoothly from right to left if it exceeds 16 characters while keeping line 2 static
void showMessageWithScroll(const char* line1, const char* line2 = "", int scrollSpeedMs = 280, int totalDurationMs = 3500) {
  Serial.print(F("[LCD]      Line 1: \""));
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

  // Initial view pause so patron can begin reading
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

// Helper to handle the post-scroll 8-second shelf/rack hold and 3-second warning beeps:
// 1. Initial 4 seconds (0 - 4s): Displays original screen with Green LED solid ON.
// 2. Next 4 seconds (4s - 8s):
//    - Line 1 becomes the shelf/rack info (locationLine)
//    - Line 2 continuously scrolls "Please place it in given shelf" at 280ms speed, repeating until the end.
// 3. 3-second warning phase:
//    - 3 medium beeps spaced evenly across 3 seconds (500ms ON / 500ms OFF), while Green LED blinks simultaneously.
// 4. Green LED turns OFF and returns to normal standby.
void holdLocationWait(const char* locationLine) {
  // Phase 1: First 4 seconds (0 - 4000ms) with Green LED solid ON
  digitalWrite(GREEN_LED_PIN, HIGH);
  Serial.println(F("[TIMER]    Holding book view (0-4s) with solid Green LED..."));
  for (int sec = 1; sec <= 4; sec++) {
    delay(1000);
    Serial.print(F("[TIMER]    Hold elapsed: "));
    Serial.print(sec);
    Serial.println(F("s / 8s [View: Title + Shelf & Rack]"));
  }

  // Phase 2: After 4 seconds, move shelf/rack to Line 1; display prompt on Line 2
  Serial.println(F("[STATE]    4 seconds elapsed. Moving shelf/rack to Line 1; scrolling prompt on Line 2..."));
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
  Serial.println(F("[TIMER]    Hold elapsed: 8s / 8s [Line 2: \"Please place it in given shelf\"]"));

  // Phase 3: 3-second warning phase (3000ms total) -> 3 medium beeps with simultaneous LED blink
  Serial.println(F("[STATE]    8-second hold completed. Starting 3-second warning alert (3 beeps + LED flashes)..."));
  lcd.setCursor(0, 1);
  lcd.print(F("Session Ending.."));

  for (int b = 0; b < 3; b++) {
    Serial.print(F("[WARN]     Warning beep "));
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
  Serial.println(F("[FEEDBACK] Warning complete. Green LED OFF. Returning to standby."));
}

// Dedicated animated handler for displaying book title, shelf, and rack:
// 1. Line 2 remains empty while scrolling the book title.
// 2. Adaptive scroll speed: 280ms if <= 30 characters, faster (180ms) if > 30 characters.
// 3. Green LED blinks rhythmically during title scroll: ON for 2 characters, OFF for 1 character.
// 4. Exactly 4 characters before scroll ends, Line 2 appears showing "Shelf:X  Rack:Y" and chirps once immediately.
// 5. Once scrolling completes, holds display for 8 seconds (0-4s solid Green, 4-8s prompt scroll).
// 6. 3-second warning phase: 3 medium beeps + simultaneous Green LED flashes with "Session Ending..".
void displayBookLocationSequence(const char* title, uint8_t shelf, uint8_t rack) {
  char locationLine[32];
  snprintf(locationLine, sizeof(locationLine), "Shelf:%u  Rack:%u", (unsigned int)shelf, (unsigned int)rack);

  Serial.print(F("[LCD]      Book Title: \""));
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

    // Wait 8 seconds followed by 3-second warning beeps/blinks
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
    // Exactly 4 characters before finishing scroll, reveal Shelf and Rack on line 2!
    if (step >= triggerStep && !locationShown) {
      lcd.setCursor(0, 1);
      lcd.print(locationLine);
      locationShown = true;
      Serial.print(F("[LCD]      Line 2 revealed (4 chars before end): \""));
      Serial.print(locationLine);
      Serial.println(F("\""));

      // Audio chime chirp immediately when shelf & rack appears
      digitalWrite(BUZZER_PIN, HIGH);
      delay(100);
      digitalWrite(BUZZER_PIN, LOW);
    }

    // Green LED rhythmic blinking pattern: ON for 2 chars, OFF for 1 char
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

  // Ensure Line 2 is displayed if it was triggered late
  if (!locationShown) {
    lcd.setCursor(0, 1);
    lcd.print(locationLine);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
  }

  // After scrolling completes: wait 8 seconds with solid LED, followed by 3 warning beeps/blinks over 3 seconds
  holdLocationWait(locationLine);
}

// Resets system back to the default student check-in state (turns off all LEDs)
void resetSystem() {
  SystemState previousState = currentState;
  currentState = WAITING_FOR_STUDENT;
  sessionStartTime = 0; // Reset session inactivity timer

  // Clear active authenticated student session credentials
  activeStudentName[0] = '\0';
  activeStudentUID[0]  = '\0';

  // Ensure both indicator LEDs are OFF in standby
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);

  if (previousState != WAITING_FOR_STUDENT) {
    logStateTransition(previousState, WAITING_FOR_STUDENT);
  } else {
    logInfo(F("System reset to ready state (waiting for student ID)"));
  }

  // Show "ShelfSense Ready" for 1.8 seconds, then transition to steady "Please Tap Your Student ID Card"
  showMessage("ShelfSense Ready", " Smart Library  ");
  delay(1800);
  showMessage("Please Tap Your ", "Student ID Card ");
}

// SUCCESS FEEDBACK: Short buzzer chirp + Green LED flash SIMULTANEOUSLY
void indicateSuccess(int times = 1, int durationMs = 150) {
  Serial.print(F("[FEEDBACK] SUCCESS -> Green LED + Buzzer ("));
  Serial.print(times);
  Serial.print(F(" pulse(s) @ "));
  Serial.print(durationMs);
  Serial.println(F(" ms simultaneously)"));

  for (int i = 0; i < times; i++) {
    digitalWrite(GREEN_LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);

    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);

    if (i < times - 1) {
      delay(100);
    }
  }
}

// EMERGENCY ERROR FEEDBACK: Rapid Red LED + Buzzer pulsing alarm for unrecognized/invalid tags (2 seconds)
void showEmergencyAlarm(const char* line1, const char* line2, const char* serialReason) {
  Serial.print(F("[ERROR]    EMERGENCY ALARM TRIGGERED: "));
  Serial.println(serialReason);
  showMessage(line1, line2);

  Serial.println(F("[FEEDBACK] EMERGENCY ALARM -> 10 rapid pulses (Red LED + Buzzer) across 2 seconds!"));

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

// STANDARD ERROR FEEDBACK: Red LED + Buzzer pulsing simultaneously (2 beeps @ 180 ms)
void showError(const char* line1, const char* line2, const char* serialReason) {
  Serial.print(F("[WARN]     NOTICE: "));
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

  delay(1500); // Pause so patron can read prompt on LCD
}

// =========================================================================================
// SECTION 8: RFID & DATABASE LOOKUP FUNCTIONS
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
  Serial.print(F("[RFID]     Card Detected! UID: "));
  Serial.print(outBuffer);
  Serial.print(F(" | Size: "));
  Serial.print(mfrc522.uid.size);
  Serial.print(F(" bytes | PICC Type: "));
  Serial.println(mfrc522.PICC_GetTypeName(piccType));
  Serial.print(F("[RFID]     Current System State: "));
  Serial.println(getStateName(currentState));

  // Halt card to prevent repeated reads in the same pass
  mfrc522.PICC_HaltA();
}

// Searches registered students for UID. Returns index if found, -1 if not found
int findStudentIndex(const char* uid) {
  Serial.print(F("[AUTH]     Checking student database for UID ["));
  Serial.print(uid);
  Serial.println(F("]..."));

  for (size_t i = 0; i < NUM_STUDENTS; i++) {
    if (strcmp(registeredStudents[i].uid, uid) == 0) {
      Serial.print(F("[AUTH]     Match found! Student: \""));
      Serial.print(registeredStudents[i].name);
      Serial.print(F("\" at index ["));
      Serial.print(i);
      Serial.println(F("]"));
      return (int)i;
    }
  }

  Serial.println(F("[AUTH]     No match found in student database!"));
  return -1;
}

// Searches registered books for UID. Returns index if found, -1 if not found
int findBookIndex(const char* uid) {
  Serial.print(F("[LOOKUP]   Searching library database for book UID ["));
  Serial.print(uid);
  Serial.println(F("]..."));

  for (size_t i = 0; i < NUM_BOOKS; i++) {
    if (strcmp(registeredBooks[i].uid, uid) == 0) {
      Serial.print(F("[LOOKUP]   Match found! Book: \""));
      Serial.print(registeredBooks[i].title);
      Serial.print(F("\" -> Shelf "));
      Serial.print(registeredBooks[i].shelf);
      Serial.print(F(", Rack "));
      Serial.println(registeredBooks[i].rack);
      return (int)i;
    }
  }

  Serial.println(F("[LOOKUP]   No match found in book database!"));
  return -1;
}

// =========================================================================================
// SECTION 9: STEP HANDLERS & FINITE STATE MACHINE LOGIC
// =========================================================================================

// Handles Student Check-In (State 1: WAITING_FOR_STUDENT)
void handleStudentScan(const char* uid) {
  int studentIdx = findStudentIndex(uid);
  if (studentIdx != -1) {
    // 1. Authorized Student scanned
    const Student& student = registeredStudents[studentIdx];
    Serial.print(F("[SUCCESS]  Student authenticated: "));
    Serial.print(student.name);
    Serial.print(F(" ("));
    Serial.print(student.uid);
    Serial.println(F(")"));

    // Immediate local feedback: short green LED flash + simultaneous buzzer beep
    indicateSuccess(1, 100);

    // Store authenticated student credentials in session memory
    strncpy(activeStudentName, student.name, sizeof(activeStudentName) - 1);
    activeStudentName[sizeof(activeStudentName) - 1] = '\0';
    strncpy(activeStudentUID, student.uid, sizeof(activeStudentUID) - 1);
    activeStudentUID[sizeof(activeStudentUID) - 1] = '\0';

    // Silent Background Cloud Sync: Dispatch STUDENT_LOGIN event to Firestore
    queueFirestoreLog("STUDENT_LOGIN", activeStudentName, activeStudentUID, "", "", 0, 0, "Student authenticated successfully");

    // Transition state
    logStateTransition(WAITING_FOR_STUDENT, WAITING_FOR_BOOK);
    currentState = WAITING_FOR_BOOK;

    // Start 20-second non-blocking session timer
    sessionStartTime = millis();
    Serial.print(F("[INFO]     Session timer started ("));
    Serial.print(SESSION_TIMEOUT_MS / 1000);
    Serial.println(F("s window for book scan)"));

    // Display student greeting: smoothly scroll if name > 16 characters
    char studentLine[48];
    snprintf(studentLine, sizeof(studentLine), "Hi %s", student.name);
    showMessageWithScroll(studentLine, "Tap Book Tag...", 260, 2500);

    Serial.println(F("[INFO]     Ready for book tag. Waiting for book scan..."));
    return;
  }

  // 2. Book tag scanned instead of Student ID Card
  int bookIdx = findBookIndex(uid);
  if (bookIdx != -1) {
    const Book& scannedBook = registeredBooks[bookIdx];
    Serial.print(F("[WARN]     Book tag (\""));
    Serial.print(scannedBook.title);
    Serial.println(F("\") scanned at Step 1 instead of Student ID!"));

    // Red LED + error buzz, prompt user
    showError("Please Tap Your ", "Student ID First", "Book tag scanned before student ID authentication");

    // Return to standby
    resetSystem();
    return;
  }

  // 3. Unrecognized tag scanned -> Emergency alarm & SECURITY_ALERT
  Serial.print(F("[ERROR]    Unrecognized tag UID: "));
  Serial.println(uid);
  char reason[96];
  snprintf(reason, sizeof(reason), "Unrecognized card scanned at Step 1 (UID: %s)", uid);

  // Silent Background Cloud Sync: Dispatch SECURITY_ALERT document to Firestore
  queueFirestoreLog("SECURITY_ALERT", "UNKNOWN", uid, "", "", 0, 0, reason);

  // Emergency Alarm: 10 rapid pulses of Red LED + Buzzer across 2 seconds
  showEmergencyAlarm("Unrecognized ID ", " Access Denied! ", reason);
  resetSystem();
}

// Handles Book Scanning & Shelf Guidance (State 2: WAITING_FOR_BOOK)
void handleBookScan(const char* uid) {
  int bookIdx = findBookIndex(uid);

  if (bookIdx != -1) {
    // 1. Registered Book Tag identified
    const Book& foundBook = registeredBooks[bookIdx];

    Serial.print(F("[SUCCESS]  Book identified: \""));
    Serial.print(foundBook.title);
    Serial.print(F("\" located on Shelf "));
    Serial.print(foundBook.shelf);
    Serial.print(F(", Rack "));
    Serial.println(foundBook.rack);

    // Immediate confirmation chirp + green LED flash
    indicateSuccess(1, 150);

    // Stop session timer
    sessionStartTime = 0;

    // Silent Background Cloud Sync: Dispatch BOOK_PROCESSED document to Firestore
    char note[96];
    snprintf(note, sizeof(note), "Book mapped to Shelf %u, Rack %u", foundBook.shelf, foundBook.rack);
    queueFirestoreLog("BOOK_PROCESSED", activeStudentName, activeStudentUID, foundBook.title, foundBook.uid, foundBook.shelf, foundBook.rack, note);

    // Run custom animated display sequence (title scroll, reveal, 8s hold, 3s warning)
    displayBookLocationSequence(foundBook.title, foundBook.shelf, foundBook.rack);

    Serial.println(F("[INFO]     Book sequence completed. Resetting for next patron..."));
    resetSystem();
    return;
  }

  // 2. Student card scanned again instead of book tag
  int studentIdx = findStudentIndex(uid);
  if (studentIdx != -1) {
    Serial.println(F("[WARN]     Student ID scanned again instead of a book! Prompting for book tag..."));
    showError("Please Tap Book ", "Tag (Not ID)    ", "Student ID card tapped during book scan step");

    // Renew session timer (+20s)
    sessionStartTime = millis();
    Serial.println(F("[INFO]     Session timer renewed (+20s). Ready for book tag retry..."));
    showMessage("Tap Your Book...");
    return;
  }

  // 3. Unrecognized tag scanned during book scan -> Emergency alarm & SECURITY_ALERT
  Serial.print(F("[ERROR]    Unrecognized tag UID: "));
  Serial.print(uid);
  Serial.println(F(" during book lookup step!"));
  char reason[96];
  snprintf(reason, sizeof(reason), "Unregistered tag scanned at Step 2 (UID: %s)", uid);

  // Silent Background Cloud Sync: Dispatch SECURITY_ALERT document to Firestore
  queueFirestoreLog("SECURITY_ALERT", activeStudentName, activeStudentUID, "UNKNOWN", uid, 0, 0, reason);

  // Emergency Alarm: 10 rapid pulses of Red LED + Buzzer across 2 seconds
  showEmergencyAlarm("Unrecognized ID ", "Unknown Book Tag", reason);

  // Renew session timer (+20s) and restore book prompt
  sessionStartTime = millis();
  Serial.println(F("[INFO]     Session timer renewed (+20s). Ready for book tag retry..."));
  showMessage("Tap Your Book...");
}

// =========================================================================================
// SECTION 10: HARDWARE DIAGNOSTICS AT BOOT
// =========================================================================================

void runHardwareDiagnostics() {
  Serial.println();
  Serial.println(F("==================================================="));
  Serial.println(F("         SHELFSENSE ESP32 SYSTEM STARTUP           "));
  Serial.println(F("==================================================="));
  Serial.print(F("[BOOT]     CPU Frequency: "));
  Serial.print(getCpuFrequencyMhz());
  Serial.println(F(" MHz"));
  Serial.print(F("[BOOT]     Free Heap RAM: "));
  Serial.print(ESP.getFreeHeap());
  Serial.println(F(" bytes"));

  // 1. Initialize I2C Bus & Scan for LCD Display
  Serial.println(F("[BOOT]     Initializing I2C bus and scanning for LCD..."));
  Wire.begin(21, 22); // Explicit SDA=21, SCL=22
  delay(50);

  uint8_t targetAddr = LCD_I2C_ADDR;
  Wire.beginTransmission(targetAddr);
  byte i2cError = Wire.endTransmission();

  // If configured address does not reply, dynamically scan bus
  if (i2cError != 0) {
    Serial.print(F("[BOOT]     LCD not found at 0x"));
    Serial.print(targetAddr, HEX);
    Serial.println(F(", scanning I2C bus..."));
    for (uint8_t addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        Serial.print(F("[BOOT]     Found I2C device at address 0x"));
        Serial.println(addr, HEX);
        targetAddr = addr;
        i2cError = 0;
        break;
      }
    }
  }

  if (i2cError == 0) {
    Serial.print(F("[BOOT]     I2C LCD detected at address 0x"));
    Serial.println(targetAddr, HEX);
    lcd = LiquidCrystal_I2C(targetAddr, LCD_COLUMNS, LCD_ROWS);
    lcd.init();
    lcd.backlight();
    lcd.clear();

    // Welcome splash screens (ABPS Nandyal Branding)
    showMessage("  ABPS Nandyal  ", "    Welcome!    ");
    delay(2000);
    showMessage("   ShelfSense   ", " Smart Library  ");
    delay(2000);
  } else {
    logError(F("I2C LCD NOT responding! Check 5V power and SDA(21)/SCL(22) wiring."));
  }

  // 2. Initialize SPI Bus & RC522 RFID Reader
  Serial.println(F("[BOOT]     Initializing SPI and RC522 Reader..."));
  SPI.begin();
  mfrc522.PCD_Init();

  byte readerVersion = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print(F("[BOOT]     RC522 Firmware Version Register: 0x"));
  Serial.println(readerVersion, HEX);

  if (readerVersion == 0x92) {
    logSuccess(F("RC522 Reader verified (Version 2.0 detected)"));
  } else if (readerVersion == 0x91) {
    logSuccess(F("RC522 Reader verified (Version 1.0 detected)"));
  } else if (readerVersion == 0x88) {
    logSuccess(F("RC522 Reader verified (FM17522 clone detected - OK)"));
  } else if (readerVersion == 0x00 || readerVersion == 0xFF) {
    logError(F("RC522 RFID READER COMMUNICATION FAILED!"));
    Serial.println(F("           [Troubleshooting RC522]:"));
    Serial.println(F("           - CRITICAL: VCC must be connected to 3.3V ONLY (5V can burn RC522)"));
    Serial.println(F("           - Ensure common GND with ESP32"));
    Serial.println(F("           - Verify SPI Pins: SCK=18, MISO=19, MOSI=23, SS=5, RST=4"));
  } else {
    Serial.print(F("[WARN]     Unrecognized RC522 version 0x"));
    Serial.print(readerVersion, HEX);
    Serial.println(F(" (Reader may still operate)"));
  }

  // 3. Test Output Indicators (Active Buzzer, Red LED, Green LED)
  Serial.println(F("[BOOT]     Testing Buzzer (GPIO 27), Red LED (GPIO 26), Green LED (GPIO 25)..."));
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);

  // Quick sequential peripheral test
  digitalWrite(GREEN_LED_PIN, HIGH);
  delay(120);
  digitalWrite(GREEN_LED_PIN, LOW);
  delay(80);

  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(120);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  logSuccess(F("Buzzer and Dual-LED hardware diagnostic test passed"));

  // 4. Print In-Memory Database Summary
  Serial.println(F("[BOOT]     Database Configuration:"));
  Serial.print(F("           - Registered Students: "));
  Serial.println(NUM_STUDENTS);
  for (size_t i = 0; i < NUM_STUDENTS; i++) {
    Serial.print(F("             ["));
    Serial.print(i + 1);
    Serial.print(F("] \""));
    Serial.print(registeredStudents[i].name);
    Serial.print(F("\" (UID: "));
    Serial.print(registeredStudents[i].uid);
    Serial.println(F(")"));
  }

  Serial.print(F("           - Registered Books:    "));
  Serial.println(NUM_BOOKS);
  for (size_t i = 0; i < NUM_BOOKS; i++) {
    Serial.print(F("             ["));
    Serial.print(i + 1);
    Serial.print(F("] \""));
    Serial.print(registeredBooks[i].title);
    Serial.print(F("\" (UID: "));
    Serial.print(registeredBooks[i].uid);
    Serial.print(F(" -> Shelf "));
    Serial.print(registeredBooks[i].shelf);
    Serial.print(F(", Rack "));
    Serial.print(registeredBooks[i].rack);
    Serial.println(F(")"));
  }

  Serial.println(F("==================================================="));
  Serial.println(F("[BOOT]     Hardware Diagnostics Complete."));
  Serial.println(F("==================================================="));
  Serial.println();
}

// =========================================================================================
// SECTION 11: MAIN SETUP & LOOP
// =========================================================================================

void setup() {
  // Start Serial Monitor at 115200 baud
  Serial.begin(SERIAL_BAUD);
  delay(500); // Allow USB serial link to stabilize

  // Perform full hardware diagnostics & welcome display sequence
  runHardwareDiagnostics();

  // Create FreeRTOS Queue for non-blocking Firestore document synchronization (up to 10 events)
  firestoreQueue = xQueueCreate(10, sizeof(FirestoreLogEvent));
  if (firestoreQueue == NULL) {
    Serial.println(F("[FIREBASE] ERROR: Failed to create FreeRTOS Firestore queue!"));
  }

  // Register Wi-Fi Event Handler for diagnostic monitoring
  WiFi.onEvent(onWiFiEvent);

  // Initialize Wi-Fi connection in Station mode (non-blocking)
  Serial.print(F("[WIFI]     Connecting to network: "));
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Launch background Firestore sync task on Core 0 with 10KB stack
  xTaskCreatePinnedToCore(
    firestoreSyncTask,         // Task function
    "FirestoreSyncTask",       // Task name
    10240,                     // Stack size in bytes (plenty for TLS BearSSL handshake)
    NULL,                      // Parameter
    1,                         // Priority
    &firestoreTaskHandle,      // Task handle
    0                          // Pin to Core 0 (leaving Core 1 for Arduino loop)
  );

  // Reset system to ready state (Line 1: "Please Tap Your ", Line 2: "Student ID Card ")
  resetSystem();
}

void loop() {
  // Non-blocking inactivity detection in WAITING_FOR_BOOK (20-second timeout)
  if (currentState == WAITING_FOR_BOOK && (millis() - sessionStartTime >= SESSION_TIMEOUT_MS)) {
    Serial.print(F("[WARN]     Session timed out: No book scan received within "));
    Serial.print(SESSION_TIMEOUT_MS / 1000);
    Serial.println(F(" seconds."));

    // Silent Background Cloud Sync: Dispatch SESSION_TIMEOUT document to Firestore
    queueFirestoreLog("SESSION_TIMEOUT", activeStudentName, activeStudentUID, "", "", 0, 0, "Session expired after 20 seconds of inactivity");

    // Auditory and visual timeout notification on LCD and status peripherals
    showMessage("Session Expired ", "Please Try Again");

    // 2 warning beeps on the buzzer and Red LED flashes
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
    delay(1000); // Brief pause so student can read notification

    // Revert to WAITING_FOR_STUDENT, clear active session, and restore standby display
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
    delay(50);
    return;
  }

  // Retrieve formatted UID into stack buffer (prevents dynamic heap fragmentation)
  char scannedUID[32];
  readCardUID(scannedUID, sizeof(scannedUID));

  // Route scanned card to the appropriate state handler
  if (currentState == WAITING_FOR_STUDENT) {
    handleStudentScan(scannedUID);
  } else if (currentState == WAITING_FOR_BOOK) {
    handleBookScan(scannedUID);
  }
}
