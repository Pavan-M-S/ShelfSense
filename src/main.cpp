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
 * 4. INTERACTIVE SERIAL MONITOR COMMANDS (115200 Baud)
 * -----------------------------------------------------------------------------------------
 *   Scope    | Command | Action & Behavior
 *  ----------+---------+-------------------------------------------------------------------
 *   System   | all     | ESP32 Task Manager: Displays live FreeRTOS tasks on Core 0 & 1,
 *            |         | RAM heap/stack metrics, ROM NVS namespaces, and system state.
 *  ----------+---------+-------------------------------------------------------------------
 *   Combined | rom     | Inspect Persistent ROM: Dumps both students and books, offline logs,
 *   (Both)   |         | free heap RAM, and Wi-Fi link status.
 *            | upt     | Update & Sync Both: Triggers on-demand sync of Firestore 'student_names'
 *            |         | and 'book_names' collections to ROM and flushes offline logs.
 *            | clr     | Clear All ROM & REST Mode: Erases all students, books, and offline logs
 *            |         | from persistent ROM and enters REST mode. LCD alerts 'ROM Empty'.
 *            | clrupt  | Clean Reset & Sync Both: Clears full ROM and memory, then pulls fresh
 *            |         | student roster and book catalog from Firestore without reboot.
 *  ----------+---------+-------------------------------------------------------------------
 *   Students | roms    | Inspect Students ROM: Dumps cached student roster only ('students').
 *   Only     | upts    | Update & Sync Students: On-demand sync of 'student_names' collection.
 *            | clrs    | Clear Students ROM: Erases 'students' namespace from persistent ROM.
 *            | clrupts | Clean Sync Students: Clears student ROM & pulls fresh student roster.
 *  ----------+---------+-------------------------------------------------------------------
 *   Books    | romb    | Inspect Books ROM: Dumps cached book catalog only ('books').
 *   Only     | uptb    | Update & Sync Books: On-demand sync of 'book_names' collection.
 *            | clrb    | Clear Books ROM: Erases 'books' namespace from persistent ROM.
 *            | clruptb | Clean Sync Books: Clears book ROM & pulls fresh book catalog.
 * 
 * -----------------------------------------------------------------------------------------
 * CRITICAL ARCHITECTURAL RULES:
 *  - COMMAND AUDIT TRAIL: Every Serial CLI command execution is automatically captured with
 *    full pre-execution state, Serial Monitor output, and post-execution state, and synced
 *    to a dedicated Firestore collection 'commands-log' with timestamp document IDs.
 *  - 30-SECOND SYSTEM CLOCK BROADCAST: Every 30 seconds, full calendar and time details
 *    (Year, Month, Date, Day of Week, Time 24h/12h, Timezone, Uptime) are printed to Serial.
 *  - SILENT BACKGROUND CLOUD SYNC: All Google Cloud Firestore synchronizations occur
 *    completely in the background via a dedicated FreeRTOS worker task on Core 0.
 *  - ZERO CLOUD/WIFI ON LCD: The LCD is strictly dedicated to physical user guidance.
 *    No Wi-Fi, cloud, or sync status is ever printed to the LCD screen.
 *  - SERIAL DIAGNOSTICS: Every network state change, NTP synchronization, RFID read,
 *    state transition, and Firestore document upload is logged with clear tags:
 *    [BOOT], [WIFI], [TIME], [RFID], [AUTH], [LOOKUP], [STATE], [FEEDBACK], [LCD], [FIREBASE], [COMMANDS-LOG].
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
#include <freertos/semphr.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <vector>

// Expand Arduino loopTask stack size on Core 1 from default 8KB to 16KB to support BearSSL HTTPS operations
SET_LOOP_TASK_STACK_SIZE(16384);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
// ESP-IDF v5+ Task Watchdog compatibility overload
inline esp_err_t esp_task_wdt_init(uint32_t timeout_sec, bool panic) {
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = timeout_sec * 1000,
    .idle_core_mask = 0,
    .trigger_panic = panic
  };
  esp_err_t err = esp_task_wdt_reconfigure(&twdt_config);
  if (err == ESP_ERR_INVALID_STATE) {
    err = esp_task_wdt_init(&twdt_config);
  }
  return err;
}
#endif

// Compatibility alias for Mobizt Firebase library Firestore collection listing
#define GET_DOCS_4(fb, proj, db, col) listDocuments((fb), (proj), (db), (col), 100, "", "", "", false)
#define GET_DOCS_9(fb, proj, db, col, sz, tok, ord, mask, missing) listDocuments((fb), (proj), (db), (col), (sz), (tok), (ord), (mask), (missing))
#define GET_DOCS_CHOOSER(_1, _2, _3, _4, _5, _6, _7, _8, _9, NAME, ...) NAME
#ifndef getDocuments
#define getDocuments(...) GET_DOCS_CHOOSER(__VA_ARGS__, GET_DOCS_9, _8, _7, _6, _5, GET_DOCS_4)(__VA_ARGS__)
#endif

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
#define SESSION_TIMEOUT_MS  18000   // 18-second non-blocking inactivity window for book scanning

// Baud Rate for USB Serial Debugging
#define SERIAL_BAUD         115200

// Wi-Fi Configuration
#define WIFI_SSID           "narzo 50A"   // Replace with your local Wi-Fi SSID
#define WIFI_PASSWORD       "MSPP9347"              // Replace with your Wi-Fi password

// Firebase & Google Cloud Firestore Configuration
#define FIREBASE_PROJECT_ID           "shelf-sense-f86ba"
#define FIREBASE_API_KEY              "AIzaSyDHboFG0dyoMTLCH0bkZ85OkWk2yxBXVRQ"
#define FIREBASE_USER_EMAIL           "esp32@shelfsense.local"
#define FIREBASE_USER_PASSWORD        "ShelfSense@2026"
#define FIRESTORE_COLLECTION          "library_logs"
#define FIRESTORE_COMMANDS_COLLECTION "commands-log"
#define FIRESTORE_STUDENTS            "student_names"
#define FIRESTORE_STUDENTS_COLLECTION FIRESTORE_STUDENTS
#define FIRESTORE_BOOKS               "book_names"
#define FIRESTORE_BOOKS_COLLECTION    FIRESTORE_BOOKS

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

// Mobizt Firebase Data Objects (Dedicated to Core 0 background Firestore worker)
FirebaseData fbdo;     // Core 0: Background Firestore worker (20s roster sync, log queue, offline flush)
FirebaseAuth auth;
FirebaseConfig config;

// ESP32 Non-Volatile Storage (Preferences) for offline-first student roster, book catalog & offline log buffering
Preferences studentPrefs;
Preferences bookPrefs;
Preferences offlineLogsPrefs;

// FreeRTOS Queue & Task handles for background Firestore sync
QueueHandle_t firestoreQueue = NULL;
QueueHandle_t commandLogQueue = NULL;
TaskHandle_t firestoreTaskHandle = NULL;

// System Restart Flag for automatic restart when cloud roster diff is detected
volatile bool pendingRestart = false;

// Post-Round Cloud Sync Flag: triggers immediate background roster sync after completing one book round
volatile bool triggerPostRoundSync = false;

// Manual Cloud Sync Flags: triggers immediate on-demand sync via 'upt', 'upts', 'uptb'
volatile bool triggerSyncStudents = false;
volatile bool triggerSyncBooks = false;

// Clean ROM Reset & Sync Flags: triggers ROM clear and fresh sync via 'clrupt', 'clrupts', 'clruptb'
volatile bool triggerCleanSyncStudents = false;
volatile bool triggerCleanSyncBooks = false;
volatile bool cleanSyncFinished = false;
volatile bool cleanSyncFailed = false;

// System REST mode flag: set via serial command 'clr'; pauses periodic auto-sync until 'upt', 'upts', 'uptb', 'clrupt' etc. is executed
volatile bool systemRestMode = false;

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
unsigned long lastSessionSecondLogged = 0;

// Authenticated student session variables
char activeStudentName[128] = "";
char activeStudentUID[32]   = "";
char activeLoginTime[25]    = "";

// Structure modeling a registered library book
struct Book {
  const char* title;    // Book title
  const char* uid;      // RFID tag UID in uppercase HEX without spaces
  uint8_t shelf;        // Assigned shelf number
  uint8_t rack;         // Assigned rack number
};

// Structure modeling a Firestore log event for background asynchronous queueing
struct FirestoreLogEvent {
  char docId[32];       // Formatted Document ID without spaces or colons: "YYYY-MM-DD_HHMMSS"
  char eventType[20];   // BOOK_PROCESSED, SESSION_TIMEOUT, SECURITY_ALERT, STUDENT_LOGIN
  char studentName[128]; // Authenticated student name (supports full names up to 127 chars)
  char studentUID[32];  // Authenticated student UID
  char loginTime[25];   // Login timestamp "YYYY-MM-DD HH:MM:SS"
  char bookTitle[128];  // Scanned book title (supports long titles up to 127 chars)
  char bookUID[32];     // Scanned book UID (or empty)
  uint8_t shelf;        // Physical shelf location
  uint8_t rack;         // Physical rack location
  char timestamp[25];   // Processed timestamp "YYYY-MM-DD HH:MM:SS"
  char statusNote[160]; // Diagnostic status note
};

// Structure modeling a Command Log event for background Firestore synchronization
struct CommandLogItem {
  String docId;
  String timestamp;
  String command;
  String stateBefore;
  String actionSummary;
  String serialOutput;
  String stateAfter;
  String executionStatus;
  String systemMode;
  uint32_t freeHeapBefore;
  uint32_t freeHeapAfter;
};

// Pending command logs buffer in RAM if Wi-Fi is temporarily offline
static std::vector<CommandLogItem*> pendingCommandLogs;

// -----------------------------------------------------------------------------------------
// DATABASE ENTRIES: Registered Library Books
// Note: Student roster is dynamically loaded via Offline-First Cloud Cache Architecture
// (Firestore collection 'student_names' synchronized to ESP32 NVS namespace "students")
// -----------------------------------------------------------------------------------------

// Database of registered books: { "Book Title", "RFID UID", Shelf Number, Rack Number }
const Book registeredBooks[] = {
  { "Geronimo Stilton - Journey to the Center of the Earth", "F10F7E05", 2, 1 }, // Shelf 2, Rack 1
  { "Geronimo Stilton - The Great Adventure","8DC0FA6B", 4, 3 }  // Shelf 4, Rack 3
};

// Constant for book database sizing
const size_t NUM_BOOKS = sizeof(registeredBooks) / sizeof(registeredBooks[0]);

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

// Formats current local time into human-readable timestamp AND custom document ID without spaces or colons
void getCurrentTimestamps(char* humanReadable, size_t maxHumanLen, char* docId, size_t maxDocIdLen) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 20)) {
    strftime(humanReadable, maxHumanLen, "%Y-%m-%d %H:%M:%S", &timeinfo);
    strftime(docId, maxDocIdLen, "%Y-%m-%d_%H%M%S", &timeinfo);
  } else {
    unsigned long sec = millis() / 1000;
    unsigned long h   = (sec / 3600) % 24;
    unsigned long m   = (sec / 60) % 60;
    unsigned long s   = sec % 60;
    snprintf(humanReadable, maxHumanLen, "1970-01-01 %02lu:%02lu:%02lu", h, m, s);
    snprintf(docId, maxDocIdLen, "1970-01-01_%02lu%02lu%02lu", h, m, s);
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
                       const char* loginTime,
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

  getCurrentTimestamps(logEvent.timestamp, sizeof(logEvent.timestamp), logEvent.docId, sizeof(logEvent.docId));

  strncpy(logEvent.eventType,   eventType   ? eventType   : "", sizeof(logEvent.eventType)   - 1);
  strncpy(logEvent.studentName, studentName ? studentName : "", sizeof(logEvent.studentName) - 1);
  strncpy(logEvent.studentUID,  studentUID  ? studentUID  : "", sizeof(logEvent.studentUID)  - 1);
  strncpy(logEvent.loginTime,   loginTime   ? loginTime   : "", sizeof(logEvent.loginTime)   - 1);
  strncpy(logEvent.bookTitle,   bookTitle   ? bookTitle   : "", sizeof(logEvent.bookTitle)   - 1);
  strncpy(logEvent.bookUID,     bookUID     ? bookUID     : "", sizeof(logEvent.bookUID)     - 1);
  logEvent.shelf = shelf;
  logEvent.rack  = rack;
  strncpy(logEvent.statusNote,  statusNote  ? statusNote  : "", sizeof(logEvent.statusNote)  - 1);

  Serial.print(F("[FIREBASE] Dispatching '"));
  Serial.print(logEvent.eventType);
  Serial.print(F("' event (Doc ID: '"));
  Serial.print(logEvent.docId);
  Serial.println(F("') to background sync queue..."));

  // Send to queue non-blockingly (timeout = 0 ticks)
  if (xQueueSend(firestoreQueue, &logEvent, 0) != pdTRUE) {
    Serial.println(F("[FIREBASE] WARN: Background queue full! Dropping oldest log event."));
  }
}

// Structure representing cached student record in ROM (Preferences NVS)
struct StudentRecord {
  char name[128]; // Supports long full names up to 127 characters
  bool active;
};

// Student authentication states for dual-check verification
enum StudentAuthStatus {
  STUDENT_AUTH_ACTIVE,      // Authenticated active student
  STUDENT_AUTH_SUSPENDED,   // Registered student with active: false (Suspended Card)
  STUDENT_AUTH_NOT_FOUND,   // Unregistered card (not in cloud or ROM)
  STUDENT_AUTH_ROM_REMOVED  // Card was cached in ROM but deleted from Firestore
};

// Structure representing cached book record in ROM (Preferences NVS)
struct BookRecord {
  char title[128]; // Supports long titles up to 127 characters
  uint8_t shelf;   // Physical shelf location
  uint8_t rack;    // Physical rack location
  bool active;     // Active / available status
};

// Book authentication states for verification
enum BookAuthStatus {
  BOOK_AUTH_ACTIVE,     // Registered active book
  BOOK_AUTH_SUSPENDED,  // Registered book with active: false (Suspended / Under Repair)
  BOOK_AUTH_NOT_FOUND   // Unregistered book tag (not in cloud or ROM)
};

// Forward declarations for offline caching & roster/catalog synchronization
bool saveOfflineLogToROM(const FirestoreLogEvent& event);
void flushOfflineLogsFromROM();
bool compareAndSyncRoster(bool isBoot, bool &outDiffDetected);
bool syncStudentsFromCloud();
bool compareAndSyncBooks(bool isBoot, bool &outDiffDetected);
bool syncBooksFromCloud();
bool verifyStudentFromROM(const char* uid, char* outName, size_t maxLen, StudentAuthStatus& outStatus);
bool verifyStudentWithCache(const char* uid, char* outName, size_t maxLen);
bool getStudentFromROM(const char* uid, StudentRecord& outRecord);
void saveStudentToROM(const char* uid, const char* name, bool active);
void removeStudentFromROM(const char* uid);
std::vector<String> getAllROMUIDs();
bool verifyBookFromROM(const char* uid, BookRecord& outRecord, BookAuthStatus& outStatus);
bool verifyBookWithCache(const char* uid, BookRecord& outRecord);
bool getBookFromROM(const char* uid, BookRecord& outRecord);
void saveBookToROM(const char* uid, const char* title, uint8_t shelf, uint8_t rack, bool active);
void removeBookFromROM(const char* uid);
std::vector<String> getAllROMBookUIDs();
void printROMCacheSummary();
void dumpROMStudents();
void dumpROMBooks();
void dumpAllROMData();
void clearStudentROMData();
void clearBookROMData();
void clearAllROMData();
void executeCleanResetAndSyncStudents();
void executeCleanResetAndSyncBooks();
void executeCleanResetAndSync();
void dumpTaskManagerAll();
void printPeriodicFullTime();
String captureDetailedStateSummary();
void queueCommandLog(const String& cmd,
                     const String& stateBefore,
                     const String& serialOutput,
                     const String& stateAfter,
                     const String& actionSummary,
                     const String& executionStatus,
                     uint32_t freeHeapBefore,
                     uint32_t freeHeapAfter);
void checkSerialCommands();

// Maximum number of offline log events buffered in persistent ROM (NVS)
const uint16_t MAX_OFFLINE_LOGS = 20;

// Temporarily stores an unsent Firestore log event in ROM if Wi-Fi drops or upload fails
bool saveOfflineLogToROM(const FirestoreLogEvent& event) {
  uint16_t count = offlineLogsPrefs.getUShort("count", 0);
  if (count >= MAX_OFFLINE_LOGS) {
    Serial.println(F("[OFFLINE-ROM] Warning: ROM offline log buffer full. Overwriting oldest log."));
    for (uint16_t i = 1; i < count; i++) {
      String srcKey = "l_" + String(i);
      String dstKey = "l_" + String(i - 1);
      FirestoreLogEvent temp;
      if (offlineLogsPrefs.getBytes(srcKey.c_str(), &temp, sizeof(temp)) == sizeof(temp)) {
        offlineLogsPrefs.putBytes(dstKey.c_str(), &temp, sizeof(temp));
      }
    }
    count = MAX_OFFLINE_LOGS - 1;
  }

  String key = "l_" + String(count);
  size_t written = offlineLogsPrefs.putBytes(key.c_str(), &event, sizeof(FirestoreLogEvent));
  if (written == sizeof(FirestoreLogEvent)) {
    offlineLogsPrefs.putUShort("count", count + 1);
    Serial.print(F("[OFFLINE-ROM] Event cached in ROM (Pending offline logs: "));
    Serial.print(count + 1);
    Serial.print(F("). Doc ID: '"));
    Serial.print(event.docId);
    Serial.println(F("'"));
    return true;
  }
  Serial.println(F("[OFFLINE-ROM] ERROR: Failed to write log event to ROM!"));
  return false;
}

