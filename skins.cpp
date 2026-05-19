// keyboard emoji style eyes
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <driver/i2s.h>
#include <math.h>
#include <time.h>

// =====================================================================
// 1. PIN DEFINITIONS
// =====================================================================
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
#define TRIG_PIN 13
#define ECHO_PIN 12
#define PIR_PIN  33

// =====================================================================
// 2. NETWORK CONFIGURATION
// =====================================================================
const char* ssid             = "YOUR_WIFI_NAME";
const char* password         = "YOUR_WIFI_PASSWORD";
const char* url_ask          = "http://YOUR_LAPTOP_IP:5000/ask";
const char* url_cancel       = "http://YOUR_LAPTOP_IP:5000/cancel";
const char* url_health       = "http://YOUR_LAPTOP_IP:5000/health";

// =====================================================================
// 3. DISPLAY GEOMETRY
// =====================================================================
#define CX      120
#define CY      120
#define SCR     120
#define LEX      78
#define REX     162
#define EYE_Y    95
#define IRIS_R   26
#define PUPIL_R  12

// =====================================================================
// 4. COLOURS (RGB565)
// =====================================================================
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_CYAN    0x07FF
#define C_DCYAN   0x0410
#define C_ORANGE  0xFD20
#define C_AMBER   0xFB60
#define C_RED     0xF800
#define C_GREEN   0x07E0
#define C_PLUM    0xC81F
#define C_PLUM2   0xE01F
#define C_DGRAY   0x2104
#define C_MGRAY   0x4208
#define C_NAVY    0x000F

// =====================================================================
// 5. TIMING CONSTANTS
// =====================================================================
#define PIR_DEBOUNCE_MS           2000
#define SLEEP_TIMEOUT_MS         60000
#define STATIC_SETTLE_MS          8000
#define BASELINE_MIN_CM             15
#define BASELINE_MAX_CM             40
#define STATIC_TOLERANCE_CM          3
#define POSTURE_CLOSE_THRESHOLD_CM   8
#define POSTURE_BAD_MS           60000
#define POSTURE_RESET_MS        120000
#define CLOCK_TRIGGER_MS          3000
#define CLOCK_DISPLAY_MS          4000
#define LISTENING_TIMEOUT_MS     12000
#define THINKING_TIMEOUT_MS      15000
#define ERROR_HOLD_MS             3000
#define BLINK_INTERVAL_MS         5000
#define ULTRASONIC_DEBOUNCE_MS     800
#define NTP_SERVER              "pool.ntp.org"
#define TZ_OFFSET                19800

// =====================================================================
// 6. STATE MACHINE
// =====================================================================
enum BuddyState {
  SLEEP, BOOT, IDLE, LISTENING, THINKING, CLOCK_STATE, CONCERNED, ERROR_STATE
};

BuddyState    currentState    = BOOT;
BuddyState    previousState   = BOOT;
unsigned long stateEnteredAt  = 0;

// =====================================================================
// 7. GLOBALS
// =====================================================================
Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);

// Baseline / posture
float         baseline_cm       = 0;
bool          baselineLocked    = false;
float         staticSampleSum   = 0;
int           staticSampleCount = 0;
unsigned long staticStartMs     = 0;
float         lastStableReading = 0;
unsigned long postureStartMs    = 0;
bool          inBadPosture      = false;
int           concernedStage    = 0;

// Clock trigger
unsigned long deviationStartMs  = 0;
bool          inDeviation       = false;
BuddyState    stateBeforeClock  = IDLE;

// HTTP
struct HttpResult {
  volatile bool done;
  volatile bool success;
  char speech[256];
  char expression[16];
};
HttpResult   httpResult;
TaskHandle_t httpTaskHandle = NULL;
char         pendingQuery[512];

// Serial
String serialInputBuffer = "";
bool   serialQueryReady  = false;