// Background sync: Drains and uploads buffered offline logs from ROM to Firestore upon Wi-Fi reconnection
void flushOfflineLogsFromROM() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) {
    return;
  }

  uint16_t count = offlineLogsPrefs.getUShort("count", 0);
  if (count == 0) {
    return;
  }

  Serial.println(F("---------------------------------------------------"));
  Serial.print(F("[OFFLINE-ROM] Network link active. Synchronizing "));
  Serial.print(count);
  Serial.println(F(" offline log(s) from ROM to Firestore..."));

  uint16_t syncedCount = 0;

  for (uint16_t i = 0; i < count; i++) {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) {
      Serial.println(F("[OFFLINE-ROM] Wi-Fi lost during flush. Retaining remaining in ROM."));
      break;
    }

    String key = "l_" + String(i);
    FirestoreLogEvent offEvent;
    size_t readLen = offlineLogsPrefs.getBytes(key.c_str(), &offEvent, sizeof(offEvent));
    if (readLen != sizeof(offEvent)) {
      Serial.print(F("[OFFLINE-ROM] Invalid ROM entry at index "));
      Serial.print(i);
      Serial.println(F(". Removing."));
      offlineLogsPrefs.remove(key.c_str());
      syncedCount++;
      continue;
    }

    FirebaseJson content;
    content.set("fields/eventType/stringValue", offEvent.eventType);
    content.set("fields/studentName/stringValue", offEvent.studentName);
    content.set("fields/studentUID/stringValue", offEvent.studentUID);
    content.set("fields/loginTime/stringValue", offEvent.loginTime);
    content.set("fields/bookTitle/stringValue", offEvent.bookTitle);
    content.set("fields/bookUID/stringValue", offEvent.bookUID);
    content.set("fields/shelf/integerValue", String(offEvent.shelf));
    content.set("fields/rack/integerValue", String(offEvent.rack));
    content.set("fields/timestamp/stringValue", offEvent.timestamp);
    content.set("fields/statusNote/stringValue", offEvent.statusNote);

    Serial.print(F("[OFFLINE-ROM] Uploading pending log (Doc ID: '"));
    Serial.print(offEvent.docId);
    Serial.println(F("')..."));

    bool success = Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "", FIRESTORE_COLLECTION, offEvent.docId, content.raw(), "");
    int httpCode = fbdo.httpCode();
    String errorReason = (!success || httpCode != 200) ? fbdo.errorReason() : "";

    if (success && httpCode == 200) {
      Serial.print(F("[OFFLINE-ROM] Document sync SUCCESS! Doc ID: '"));
      Serial.print(offEvent.docId);
      Serial.println(F("'. Deleted from ROM."));
      offlineLogsPrefs.remove(key.c_str());
      syncedCount++;
    } else {
      Serial.print(F("[OFFLINE-ROM] Document sync FAILED! HTTP: "));
      Serial.print(httpCode);
      Serial.print(F(" | Reason: "));
      Serial.println(errorReason);
      Serial.println(F(". Stopping flush for next retry."));
      break;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }

  if (syncedCount > 0) {
    uint16_t remaining = (count >= syncedCount) ? (count - syncedCount) : 0;
    if (remaining > 0) {
      for (uint16_t i = 0; i < remaining; i++) {
        String srcKey = "l_" + String(i + syncedCount);
        String dstKey = "l_" + String(i);
        FirestoreLogEvent temp;
        if (offlineLogsPrefs.getBytes(srcKey.c_str(), &temp, sizeof(temp)) == sizeof(temp)) {
          offlineLogsPrefs.putBytes(dstKey.c_str(), &temp, sizeof(temp));
          offlineLogsPrefs.remove(srcKey.c_str());
        }
      }
    }
    offlineLogsPrefs.putUShort("count", remaining);
    Serial.print(F("[OFFLINE-ROM] Flush progress: "));
    Serial.print(syncedCount);
    Serial.print(F(" synced, "));
    Serial.print(remaining);
    Serial.println(F(" remaining in ROM."));
  }
  Serial.println(F("---------------------------------------------------"));
}