// Animation
unsigned long lastBlinkMs  = 0;
unsigned long lastPirMs    = 0;
unsigned long lastRingMs   = 0;
unsigned long lastEyeMs    = 0;
unsigned long plumPhaseMs  = 0;
int           lastPupilX   = 0;
int           lastPupilY   = 0;

// =====================================================================
// 8. FORWARD DECLARATIONS
// =====================================================================
int  getDistance();
void connectToWiFi();
void syncNTP();
void enterState(BuddyState s);
void handleSerialInput();
void handleBaseline(int dist, unsigned long now);
void handlePosture(int dist, unsigned long now);
void handleClockTrigger(int dist, unsigned long now);
void drawHalo(uint16_t col, float speed);
void clearHalo();
void drawEyePair(uint16_t irisCol, int ox, int oy);
void updatePupil(uint16_t irisCol, int ox0, int oy0, int ox1, int oy1);
void drawEyebrow(int cx, int topY, bool angry, uint16_t col);
void drawIdleFace();
void drawListeningFace();
void drawThinkingFace();
void drawConcernedFace(int stage);
void drawErrorFace();
void drawClockFace();
void drawBootAnimation();
void drawSleepFace();
void httpTask(void* p);
void fireCancel();

// =====================================================================
// 9. SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PIR_PIN,  INPUT);

  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(C_BLACK);

  enterState(BOOT);
  drawBootAnimation();

  connectToWiFi();
  syncNTP();

  Serial.println("================================================");
  Serial.println(" DESK BUDDY // READY");
  Serial.println(" Type 'wake' then your question");
  Serial.println(" Type 'cancel' to abort");
  Serial.println("================================================");

  enterState(SLEEP);
}

// =====================================================================
// 10. LOOP
// =====================================================================
void loop() {
  int  dist      = getDistance();
  bool pirActive = digitalRead(PIR_PIN) == HIGH;
  unsigned long now = millis();

  if (pirActive) lastPirMs = now;

  handleSerialInput();

  if (currentState == IDLE && !inBadPosture)
    handleBaseline(dist, now);

  switch (currentState) {

    case SLEEP:
      if (pirActive && (now - lastPirMs > PIR_DEBOUNCE_MS)) {
        Serial.println("[PIR] Presence — waking.");
        enterState(IDLE);
      }
      break;

    case IDLE: {
      if (now - lastPirMs > SLEEP_TIMEOUT_MS) {
        inBadPosture   = false;
        baselineLocked = false;
        enterState(SLEEP);
        break;
      }

      if (baselineLocked) {
        handlePosture(dist, now);
        if (currentState != IDLE) break;
        handleClockTrigger(dist, now);
        if (currentState != IDLE) break;
      }

      if (dist > 0 && dist < 5 &&
          now - stateEnteredAt > ULTRASONIC_DEBOUNCE_MS) {
        Serial.println("[Wave] Wake gesture.");
        enterState(LISTENING);
        break;
      }

      // Dynamic pupil drift (Clean - No Halo)
      if (now - lastEyeMs > 60) {
        int newPx = (int)(sinf(now / 2400.0f) * 7.0f);
        int newPy = (int)(cosf(now / 3300.0f) * 4.0f);
        if (abs(newPx - lastPupilX) > 0 || abs(newPy - lastPupilY) > 0) {
          updatePupil(C_CYAN, lastPupilX, lastPupilY, newPx, newPy);
          lastPupilX = newPx;
          lastPupilY = newPy;
        }
        lastEyeMs = now;
      }

      // Blink
      if (now - lastBlinkMs > BLINK_INTERVAL_MS) {
        tft.fillCircle(LEX, EYE_Y, IRIS_R + 1, C_BLACK);
        tft.fillCircle(REX, EYE_Y, IRIS_R + 1, C_BLACK);
        delay(110);
        drawEyePair(C_CYAN, 0, 0);
        lastBlinkMs = now;
        lastPupilX  = 0;
        lastPupilY  = 0;
      }
      break;
    }

    case LISTENING:
      if (dist > 0 && dist < 5 &&
          now - stateEnteredAt > ULTRASONIC_DEBOUNCE_MS) {
        fireCancel();
        enterState(IDLE);
        break;
      }
      if (now - stateEnteredAt > LISTENING_TIMEOUT_MS) {
        Serial.println("[Timeout] Listening expired.");
        fireCancel();
        enterState(IDLE);
        break;
      }
      
      // Pulsating Halo specifically for Listening State
      if (now - lastRingMs > 40) {
        clearHalo();
        drawHalo(C_PLUM, 2.5f);
        lastRingMs = now;
      }
      
      if (serialQueryReady) {
        serialQueryReady = false;
        enterState(THINKING);
      }
      break;

    case THINKING:
      if (dist > 0 && dist < 5 &&
          now - stateEnteredAt > ULTRASONIC_DEBOUNCE_MS) {
        fireCancel();
        if (httpTaskHandle) { vTaskDelete(httpTaskHandle); httpTaskHandle = NULL; }
        enterState(IDLE);
        break;
      }
      if (now - stateEnteredAt > THINKING_TIMEOUT_MS) {
        fireCancel();
        if (httpTaskHandle) { vTaskDelete(httpTaskHandle); httpTaskHandle = NULL; }
        enterState(ERROR_STATE);
        break;
      }
      if (httpResult.done) {
        httpTaskHandle = NULL;
        if (httpResult.success) {
          Serial.println("+---------------------------------+");
          Serial.printf ("| Buddy: %s\n", httpResult.speech);
          Serial.println("+---------------------------------+");
          drawIdleFace();
          enterState(LISTENING);
        } else {
          enterState(ERROR_STATE);
        }
      }
      break;

    case CLOCK_STATE:
      drawClockFace();
      if (now - stateEnteredAt > CLOCK_DISPLAY_MS)
        enterState(stateBeforeClock);
      break;

    case CONCERNED:
      if (now - plumPhaseMs > 80) {
        plumPhaseMs = now;
        drawConcernedFace(concernedStage);
      }
      if (inBadPosture) {
        unsigned long elapsed = now - postureStartMs;
        int newStage = 1;
        if      (elapsed > POSTURE_RESET_MS)   newStage = 3;
        else if (elapsed > POSTURE_BAD_MS * 2) newStage = 2;
        if (newStage != concernedStage) {
          concernedStage = newStage;
          Serial.printf("[Posture] Stage %d\n", concernedStage);
        }
        if (elapsed > POSTURE_RESET_MS) {
          Serial.println("[Posture] Reset — re-calibrating.");
          inBadPosture      = false;
          concernedStage    = 0;
          baselineLocked    = false;
          staticSampleCount = 0;
          enterState(IDLE);
        }
      } else {
        concernedStage = 0;
        enterState(IDLE);
      }
      handlePosture(dist, now);
      break;

    case ERROR_STATE:
      if (now - stateEnteredAt > ERROR_HOLD_MS)
        enterState(IDLE);
      break;

    case BOOT:
      break;
  }
}

// =====================================================================
// 11. BASELINE CALIBRATION
// =====================================================================
void handleBaseline(int dist, unsigned long now) {
  if (dist < BASELINE_MIN_CM || dist > BASELINE_MAX_CM) {
    staticSampleCount = 0;
    staticStartMs     = 0;
    return;
  }
  if (staticSampleCount == 0) {
    staticSampleSum   = dist;
    staticSampleCount = 1;
    staticStartMs     = now;
    lastStableReading = dist;
    return;
  }
  if (abs(dist - (int)lastStableReading) <= STATIC_TOLERANCE_CM) {
    staticSampleSum  += dist;
    staticSampleCount++;
    lastStableReading = dist;
  } else {
    staticSampleSum   = dist;
    staticSampleCount = 1;
    staticStartMs     = now;
    lastStableReading = dist;
    return;
  }
  if ((now - staticStartMs) >= STATIC_SETTLE_MS && staticSampleCount > 5) {
    float newBaseline = staticSampleSum / staticSampleCount;
    if (!baselineLocked || fabsf(newBaseline - baseline_cm) > 4.0f) {
      baseline_cm    = newBaseline;
      baselineLocked = true;
      Serial.printf("[Baseline] Locked at %.1f cm\n", baseline_cm);
      staticSampleSum   = 0;
      staticSampleCount = 0;
    }
  }
}