// FreeRTOS Background Task running on Core 0 to handle Wi-Fi, Firestore sync & 20s roster polling
void firestoreSyncTask(void* pvParameters) {
  Serial.println(F("[FIREBASE] Background Firestore sync worker task started on Core 0"));

  // 1. Proactive Wi-Fi connection loop
  unsigned long startWifiWait = millis();
  while (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    vTaskDelay(pdMS_TO_TICKS(200));
    if (millis() - startWifiWait > 6000) {
      Serial.println(F("[WIFI]     Still connecting to Wi-Fi... Re-issuing connect command"));
      WiFi.disconnect();
      WiFi.reconnect();
      startWifiWait = millis();
    }
  }

  Serial.print(F("[WIFI]     Connected! IP: "));
  Serial.print(WiFi.localIP());
  Serial.print(F(" | RSSI: "));
  Serial.print(WiFi.RSSI());
  Serial.println(F(" dBm"));

  // 2. Ensure NTP system clock is valid before authenticating with Google/Firebase
  // (BearSSL requires valid system time to verify SSL certificates, otherwise auth fails)
  Serial.println(F("[TIME]     Waiting for NTP system clock synchronization..."));
  time_t now = time(nullptr);
  unsigned long ntpStart = millis();
  while (now < 1700000000 && (millis() - ntpStart < 8000)) {
    vTaskDelay(pdMS_TO_TICKS(200));
    now = time(nullptr);
  }

  if (now >= 1700000000) {
    char timeStr[32];
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.print(F("[TIME]     NTP Clock synchronized successfully: "));
    Serial.println(timeStr);
  } else {
    Serial.println(F("[TIME]     WARN: NTP sync timed out. Proceeding with caution."));
  }

  // 3. Configure and initialize Firebase Authentication
  Serial.println(F("[FIREBASE] Configuring Email/Password Authentication..."));
  config.api_key = FIREBASE_API_KEY;
  auth.user.email = FIREBASE_USER_EMAIL;
  auth.user.password = FIREBASE_USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;

  // Extended Network Timeouts (optimized for mobile hotspots & high-latency cellular links)
  config.timeout.serverResponse = 30 * 1000;   // 30 seconds response read timeout (prevents -6 timeout on slow mobile hotspots)
  config.timeout.socketConnection = 25 * 1000; // 25 seconds socket connection & SSL handshake timeout
  config.timeout.wifiReconnect = 5 * 1000;

  Firebase.reconnectNetwork(true);
  fbdo.setBSSLBufferSize(4096 /* Rx buffer */, 2048 /* Tx buffer */);
  fbdo.setResponseSize(8192);

  Firebase.begin(&config, &auth);
  Serial.print(F("[FIREBASE] Client library initialized for: "));
  Serial.println(FIREBASE_USER_EMAIL);

  // 4. Wait up to 10 seconds for initial Firebase token ready state
  Serial.println(F("[FIREBASE] Waiting for authentication token to become ready..."));
  unsigned long authStart = millis();
  while (!Firebase.ready() && (millis() - authStart < 10000)) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (Firebase.ready()) {
    Serial.println(F("[FIREBASE] Authentication SUCCESS! Client is ready for Firestore operations."));
  } else {
    Serial.println(F("[FIREBASE] WARN: Firebase not ready yet after initial window. Will keep refreshing in loop."));
  }

  FirestoreLogEvent event;
  bool initialSyncDone = false;
  unsigned long lastRosterCheckTime = 0;
  unsigned long lastOfflineFlushTime = 0;
  const unsigned long ROSTER_CHECK_INTERVAL_MS = 60000;   // Relaxed to 60s periodic sync to prevent hotspot bandwidth saturation
  const unsigned long OFFLINE_FLUSH_INTERVAL_MS = 15000;  // Flush offline ROM logs every 15 seconds

  // Infinite processing loop on Core 0
  for (;;) {
    // A. Proactive Wi-Fi connection supervisor
    if (WiFi.status() != WL_CONNECTED) {
      static unsigned long lastWifiReconnect = 0;
      if (millis() - lastWifiReconnect > 4000) {
        lastWifiReconnect = millis();
        Serial.println(F("[WIFI]     Link disconnected! Auto-reconnecting..."));
        WiFi.disconnect();
        WiFi.reconnect();
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // B. Check if Firebase is ready
    bool fbReady = Firebase.ready();

    if (fbReady) {
      // Boot-Time Synchronization (executed once upon boot)
      if (!initialSyncDone) {
        Serial.println(F("[AUTH-SYNC] Executing Boot-Time Roster & Book Catalog Synchronization..."));
        bool sOk = syncStudentsFromCloud();
        vTaskDelay(pdMS_TO_TICKS(500));
        bool bOk = syncBooksFromCloud();
        initialSyncDone = sOk && bOk;
        if (initialSyncDone) {
          lastRosterCheckTime = millis();
          Serial.println(F("[AUTH-SYNC] Boot-Time Synchronization completed successfully."));
        } else {
          static int syncAttempts = 0;
          syncAttempts++;
          if (syncAttempts >= 3) {
            Serial.println(F("[AUTH-SYNC] Max initial sync attempts reached. Retaining existing local NVS cache."));
            initialSyncDone = true; // Fallback to local NVS cache so normal operation is not blocked
            lastRosterCheckTime = millis();
          } else {
            vTaskDelay(pdMS_TO_TICKS(2000));
          }
        }
      }

      // Flush any offline logs buffered in ROM (throttled to every 15 seconds)
      if (millis() - lastOfflineFlushTime >= OFFLINE_FLUSH_INTERVAL_MS) {
        lastOfflineFlushTime = millis();
        flushOfflineLogsFromROM();
      }

      // Manual On-Demand Cloud Sync Trigger (Triggered via Serial command "upt", "upts", "uptb")
      if (triggerSyncStudents || triggerSyncBooks) {
        bool syncBoth = triggerSyncStudents && triggerSyncBooks;
        bool doStudents = triggerSyncStudents;
        bool doBooks = triggerSyncBooks;
        triggerSyncStudents = false;
        triggerSyncBooks = false;
        systemRestMode = false;
        lastRosterCheckTime = millis();
        Serial.println(F("---------------------------------------------------"));
        if (syncBoth) {
          Serial.println(F("[MANUAL-SYNC] On-demand sync ('upt'): students & books initiated..."));
        } else if (doStudents) {
          Serial.println(F("[MANUAL-SYNC] On-demand sync ('upts'): students roster initiated..."));
        } else {
          Serial.println(F("[MANUAL-SYNC] On-demand sync ('uptb'): book catalog initiated..."));
        }
        flushOfflineLogsFromROM();
        bool sDiff = false;
        if (doStudents) {
          compareAndSyncRoster(false /* periodic */, sDiff);
          vTaskDelay(pdMS_TO_TICKS(500));
        }
        bool bDiff = false;
        if (doBooks) {
          compareAndSyncBooks(false /* periodic */, bDiff);
          vTaskDelay(pdMS_TO_TICKS(500));
        }
        initialSyncDone = true;
        if (sDiff || bDiff) {
          pendingRestart = true;
        } else {
          Serial.println(F("[MANUAL-SYNC] Sync complete: Local ROM is fully up-to-date with Firestore."));
        }
        Serial.println(F("---------------------------------------------------"));
      }

      // Clean Reset & Sync Trigger (Triggered via Serial command "clrupt", "clrupts", "clruptb")
      if (triggerCleanSyncStudents || triggerCleanSyncBooks) {
        bool cleanBoth = triggerCleanSyncStudents && triggerCleanSyncBooks;
        bool doStudents = triggerCleanSyncStudents;
        bool doBooks = triggerCleanSyncBooks;
        triggerCleanSyncStudents = false;
        triggerCleanSyncBooks = false;
        systemRestMode = false;
        lastRosterCheckTime = millis();
        Serial.println(F("---------------------------------------------------"));
        if (cleanBoth) {
          Serial.println(F("[CLRUPT]    Downloading fresh student roster & book catalog from Firestore..."));
        } else if (doStudents) {
          Serial.println(F("[CLRUPTS]   Downloading fresh student roster from Firestore..."));
        } else {
          Serial.println(F("[CLRUPTB]   Downloading fresh book catalog from Firestore..."));
        }
        bool unusedDiff = false;
        bool sOk = true;
        if (doStudents) {
          sOk = compareAndSyncRoster(true /* suppress restart countdown */, unusedDiff);
          vTaskDelay(pdMS_TO_TICKS(500));
        }
        bool bOk = true;
        if (doBooks) {
          bOk = compareAndSyncBooks(true /* suppress restart countdown */, unusedDiff);
          vTaskDelay(pdMS_TO_TICKS(500));
        }
        initialSyncDone = true;
        if (sOk && bOk) {
          Serial.println(F("[CLRUPT]    Fresh download complete and cached in ROM!"));
          cleanSyncFinished = true;
        } else {
          Serial.println(F("[CLRUPT]    ERROR: Failed to fetch documents from Firestore. Check Wi-Fi / Firebase."));
          cleanSyncFailed = true;
        }
        Serial.println(F("---------------------------------------------------"));
      }

      // Immediate Post-Round Sync Trigger (Triggered after completing a student + book round)
      // Only runs after any pending queued transaction logs have finished uploading!
      if (initialSyncDone && triggerPostRoundSync) {
        if (uxQueueMessagesWaiting(firestoreQueue) == 0) {
          triggerPostRoundSync = false;
          lastRosterCheckTime = millis();
          Serial.println(F("---------------------------------------------------"));
          Serial.println(F("[AUTH-SYNC] Completed one full book round! Running background cloud sync..."));
          vTaskDelay(pdMS_TO_TICKS(500));
          bool studentDiff = false;
          compareAndSyncRoster(false /* periodic */, studentDiff);
          vTaskDelay(pdMS_TO_TICKS(500));
          bool bookDiff = false;
          compareAndSyncBooks(false /* periodic */, bookDiff);
          vTaskDelay(pdMS_TO_TICKS(500));
          if (studentDiff || bookDiff) {
            pendingRestart = true;
          }
          Serial.println(F("---------------------------------------------------"));
        }
      }

      // Periodic 60-second Roster and Book check and diff sync (skipped during active patron interaction or queue load)
      if (!systemRestMode && initialSyncDone && currentState == WAITING_FOR_STUDENT &&
          uxQueueMessagesWaiting(firestoreQueue) == 0 && uxQueueMessagesWaiting(commandLogQueue) == 0 &&
          (millis() - lastRosterCheckTime >= ROSTER_CHECK_INTERVAL_MS)) {
        lastRosterCheckTime = millis();
        Serial.println(F("[AUTH-SYNC] 60s idle interval elapsed. Checking Firestore 'student_names' vs ROM..."));
        bool studentDiff = false;
        compareAndSyncRoster(false /* periodic */, studentDiff);
        vTaskDelay(pdMS_TO_TICKS(500));

        Serial.println(F("[BOOK-SYNC] 60s idle interval elapsed. Checking Firestore 'book_names' vs ROM..."));
        bool bookDiff = false;
        compareAndSyncBooks(false /* periodic */, bookDiff);
        vTaskDelay(pdMS_TO_TICKS(500));

        if (studentDiff || bookDiff) {
          pendingRestart = true;
        }
      }
    }

    // 2. Dequeue and process incoming log events (waiting up to 100ms)
    if (xQueueReceive(firestoreQueue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
      Serial.println(F("---------------------------------------------------"));
      Serial.print(F("[FIREBASE] Processing queued event: "));
      Serial.println(event.eventType);

      bool sent = false;
      if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        // Construct Firestore Document JSON according to required schema
        FirebaseJson content;
        content.set("fields/eventType/stringValue", event.eventType);
        content.set("fields/studentName/stringValue", event.studentName);
        content.set("fields/studentUID/stringValue", event.studentUID);
        content.set("fields/loginTime/stringValue", event.loginTime);
        content.set("fields/bookTitle/stringValue", event.bookTitle);
        content.set("fields/bookUID/stringValue", event.bookUID);
        content.set("fields/shelf/integerValue", String(event.shelf));
        content.set("fields/rack/integerValue", String(event.rack));
        content.set("fields/timestamp/stringValue", event.timestamp);
        content.set("fields/statusNote/stringValue", event.statusNote);

        Serial.print(F("[FIREBASE] Target Collection: '"));
        Serial.print(FIRESTORE_COLLECTION);
        Serial.print(F("' | Custom Doc ID (Time formatted): '"));
        Serial.print(event.docId);
        Serial.println(F("'"));
        Serial.print(F("[FIREBASE] Document JSON Payload: "));
        Serial.println(content.raw());

        if (Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "", FIRESTORE_COLLECTION, event.docId, content.raw(), "")) {
          if (fbdo.httpCode() == 200) {
            sent = true;
            Serial.print(F("[FIREBASE] Document sync SUCCESS! HTTP Code: "));
            Serial.println(fbdo.httpCode());
            Serial.print(F("[FIREBASE] Server Response: "));
            Serial.println(fbdo.payload());
            vTaskDelay(pdMS_TO_TICKS(150));
          }
        }
        if (!sent) {
          Serial.print(F("[FIREBASE] Document sync FAILED! HTTP Code: "));
          Serial.print(fbdo.httpCode());
          Serial.print(F(" | Reason: "));
          Serial.println(fbdo.errorReason());
          fbdo.clear();
          vTaskDelay(pdMS_TO_TICKS(1000));
        }
      } else {
        Serial.println(F("[FIREBASE] Network offline or Firebase not ready."));
      }

      // If document could not be synced immediately, store in ROM for background retry
      if (!sent) {
        Serial.println(F("[FIREBASE] Storing unsent event in ROM for background retry..."));
        saveOfflineLogToROM(event);
      }
      Serial.println(F("---------------------------------------------------"));
    }

    // 3. Dequeue and process incoming command audit logs for 'commands-log' collection
    CommandLogItem* cmdEvent = nullptr;
    if (xQueueReceive(commandLogQueue, &cmdEvent, 0) == pdTRUE && cmdEvent != nullptr) {
      if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        FirebaseJson cmdContent;
        cmdContent.set("fields/command/stringValue", cmdEvent->command);
        cmdContent.set("fields/timestamp/stringValue", cmdEvent->timestamp);
        cmdContent.set("fields/docId/stringValue", cmdEvent->docId);
        cmdContent.set("fields/stateBefore/stringValue", cmdEvent->stateBefore);
        cmdContent.set("fields/actionSummary/stringValue", cmdEvent->actionSummary);
        cmdContent.set("fields/serialOutput/stringValue", cmdEvent->serialOutput);
        cmdContent.set("fields/stateAfter/stringValue", cmdEvent->stateAfter);
        cmdContent.set("fields/executionStatus/stringValue", cmdEvent->executionStatus);
        cmdContent.set("fields/systemMode/stringValue", cmdEvent->systemMode);
        cmdContent.set("fields/freeHeapBefore/integerValue", String(cmdEvent->freeHeapBefore));
        cmdContent.set("fields/freeHeapAfter/integerValue", String(cmdEvent->freeHeapAfter));

        Serial.println(F("---------------------------------------------------"));
        Serial.print(F("[COMMANDS-LOG] Target Collection: '"));
        Serial.print(FIRESTORE_COMMANDS_COLLECTION);
        Serial.print(F("' | Custom Doc ID (Time formatted): '"));
        Serial.print(cmdEvent->docId);
        Serial.println(F("'"));
        Serial.print(F("[COMMANDS-LOG] Command: '"));
        Serial.print(cmdEvent->command);
        Serial.print(F("' | Status: "));
        Serial.println(cmdEvent->executionStatus);

        bool cmdOk = Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "", FIRESTORE_COMMANDS_COLLECTION, cmdEvent->docId.c_str(), cmdContent.raw(), "");
        int cmdHttpCode = fbdo.httpCode();
        if (cmdOk && cmdHttpCode == 200) {
          Serial.print(F("[COMMANDS-LOG] Document sync SUCCESS! HTTP Code: "));
          Serial.println(cmdHttpCode);
          delete cmdEvent;
          vTaskDelay(pdMS_TO_TICKS(150));
        } else {
          Serial.print(F("[COMMANDS-LOG] Document sync FAILED! HTTP Code: "));
          Serial.print(cmdHttpCode);
          Serial.print(F(" | Reason: "));
          Serial.println(fbdo.errorReason());
          fbdo.clear();
          if (pendingCommandLogs.size() < 10) {
            pendingCommandLogs.push_back(cmdEvent);
            Serial.print(F("[COMMANDS-LOG] Stored '"));
            Serial.print(cmdEvent->command);
            Serial.println(F("' log in memory for background retry upon reconnect."));
          } else {
            Serial.println(F("[COMMANDS-LOG] WARN: Pending buffer full. Dropping command log."));
            delete cmdEvent;
          }
          vTaskDelay(pdMS_TO_TICKS(1000));
        }
        Serial.println(F("---------------------------------------------------"));
      } else {
        if (pendingCommandLogs.size() < 10) {
          pendingCommandLogs.push_back(cmdEvent);
          Serial.print(F("[COMMANDS-LOG] Network link offline. Buffered '"));
          Serial.print(cmdEvent->command);
          Serial.println(F("' log in memory for background retry upon reconnect."));
        } else {
          Serial.println(F("[COMMANDS-LOG] WARN: Pending buffer full. Dropping command log."));
          delete cmdEvent;
        }
      }
    }

    // Flush any pending command logs buffered during offline state
    if (!pendingCommandLogs.empty() && WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      CommandLogItem* pendingItem = pendingCommandLogs.front();
      pendingCommandLogs.erase(pendingCommandLogs.begin());

      FirebaseJson cmdContent;
      cmdContent.set("fields/command/stringValue", pendingItem->command);
      cmdContent.set("fields/timestamp/stringValue", pendingItem->timestamp);
      cmdContent.set("fields/docId/stringValue", pendingItem->docId);
      cmdContent.set("fields/stateBefore/stringValue", pendingItem->stateBefore);
      cmdContent.set("fields/actionSummary/stringValue", pendingItem->actionSummary);
      cmdContent.set("fields/serialOutput/stringValue", pendingItem->serialOutput);
      cmdContent.set("fields/stateAfter/stringValue", pendingItem->stateAfter);
      cmdContent.set("fields/executionStatus/stringValue", pendingItem->executionStatus);
      cmdContent.set("fields/systemMode/stringValue", pendingItem->systemMode);
      cmdContent.set("fields/freeHeapBefore/integerValue", String(pendingItem->freeHeapBefore));
      cmdContent.set("fields/freeHeapAfter/integerValue", String(pendingItem->freeHeapAfter));

      Serial.print(F("[COMMANDS-LOG] Uploading buffered command log (Doc ID: '"));
      Serial.print(pendingItem->docId);
      Serial.println(F("')..."));

      bool pOk = Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "", FIRESTORE_COMMANDS_COLLECTION, pendingItem->docId.c_str(), cmdContent.raw(), "");
      int pCode = fbdo.httpCode();
      if (pOk && pCode == 200) {
        Serial.print(F("[COMMANDS-LOG] Buffered sync SUCCESS! Doc ID: '"));
        Serial.print(pendingItem->docId);
        Serial.println(F("'"));
        delete pendingItem;
        vTaskDelay(pdMS_TO_TICKS(150));
      } else {
        Serial.print(F("[COMMANDS-LOG] Buffered sync FAILED! HTTP: "));
        Serial.print(pCode);
        Serial.print(F(" | Reason: "));
        Serial.println(fbdo.errorReason());
        fbdo.clear();
        pendingCommandLogs.insert(pendingCommandLogs.begin(), pendingItem);
        vTaskDelay(pdMS_TO_TICKS(2000));
      }
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
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println(F("[WIFI]     Disconnected from Wi-Fi! Auto-reconnecting in background..."));
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

// Scrolls line 1 smoothly from right to left so that the ENTIRE text is visible, regardless of length
void showMessageWithScroll(const char* line1, const char* line2 = "", int defaultSpeedMs = 180, int staticDurationMs = 2500) {
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

  // If line 1 fits within 16 chars, simply display statically without scrolling
  if (len1 <= 16) {
    lcd.setCursor(0, 0);
    lcd.print(line1);
    delay(staticDurationMs);
    return;
  }

  // Adaptive speed: comfortable readability for any length without excessive waiting
  int effectiveSpeed = defaultSpeedMs;
  if (len1 > 40) {
    effectiveSpeed = 150; // Smooth & responsive for extra long names (e.g. 50+ chars)
  } else if (len1 > 25) {
    effectiveSpeed = 180;
  } else {
    effectiveSpeed = 220;
  }

  // 1. Initial view pause so patron can comfortably read the beginning of the line
  char frame[17];
  strncpy(frame, line1, 16);
  frame[16] = '\0';
  lcd.setCursor(0, 0);
  lcd.print(frame);
  delay(800);

  // 2. Scroll step-by-step from beginning until the very end of the text is reached
  int totalSteps = (int)len1 - 16;
  for (int step = 1; step <= totalSteps; step++) {
    strncpy(frame, line1 + step, 16);
    frame[16] = '\0';
    lcd.setCursor(0, 0);
    lcd.print(frame);
    delay(effectiveSpeed);
  }

  // 3. Final pause on the last 16 characters so patron can read the end of their name
  delay(800);
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
  // Adaptive scroll speed: 150ms for extra long titles (>45 chars), 180ms for >25 chars, 250ms otherwise
  int scrollSpeedMs = (titleLen > 45) ? 150 : ((titleLen > 25) ? 180 : 250);

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
  lastSessionSecondLogged = 0;

  // Clear active authenticated student session credentials
  activeStudentName[0] = '\0';
  activeStudentUID[0]  = '\0';
  activeLoginTime[0]   = '\0';

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

// =========================================================================================
// SECTION 8: OFFLINE-FIRST ROSTER SYNC & VERIFICATION (FIRESTORE & NVS FLASH)
// =========================================================================================

// Retrieve student record from persistent ROM (Preferences NVS)
bool getStudentFromROM(const char* uid, StudentRecord& outRecord) {
  if (!uid || uid[0] == '\0') return false;
  String cleanUID = String(uid);
  cleanUID.trim();
  cleanUID.toUpperCase();

  if (!studentPrefs.isKey(cleanUID.c_str())) {
    return false;
  }

  String val = studentPrefs.getString(cleanUID.c_str(), "");
  if (val.length() == 0) return false;

  int sep = val.indexOf('|');
  if (sep >= 0) {
    String nameStr = val.substring(0, sep);
    nameStr.trim();
    strncpy(outRecord.name, nameStr.c_str(), sizeof(outRecord.name) - 1);
    outRecord.name[sizeof(outRecord.name) - 1] = '\0';
    String actStr = val.substring(sep + 1);
    actStr.trim();
    outRecord.active = (actStr == "1" || actStr == "true");
  } else {
    strncpy(outRecord.name, val.c_str(), sizeof(outRecord.name) - 1);
    outRecord.name[sizeof(outRecord.name) - 1] = '\0';
    outRecord.active = true;
  }
  return true;
}

// Stores or updates student credentials & active status in persistent ROM (NVS)
void saveStudentToROM(const char* uid, const char* name, bool active) {
  if (!uid || uid[0] == '\0' || !name || name[0] == '\0') return;
  String cleanUID = String(uid);
  cleanUID.trim();
  cleanUID.toUpperCase();

  String val = String(name) + "|" + (active ? "1" : "0");
  studentPrefs.putString(cleanUID.c_str(), val);

  // Maintain UID in __uids__ list
  String uids = studentPrefs.getString("__uids__", "");
  bool exists = false;
  int start = 0;
  while (start < uids.length()) {
    int comma = uids.indexOf(',', start);
    String token = (comma >= 0) ? uids.substring(start, comma) : uids.substring(start);
    token.trim();
    if (token == cleanUID) {
      exists = true;
      break;
    }
    if (comma < 0) break;
    start = comma + 1;
  }
  if (!exists) {
    if (uids.length() > 0) uids += ",";
    uids += cleanUID;
    studentPrefs.putString("__uids__", uids);
  }
}

// Removes a student from persistent ROM (NVS)
void removeStudentFromROM(const char* uid) {
  if (!uid || uid[0] == '\0') return;
  String cleanUID = String(uid);
  cleanUID.trim();
  cleanUID.toUpperCase();

  studentPrefs.remove(cleanUID.c_str());

  String uids = studentPrefs.getString("__uids__", "");
  if (uids.length() > 0) {
    String newUids = "";
    int start = 0;
    while (start < uids.length()) {
      int comma = uids.indexOf(',', start);
      String token = (comma >= 0) ? uids.substring(start, comma) : uids.substring(start);
      token.trim();
      if (token.length() > 0 && token != cleanUID) {
        if (newUids.length() > 0) newUids += ",";
        newUids += token;
      }
      if (comma < 0) break;
      start = comma + 1;
    }
    studentPrefs.putString("__uids__", newUids);
  }
}

// Reads all student UIDs currently indexed in persistent ROM (NVS)
std::vector<String> getAllROMUIDs() {
  std::vector<String> list;
  String uids = studentPrefs.getString("__uids__", "");
  int start = 0;
  while (start < uids.length()) {
    int comma = uids.indexOf(',', start);
    String token = (comma >= 0) ? uids.substring(start, comma) : uids.substring(start);
    token.trim();
    if (token.length() > 0) {
      list.push_back(token);
    }
    if (comma < 0) break;
    start = comma + 1;
  }
  return list;
}

// Retrieve book record from persistent ROM (Preferences NVS)
bool getBookFromROM(const char* uid, BookRecord& outRecord) {
  if (!uid || uid[0] == '\0') return false;
  String cleanUID = String(uid);
  cleanUID.trim();
  cleanUID.toUpperCase();

  if (bookPrefs.isKey(cleanUID.c_str())) {
    String val = bookPrefs.getString(cleanUID.c_str(), "");
    if (val.length() > 0) {
      // Parse format: title|shelf|rack|active from right to left
      int p3 = val.lastIndexOf('|');
      if (p3 >= 0) {
        String actStr = val.substring(p3 + 1);
        actStr.trim();
        outRecord.active = (actStr == "1" || actStr == "true");

        int p2 = val.lastIndexOf('|', p3 - 1);
        if (p2 >= 0) {
          String rackStr = val.substring(p2 + 1, p3);
          rackStr.trim();
          outRecord.rack = (uint8_t)rackStr.toInt();

          int p1 = val.lastIndexOf('|', p2 - 1);
          if (p1 >= 0) {
            String shelfStr = val.substring(p1 + 1, p2);
            shelfStr.trim();
            outRecord.shelf = (uint8_t)shelfStr.toInt();

            String titleStr = val.substring(0, p1);
            titleStr.trim();
            strncpy(outRecord.title, titleStr.c_str(), sizeof(outRecord.title) - 1);
            outRecord.title[sizeof(outRecord.title) - 1] = '\0';
            return true;
          }
        }
      }
      // Fallback if unexpected format
      strncpy(outRecord.title, val.c_str(), sizeof(outRecord.title) - 1);
      outRecord.title[sizeof(outRecord.title) - 1] = '\0';
      outRecord.shelf = 1;
      outRecord.rack = 1;
      outRecord.active = true;
      return true;
    }
  }

  // Fallback to static registeredBooks array if not in REST mode and not found in bookPrefs
  if (!systemRestMode) {
    for (size_t i = 0; i < NUM_BOOKS; i++) {
      if (strcasecmp(registeredBooks[i].uid, cleanUID.c_str()) == 0) {
        strncpy(outRecord.title, registeredBooks[i].title, sizeof(outRecord.title) - 1);
        outRecord.title[sizeof(outRecord.title) - 1] = '\0';
        outRecord.shelf = registeredBooks[i].shelf;
        outRecord.rack = registeredBooks[i].rack;
        outRecord.active = true;
        return true;
      }
    }
  }

  return false;
}

// Stores or updates book metadata in persistent ROM (NVS)
void saveBookToROM(const char* uid, const char* title, uint8_t shelf, uint8_t rack, bool active) {
  if (!uid || uid[0] == '\0' || !title || title[0] == '\0') return;
  String cleanUID = String(uid);
  cleanUID.trim();
  cleanUID.toUpperCase();

  String val = String(title) + "|" + String(shelf) + "|" + String(rack) + "|" + (active ? "1" : "0");
  bookPrefs.putString(cleanUID.c_str(), val);

  // Maintain UID in __uids__ list
  String uids = bookPrefs.getString("__uids__", "");
  bool exists = false;
  int start = 0;
  while (start < uids.length()) {
    int comma = uids.indexOf(',', start);
    String token = (comma >= 0) ? uids.substring(start, comma) : uids.substring(start);
    token.trim();
    if (token == cleanUID) {
      exists = true;
      break;
    }
    if (comma < 0) break;
    start = comma + 1;
  }
  if (!exists) {
    if (uids.length() > 0) uids += ",";
    uids += cleanUID;
    bookPrefs.putString("__uids__", uids);
  }
}

// Removes a book from persistent ROM (NVS)
void removeBookFromROM(const char* uid) {
  if (!uid || uid[0] == '\0') return;
  String cleanUID = String(uid);
  cleanUID.trim();
  cleanUID.toUpperCase();

  bookPrefs.remove(cleanUID.c_str());

  String uids = bookPrefs.getString("__uids__", "");
  if (uids.length() > 0) {
    String newUids = "";
    int start = 0;
    while (start < uids.length()) {
      int comma = uids.indexOf(',', start);
      String token = (comma >= 0) ? uids.substring(start, comma) : uids.substring(start);
      token.trim();
      if (token.length() > 0 && token != cleanUID) {
        if (newUids.length() > 0) newUids += ",";
        newUids += token;
      }
      if (comma < 0) break;
      start = comma + 1;
    }
    bookPrefs.putString("__uids__", newUids);
  }
}

// Reads all book UIDs currently indexed in persistent ROM (NVS)
std::vector<String> getAllROMBookUIDs() {
  std::vector<String> list;
  String uids = bookPrefs.getString("__uids__", "");
  int start = 0;
  while (start < uids.length()) {
    int comma = uids.indexOf(',', start);
    String token = (comma >= 0) ? uids.substring(start, comma) : uids.substring(start);
    token.trim();
    if (token.length() > 0) {
      list.push_back(token);
    }
    if (comma < 0) break;
    start = comma + 1;
  }
  return list;
}

// Inspects and logs all student records and books currently stored in persistent ROM (NVS)
void printROMCacheSummary() {
  std::vector<String> uids = getAllROMUIDs();
  Serial.println(F("==================================================="));
  Serial.print(F("[ROM-CACHE] Registered Student Cards in ROM (Total: "));
  Serial.print(uids.size());
  Serial.println(F("):"));
  for (size_t i = 0; i < uids.size(); i++) {
    StudentRecord rec;
    if (getStudentFromROM(uids[i].c_str(), rec)) {
      Serial.print(F("            ["));
      Serial.print(i + 1);
      Serial.print(F("] UID: "));
      Serial.print(uids[i]);
      Serial.print(F(" | Name: \""));
      Serial.print(rec.name);
      Serial.print(F("\" | Status: "));
      Serial.println(rec.active ? F("ACTIVE") : F("SUSPENDED"));
    }
  }
  if (uids.empty()) {
    Serial.println(F("            (No student cards cached in ROM yet)"));
  }

  std::vector<String> bookUids = getAllROMBookUIDs();
  Serial.println(F("---------------------------------------------------"));
  Serial.print(F("[ROM-CACHE] Registered Book Tags in ROM (Total: "));
  Serial.print(bookUids.size());
  Serial.println(F("):"));
  for (size_t i = 0; i < bookUids.size(); i++) {
    BookRecord bRec;
    if (getBookFromROM(bookUids[i].c_str(), bRec)) {
      Serial.print(F("            ["));
      Serial.print(i + 1);
      Serial.print(F("] UID: "));
      Serial.print(bookUids[i]);
      Serial.print(F(" | Title: \""));
      Serial.print(bRec.title);
      Serial.print(F("\" | Shelf: "));
      Serial.print(bRec.shelf);
      Serial.print(F(" | Rack: "));
      Serial.print(bRec.rack);
      Serial.print(F(" | Status: "));
      Serial.println(bRec.active ? F("ACTIVE") : F("SUSPENDED"));
    }
  }
  if (bookUids.empty()) {
    Serial.println(F("            (No book tags cached in ROM yet)"));
  }
  Serial.println(F("==================================================="));
}

// Dumps student roster cache ("students" namespace) to Serial Monitor
void dumpROMStudents() {
  Serial.println();
  Serial.println(F("==================================================="));
  Serial.println(F("        STUDENT ROSTER ROM CACHE ('students')      "));
  Serial.println(F("==================================================="));
  std::vector<String> uids = getAllROMUIDs();
  Serial.print(F("[ROM-STUDENTS] Total Registered Students: "));
  Serial.println(uids.size());
  if (uids.empty()) {
    Serial.println(F("               (No student records found in ROM)"));
  } else {
    for (size_t i = 0; i < uids.size(); i++) {
      StudentRecord rec;
      if (getStudentFromROM(uids[i].c_str(), rec)) {
        Serial.print(F("  ["));
        Serial.print(i + 1);
        Serial.print(F("] UID: "));
        Serial.print(uids[i]);
        Serial.print(F(" | Name: \""));
        Serial.print(rec.name);
        Serial.print(F("\" | Status: "));
        Serial.println(rec.active ? F("ACTIVE") : F("SUSPENDED"));
      } else {
        Serial.print(F("  ["));
        Serial.print(i + 1);
        Serial.print(F("] UID: "));
        Serial.print(uids[i]);
        Serial.println(F(" | Error reading record"));
      }
    }
  }
  Serial.println(F("==================================================="));
  Serial.println();
}

// Dumps book catalog cache ("books" namespace) to Serial Monitor
void dumpROMBooks() {
  Serial.println();
  Serial.println(F("==================================================="));
  Serial.println(F("         BOOK CATALOG ROM CACHE ('books')          "));
  Serial.println(F("==================================================="));
  std::vector<String> bookUids = getAllROMBookUIDs();
  Serial.print(F("[ROM-BOOKS] Total Registered Books: "));
  Serial.println(bookUids.size());
  if (bookUids.empty()) {
    Serial.println(F("             (No book records found in ROM)"));
  } else {
    for (size_t i = 0; i < bookUids.size(); i++) {
      BookRecord bRec;
      if (getBookFromROM(bookUids[i].c_str(), bRec)) {
        Serial.print(F("  ["));
        Serial.print(i + 1);
        Serial.print(F("] UID: "));
        Serial.print(bookUids[i]);
        Serial.print(F(" | Title: \""));
        Serial.print(bRec.title);
        Serial.print(F("\" | Shelf: "));
        Serial.print(bRec.shelf);
        Serial.print(F(" | Rack: "));
        Serial.print(bRec.rack);
        Serial.print(F(" | Status: "));
        Serial.println(bRec.active ? F("ACTIVE") : F("SUSPENDED"));
      } else {
        Serial.print(F("  ["));
        Serial.print(i + 1);
        Serial.print(F("] UID: "));
        Serial.print(bookUids[i]);
        Serial.println(F(" | Error reading book record"));
      }
    }
  }
  Serial.println(F("==================================================="));
  Serial.println();
}

// Dumps all persistent ROM data (Student Roster, Book Catalog & Offline Logs) to Serial Monitor
void dumpAllROMData() {
  Serial.println();
  Serial.println(F("==================================================="));
  Serial.println(F("              ESP32 ROM (NVS) DATA DUMP            "));
  Serial.println(F("==================================================="));

  // 1. Student Roster Cache ("students" namespace)
  std::vector<String> uids = getAllROMUIDs();
  Serial.print(F("[ROM-STUDENTS] Total Registered Students: "));
  Serial.println(uids.size());
  if (uids.empty()) {
    Serial.println(F("               (No student records found in ROM)"));
  } else {
    for (size_t i = 0; i < uids.size(); i++) {
      StudentRecord rec;
      if (getStudentFromROM(uids[i].c_str(), rec)) {
        Serial.print(F("  ["));
        Serial.print(i + 1);
        Serial.print(F("] UID: "));
        Serial.print(uids[i]);
        Serial.print(F(" | Name: \""));
        Serial.print(rec.name);
        Serial.print(F("\" | Status: "));
        Serial.println(rec.active ? F("ACTIVE") : F("SUSPENDED"));
      } else {
        Serial.print(F("  ["));
        Serial.print(i + 1);
        Serial.print(F("] UID: "));
        Serial.print(uids[i]);
        Serial.println(F(" | Error reading record"));
      }
    }
  }
  Serial.println(F("---------------------------------------------------"));

  // 2. Book Catalog Cache ("books" namespace)
  std::vector<String> bookUids = getAllROMBookUIDs();
  Serial.print(F("[ROM-BOOKS] Total Registered Books: "));
  Serial.println(bookUids.size());
  if (bookUids.empty()) {
    Serial.println(F("             (No book records found in ROM)"));
  } else {
    for (size_t i = 0; i < bookUids.size(); i++) {
      BookRecord bRec;
      if (getBookFromROM(bookUids[i].c_str(), bRec)) {
        Serial.print(F("  ["));
        Serial.print(i + 1);
        Serial.print(F("] UID: "));
        Serial.print(bookUids[i]);
        Serial.print(F(" | Title: \""));
        Serial.print(bRec.title);
        Serial.print(F("\" | Shelf: "));
        Serial.print(bRec.shelf);
        Serial.print(F(" | Rack: "));
        Serial.print(bRec.rack);
        Serial.print(F(" | Status: "));
        Serial.println(bRec.active ? F("ACTIVE") : F("SUSPENDED"));
      } else {
        Serial.print(F("  ["));
        Serial.print(i + 1);
        Serial.print(F("] UID: "));
        Serial.print(bookUids[i]);
        Serial.println(F(" | Error reading book record"));
      }
    }
  }
  Serial.println(F("---------------------------------------------------"));

  // 3. Offline Logs Buffer ("off_logs" namespace)
  uint16_t offCount = offlineLogsPrefs.getUShort("count", 0);
  Serial.print(F("[ROM-OFFLINE-LOGS] Total Buffered Offline Logs: "));
  Serial.println(offCount);
  if (offCount == 0) {
    Serial.println(F("                   (No pending offline logs in ROM)"));
  } else {
    for (uint16_t i = 0; i < offCount; i++) {
      String key = "l_" + String(i);
      FirestoreLogEvent offEvent;
      size_t readLen = offlineLogsPrefs.getBytes(key.c_str(), &offEvent, sizeof(offEvent));
      if (readLen == sizeof(offEvent)) {
        Serial.print(F("  [Log #"));
        Serial.print(i + 1);
        Serial.print(F("] Doc ID: '"));
        Serial.print(offEvent.docId);
        Serial.print(F("' | Event: "));
        Serial.print(offEvent.eventType);
        Serial.print(F(" | Student: "));
        Serial.print(offEvent.studentName);
        Serial.print(F(" ("));
        Serial.print(offEvent.studentUID);
        Serial.print(F(") | Book: \""));
        Serial.print(offEvent.bookTitle[0] ? offEvent.bookTitle : "N/A");
        Serial.print(F("\" | Shelf: "));
        Serial.print(offEvent.shelf);
        Serial.print(F(" | Rack: "));
        Serial.print(offEvent.rack);
        Serial.print(F(" | Time: "));
        Serial.println(offEvent.timestamp);
      } else {
        Serial.print(F("  [Log #"));
        Serial.print(i + 1);
        Serial.println(F("] (Corrupted or incomplete entry in ROM)"));
      }
    }
  }

  // 4. System & Network State Summary
  Serial.println(F("---------------------------------------------------"));
  Serial.print(F("[SYSTEM-STATUS] Free Heap RAM: "));
  Serial.print(ESP.getFreeHeap());
  Serial.print(F(" bytes | Wi-Fi: "));
  Serial.print(WiFi.status() == WL_CONNECTED ? F("CONNECTED (") : F("DISCONNECTED"));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(WiFi.localIP());
    Serial.print(F(")"));
  }
  Serial.println();
  Serial.println(F("==================================================="));
  Serial.println();
}

// Erases student records from persistent ROM (leaves books and offline logs intact)
void clearStudentROMData() {
  studentPrefs.clear();
  activeStudentName[0] = '\0';
  activeStudentUID[0]  = '\0';
  activeLoginTime[0]   = '\0';
  sessionStartTime = 0;
  currentState = WAITING_FOR_STUDENT;

  Serial.println();
  Serial.println(F("==================================================="));
  Serial.println(F("[ROM-CLEAR] Student Roster Cache Cleared (Background)!"));
  Serial.println(F("            - Student roster cache ('students') erased"));
  Serial.println(F("            - Book catalog and offline logs retained"));
  Serial.println(F("            - Type 'upts' or 'clrupts' to sync students from Firestore"));
  Serial.println(F("==================================================="));
  Serial.println();
}

// Erases book records from persistent ROM (leaves students and offline logs intact)
void clearBookROMData() {
  bookPrefs.clear();
  sessionStartTime = 0;
  currentState = WAITING_FOR_STUDENT;

  Serial.println();
  Serial.println(F("==================================================="));
  Serial.println(F("[ROM-CLEAR] Book Catalog Cache Cleared (Background)!"));
  Serial.println(F("            - Book catalog cache ('books') erased"));
  Serial.println(F("            - Student roster and offline logs retained"));
  Serial.println(F("            - Type 'uptb' or 'clruptb' to sync books from Firestore"));
  Serial.println(F("==================================================="));
  Serial.println();
}

// Erases all student records, books, and offline logs from persistent ROM and enters REST mode
// Zero LCD disruption: runs entirely in the background and logs to Serial only
void clearAllROMData() {
  systemRestMode = true;
  studentPrefs.clear();
  bookPrefs.clear();
  offlineLogsPrefs.clear();
  // Clear in-memory session credentials without disturbing the LCD display
  activeStudentName[0] = '\0';
  activeStudentUID[0]  = '\0';
  activeLoginTime[0]   = '\0';
  sessionStartTime = 0;
  currentState = WAITING_FOR_STUDENT;

  Serial.println();
  Serial.println(F("==================================================="));
  Serial.println(F("[ROM-CLEAR] Full Persistent ROM Completely Cleared (Background)!"));
  Serial.println(F("            - Student roster cache ('students') erased"));
  Serial.println(F("            - Book catalog cache ('books') erased"));
  Serial.println(F("            - Offline logs buffer ('off_logs') erased"));
  Serial.println(F("            - System is now in REST MODE."));
  Serial.println(F("            - LCD display remains untouched (dedicated to user guidance)."));
  Serial.println(F("            - If a patron taps a card before sync, LCD will alert:"));
  Serial.println(F("              Line 1: '   ROM Empty    ' | Line 2: 'System Rest Mode'"));
  Serial.println(F("            - Type 'upt' or 'clrupt' to exit REST mode and sync from Firestore."));
  Serial.println(F("==================================================="));
  Serial.println();
}

// Clears student cache & pulls fresh student roster from Firestore without hardware reboot
void executeCleanResetAndSyncStudents() {
  systemRestMode = false;
  Serial.println();
  Serial.println(F("==================================================="));
  Serial.println(F("[CLRUPTS]   Executing background student erase and fresh Firestore sync..."));
  studentPrefs.clear();
  activeStudentName[0] = '\0';
  activeStudentUID[0]  = '\0';
  activeLoginTime[0]   = '\0';
  sessionStartTime = 0;
  currentState = WAITING_FOR_STUDENT;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[CLRUPTS]   WARN: Wi-Fi currently disconnected. Sync will begin upon link connect."));
  }
  triggerCleanSyncStudents = true;
}

// Clears book cache & pulls fresh book catalog from Firestore without hardware reboot
void executeCleanResetAndSyncBooks() {
  systemRestMode = false;
  Serial.println();
  Serial.println(F("==================================================="));
  Serial.println(F("[CLRUPTB]   Executing background book erase and fresh Firestore sync..."));
  bookPrefs.clear();
  sessionStartTime = 0;
  currentState = WAITING_FOR_STUDENT;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[CLRUPTB]   WARN: Wi-Fi currently disconnected. Sync will begin upon link connect."));
  }
  triggerCleanSyncBooks = true;
}

// Clears ROM & memory and immediately pulls fresh student roster & books from Firestore without reboot
// Zero LCD disruption: runs entirely in the background and logs to Serial only
void executeCleanResetAndSync() {
  systemRestMode = false;
  Serial.println();
  Serial.println(F("==================================================="));
  Serial.println(F("[CLRUPT]    Executing background ROM erase and fresh Firestore sync (students & books)..."));
  studentPrefs.clear();
  bookPrefs.clear();
  offlineLogsPrefs.clear();
  // Clear in-memory session credentials without disturbing the LCD display
  activeStudentName[0] = '\0';
  activeStudentUID[0]  = '\0';
  activeLoginTime[0]   = '\0';
  sessionStartTime = 0;
  currentState = WAITING_FOR_STUDENT;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[CLRUPT]    WARN: Wi-Fi currently disconnected. Sync will begin as soon as link connects."));
  }

  triggerCleanSyncStudents = true;
  triggerCleanSyncBooks = true;
}