// =====================================================================
// 12. POSTURE DETECTION
// =====================================================================
void handlePosture(int dist, unsigned long now) {
  if (!baselineLocked) return;
  float deviation = baseline_cm - (float)dist;
  if (deviation > POSTURE_CLOSE_THRESHOLD_CM) {
    if (!inBadPosture) {
      inBadPosture   = true;
      postureStartMs = now;
      Serial.printf("[Posture] Bad detected (%.1fcm closer)\n", deviation);
    }
    if ((now - postureStartMs) >= POSTURE_BAD_MS && currentState != CONCERNED) {
      concernedStage = 1;
      enterState(CONCERNED);
    }
  } else {
    if (inBadPosture) {
      Serial.println("[Posture] Corrected.");
      inBadPosture = false;
      if (currentState == CONCERNED) enterState(IDLE);
    }
  }
}

// =====================================================================
// 13. CLOCK TRIGGER
// =====================================================================
void handleClockTrigger(int dist, unsigned long now) {
  if (!baselineLocked) return;
  float deviation  = baseline_cm - (float)dist;
  bool  inClockZone = (deviation > 0 &&
                       deviation <= POSTURE_CLOSE_THRESHOLD_CM &&
                       dist > 5);
  if (inClockZone) {
    if (!inDeviation) {
      inDeviation      = true;
      deviationStartMs = now;
    } else if (now - deviationStartMs >= CLOCK_TRIGGER_MS) {
      inDeviation      = false;
      deviationStartMs = 0;
      stateBeforeClock = IDLE;
      Serial.println("[Clock] Triggered.");
      enterState(CLOCK_STATE);
    }
  } else {
    inDeviation      = false;
    deviationStartMs = 0;
  }
}

// =====================================================================
// 14. SERIAL INPUT
// =====================================================================
void handleSerialInput() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialInputBuffer.length() > 0) {
        serialInputBuffer.trim();
        if (serialInputBuffer.equalsIgnoreCase("wake")) {
          if (currentState == IDLE || currentState == SLEEP) {
            lastPirMs = millis();
            enterState(LISTENING);
          }
        }
        else if (serialInputBuffer.equalsIgnoreCase("cancel")) {
          if (currentState == THINKING) {
            fireCancel();
            if (httpTaskHandle) { vTaskDelete(httpTaskHandle); httpTaskHandle = NULL; }
          }
          if (currentState != IDLE && currentState != SLEEP)
            enterState(IDLE);
        }
        else if (currentState == LISTENING) {
          strncpy(pendingQuery, serialInputBuffer.c_str(), sizeof(pendingQuery));
          Serial.printf("[Serial] Query: %s\n", pendingQuery);
          serialQueryReady = true;
        }
        else {
          Serial.println("[Serial] Type 'wake' first.");
        }
        serialInputBuffer = "";
      }
    } else {
      serialInputBuffer += c;
    }
  }
}

// =====================================================================
// 15. STATE TRANSITION
// =====================================================================
void enterState(BuddyState next) {
  previousState  = currentState;
  currentState   = next;
  stateEnteredAt = millis();
  Serial.printf("[FSM] %d -> %d\n", previousState, next);

  switch (next) {
    case SLEEP:
      drawSleepFace();
      break;
    case IDLE:
      tft.fillScreen(C_BLACK);
      drawIdleFace();
      lastPupilX = 0;
      lastPupilY = 0;
      Serial.println("[Hint] 'wake' or wave (<5cm) to ask.");
      break;
    case LISTENING:
      tft.fillScreen(C_BLACK);
      drawListeningFace();
      Serial.println("[Listening] Type your question:");
      break;
    case THINKING:
      tft.fillScreen(C_BLACK);
      drawThinkingFace();
      httpResult.done    = false;
      httpResult.success = false;
      xTaskCreatePinnedToCore(httpTask, "http", 8192, NULL, 1, &httpTaskHandle, 0);
      break;
    case CLOCK_STATE:
      tft.fillScreen(C_BLACK);
      drawClockFace();
      break;
    case CONCERNED:
      tft.fillScreen(C_BLACK);
      plumPhaseMs = millis();
      drawConcernedFace(concernedStage);
      break;
    case ERROR_STATE:
      drawErrorFace();
      break;
    case BOOT:
      break;
  }
}

// =====================================================================
// 16. DRAW HELPERS
// =====================================================================

void drawHalo(uint16_t col, float speed) {
  float phase = millis() / (800.0f / speed);

  uint8_t rFull = (col >> 11) & 0x1F;
  uint8_t gFull = (col >> 5)  & 0x3F;
  uint8_t bFull =  col        & 0x1F;

  const int centres[4] = { 45, 135, 225, 315 };
  const int arcSpan    = 48;

  for (int c = 0; c < 4; c++) {
    float phOff     = c * (PI / 2.0f);
    float brightness = 0.35f + 0.65f * sinf(phase + phOff);

    uint8_t  r   = (uint8_t)(rFull * brightness);
    uint8_t  g   = (uint8_t)(gFull * brightness);
    uint8_t  b   = (uint8_t)(bFull * brightness);
    uint16_t seg = tft.color565(r << 3, g << 2, b << 3);

    for (int a = centres[c] - arcSpan / 2;
             a <= centres[c] + arcSpan / 2; a += 2) {
      float rad = a * PI / 180.0f;
      for (int off = 4; off <= 6; off++) {
        int x = CX + (int)((SCR - off) * cosf(rad));
        int y = CY + (int)((SCR - off) * sinf(rad));
        tft.drawPixel(x, y, seg);
      }
    }
  }
}

void clearHalo() {
  const int centres[4] = { 45, 135, 225, 315 };
  const int arcSpan    = 52;
  for (int c = 0; c < 4; c++) {
    for (int a = centres[c] - arcSpan / 2;
             a <= centres[c] + arcSpan / 2; a += 2) {
      float rad = a * PI / 180.0f;
      for (int off = 3; off <= 7; off++) {
        int x = CX + (int)((SCR - off) * cosf(rad));
        int y = CY + (int)((SCR - off) * sinf(rad));
        tft.drawPixel(x, y, C_BLACK);
      }
    }
  }
}

void drawSleepFace() {
  tft.fillScreen(C_BLACK);
  // V V shaped sleeping eyes with slight thickness
  for(int i = 0; i < 2; i++) {
    tft.drawLine(LEX - 12, EYE_Y - 8 + i, LEX, EYE_Y + 6 + i, C_DCYAN);
    tft.drawLine(LEX, EYE_Y + 6 + i, LEX + 12, EYE_Y - 8 + i, C_DCYAN);
    tft.drawLine(REX - 12, EYE_Y - 8 + i, REX, EYE_Y + 6 + i, C_DCYAN);
    tft.drawLine(REX, EYE_Y + 6 + i, REX + 12, EYE_Y - 8 + i, C_DCYAN);
  }
  
  tft.setTextColor(C_DCYAN); 
  tft.setTextSize(2);
  tft.setCursor(REX + 15, EYE_Y - 30);
  tft.print("Zzz");
}