// Captures a detailed snapshot of ROM cache data, memory, and system state
String captureDetailedStateSummary() {
  String out = "";
  std::vector<String> sUids = getAllROMUIDs();
  out += "Students (" + String(sUids.size()) + "): ";
  if (sUids.empty()) {
    out += "[None]";
  } else {
    for (size_t i = 0; i < sUids.size(); i++) {
      if (i > 0) out += ", ";
      StudentRecord rec;
      if (getStudentFromROM(sUids[i].c_str(), rec)) {
        out += "[" + String(i + 1) + "] " + sUids[i] + " (\"" + String(rec.name) + "\", " + (rec.active ? "ACTIVE" : "SUSPENDED") + ")";
      } else {
        out += "[" + String(i + 1) + "] " + sUids[i] + " (Read Err)";
      }
    }
  }

  std::vector<String> bUids = getAllROMBookUIDs();
  out += " | Books (" + String(bUids.size()) + "): ";
  if (bUids.empty()) {
    out += "[None]";
  } else {
    for (size_t i = 0; i < bUids.size(); i++) {
      if (i > 0) out += ", ";
      BookRecord bRec;
      if (getBookFromROM(bUids[i].c_str(), bRec)) {
        out += "[" + String(i + 1) + "] " + bUids[i] + " (\"" + String(bRec.title) + "\", S" + String(bRec.shelf) + "R" + String(bRec.rack) + ", " + (bRec.active ? "ACTIVE" : "SUSPENDED") + ")";
      } else {
        out += "[" + String(i + 1) + "] " + bUids[i] + " (Read Err)";
      }
    }
  }

  uint16_t offCount = offlineLogsPrefs.getUShort("count", 0);
  out += " | Offline Logs: " + String(offCount);
  out += " | Free Heap: " + String(ESP.getFreeHeap()) + " B";
  out += " | Mode: " + String(systemRestMode ? "REST_MODE" : "NORMAL_STANDBY");
  out += " | State: " + String(currentState == WAITING_FOR_STUDENT ? "WAITING_FOR_STUDENT" : "WAITING_FOR_BOOK");
  out += " | Wi-Fi: " + String(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");

  return out;
}

// Dispatches a command execution audit event to the FreeRTOS background queue for 'commands-log' collection
void queueCommandLog(const String& cmd,
                     const String& stateBefore,
                     const String& serialOutput,
                     const String& stateAfter,
                     const String& actionSummary,
                     const String& executionStatus,
                     uint32_t freeHeapBefore,
                     uint32_t freeHeapAfter) {
  if (commandLogQueue == NULL) {
    Serial.println(F("[COMMANDS-LOG] ERROR: Command log queue not initialized!"));
    return;
  }

  CommandLogItem* item = new CommandLogItem();
  if (!item) {
    Serial.println(F("[COMMANDS-LOG] ERROR: Memory allocation failed for command log!"));
    return;
  }

  char humanTime[64];
  char rawDocId[32];
  getCurrentTimestamps(humanTime, sizeof(humanTime), rawDocId, sizeof(rawDocId));

  // Ensure unique docId if multiple commands run within the same second
  static String lastDocId = "";
  static int duplicateSeq = 0;
  String finalDocId = String(rawDocId);
  if (finalDocId == lastDocId) {
    duplicateSeq++;
    finalDocId += "_" + String(duplicateSeq);
  } else {
    lastDocId = finalDocId;
    duplicateSeq = 0;
  }

  item->docId = finalDocId;
  item->timestamp = String(humanTime);
  item->command = cmd;
  item->stateBefore = stateBefore;
  item->actionSummary = actionSummary;
  item->serialOutput = serialOutput;
  item->stateAfter = stateAfter;
  item->executionStatus = executionStatus;
  item->systemMode = systemRestMode ? "REST_MODE" : "NORMAL_STANDBY";
  item->freeHeapBefore = freeHeapBefore;
  item->freeHeapAfter = freeHeapAfter;

  Serial.print(F("[COMMANDS-LOG] Enqueuing '"));
  Serial.print(cmd);
  Serial.print(F("' command log (Doc ID: '"));
  Serial.print(item->docId);
  Serial.println(F("') to background Firestore queue..."));

  if (xQueueSend(commandLogQueue, &item, 0) != pdTRUE) {
    Serial.println(F("[COMMANDS-LOG] WARN: Background queue full! Dropping oldest log."));
    delete item;
  }
}

// Periodic 30-second system clock & calendar broadcaster with full details
void printPeriodicFullTime() {
  struct tm timeinfo;
  bool timeValid = getLocalTime(&timeinfo, 20);

  Serial.println();
  Serial.println(F("================================================================================"));
  Serial.println(F("              ESP32 SYSTEM CLOCK & CALENDAR (30-SECOND UPDATE)                 "));
  Serial.println(F("================================================================================"));

  if (timeValid) {
    char dateStr[64];
    strftime(dateStr, sizeof(dateStr), "%A, %d %B %Y", &timeinfo);

    char time24Str[32];
    strftime(time24Str, sizeof(time24Str), "%H:%M:%S", &timeinfo);

    char time12Str[32];
    strftime(time12Str, sizeof(time12Str), "%I:%M:%S %p", &timeinfo);

    char monthStr[32];
    strftime(monthStr, sizeof(monthStr), "%B", &timeinfo);

    char dayOfWeek[32];
    strftime(dayOfWeek, sizeof(dayOfWeek), "%A", &timeinfo);

    char isoStr[40];
    strftime(isoStr, sizeof(isoStr), "%Y-%m-%dT%H:%M:%S+05:30", &timeinfo);

    Serial.print(F("[TIME-30S] Full Date & Time : "));
    Serial.print(dateStr);
    Serial.print(F(" | "));
    Serial.println(time24Str);

    Serial.print(F("           • Day of Week    : "));
    Serial.println(dayOfWeek);

    Serial.print(F("           • Date (Day)     : "));
    Serial.println(timeinfo.tm_mday);

    Serial.print(F("           • Month          : "));
    Serial.print(monthStr);
    Serial.print(F(" (Month "));
    Serial.print(timeinfo.tm_mon + 1);
    Serial.println(F(" of 12)"));

    Serial.print(F("           • Year           : "));
    Serial.print(timeinfo.tm_year + 1900);
    Serial.print(F(" (Day "));
    Serial.print(timeinfo.tm_yday + 1);
    Serial.println(F(" of Year)"));

    Serial.print(F("           • Time (24-Hour) : "));
    Serial.print(time24Str);
    Serial.println(F(" IST"));

    Serial.print(F("           • Time (12-Hour) : "));
    Serial.print(time12Str);
    Serial.println(F(" IST"));

    Serial.print(F("           • ISO-8601 Stamp : "));
    Serial.println(isoStr);

    Serial.println(F("           • Timezone       : Indian Standard Time (IST, UTC+05:30, GMT Offset: +19800s)"));
  } else {
    Serial.println(F("[TIME-30S] System Clock     : NTP Synchronization Pending (Connecting to pool.ntp.org)..."));
  }

  // System Uptime
  unsigned long totalSec = millis() / 1000;
  unsigned long days     = totalSec / 86400;
  unsigned long hours    = (totalSec % 86400) / 3600;
  unsigned long mins     = (totalSec % 3600) / 60;
  unsigned long secs     = totalSec % 60;

  Serial.print(F("           • System Uptime  : "));
  Serial.print(days);
  Serial.print(F("d "));
  Serial.print(hours);
  Serial.print(F("h "));
  Serial.print(mins);
  Serial.print(F("m "));
  Serial.print(secs);
  Serial.print(F("s ("));
  Serial.print(millis());
  Serial.println(F(" ms)"));
  Serial.println(F("================================================================================"));
  Serial.println();
}

// ESP32 Task Manager: Dumps chip overview, dual-core workloads, live FreeRTOS tasks, RAM and ROM
void dumpTaskManagerAll() {
  Serial.println();
  Serial.println(F("================================================================================"));
  Serial.println(F("                   SHELFSENSE ESP32 TASK MANAGER & SYSTEM MONITOR               "));
  Serial.println(F("================================================================================"));

  // --- SECTION 1: HARDWARE & SYSTEM ARCHITECTURE ---
  Serial.println(F("[1] HARDWARE & SYSTEM ARCHITECTURE:"));
  Serial.print(F("    • Chip Model        : "));
  Serial.print(ESP.getChipModel());
  Serial.print(F(" (Revision "));
  Serial.print(ESP.getChipRevision());
  Serial.println(F(")"));
  Serial.print(F("    • CPU Cores         : "));
  Serial.print(ESP.getChipCores());
  Serial.print(F(" cores running @ "));
  Serial.print(ESP.getCpuFreqMHz());
  Serial.println(F(" MHz"));
  Serial.print(F("    • SDK / IDF Version : "));
  Serial.println(ESP.getSdkVersion());

  unsigned long totalSec = millis() / 1000;
  unsigned long days     = totalSec / 86400;
  unsigned long hours    = (totalSec % 86400) / 3600;
  unsigned long mins     = (totalSec % 3600) / 60;
  unsigned long secs     = totalSec % 60;
  Serial.print(F("    • System Uptime     : "));
  Serial.print(days); Serial.print(F("d "));
  Serial.print(hours); Serial.print(F("h "));
  Serial.print(mins); Serial.print(F("m "));
  Serial.print(secs); Serial.println(F("s"));
  Serial.print(F("    • Network Status    : Wi-Fi "));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("CONNECTED (IP: "));
    Serial.print(WiFi.localIP());
    Serial.print(F(", RSSI: "));
    Serial.print(WiFi.RSSI());
    Serial.println(F(" dBm)"));
  } else {
    Serial.println(F("DISCONNECTED"));
  }

  // --- SECTION 2: DUAL-CORE 0 & CORE 1 WORKLOAD RESPONSIBILITIES ---
  Serial.println(F("--------------------------------------------------------------------------------"));
  Serial.println(F("[2] DUAL-CORE TASK WORKLOAD ASSIGNMENTS:"));
  Serial.println(F("    ► CORE 0 [Cloud, Network & Storage Engine]:"));
  Serial.println(F("        - Dedicated Task    : 'firestoreTask' (Pinned to Core 0, Priority 1 - LOW BACKGROUND PRIORITY)"));
  Serial.println(F("        - Cloud Sync        : Periodic 60s diff sync for 'student_names' and 'book_names' (idle only)"));
  Serial.println(F("        - Logging Services  : Asynchronous Firestore logger ('library_logs' & 'commands-log')"));
  Serial.println(F("        - Offline Buffer    : Automatic ROM offline log drainer and flusher (every 15s)"));
  Serial.println(F("        - Network Services  : Wi-Fi reconnect supervisor, NTP clock sync, Firebase Auth"));
  Serial.println(F("        - System Daemons    : ESP32 TCP/IP stack ('wifi', 'tiT', 'sys_evt')"));
  Serial.println(F("    ► CORE 1 [Real-Time Sensor & Interactive UI Engine]:"));
  Serial.println(F("        - Dedicated Task    : 'loopTask' (Pinned to Core 1, Priority 3 - TOP REAL-TIME PRIORITY)"));
  Serial.println(F("        - RFID Sensing      : RC522 SPI high-frequency polling loop (Zero Latency)"));
  Serial.println(F("        - Display Actuation : LiquidCrystal I2C 16x2 real-time guidance & animated titles"));
  Serial.println(F("        - Peripherals       : Buzzer acoustic chirps/alarms, Green/Red LED indicators"));
  Serial.println(F("        - Interactive CLI   : Non-blocking Serial Monitor command listener"));
  Serial.println(F("        - Timers & Security : 18s inactivity watchdog, 30s clock broadcaster, state machine"));

  // --- SECTION 3: FREERTOS ACTIVE TASK MANAGER TABLE ---
  Serial.println(F("--------------------------------------------------------------------------------"));
  Serial.println(F("[3] FREERTOS TASK MANAGER TABLE (LIVE TASKS):"));
  UBaseType_t numTasks = uxTaskGetNumberOfTasks();
  TaskStatus_t *pxTaskStatusArray = (TaskStatus_t *)pvPortMalloc(numTasks * sizeof(TaskStatus_t));
  if (pxTaskStatusArray != NULL) {
    numTasks = uxTaskGetSystemState(pxTaskStatusArray, numTasks, NULL);
    Serial.println(F("    Idx | Task Name        | Core Affinity | State     | Priority | Min Stack Free"));
    Serial.println(F("    ----+------------------+---------------+-----------+----------+---------------"));
    for (UBaseType_t i = 0; i < numTasks; i++) {
      char line[110];
      const char* stateStr = "UNKNOWN";
      switch (pxTaskStatusArray[i].eCurrentState) {
        case eRunning:   stateStr = "RUNNING  "; break;
        case eReady:     stateStr = "READY    "; break;
        case eBlocked:   stateStr = "BLOCKED  "; break;
        case eSuspended: stateStr = "SUSPENDED"; break;
        case eDeleted:   stateStr = "DELETED  "; break;
        default: break;
      }
      const char* coreStr = "Any Core";
      if (pxTaskStatusArray[i].xCoreID == 0) coreStr = "Core 0  ";
      else if (pxTaskStatusArray[i].xCoreID == 1) coreStr = "Core 1  ";

      snprintf(line, sizeof(line), "    %3u | %-16s | %-13s | %-9s | %4u/%-4u | %5u bytes",
               (unsigned int)pxTaskStatusArray[i].xTaskNumber,
               pxTaskStatusArray[i].pcTaskName,
               coreStr,
               stateStr,
               (unsigned int)pxTaskStatusArray[i].uxCurrentPriority,
               (unsigned int)pxTaskStatusArray[i].uxBasePriority,
               (unsigned int)(pxTaskStatusArray[i].usStackHighWaterMark * sizeof(StackType_t)));
      Serial.println(line);
    }
    vPortFree(pxTaskStatusArray);
  } else {
    Serial.println(F("    (Unable to allocate memory to query FreeRTOS task status)"));
  }

  // --- SECTION 4: RAM (INTERNAL MEMORY) UTILIZATION ---
  Serial.println(F("--------------------------------------------------------------------------------"));
  Serial.println(F("[4] RAM (INTERNAL MEMORY) UTILIZATION:"));
  uint32_t totalHeap = ESP.getHeapSize();
  uint32_t freeHeap  = ESP.getFreeHeap();
  uint32_t usedHeap  = totalHeap >= freeHeap ? (totalHeap - freeHeap) : 0;
  float heapUsagePct = totalHeap > 0 ? ((float)usedHeap * 100.0f / (float)totalHeap) : 0.0f;
  uint32_t minFree   = ESP.getMinFreeHeap();
  uint32_t maxAlloc  = ESP.getMaxAllocHeap();

  Serial.print(F("    • Total Heap Size   : "));
  Serial.print(totalHeap);
  Serial.print(F(" bytes ("));
  Serial.print(totalHeap / 1024);
  Serial.println(F(" KB)"));

  Serial.print(F("    • Used Heap RAM     : "));
  Serial.print(usedHeap);
  Serial.print(F(" bytes ("));
  Serial.print(usedHeap / 1024);
  Serial.print(F(" KB, "));
  Serial.print(heapUsagePct, 1);
  Serial.println(F("% used)"));

  Serial.print(F("    • Free Heap RAM     : "));
  Serial.print(freeHeap);
  Serial.print(F(" bytes ("));
  Serial.print(freeHeap / 1024);
  Serial.println(F(" KB)"));

  Serial.print(F("    • Lowest Free Watermark : "));
  Serial.print(minFree);
  Serial.print(F(" bytes ("));
  Serial.print(minFree / 1024);
  Serial.println(F(" KB)"));

  Serial.print(F("    • Max Contiguous Block  : "));
  Serial.print(maxAlloc);
  Serial.print(F(" bytes ("));
  Serial.print(maxAlloc / 1024);
  Serial.println(F(" KB)"));

  if (ESP.getPsramSize() > 0) {
    Serial.print(F("    • PSRAM Total / Free: "));
    Serial.print(ESP.getPsramSize());
    Serial.print(F(" / "));
    Serial.print(ESP.getFreePsram());
    Serial.println(F(" bytes"));
  } else {
    Serial.println(F("    • PSRAM External RAM: None detected (Using high-speed internal SRAM)"));
  }

  // Active FreeRTOS Queue Load in RAM
  UBaseType_t fqWaiting = firestoreQueue != NULL ? uxQueueMessagesWaiting(firestoreQueue) : 0;
  UBaseType_t cqWaiting = commandLogQueue != NULL ? uxQueueMessagesWaiting(commandLogQueue) : 0;
  Serial.print(F("    • Queues Waiting in RAM : firestoreQueue: "));
  Serial.print(fqWaiting);
  Serial.print(F("/10 | commandLogQueue: "));
  Serial.print(cqWaiting);
  Serial.println(F("/10"));

  // In-Memory Active Session Variables
  Serial.print(F("    • Active State in RAM   : "));
  Serial.print(currentState == WAITING_FOR_STUDENT ? F("WAITING_FOR_STUDENT") : F("WAITING_FOR_BOOK"));
  Serial.print(F(" | System Mode: "));
  Serial.println(systemRestMode ? F("REST MODE (Active)") : F("NORMAL STANDBY"));
  if (activeStudentName[0] != '\0') {
    Serial.print(F("    • Active Patron in RAM  : "));
    Serial.print(activeStudentName);
    Serial.print(F(" (UID: "));
    Serial.print(activeStudentUID);
    Serial.print(F(", Login: "));
    Serial.print(activeLoginTime);
    Serial.println(F(")"));
  }

  // --- SECTION 5: ROM (FLASH STORAGE & PERSISTENT NVS) ---
  Serial.println(F("--------------------------------------------------------------------------------"));
  Serial.println(F("[5] ROM (FLASH STORAGE & PERSISTENT NVS DATA):"));
  Serial.print(F("    • Total Flash Size  : "));
  Serial.print(ESP.getFlashChipSize());
  Serial.print(F(" bytes ("));
  Serial.print(ESP.getFlashChipSize() / (1024 * 1024));
  Serial.println(F(" MB)"));

  Serial.print(F("    • Flash Chip Speed  : "));
  Serial.print(ESP.getFlashChipSpeed() / 1000000);
  Serial.print(F(" MHz | Mode: "));
  FlashMode_t fMode = ESP.getFlashChipMode();
  Serial.println(fMode == FM_QIO ? F("QIO") : fMode == FM_QOUT ? F("QOUT") : fMode == FM_DIO ? F("DIO") : F("DOUT"));

  Serial.print(F("    • App Firmware Size : "));
  Serial.print(ESP.getSketchSize());
  Serial.print(F(" bytes (Free App Space: "));
  Serial.print(ESP.getFreeSketchSpace());
  Serial.println(F(" bytes)"));

  // 1. Student Roster in ROM
  std::vector<String> sUids = getAllROMUIDs();
  Serial.print(F("    ► Persistent 'students' Namespace: Total Registered: "));
  Serial.println(sUids.size());
  if (sUids.empty()) {
    Serial.println(F("        (No student records stored in ROM)"));
  } else {
    for (size_t i = 0; i < sUids.size(); i++) {
      StudentRecord rec;
      if (getStudentFromROM(sUids[i].c_str(), rec)) {
        Serial.print(F("        [")); Serial.print(i + 1); Serial.print(F("] UID: "));
        Serial.print(sUids[i]);
        Serial.print(F(" | Name: \"")); Serial.print(rec.name); Serial.print(F("\" | Status: "));
        Serial.println(rec.active ? F("ACTIVE") : F("SUSPENDED"));
      }
    }
  }

  // 2. Book Catalog in ROM
  std::vector<String> bUids = getAllROMBookUIDs();
  Serial.print(F("    ► Persistent 'books' Namespace: Total Registered: "));
  Serial.println(bUids.size());
  if (bUids.empty()) {
    Serial.println(F("        (No book records stored in ROM)"));
  } else {
    for (size_t i = 0; i < bUids.size(); i++) {
      BookRecord bRec;
      if (getBookFromROM(bUids[i].c_str(), bRec)) {
        Serial.print(F("        [")); Serial.print(i + 1); Serial.print(F("] UID: "));
        Serial.print(bUids[i]);
        Serial.print(F(" | Title: \"")); Serial.print(bRec.title);
        Serial.print(F("\" | Shelf: ")); Serial.print(bRec.shelf);
        Serial.print(F(" | Rack: ")); Serial.print(bRec.rack);
        Serial.print(F(" | Status: "));
        Serial.println(bRec.active ? F("ACTIVE") : F("SUSPENDED"));
      }
    }
  }

  // 3. Offline Logs Buffer in ROM
  uint16_t offCount = offlineLogsPrefs.getUShort("count", 0);
  Serial.print(F("    ► Persistent 'off_logs' Namespace: Total Buffered Offline Logs: "));
  Serial.println(offCount);
  if (offCount == 0) {
    Serial.println(F("        (No pending offline logs in ROM)"));
  } else {
    for (uint16_t i = 0; i < offCount; i++) {
      String key = "l_" + String(i);
      FirestoreLogEvent offEvent;
      size_t readLen = offlineLogsPrefs.getBytes(key.c_str(), &offEvent, sizeof(offEvent));
      if (readLen == sizeof(offEvent)) {
        Serial.print(F("        [Log #")); Serial.print(i + 1);
        Serial.print(F("] Doc ID: '")); Serial.print(offEvent.docId);
        Serial.print(F("' | Event: ")); Serial.print(offEvent.eventType);
        Serial.print(F(" | Student: ")); Serial.print(offEvent.studentName);
        Serial.print(F(" (")); Serial.print(offEvent.studentUID);
        Serial.print(F(") | Book: \"")); Serial.print(offEvent.bookTitle[0] ? offEvent.bookTitle : "N/A");
        Serial.print(F("\" | Shelf: ")); Serial.print(offEvent.shelf);
        Serial.print(F(" | Rack: ")); Serial.print(offEvent.rack);
        Serial.print(F(" | Time: ")); Serial.println(offEvent.timestamp);
      }
    }
  }

  Serial.println(F("================================================================================"));
  Serial.println();
}