void drawEyePair(uint16_t irisCol, int ox, int oy) {
  const int maxOff = IRIS_R - PUPIL_R - 3;
  int cx = constrain(ox, -maxOff, maxOff);
  int cy = constrain(oy, -maxOff, maxOff);
  for (int ex : { LEX, REX }) {
    tft.fillCircle(ex,        EYE_Y,        IRIS_R,  irisCol);
    tft.fillCircle(ex + cx,   EYE_Y + cy,   PUPIL_R, C_BLACK);
    tft.fillCircle(ex + cx - 5, EYE_Y + cy - 5, 3,  C_WHITE); // Main highlight
    tft.fillCircle(ex + cx + 4, EYE_Y + cy + 4, 1,  C_WHITE); // Secondary cute glare
  }
}

void updatePupil(uint16_t irisCol, int ox0, int oy0, int ox1, int oy1) {
  const int maxOff = IRIS_R - PUPIL_R - 3;
  int x0 = constrain(ox0, -maxOff, maxOff);
  int y0 = constrain(oy0, -maxOff, maxOff);
  int x1 = constrain(ox1, -maxOff, maxOff);
  int y1 = constrain(oy1, -maxOff, maxOff);
  for (int ex : { LEX, REX }) {
    tft.fillCircle(ex + x0,       EYE_Y + y0,     PUPIL_R + 1, irisCol);
    tft.fillCircle(ex + x1,       EYE_Y + y1,     PUPIL_R,     C_BLACK);
    tft.fillCircle(ex + x1 - 5,   EYE_Y + y1 - 5, 3,           C_WHITE);
    tft.fillCircle(ex + x1 + 4,   EYE_Y + y1 + 4, 1,           C_WHITE);
  }
}

void drawEyebrow(int cx, int topY, bool angry, uint16_t col) {
  int y    = topY - IRIS_R - 10;
  int sign = (cx < CX) ? 1 : -1;
  for (int i = 0; i < 3; i++) {
    if (angry)
      tft.drawLine(cx - 16, y + sign * 5 + i,
                   cx + 16, y - sign * 5 + i, col);
    else
      tft.drawLine(cx - 16, y + i, cx + 16, y + i, col);
  }
}

void drawIdleFace() {
  drawEyePair(C_CYAN, 0, 0); // Clean display, just the eyes
}

void drawListeningFace() {
  for (int ex : { LEX, REX }) {
    // Pink blush cheeks below eyes
    tft.fillCircle(ex, EYE_Y + 28, 10, C_PLUM);
    // Upward crescent eyes (Happy ^ ^)
    tft.fillCircle(ex, EYE_Y, IRIS_R, C_CYAN);
    tft.fillCircle(ex, EYE_Y + 12, IRIS_R, C_BLACK);
  }
}

void drawThinkingFace() {
  // Cute "looking up and to the side" expression
  drawEyePair(C_CYAN, 12, -12); 
}

void drawConcernedFace(int stage) {
  static float phase = 0.0f;
  phase += 0.18f;
  if (phase > TWO_PI) phase -= TWO_PI;

  int bob = (int)(sinf(phase) * 4.0f);
  int currentPupilR = (stage >= 2) ? 6 : PUPIL_R; // "Whale eyes" for firm warning

  tft.fillScreen(C_BLACK);

  for (int ex : { LEX, REX }) {
    tft.fillCircle(ex,       EYE_Y + bob,     IRIS_R,        C_WHITE);
    tft.fillCircle(ex,       EYE_Y + bob,     currentPupilR, C_BLACK);
    tft.fillCircle(ex - 5,   EYE_Y + bob - 5, (stage >= 2) ? 2 : 3, C_MGRAY);
  }

  // Stage 1: Sweat-drop
  if (stage == 1) {
    tft.fillTriangle(REX + 35, EYE_Y - 10, REX + 30, EYE_Y + 5, REX + 40, EYE_Y + 5, C_CYAN);
    tft.fillCircle(REX + 35, EYE_Y + 5, 5, C_CYAN);
  }

  // Stage 2 & 3: Angry Brows
  if (stage >= 2) {
    drawEyebrow(LEX, EYE_Y + bob, true, C_WHITE);
    drawEyebrow(REX, EYE_Y + bob, true, C_WHITE);
  }

  // Stage 3: The Pout (Flat bottom cut-off)
  if (stage == 3) {
    tft.fillRect(LEX - IRIS_R, EYE_Y + bob + 8, IRIS_R * 2, IRIS_R, C_BLACK);
    tft.fillRect(REX - IRIS_R, EYE_Y + bob + 8, IRIS_R * 2, IRIS_R, C_BLACK);
  }

  tft.setTextColor(C_MGRAY); tft.setTextSize(1);
  tft.setCursor(CX - 28, 210);
  if      (stage == 1) tft.print("sit up a bit");
  else if (stage == 2) tft.print("posture alert");
  else if (stage == 3) tft.print("!! fix posture !!");

  drawHalo(C_WHITE, 1.2f);
}