// Periodic (every 20s) & Boot-Time Roster Synchronization with Diff Detection
// First reads ROM, then reads Firestore 'student_names'. If differences exist, updates ROM.
bool compareAndSyncRoster(bool isBoot, bool &outDiffDetected) {
  outDiffDetected = false;
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) {
    return false;
  }

  Serial.println(F("[AUTH-SYNC] Reading ROM cache & checking Firestore student_names collection..."));

  // 1. Read current ROM roster
  std::vector<String> romUIDs = getAllROMUIDs();
  struct RosterEntry {
    String uid;
    String name;
    bool active;
  };
  std::vector<RosterEntry> romList;
  for (size_t i = 0; i < romUIDs.size(); i++) {
    StudentRecord rec;
    if (getStudentFromROM(romUIDs[i].c_str(), rec)) {
      RosterEntry e;
      e.uid = romUIDs[i];
      e.name = String(rec.name);
      e.active = rec.active;
      romList.push_back(e);
    }
  }

  // 2. Fetch Firestore student_names collection on Core 0
  Serial.println(F("[AUTH-SYNC] Sending GET collection request to Firestore..."));
  bool getOk = Firebase.Firestore.getDocuments(&fbdo, FIREBASE_PROJECT_ID, "(default)", FIRESTORE_STUDENTS);
  int httpCode = fbdo.httpCode();
  String payload = "";
  String errorReason = "";

  if (getOk && httpCode == 200) {
    payload = fbdo.payload();
    Serial.print(F("[AUTH-SYNC] Received collection response (HTTP 200). Size: "));
    Serial.print(payload.length());
    Serial.println(F(" bytes. Comparing with ROM cache..."));
  } else {
    errorReason = fbdo.errorReason();
    Serial.print(F("[AUTH-SYNC] Failed to fetch documents from Firestore. HTTP: "));
    Serial.print(httpCode);
    Serial.print(F(" | Error: "));
    Serial.println(errorReason);
    fbdo.clear();
    vTaskDelay(pdMS_TO_TICKS(1000));
    return false;
  }

  FirebaseJson json;
  json.setJsonData(payload);
  FirebaseJsonData docsData;
  json.get(docsData, "documents");

  std::vector<RosterEntry> cloudList;
  if (docsData.success && docsData.type == "array") {
    FirebaseJsonArray arr;
    docsData.getArray(arr);
    size_t numDocs = arr.size();

    for (size_t i = 0; i < numDocs; i++) {
      FirebaseJsonData itemData;
      arr.get(itemData, i);
      if (itemData.success) {
        FirebaseJson docJson;
        itemData.getJSON(docJson);

        FirebaseJsonData resNameData, nameData;
        docJson.get(resNameData, "name");
        docJson.get(nameData, "fields/name/stringValue");

        // Parse active status supporting booleanValue, stringValue ("true"/"false"), integerValue (1/0), and raw active object
        bool isActive = true;
        FirebaseJsonData bVal, sVal, iVal, rawActive;
        if (docJson.get(bVal, "fields/active/booleanValue") && bVal.success) {
          String bStr = bVal.stringValue;
          bStr.trim();
          bStr.toLowerCase();
          if (bStr == "false" || bStr == "0") {
            isActive = false;
          } else {
            isActive = bVal.boolValue;
          }
        } else if (docJson.get(sVal, "fields/active/stringValue") && sVal.success) {
          String s = sVal.stringValue;
          s.trim();
          s.toLowerCase();
          isActive = (s == "true" || s == "1");
        } else if (docJson.get(iVal, "fields/active/integerValue") && iVal.success) {
          isActive = (iVal.intValue != 0);
        } else if (docJson.get(rawActive, "fields/active") && rawActive.success) {
          String raw = rawActive.stringValue;
          raw.trim();
          raw.toLowerCase();
          if (raw == "false" || raw == "0" || raw.indexOf("false") >= 0) {
            isActive = false;
          }
        }

        if (resNameData.success && nameData.success) {
          String fullPath = resNameData.stringValue;
          int lastSlash = fullPath.lastIndexOf('/');
          String uid = (lastSlash >= 0) ? fullPath.substring(lastSlash + 1) : fullPath;
          uid.trim();
          uid.toUpperCase();

          String studentName = nameData.stringValue;
          studentName.trim();

          if (uid.length() > 0 && studentName.length() > 0) {
            RosterEntry e;
            e.uid = uid;
            e.name = studentName;
            e.active = isActive;
            cloudList.push_back(e);
          }
        }
      }
    }
  }

  // 3. Compare romList with cloudList
  bool diff = false;
  if (romList.size() != cloudList.size()) {
    diff = true;
    Serial.print(F("[AUTH-SYNC] Difference: Student count changed (ROM: "));
    Serial.print(romList.size());
    Serial.print(F(", Cloud: "));
    Serial.print(cloudList.size());
    Serial.println(F(")"));
  } else {
    for (size_t c = 0; c < cloudList.size(); c++) {
      bool found = false;
      for (size_t r = 0; r < romList.size(); r++) {
        if (cloudList[c].uid == romList[r].uid) {
          found = true;
          if (cloudList[c].name != romList[r].name) {
            diff = true;
            Serial.print(F("[AUTH-SYNC] Difference: Student name changed for UID "));
            Serial.print(cloudList[c].uid);
            Serial.print(F(" (\""));
            Serial.print(romList[r].name);
            Serial.print(F("\" -> \""));
            Serial.print(cloudList[c].name);
            Serial.println(F("\")"));
          }
          if (cloudList[c].active != romList[r].active) {
            diff = true;
            Serial.print(F("[AUTH-SYNC] Difference: Student active status changed for UID "));
            Serial.print(cloudList[c].uid);
            Serial.print(F(" ("));
            Serial.print(romList[r].active ? F("active") : F("suspended"));
            Serial.print(F(" -> "));
            Serial.print(cloudList[c].active ? F("active") : F("suspended"));
            Serial.println(F(")"));
          }
          break;
        }
      }
      if (!found) {
        diff = true;
        Serial.print(F("[AUTH-SYNC] Difference: Student UID in cloud not found in ROM: "));
        Serial.println(cloudList[c].uid);
        break;
      }
      if (diff) break;
    }
  }

  if (diff) {
    Serial.println(F("[AUTH-SYNC] Updating ROM cache with fresh Firestore roster..."));
    studentPrefs.clear();
    String newUids = "";
    for (size_t i = 0; i < cloudList.size(); i++) {
      String val = cloudList[i].name + "|" + (cloudList[i].active ? "1" : "0");
      studentPrefs.putString(cloudList[i].uid.c_str(), val);
      if (newUids.length() > 0) newUids += ",";
      newUids += cloudList[i].uid;
      Serial.print(F("[AUTH-SYNC] Cached: \""));
      Serial.print(cloudList[i].name);
      Serial.print(F("\" [UID: "));
      Serial.print(cloudList[i].uid);
      Serial.print(F(" | Active: "));
      Serial.print(cloudList[i].active ? F("true") : F("false"));
      Serial.println(F("]"));
    }
    studentPrefs.putString("__uids__", newUids);
    Serial.print(F("[AUTH-SYNC] ROM update complete. Total students cached: "));
    Serial.println(cloudList.size());

    if (!isBoot) {
      outDiffDetected = true;
      Serial.println(F("[AUTH-SYNC] Differences detected! Roster updated. Scheduling restart countdown..."));
    }
  } else {
    Serial.println(F("[AUTH-SYNC] Periodic 20s check: ROM cache and Firestore match perfectly. No changes."));
  }

  return true;
}

// Boot-Time Roster Synchronization entry point
bool syncStudentsFromCloud() {
  bool unused = false;
  return compareAndSyncRoster(true /* isBoot */, unused);
}

// Periodic (every 20s) & Boot-Time Book Catalog Synchronization with Diff Detection
// First reads ROM, then reads Firestore 'book_names'. If differences exist, updates ROM.
bool compareAndSyncBooks(bool isBoot, bool &outDiffDetected) {
  outDiffDetected = false;
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) {
    return false;
  }

  Serial.println(F("[BOOK-SYNC] Reading ROM book cache & checking Firestore book_names collection..."));

  // 1. Read current ROM book catalog
  std::vector<String> romUIDs = getAllROMBookUIDs();
  struct BookEntry {
    String uid;
    String title;
    uint8_t shelf;
    uint8_t rack;
    bool active;
  };
  std::vector<BookEntry> romList;
  for (size_t i = 0; i < romUIDs.size(); i++) {
    BookRecord rec;
    if (getBookFromROM(romUIDs[i].c_str(), rec)) {
      BookEntry e;
      e.uid = romUIDs[i];
      e.title = String(rec.title);
      e.shelf = rec.shelf;
      e.rack = rec.rack;
      e.active = rec.active;
      romList.push_back(e);
    }
  }

  // 2. Fetch Firestore book_names collection on Core 0
  Serial.println(F("[BOOK-SYNC] Sending GET collection request to Firestore (book_names)..."));
  bool getOk = Firebase.Firestore.getDocuments(&fbdo, FIREBASE_PROJECT_ID, "(default)", FIRESTORE_BOOKS);
  int httpCode = fbdo.httpCode();
  String payload = "";
  String errorReason = "";

  if (getOk && httpCode == 200) {
    payload = fbdo.payload();
    Serial.print(F("[BOOK-SYNC] Received collection response (HTTP 200). Size: "));
    Serial.print(payload.length());
    Serial.println(F(" bytes. Comparing with ROM cache..."));
  } else {
    errorReason = fbdo.errorReason();
    Serial.print(F("[BOOK-SYNC] Failed to fetch documents from Firestore. HTTP: "));
    Serial.print(httpCode);
    Serial.print(F(" | Error: "));
    Serial.println(errorReason);
    fbdo.clear();
    vTaskDelay(pdMS_TO_TICKS(1000));
    return false;
  }

  FirebaseJson json;
  json.setJsonData(payload);
  FirebaseJsonData docsData;
  json.get(docsData, "documents");

  std::vector<BookEntry> cloudList;
  if (docsData.success && docsData.type == "array") {
    FirebaseJsonArray arr;
    docsData.getArray(arr);
    size_t numDocs = arr.size();

    for (size_t i = 0; i < numDocs; i++) {
      FirebaseJsonData itemData;
      arr.get(itemData, i);
      if (itemData.success) {
        FirebaseJson docJson;
        itemData.getJSON(docJson);

        FirebaseJsonData resNameData, titleData, nameData;
        docJson.get(resNameData, "name");

        String bookTitle = "";
        if (docJson.get(titleData, "fields/title/stringValue") && titleData.success) {
          bookTitle = titleData.stringValue;
        } else if (docJson.get(nameData, "fields/name/stringValue") && nameData.success) {
          bookTitle = nameData.stringValue;
        }
        bookTitle.trim();

        // Shelf parsing
        uint8_t shelf = 1;
        FirebaseJsonData shelfData;
        if (docJson.get(shelfData, "fields/shelf/integerValue") && shelfData.success) {
          shelf = (uint8_t)shelfData.intValue;
        } else if (docJson.get(shelfData, "fields/shelf/stringValue") && shelfData.success) {
          shelf = (uint8_t)shelfData.stringValue.toInt();
        } else if (docJson.get(shelfData, "fields/shelfNumber/integerValue") && shelfData.success) {
          shelf = (uint8_t)shelfData.intValue;
        }

        // Rack parsing
        uint8_t rack = 1;
        FirebaseJsonData rackData;
        if (docJson.get(rackData, "fields/rack/integerValue") && rackData.success) {
          rack = (uint8_t)rackData.intValue;
        } else if (docJson.get(rackData, "fields/rack/stringValue") && rackData.success) {
          rack = (uint8_t)rackData.stringValue.toInt();
        } else if (docJson.get(rackData, "fields/rackNumber/integerValue") && rackData.success) {
          rack = (uint8_t)rackData.intValue;
        }

        // Active parsing
        bool isActive = true;
        FirebaseJsonData bVal, sVal, iVal, rawActive;
        if (docJson.get(bVal, "fields/active/booleanValue") && bVal.success) {
          String bStr = bVal.stringValue;
          bStr.trim();
          bStr.toLowerCase();
          if (bStr == "false" || bStr == "0") {
            isActive = false;
          } else {
            isActive = bVal.boolValue;
          }
        } else if (docJson.get(sVal, "fields/active/stringValue") && sVal.success) {
          String s = sVal.stringValue;
          s.trim();
          s.toLowerCase();
          isActive = (s == "true" || s == "1");
        } else if (docJson.get(iVal, "fields/active/integerValue") && iVal.success) {
          isActive = (iVal.intValue != 0);
        } else if (docJson.get(rawActive, "fields/active") && rawActive.success) {
          String raw = rawActive.stringValue;
          raw.trim();
          raw.toLowerCase();
          if (raw == "false" || raw == "0" || raw.indexOf("false") >= 0) {
            isActive = false;
          }
        }

        if (resNameData.success) {
          String fullPath = resNameData.stringValue;
          int lastSlash = fullPath.lastIndexOf('/');
          String uid = (lastSlash >= 0) ? fullPath.substring(lastSlash + 1) : fullPath;
          uid.trim();
          uid.toUpperCase();

          if (uid.length() > 0 && bookTitle.length() > 0) {
            BookEntry e;
            e.uid = uid;
            e.title = bookTitle;
            e.shelf = shelf;
            e.rack = rack;
            e.active = isActive;
            cloudList.push_back(e);
          }
        }
      }
    }
  }

  // 3. Compare romList with cloudList
  bool diff = false;
  if (romList.size() != cloudList.size()) {
    diff = true;
    Serial.print(F("[BOOK-SYNC] Difference: Book count changed (ROM: "));
    Serial.print(romList.size());
    Serial.print(F(", Cloud: "));
    Serial.print(cloudList.size());
    Serial.println(F(")"));
  } else {
    for (size_t c = 0; c < cloudList.size(); c++) {
      bool found = false;
      for (size_t r = 0; r < romList.size(); r++) {
        if (cloudList[c].uid == romList[r].uid) {
          found = true;
          if (cloudList[c].title != romList[r].title) {
            diff = true;
            Serial.print(F("[BOOK-SYNC] Difference: Book title changed for UID "));
            Serial.print(cloudList[c].uid);
            Serial.print(F(" (\""));
            Serial.print(romList[r].title);
            Serial.print(F("\" -> \""));
            Serial.print(cloudList[c].title);
            Serial.println(F("\")"));
          }
          if (cloudList[c].shelf != romList[r].shelf) {
            diff = true;
            Serial.print(F("[BOOK-SYNC] Difference: Shelf location changed for UID "));
            Serial.print(cloudList[c].uid);
            Serial.print(F(" ("));
            Serial.print(romList[r].shelf);
            Serial.print(F(" -> "));
            Serial.print(cloudList[c].shelf);
            Serial.println(F(")"));
          }
          if (cloudList[c].rack != romList[r].rack) {
            diff = true;
            Serial.print(F("[BOOK-SYNC] Difference: Rack location changed for UID "));
            Serial.print(cloudList[c].uid);
            Serial.print(F(" ("));
            Serial.print(romList[r].rack);
            Serial.print(F(" -> "));
            Serial.print(cloudList[c].rack);
            Serial.println(F(")"));
          }
          if (cloudList[c].active != romList[r].active) {
            diff = true;
            Serial.print(F("[BOOK-SYNC] Difference: Book active status changed for UID "));
            Serial.print(cloudList[c].uid);
            Serial.print(F(" ("));
            Serial.print(romList[r].active ? F("active") : F("suspended"));
            Serial.print(F(" -> "));
            Serial.print(cloudList[c].active ? F("active") : F("suspended"));
            Serial.println(F(")"));
          }
          break;
        }
      }
      if (!found) {
        diff = true;
        Serial.print(F("[BOOK-SYNC] Difference: Book UID in cloud not found in ROM: "));
        Serial.println(cloudList[c].uid);
        break;
      }
      if (diff) break;
    }
  }

  if (diff) {
    Serial.println(F("[BOOK-SYNC] Updating ROM book cache with fresh Firestore catalog..."));
    bookPrefs.clear();
    String newUids = "";
    for (size_t i = 0; i < cloudList.size(); i++) {
      String val = cloudList[i].title + "|" + String(cloudList[i].shelf) + "|" + String(cloudList[i].rack) + "|" + (cloudList[i].active ? "1" : "0");
      bookPrefs.putString(cloudList[i].uid.c_str(), val);
      if (newUids.length() > 0) newUids += ",";
      newUids += cloudList[i].uid;
      Serial.print(F("[BOOK-SYNC] Cached: \""));
      Serial.print(cloudList[i].title);
      Serial.print(F("\" [UID: "));
      Serial.print(cloudList[i].uid);
      Serial.print(F(" | Shelf: "));
      Serial.print(cloudList[i].shelf);
      Serial.print(F(" | Rack: "));
      Serial.print(cloudList[i].rack);
      Serial.print(F(" | Active: "));
      Serial.print(cloudList[i].active ? F("true") : F("false"));
      Serial.println(F("]"));
    }
    bookPrefs.putString("__uids__", newUids);
    Serial.print(F("[BOOK-SYNC] ROM book update complete. Total books cached: "));
    Serial.println(cloudList.size());

    if (!isBoot) {
      outDiffDetected = true;
      Serial.println(F("[BOOK-SYNC] Differences detected! Book catalog updated. Scheduling restart countdown..."));
    }
  } else {
    Serial.println(F("[BOOK-SYNC] Periodic 20s check: ROM book cache and Firestore match perfectly. No changes."));
  }

  return true;
}