void drawErrorFace() {
  tft.fillScreen(C_BLACK);
  
  // Cute > < dizzy eyes using thick lines
  for(int i = 0; i < 3; i++) {
    // Left >
    tft.drawLine(LEX - 15, EYE_Y - 15 + i, LEX + 5,  EYE_Y + i,      C_RED);
    tft.drawLine(LEX + 5,  EYE_Y + i,      LEX - 15, EYE_Y + 15 + i, C_RED);
    // Right <
    tft.drawLine(REX + 15, EYE_Y - 15 + i, REX - 5,  EYE_Y + i,      C_RED);
    tft.drawLine(REX - 5,  EYE_Y + i,      REX + 15, EYE_Y + 15 + i, C_RED);
  }
  
  // Squiggly dizzy mouth
  tft.drawLine(CX - 12, EYE_Y + 30, CX - 6,  EYE_Y + 24, C_RED);
  tft.drawLine(CX - 6,  EYE_Y + 24, CX + 6,  EYE_Y + 30, C_RED);
  tft.drawLine(CX + 6,  EYE_Y + 30, CX + 12, EYE_Y + 24, C_RED);
}

void drawClockFace() {
  tft.fillScreen(C_BLACK);
  struct tm ti;
  if (getLocalTime(&ti)) {
    int sec = ti.tm_sec;

    for (int a = 0; a < 360; a += 2) {
      float rad = (a - 90) * PI / 180.0f;
      int   x   = CX + (int)((SCR - 6) * cosf(rad));
      int   y   = CY + (int)((SCR - 6) * sinf(rad));
      tft.drawPixel(x, y, C_DCYAN);
    }
    int sweepDeg = sec * 6;
    for (int a = 0; a <= sweepDeg; a += 2) {
      float rad = (a - 90) * PI / 180.0f;
      int   x   = CX + (int)((SCR - 6) * cosf(rad));
      int   y   = CY + (int)((SCR - 6) * sinf(rad));
      tft.drawPixel(x, y, C_CYAN);
    }
    for (int h = 0; h < 12; h++) {
      float rad = (h * 30 - 90) * PI / 180.0f;
      int   x1  = CX + (int)((SCR - 12) * cosf(rad));
      int   y1  = CY + (int)((SCR - 12) * sinf(rad));
      int   x2  = CX + (int)((SCR - 20) * cosf(rad));
      int   y2  = CY + (int)((SCR - 20) * sinf(rad));
      tft.drawLine(x1, y1, x2, y2, C_DCYAN);
    }
    char timeBuf[6];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", ti.tm_hour, ti.tm_min);
    tft.setTextColor(C_CYAN); tft.setTextSize(3);
    tft.setCursor(CX - 42, CY - 14);
    tft.print(timeBuf);

    char secBuf[3];
    snprintf(secBuf, sizeof(secBuf), "%02d", sec);
    tft.setTextColor(C_DCYAN); tft.setTextSize(2);
    tft.setCursor(CX - 14, CY + 22);
    tft.print(secBuf);
  } else {
    tft.setTextColor(C_DCYAN); tft.setTextSize(2);
    tft.setCursor(CX - 28, CY - 8);
    tft.print("--:--");
  }
}