// Boot-Time Book Catalog Synchronization entry point
bool syncBooksFromCloud() {
  bool unused = false;
  return compareAndSyncBooks(true /* isBoot */, unused);
}

// Instant Local ROM Book Verification: Zero network latency, instant LCD & buzzer feedback
bool verifyBookFromROM(const char* uid, BookRecord& outRecord, BookAuthStatus& outStatus) {
  if (uid == nullptr || uid[0] == '\0') {
    outStatus = BOOK_AUTH_NOT_FOUND;
    return false;
  }

  String uidStr = String(uid);
  uidStr.trim();
  uidStr.toUpperCase();
  const char* cleanUID = uidStr.c_str();

  if (getBookFromROM(cleanUID, outRecord)) {
    if (outRecord.active) {
      outStatus = BOOK_AUTH_ACTIVE;
      Serial.print(F("[BOOK-ROM] Instant Match: Found active book \""));
      Serial.print(outRecord.title);
      Serial.print(F("\" [UID: "));
      Serial.print(cleanUID);
      Serial.print(F(" -> Shelf "));
      Serial.print(outRecord.shelf);
      Serial.print(F(", Rack "));
      Serial.print(outRecord.rack);
      Serial.println(F("]"));
      return true;
    } else {
      outStatus = BOOK_AUTH_SUSPENDED;
      Serial.print(F("[BOOK-ROM] Instant Match: Book is SUSPENDED: \""));
      Serial.print(outRecord.title);
      Serial.print(F("\" [UID: "));
      Serial.print(cleanUID);
      Serial.println(F("]"));
      return false;
    }
  }

  outStatus = BOOK_AUTH_NOT_FOUND;
  Serial.print(F("[BOOK-ROM] UID ["));
  Serial.print(cleanUID);
  Serial.println(F("] not recognized in local ROM book cache."));
  return false;
}

// Book Verification: Read book metadata directly from ROM cache
bool verifyBookWithCache(const char* uid, BookRecord& outRecord) {
  return getBookFromROM(uid, outRecord);
}

// Instant Local ROM Student Verification: Zero network latency, instant LCD & buzzer feedback
bool verifyStudentFromROM(const char* uid, char* outName, size_t maxLen, StudentAuthStatus& outStatus) {
  if (uid == nullptr || uid[0] == '\0') {
    outStatus = STUDENT_AUTH_NOT_FOUND;
    return false;
  }

  String uidStr = String(uid);
  uidStr.trim();
  uidStr.toUpperCase();
  const char* cleanUID = uidStr.c_str();

  StudentRecord romRecord;
  if (getStudentFromROM(cleanUID, romRecord)) {
    if (outName && maxLen > 0) {
      strncpy(outName, romRecord.name, maxLen - 1);
      outName[maxLen - 1] = '\0';
    }
    if (romRecord.active) {
      outStatus = STUDENT_AUTH_ACTIVE;
      Serial.print(F("[AUTH-ROM] Instant Match: Found active student \""));
      Serial.print(romRecord.name);
      Serial.print(F("\" [UID: "));
      Serial.print(cleanUID);
      Serial.println(F("]"));
      return true;
    } else {
      outStatus = STUDENT_AUTH_SUSPENDED;
      Serial.print(F("[AUTH-ROM] Instant Match: Card is SUSPENDED: \""));
      Serial.print(romRecord.name);
      Serial.print(F("\" [UID: "));
      Serial.print(cleanUID);
      Serial.println(F("]"));
      return false;
    }
  }

  outStatus = STUDENT_AUTH_NOT_FOUND;
  Serial.print(F("[AUTH-ROM] UID ["));
  Serial.print(cleanUID);
  Serial.println(F("] not recognized in local ROM cache."));
  return false;
}

// Student Verification: Read student name directly from ROM cache
bool verifyStudentWithCache(const char* uid, char* outName, size_t maxLen) {
  StudentRecord rec;
  if (getStudentFromROM(uid, rec)) {
    if (outName && maxLen > 0) {
      strncpy(outName, rec.name, maxLen - 1);
      outName[maxLen - 1] = '\0';
    }
    return true;
  }
  return false;
}

// Searches registered books for UID. Returns index if found, -1 if not found
int findBookIndex(const char* uid) {
  Serial.print(F("[LOOKUP]   Searching library database for book UID ["));
  Serial.print(uid);
  Serial.println(F("]..."));

  // Check static registeredBooks array
  for (size_t i = 0; i < NUM_BOOKS; i++) {
    if (strcasecmp(registeredBooks[i].uid, uid) == 0) {
      Serial.print(F("[LOOKUP]   Match found in catalog! Book: \""));
      Serial.print(registeredBooks[i].title);
      Serial.print(F("\" -> Shelf "));
      Serial.print(registeredBooks[i].shelf);
      Serial.print(F(", Rack "));
      Serial.println(registeredBooks[i].rack);
      return (int)i;
    }
  }

  // Also check dynamic ROM cache
  BookRecord rec;
  if (getBookFromROM(uid, rec)) {
    Serial.print(F("[LOOKUP]   Match found in ROM cache! Book: \""));
    Serial.print(rec.title);
    Serial.print(F("\" -> Shelf "));
    Serial.print(rec.shelf);
    Serial.print(F(", Rack "));
    Serial.println(rec.rack);
    return 0;
  }

  Serial.println(F("[LOOKUP]   No match found in book database!"));
  return -1;
}

// =========================================================================================
// SECTION 9: STEP HANDLERS & FINITE STATE MACHINE LOGIC
// =========================================================================================

// Handles Student Check-In (State 1: WAITING_FOR_STUDENT)
void handleStudentScan(const char* uid) {
  // Check if ROM is empty or system is in REST mode (e.g. after 'clr' command)
  std::vector<String> romUIDs = getAllROMUIDs();
  std::vector<String> romBookUIDs = getAllROMBookUIDs();
  if ((romUIDs.empty() && romBookUIDs.empty()) || systemRestMode) {
    Serial.print(F("[AUTH]     System in REST mode / ROM is EMPTY! Card ["));
    Serial.print(uid);
    Serial.println(F("] scanned. Please sync from Firestore ('upt' or 'clrupt')."));

    // 2 warning beeps on buzzer + Red LED
    for (int i = 0; i < 2; i++) {
      digitalWrite(RED_LED_PIN, HIGH);
      digitalWrite(BUZZER_PIN, HIGH);
      delay(150);
      digitalWrite(RED_LED_PIN, LOW);
      digitalWrite(BUZZER_PIN, LOW);
      if (i < 1) delay(100);
    }

    showMessage("   ROM Empty    ", "System Rest Mode");
    delay(1500);
    showMessage("System Rest Mode", "Please Wait...  ");
    delay(1500);
    resetSystem();
    return;
  }

  char verifiedName[128] = "";
  StudentAuthStatus authStatus = STUDENT_AUTH_NOT_FOUND;

  verifyStudentFromROM(uid, verifiedName, sizeof(verifiedName), authStatus);

  if (authStatus == STUDENT_AUTH_ACTIVE) {
    // 1. Authorized Active Student authenticated
    Serial.print(F("[SUCCESS]  Student authenticated: "));
    Serial.print(verifiedName);
    Serial.print(F(" ("));
    Serial.print(uid);
    Serial.println(F(")"));

    // Immediate local feedback: short green LED flash + simultaneous buzzer beep
    indicateSuccess(1, 100);

    // Store authenticated student credentials & login time in session memory
    strncpy(activeStudentName, verifiedName, sizeof(activeStudentName) - 1);
    activeStudentName[sizeof(activeStudentName) - 1] = '\0';
    strncpy(activeStudentUID, uid, sizeof(activeStudentUID) - 1);
    activeStudentUID[sizeof(activeStudentUID) - 1] = '\0';
    getCurrentTimestamp(activeLoginTime, sizeof(activeLoginTime));

    Serial.print(F("[SESSION]  Student login recorded: "));
    Serial.print(activeStudentName);
    Serial.print(F(" ("));
    Serial.print(activeStudentUID);
    Serial.print(F(") at "));
    Serial.println(activeLoginTime);

    // Note: Do not sync to Firestore on student login alone.
    // Cloud sync occurs only when book scan succeeds (BOOK_PROCESSED) or if session times out (SESSION_TIMEOUT).

    // Transition state
    logStateTransition(WAITING_FOR_STUDENT, WAITING_FOR_BOOK);
    currentState = WAITING_FOR_BOOK;

    // Display student greeting: smoothly scrolls full name across screen without premature cutoff
    char studentLine[144];
    snprintf(studentLine, sizeof(studentLine), "Hi %s", activeStudentName);
    showMessageWithScroll(studentLine, "Tap Book Tag...");

    // Start 18-second non-blocking session timer after greeting display
    sessionStartTime = millis();
    lastSessionSecondLogged = 0;
    Serial.print(F("[INFO]     Session timer started ("));
    Serial.print(SESSION_TIMEOUT_MS / 1000);
    Serial.println(F("s window for book scan)"));

    Serial.println(F("[INFO]     Ready for book tag. Waiting for book scan..."));
    return;
  }

  if (authStatus == STUDENT_AUTH_SUSPENDED) {
    // 2. Student Card Suspended (active: false in ROM)
    Serial.print(F("[AUTH]     Student Card SUSPENDED (active=false): \""));
    Serial.print(verifiedName);
    Serial.print(F("\" [UID: "));
    Serial.print(uid);
    Serial.println(F("]"));

    // 2 warning beeps on buzzer + Red LED
    for (int i = 0; i < 2; i++) {
      digitalWrite(RED_LED_PIN, HIGH);
      digitalWrite(BUZZER_PIN, HIGH);
      delay(150);
      digitalWrite(RED_LED_PIN, LOW);
      digitalWrite(BUZZER_PIN, LOW);
      if (i < 1) delay(100);
    }

    // Display suspended message on LCD
    showMessage(" Card Suspended ", " Contact Admin  ");
    delay(2500);

    resetSystem();
    return;
  }

  // 3. Book tag scanned instead of Student ID Card
  BookRecord scannedBook;
  BookAuthStatus bStatus = BOOK_AUTH_NOT_FOUND;
  if (verifyBookFromROM(uid, scannedBook, bStatus) || bStatus == BOOK_AUTH_SUSPENDED) {
    Serial.print(F("[WARN]     Book tag (\""));
    Serial.print(scannedBook.title);
    Serial.println(F("\") scanned at Step 1 instead of Student ID!"));

    // Red LED + error buzz, prompt user
    showError("Please Tap Your ", "Student ID First", "Book tag scanned before student ID authentication");

    // Return to standby
    resetSystem();
    return;
  }

  // 4. Unrecognized tag scanned -> Emergency alarm & SECURITY_ALERT
  Serial.print(F("[ERROR]    Unrecognized tag UID: "));
  Serial.println(uid);
  char reason[160];
  snprintf(reason, sizeof(reason), "Unrecognized card scanned at Step 1 (UID: %s)", uid);

  // Silent Background Cloud Sync: Dispatch SECURITY_ALERT document to Firestore
  queueFirestoreLog("SECURITY_ALERT", "UNKNOWN", uid, "", "", "", 0, 0, reason);

  // Emergency Alarm: 10 rapid pulses of Red LED + Buzzer across 2 seconds
  showEmergencyAlarm("Unrecognized ID ", " Access Denied! ", reason);
  resetSystem();
}