void drawBootAnimation() {
  tft.fillScreen(C_BLACK);

  for (int r = SCR; r >= 20; r -= 4) {
    tft.drawCircle(CX, CY, r, C_DCYAN);
    delay(18);
  }
  delay(100);

  for (int i = IRIS_R; i >= 0; i -= 3) {
    tft.fillCircle(LEX, EYE_Y, i + 3, C_BLACK);
    tft.fillCircle(LEX, EYE_Y, i,     C_CYAN);
    delay(12);
  }
  for (int i = IRIS_R; i >= 0; i -= 3) {
    tft.fillCircle(REX, EYE_Y, i + 3, C_BLACK);
    tft.fillCircle(REX, EYE_Y, i,     C_CYAN);
    delay(12);
  }
  delay(100);

  struct {
    const char* label;
    int y;
  } checks[] = {
    { "ULTRASONIC", 165 },
    { "PIR",        180 },
    { "DISPLAY",    195 },
    { "WIFI",       210 }
  };

  for (auto& ch : checks) {
    tft.setTextColor(C_DCYAN); tft.setTextSize(1);
    tft.setCursor(CX - 36, ch.y);
    tft.print(ch.label);
    delay(280);
    tft.fillRect(CX - 38, ch.y - 2, 90, 12, C_BLACK);
    tft.setTextColor(C_GREEN);
    tft.setCursor(CX - 36, ch.y);
    tft.print(ch.label);
    tft.print(" OK");
    delay(300);
  }

  Serial.println("[Boot] Complete.");
  tft.fillScreen(C_DCYAN);
  delay(80);
  tft.fillScreen(C_BLACK);
  delay(120);
}

// =====================================================================
// 17. HTTP TASK
// =====================================================================
void httpTask(void* param) {
  HTTPClient http;
  http.begin(url_ask);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(12000);

  JsonDocument out;
  out["query"] = pendingQuery;
  String body;
  serializeJson(out, body);

  int code = http.POST(body);

  if (code == 200) {
    String resp = http.getString();
    JsonDocument in;
    if (!deserializeJson(in, resp)) {
      strncpy(httpResult.speech,     in["speech"]     | "",     sizeof(httpResult.speech));
      strncpy(httpResult.expression, in["expression"] | "IDLE", sizeof(httpResult.expression));
      httpResult.success = true;
    } else {
      Serial.println("[HTTP] JSON parse fail.");
      httpResult.success = false;
    }
  } else {
    Serial.printf("[HTTP] Non-200: %d\n", code);
    httpResult.success = false;
  }

  http.end();
  httpResult.done = true;
  vTaskDelete(NULL);
}

// =====================================================================
// 18. HELPERS
// =====================================================================
int getDistance() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long d = pulseIn(ECHO_PIN, HIGH, 30000);
  return (d == 0) ? 999 : (int)(d / 58);
}

void connectToWiFi() {
  tft.setTextColor(C_DCYAN); tft.setTextSize(1);
  tft.setCursor(70, CY - 5);
  tft.print("LINKING...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  tft.fillRect(0, CY - 10, 240, 20, C_BLACK);
  Serial.println("\n[WiFi] Connected.");
}

void syncNTP() {
  configTime(TZ_OFFSET, 0, NTP_SERVER);
  Serial.println("[NTP] Syncing...");
  struct tm ti;
  if (getLocalTime(&ti, 5000))
    Serial.printf("[NTP] %02d:%02d:%02d\n", ti.tm_hour, ti.tm_min, ti.tm_sec);
  else
    Serial.println("[NTP] Sync failed — clock shows --:--");
}

void fireCancel() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(url_cancel);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);
  http.POST("{}");
  http.end();
  Serial.println("[Cancel] Sent.");
}