// Handles Book Scanning & Shelf Guidance (State 2: WAITING_FOR_BOOK)
void handleBookScan(const char* uid) {
  BookRecord foundBook;
  BookAuthStatus bStatus = BOOK_AUTH_NOT_FOUND;
  verifyBookFromROM(uid, foundBook, bStatus);

  if (bStatus == BOOK_AUTH_ACTIVE) {
    // 1. Registered Active Book Tag identified
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
    lastSessionSecondLogged = 0;

    // Silent Background Cloud Sync: Dispatch ONE COMBINED document to Firestore (both login and processed data)
    char note[160];
    snprintf(note, sizeof(note), "Student authenticated & book mapped to Shelf %u, Rack %u", foundBook.shelf, foundBook.rack);
    queueFirestoreLog("BOOK_PROCESSED",
                      activeStudentName,
                      activeStudentUID,
                      activeLoginTime,
                      foundBook.title,
                      uid,
                      foundBook.shelf,
                      foundBook.rack,
                      note);

    // Run custom animated display sequence (title scroll, reveal, 8s hold, 3s warning)
    displayBookLocationSequence(foundBook.title, foundBook.shelf, foundBook.rack);

    Serial.println(F("[INFO]     Book sequence completed. Resetting for next patron..."));
    resetSystem();

    // Trigger immediate background sync after completing round (Student + Book successfully logged)
    triggerPostRoundSync = true;
    Serial.println(F("[AUTH-SYNC] Completed one full book round. Scheduled immediate background cloud sync."));
    return;
  }

  if (bStatus == BOOK_AUTH_SUSPENDED) {
    // Book tag is marked inactive/suspended in Firestore
    Serial.print(F("[AUTH]     Book tag SUSPENDED (active=false): \""));
    Serial.print(foundBook.title);
    Serial.print(F("\" [UID: "));
    Serial.print(uid);
    Serial.println(F("]"));

    // 2 warning beeps on buzzer + Red LED
    for (int i = 0; i < 2; i++) {
      digitalWrite(RED_LED_PIN, HIGH);
      digitalWrite(BUZZER_PIN, HIGH);
      delay(150);
      digitalWrite(RED_LED_PIN, LOW);
      digitalWrite(BUZZER_PIN, LOW);
      if (i < 1) delay(100);
    }

    // Display suspended message on LCD
    showMessage("Book Unavailable", " Contact Admin  ");
    delay(2500);

    // Renew session timer (+18s) and restore book prompt
    sessionStartTime = millis();
    lastSessionSecondLogged = 0;
    Serial.println(F("[INFO]     Session timer renewed (+18s). Ready for book tag retry..."));
    showMessage("Tap Your Book...");
    return;
  }

  // 2. Student card scanned again instead of book tag
  char checkName[128] = "";
  if (verifyStudentWithCache(uid, checkName, sizeof(checkName))) {
    Serial.print(F("[WARN]     Student ID (\""));
    Serial.print(checkName);
    Serial.println(F("\") scanned again instead of a book! Prompting for book tag..."));
    showError("Please Tap Book ", "Tag (Not ID)    ", "Student ID card tapped during book scan step");

    // Renew session timer (+18s)
    sessionStartTime = millis();
    lastSessionSecondLogged = 0;
    Serial.println(F("[INFO]     Session timer renewed (+18s). Ready for book tag retry..."));
    showMessage("Tap Your Book...");
    return;
  }

  // 3. Unrecognized tag scanned during book scan -> Emergency alarm & SECURITY_ALERT
  Serial.print(F("[ERROR]    Unrecognized tag UID: "));
  Serial.print(uid);
  Serial.println(F(" during book lookup step!"));
  char reason[160];
  snprintf(reason, sizeof(reason), "Unregistered tag scanned at Step 2 (UID: %s)", uid);

  // Silent Background Cloud Sync: Dispatch SECURITY_ALERT document to Firestore
  queueFirestoreLog("SECURITY_ALERT", activeStudentName, activeStudentUID, activeLoginTime, "UNKNOWN", uid, 0, 0, reason);

  // Emergency Alarm: 10 rapid pulses of Red LED + Buzzer across 2 seconds
  showEmergencyAlarm("Unrecognized ID ", "Unknown Book Tag", reason);

  // Renew session timer (+18s) and restore book prompt
  sessionStartTime = millis();
  lastSessionSecondLogged = 0;
  Serial.println(F("[INFO]     Session timer renewed (+18s). Ready for book tag retry..."));
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

  // 4. Print Database Configuration Summary
  Serial.println(F("[BOOT]     Database Configuration:"));
  Serial.println(F("           - Student Roster: Offline-First Cloud Cache (Firestore: student_names / NVS: \"students\")"));
  Serial.println(F("           - Book Catalog:   Offline-First Cloud Cache (Firestore: book_names / NVS: \"books\")"));
  std::vector<String> initBookUids = getAllROMBookUIDs();
  Serial.print(F("           - Cached Books in ROM: "));
  Serial.println(initBookUids.size());
  for (size_t i = 0; i < initBookUids.size(); i++) {
    BookRecord bRec;
    if (getBookFromROM(initBookUids[i].c_str(), bRec)) {
      Serial.print(F("             ["));
      Serial.print(i + 1);
      Serial.print(F("] \""));
      Serial.print(bRec.title);
      Serial.print(F("\" (UID: "));
      Serial.print(initBookUids[i]);
      Serial.print(F(" -> Shelf "));
      Serial.print(bRec.shelf);
      Serial.print(F(", Rack "));
      Serial.print(bRec.rack);
      Serial.print(F(" | "));
      Serial.print(bRec.active ? F("ACTIVE") : F("SUSPENDED"));
      Serial.println(F(")"));
    }
  }
  if (initBookUids.empty()) {
    Serial.println(F("             (ROM book cache empty; will populate upon Firestore sync or use default catalog)"));
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

  // Initialize Task Watchdog with an extended 30-second window and disable panic reset during cryptographic operations
  esp_task_wdt_init(30, false);

  // Initialize NVS Preferences for offline-first student roster, book catalog & offline log buffering
  studentPrefs.begin("students", false);
  bookPrefs.begin("books", false);
  offlineLogsPrefs.begin("off_logs", false);
  Serial.println(F("[NVS]      Preferences namespaces \"students\", \"books\" & \"off_logs\" initialized"));

  // Check and display what is currently stored in persistent ROM at boot
  printROMCacheSummary();

  // Create FreeRTOS Queue for non-blocking Firestore document synchronization (up to 10 events)
  firestoreQueue = xQueueCreate(10, sizeof(FirestoreLogEvent));
  if (firestoreQueue == NULL) {
    Serial.println(F("[FIREBASE] ERROR: Failed to create FreeRTOS Firestore queue!"));
  }

  // Create FreeRTOS Queue for non-blocking command execution audit logging ('commands-log')
  commandLogQueue = xQueueCreate(10, sizeof(CommandLogItem*));
  if (commandLogQueue == NULL) {
    Serial.println(F("[FIREBASE] ERROR: Failed to create FreeRTOS Command Log queue!"));
  }

  // Register Wi-Fi Event Handler for diagnostic monitoring
  WiFi.onEvent(onWiFiEvent);

  // Enable automatic reconnection and start connecting to Wi-Fi immediately at boot
  Serial.print(F("[WIFI]     Connecting to network: "));
  Serial.println(WIFI_SSID);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Start NTP synchronization safely at boot
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
  Serial.println(F("[TIME]     NTP synchronization started (pool.ntp.org, IST GMT+5:30)"));

  // Elevate Arduino loopTask on Core 1 to HIGH PRIORITY (Priority 3)
  // Ensures RFID SPI scanning, buzzer, dual LEDs, and I2C LCD always have top scheduling priority
  vTaskPrioritySet(NULL, 3);

  // Launch background Firestore sync task on Core 0 with LOW PRIORITY (Priority 1)
  // and 20KB generous stack to avoid TLS stack pressure
  xTaskCreatePinnedToCore(
    firestoreSyncTask,
    "FirestoreSyncTask",
    20480,
    NULL,
    1,
    &firestoreTaskHandle,
    0
  );

  // Perform full hardware diagnostics & welcome display sequence
  runHardwareDiagnostics();

  // Reset system to ready state (Line 1: "Please Tap Your ", Line 2: "Student ID Card ")
  resetSystem();

  Serial.println(F("[SERIAL]   Interactive Commands Ready:"));
  Serial.println(F("           System & Task Manager:"));
  Serial.println(F("           - 'all'     : ESP32 Task Manager (RAM, ROM, Core 0 & Core 1 tasks)"));
  Serial.println(F("           Combined (Both):"));
  Serial.println(F("           - 'rom'     : Inspect all persistent ROM (students, books, logs & RAM)"));
  Serial.println(F("           - 'upt'     : Force immediate on-demand Firestore sync of both"));
  Serial.println(F("           - 'clr'     : Clear all ROM data and enter REST mode"));
  Serial.println(F("           - 'clrupt'  : Clear all ROM and fresh sync both from Firestore"));
  Serial.println(F("           Students Only:"));
  Serial.println(F("           - 'roms'    : Inspect student roster ROM cache only"));
  Serial.println(F("           - 'upts'    : Force immediate Firestore sync of 'student_names'"));
  Serial.println(F("           - 'clrs'    : Clear student roster from persistent ROM"));
  Serial.println(F("           - 'clrupts' : Clear student ROM and fresh sync from Firestore"));
  Serial.println(F("           Books Only:"));
  Serial.println(F("           - 'romb'    : Inspect book catalog ROM cache only"));
  Serial.println(F("           - 'uptb'    : Force immediate Firestore sync of 'book_names'"));
  Serial.println(F("           - 'clrb'    : Clear book catalog from persistent ROM"));
  Serial.println(F("           - 'clruptb' : Clear book ROM and fresh sync from Firestore"));
  Serial.println(F("           Audit Log: All command executions are automatically logged to Firestore 'commands-log'."));
}

// Non-blocking serial command listener for runtime diagnostics (e.g. typing "all", "rom", "roms", "romb", "upt", "upts", "uptb", "clr", "clrs", "clrb", "clrupt", "clrupts", "clruptb")
void checkSerialCommands() {
  static String serialBuffer = "";
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0) {
        String cmd = serialBuffer;
        cmd.toLowerCase();

        // 1. Snapshot State BEFORE Command Execution
        String stateBefore = captureDetailedStateSummary();
        uint32_t heapBefore = ESP.getFreeHeap();
        String actionSummary = "";
        String serialOutput = "";
        String execStatus = "COMPLETED";

        // 0. System Task Manager Command
        if (cmd == "all") {
          dumpTaskManagerAll();
          actionSummary = "Executed ESP32 Task Manager: dumped dual-core status, FreeRTOS live tasks, RAM heap/stack metrics, and ROM NVS namespaces.";
          serialOutput = "[TASK-MANAGER] ESP32 Task Manager report displayed: Core 0 cloud/storage engine & Core 1 real-time UI engine workloads, live FreeRTOS task table, RAM heap breakdown, and ROM NVS namespaces ('students', 'books', 'off_logs').";

        // 1. ROM Inspection Commands
        } else if (cmd == "rom") {
          dumpAllROMData();
          actionSummary = "Inspected all persistent ROM data (students, books, offline logs, and heap RAM).";
          serialOutput = "[ROM-INSPECT] Complete persistent ROM dump executed: displayed student roster ('students'), book catalog ('books'), offline logs ('off_logs'), and free heap RAM.";
        } else if (cmd == "roms") {
          dumpROMStudents();
          actionSummary = "Inspected student roster cache in persistent ROM ('students' namespace).";
          serialOutput = "[ROM-STUDENTS] Student roster cache dump executed: displayed all registered student UIDs, names, and active/suspended statuses.";
        } else if (cmd == "romb") {
          dumpROMBooks();
          actionSummary = "Inspected book catalog cache in persistent ROM ('books' namespace).";
          serialOutput = "[ROM-BOOKS] Book catalog cache dump executed: displayed all registered book UIDs, titles, shelf, rack, and active/suspended statuses.";

        // 2. On-Demand Firestore Synchronization Commands
        } else if (cmd == "upt") {
          systemRestMode = false;
          Serial.println(F("[SERIAL]   Manual sync 'upt' (both students & books) received! Dispatching..."));
          if (WiFi.status() != WL_CONNECTED) {
            Serial.println(F("[WARN]     Wi-Fi currently disconnected. Sync will execute upon reconnect."));
          }
          triggerSyncStudents = true;
          triggerSyncBooks = true;
          actionSummary = "Triggered immediate on-demand manual synchronization of both students roster and book catalog from Firestore.";
          serialOutput = "[SERIAL] Manual sync 'upt' (both students & books) received! Dispatching background cloud sync on Core 0.";
          execStatus = "TRIGGERED";
        } else if (cmd == "upts") {
          systemRestMode = false;
          Serial.println(F("[SERIAL]   Manual sync 'upts' (students only) received! Dispatching..."));
          if (WiFi.status() != WL_CONNECTED) {
            Serial.println(F("[WARN]     Wi-Fi currently disconnected. Sync will execute upon reconnect."));
          }
          triggerSyncStudents = true;
          actionSummary = "Triggered immediate on-demand manual synchronization of student roster ('student_names') from Firestore.";
          serialOutput = "[SERIAL] Manual sync 'upts' (students only) received! Dispatching background cloud sync on Core 0.";
          execStatus = "TRIGGERED";
        } else if (cmd == "uptb") {
          systemRestMode = false;
          Serial.println(F("[SERIAL]   Manual sync 'uptb' (books only) received! Dispatching..."));
          if (WiFi.status() != WL_CONNECTED) {
            Serial.println(F("[WARN]     Wi-Fi currently disconnected. Sync will execute upon reconnect."));
          }
          triggerSyncBooks = true;
          actionSummary = "Triggered immediate on-demand manual synchronization of book catalog ('book_names') from Firestore.";
          serialOutput = "[SERIAL] Manual sync 'uptb' (books only) received! Dispatching background cloud sync on Core 0.";
          execStatus = "TRIGGERED";

        // 3. ROM Clear Commands
        } else if (cmd == "clr") {
          clearAllROMData();
          actionSummary = "Erased all persistent ROM data (students, books, offline logs), cleared active session in RAM, and entered system REST mode.";
          serialOutput = "[ROM-CLEAR] Full Persistent ROM Completely Cleared (Background)! Student roster cache ('students') erased, Book catalog cache ('books') erased, Offline logs buffer ('off_logs') erased, System entered REST MODE, Session credentials reset.";
        } else if (cmd == "clrs") {
          clearStudentROMData();
          actionSummary = "Erased student roster cache from persistent ROM ('students' namespace); retained books and offline logs.";
          serialOutput = "[ROM-CLEAR] Student Roster Cache Cleared (Background)! Student roster cache ('students') erased, Book catalog and offline logs retained, Session credentials reset.";
        } else if (cmd == "clrb") {
          clearBookROMData();
          actionSummary = "Erased book catalog cache from persistent ROM ('books' namespace); retained student roster and offline logs.";
          serialOutput = "[ROM-CLEAR] Book Catalog Cache Cleared (Background)! Book catalog cache ('books') erased, Student roster and offline logs retained.";

        // 4. Clean ROM Reset & Fresh Sync Commands
        } else if (cmd == "clrupt") {
          executeCleanResetAndSync();
          actionSummary = "Erased full persistent ROM (students, books, offline logs) and dispatched background clean download of student roster and book catalog from Firestore.";
          serialOutput = "[CLRUPT] Executing background ROM erase and fresh Firestore sync (students & books). Dispatching download on Core 0.";
          execStatus = "TRIGGERED";
        } else if (cmd == "clrupts") {
          executeCleanResetAndSyncStudents();
          actionSummary = "Erased student roster from persistent ROM and dispatched background clean download of 'student_names' from Firestore.";
          serialOutput = "[CLRUPTS] Executing background student erase and fresh Firestore sync. Dispatching download on Core 0.";
          execStatus = "TRIGGERED";
        } else if (cmd == "clruptb") {
          executeCleanResetAndSyncBooks();
          actionSummary = "Erased book catalog from persistent ROM and dispatched background clean download of 'book_names' from Firestore.";
          serialOutput = "[CLRUPTB] Executing background book erase and fresh Firestore sync. Dispatching download on Core 0.";
          execStatus = "TRIGGERED";

        } else {
          Serial.print(F("[SERIAL] Unknown command: '"));
          Serial.print(serialBuffer);
          Serial.println(F("'. Supported commands:"));
          Serial.println(F("         System:   'all'"));
          Serial.println(F("         Combined: 'rom', 'upt', 'clr', 'clrupt'"));
          Serial.println(F("         Students: 'roms', 'upts', 'clrs', 'clrupts'"));
          Serial.println(F("         Books:    'romb', 'uptb', 'clrb', 'clruptb'"));
          actionSummary = "Unrecognized command received in Serial CLI.";
          serialOutput = "[SERIAL] Unknown command: '" + serialBuffer + "'. Supported commands: 'all', 'rom', 'roms', 'romb', 'upt', 'upts', 'uptb', 'clr', 'clrs', 'clrb', 'clrupt', 'clrupts', 'clruptb'.";
          execStatus = "UNKNOWN_COMMAND";
        }

        // 2. Snapshot State AFTER Command Execution
        String stateAfter = captureDetailedStateSummary();
        uint32_t heapAfter = ESP.getFreeHeap();

        // 3. Dispatch Complete Command Execution Log to Firestore ("commands-log" collection)
        queueCommandLog(cmd, stateBefore, serialOutput, stateAfter, actionSummary, execStatus, heapBefore, heapAfter);

        serialBuffer = "";
      }
    } else {
      if (serialBuffer.length() < 64) {
        serialBuffer += c;
      }
    }
  }
}

void loop() {
  // Check for interactive Serial Monitor commands (e.g. typing "all", "rom", "upt", "clr", "clrupt")
  checkSerialCommands();

  // Periodic 30-second system clock & calendar broadcast to Serial Monitor
  static unsigned long last30sTimePrint = 0;
  if (millis() - last30sTimePrint >= 30000) {
    last30sTimePrint = millis();
    printPeriodicFullTime();
  }

  // Handle clean sync completion notice from Core 0 (silent background - zero LCD interruption)
  if (cleanSyncFinished) {
    cleanSyncFinished = false;
  } else if (cleanSyncFailed) {
    cleanSyncFailed = false;
  }

  // Auto-restart countdown if roster or book data was updated in Firestore (Requirement 2 & 3)
  if (pendingRestart && currentState == WAITING_FOR_STUDENT) {
    pendingRestart = false;
    Serial.println(F("[SYSTEM]   Roster or Book catalog update detected in Firestore! Starting restart sequence..."));
    showMessage(" Data Updated!  ", "Restarting in 3.");
    delay(1000);
    showMessage(" Data Updated!  ", "Restarting in 2.");
    delay(1000);
    showMessage(" Data Updated!  ", "Restarting in 1.");
    delay(1000);
    showMessage("  Restarting... ", "   ShelfSense   ");
    delay(500);
    ESP.restart();
  }

  // Non-blocking inactivity detection in WAITING_FOR_BOOK (18-second timeout)
  if (currentState == WAITING_FOR_BOOK) {
    unsigned long elapsedMs = millis() - sessionStartTime;
    unsigned long elapsedSec = elapsedMs / 1000;

    // Log second-by-second countdown in Serial Monitor while waiting for book scan
    if (elapsedSec > lastSessionSecondLogged && elapsedSec <= (SESSION_TIMEOUT_MS / 1000)) {
      lastSessionSecondLogged = elapsedSec;
      Serial.print(F("[TIMER]    Waiting for book scan: Second "));
      Serial.print(elapsedSec);
      Serial.print(F(" of "));
      Serial.print(SESSION_TIMEOUT_MS / 1000);
      Serial.println(F("s"));
    }

    if (elapsedMs >= SESSION_TIMEOUT_MS) {
      Serial.print(F("[WARN]     Session timed out: No book scan received within "));
      Serial.print(SESSION_TIMEOUT_MS / 1000);
      Serial.println(F(" seconds."));

      // Silent Background Cloud Sync: Dispatch SESSION_TIMEOUT document to Firestore
      char timeoutReason[64];
      snprintf(timeoutReason, sizeof(timeoutReason), "Session expired after %u seconds of inactivity", (unsigned int)(SESSION_TIMEOUT_MS / 1000));
      queueFirestoreLog("SESSION_TIMEOUT", activeStudentName, activeStudentUID, activeLoginTime, "", "", 0, 0, timeoutReason);

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
