#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <driver/i2s.h>

// =====================================================================
// 1. PIN DEFINITIONS
// =====================================================================
#define TFT_CS    5
#define TFT_DC    2
#define TFT_RST   4

#define TRIG_PIN  13
#define ECHO_PIN  12

#define PIR_PIN   33   // PIR OUT pin — HIGH when presence detected

#define I2S_WS    15
#define I2S_SCK   14
#define I2S_SD    32

// =====================================================================
// 2. NETWORK CONFIGURATION
// =====================================================================
const char* ssid             = "YOUR_WIFI_NAME";
const char* password         = "YOUR_WIFI_PASSWORD";
const char* url_ask          = "http://YOUR_LAPTOP_IP:5000/ask";
const char* url_cancel       = "http://YOUR_LAPTOP_IP:5000/cancel";
const char* url_health       = "http://YOUR_LAPTOP_IP:5000/health";

// =====================================================================
// 3. TIMING CONSTANTS  (all millis-based, zero blocking delays in loop)
// =====================================================================
#define LISTENING_TIMEOUT_MS     10000   // cancel if no audio captured in 10s
#define THINKING_TIMEOUT_MS      15000   // cancel if server doesn't respond in 15s
#define SPEAKING_DURATION_MS      4000   // simulated TTS playback duration
#define ERROR_HOLD_MS             3000   // how long to show error face before idle
#define BLINK_INTERVAL_MS         4000   // idle eye blink cadence
#define ULTRASONIC_DEBOUNCE_MS     800   // min gap between two valid wave gestures
#define PIR_DEBOUNCE_MS           2000   // ignore PIR chatter under 2s
#define SLEEP_TIMEOUT_MS         60000   // go to sleep if no PIR for 60s

// =====================================================================
// 4. STATE MACHINE
// =====================================================================
enum BuddyState {
  SLEEP,          // PIR sees nobody — display off, low power
  IDLE,           // Resting, blinking, ergonomics active
  LISTENING,      // Hand wave detected — capturing audio
  THINKING,       // Audio sent — waiting for server response
  SPEAKING,       // Response received — playing back TTS
  CONFUSED,       // Bad input streak
  ERROR_STATE     // Network / server failure
};

BuddyState currentState     = SLEEP;
BuddyState previousState    = SLEEP;   // for transition side-effects

// =====================================================================
// 5. GLOBALS
// =====================================================================
Adafruit_GC9A01A tft = Adafruit_GC9A01A(TFT_CS, TFT_DC, TFT_RST);

// Timers
unsigned long stateEnteredAt        = 0;   // when we entered the current state
unsigned long lastBlinkTime         = 0;
unsigned long lastUltrasonicTrigger = 0;   // debounce for wave gesture
unsigned long lastPirActivity       = 0;   // tracks last PIR HIGH for sleep timeout

// Ergonomics
unsigned long postureViolationStart = 0;
bool          isSlouching           = false;

// FreeRTOS HTTP task (keeps loop() unblocked during THINKING)
struct HttpResult {
  bool   done;
  bool   success;
  char   speech[256];
  char   expression[16];
};

volatile HttpResult httpResult;
TaskHandle_t        httpTaskHandle  = NULL;
char                pendingQuery[512];   // query passed into the HTTP task

// =====================================================================
// 6. FORWARD DECLARATIONS
// =====================================================================
void drawFace(const char* expression);
void handleIdleAnimation();
void handleErgonomics(int distance);
void handleSleepCheck();
int  getDistance();
void connectToWiFi();
void initMicrophone();
void fireCancel();
void httpTask(void* param);
void enterState(BuddyState next);

// =====================================================================
// 7. SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PIR_PIN,  INPUT);      // PIR output directly into GPIO

  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(GC9A01A_BLACK);

  connectToWiFi();
  initMicrophone();

  enterState(SLEEP);
}

// =====================================================================
// 8. LOOP  —  owns ALL state transitions, no helper sets currentState
// =====================================================================
void loop() {
  int  distance      = getDistance();
  bool pirActive     = digitalRead(PIR_PIN) == HIGH;
  unsigned long now  = millis();

  // Track PIR activity for sleep timeout
  if (pirActive) lastPirActivity = now;

  switch (currentState) {

    // ------------------------------------------------------------------
    case SLEEP:
      // Wake on PIR presence — debounced
      if (pirActive && (now - lastPirActivity > PIR_DEBOUNCE_MS)) {
        Serial.println("[PIR] Presence detected — waking up.");
        enterState(IDLE);
      }
      break;

    // ------------------------------------------------------------------
    case IDLE:
      handleErgonomics(distance);
      handleIdleAnimation();

      // Sleep if nobody around for SLEEP_TIMEOUT_MS
      if (now - lastPirActivity > SLEEP_TIMEOUT_MS) {
        Serial.println("[PIR] No presence — entering sleep.");
        enterState(SLEEP);
        break;
      }

      // Wake gesture: hand wave under 5cm, debounced
      if (distance > 0 && distance < 5 &&
          (now - lastUltrasonicTrigger > ULTRASONIC_DEBOUNCE_MS)) {
        lastUltrasonicTrigger = now;
        Serial.println("[Ultrasonic] Wake gesture detected.");
        enterState(LISTENING);
      }
      break;

    // ------------------------------------------------------------------
    case LISTENING:
      // CANCEL: second wave while listening → back to idle
      if (distance > 0 && distance < 5 &&
          (now - lastUltrasonicTrigger > ULTRASONIC_DEBOUNCE_MS)) {
        lastUltrasonicTrigger = now;
        Serial.println("[Cancel] Wave detected in LISTENING — returning to IDLE.");
        enterState(IDLE);
        break;
      }

      // CANCEL: timeout — nothing was said
      if (now - stateEnteredAt > LISTENING_TIMEOUT_MS) {
        Serial.println("[Timeout] LISTENING timed out — returning to IDLE.");
        enterState(IDLE);
        break;
      }

      // TODO: replace with real I2S DMA capture + Whisper/Google STT
      // Simulated: after 2s pretend we captured a query
      if (now - stateEnteredAt > 2000) {
        strncpy(pendingQuery, "What is a pointer in C++?", sizeof(pendingQuery));
        enterState(THINKING);
      }
      break;

    // ------------------------------------------------------------------
    case THINKING:
      // CANCEL: second wave during thinking → abort, notify server
      if (distance > 0 && distance < 5 &&
          (now - lastUltrasonicTrigger > ULTRASONIC_DEBOUNCE_MS)) {
        lastUltrasonicTrigger = now;
        Serial.println("[Cancel] Wave detected in THINKING — cancelling.");
        fireCancel();                  // POST /cancel so server resets its streak
        if (httpTaskHandle) {
          vTaskDelete(httpTaskHandle); // kill the in-flight HTTP task
          httpTaskHandle = NULL;
        }
        enterState(IDLE);
        break;
      }

      // CANCEL: server timeout — avoid hanging forever
      if (now - stateEnteredAt > THINKING_TIMEOUT_MS) {
        Serial.println("[Timeout] THINKING timed out — cancelling.");
        fireCancel();
        if (httpTaskHandle) {
          vTaskDelete(httpTaskHandle);
          httpTaskHandle = NULL;
        }
        enterState(ERROR_STATE);
        break;
      }

      // HTTP task finished — read result
      if (httpResult.done) {
        httpTaskHandle = NULL;

        if (httpResult.success) {
          Serial.printf("[AI] %s | %s\n", httpResult.expression, httpResult.speech);
          drawFace(httpResult.expression);
          enterState(SPEAKING);
        } else {
          enterState(ERROR_STATE);
        }
      }
      break;

    // ------------------------------------------------------------------
    case SPEAKING:
      // TODO: replace delay with actual TTS playback completion flag
      if (now - stateEnteredAt > SPEAKING_DURATION_MS) {
        enterState(IDLE);
      }
      break;

    // ------------------------------------------------------------------
    case CONFUSED:
    case ERROR_STATE:
      if (now - stateEnteredAt > ERROR_HOLD_MS) {
        enterState(IDLE);
      }
      break;
  }
}

// =====================================================================
// 9. STATE TRANSITION  —  single choke point, handles all side-effects
// =====================================================================
void enterState(BuddyState next) {
  previousState = currentState;
  currentState  = next;
  stateEnteredAt = millis();

  Serial.printf("[FSM] %d → %d\n", previousState, next);

  switch (next) {

    case SLEEP:
      tft.fillScreen(GC9A01A_BLACK);   // display off
      isSlouching = false;
      break;

    case IDLE:
      isSlouching = false;
      drawFace("IDLE");
      break;

    case LISTENING:
      drawFace("THINKING");            // attentive look while capturing
      break;

    case THINKING:
      drawFace("THINKING");

      // Reset result struct and spin up HTTP task on Core 0
      // (loop() runs on Core 1 — keeps UI fully responsive)
      httpResult.done    = false;
      httpResult.success = false;
      xTaskCreatePinnedToCore(
        httpTask,          // function
        "httpTask",        // name
        8192,              // stack (bytes) — ArduinoJson + HTTPClient need headroom
        NULL,              // param (pendingQuery is global)
        1,                 // priority
        &httpTaskHandle,   // handle so we can kill it on cancel
        0                  // Core 0
      );
      break;

    case SPEAKING:
      // Face already drawn in THINKING handler from httpResult.expression
      // TODO: kick off PAM8403 TTS playback here
      break;

    case CONFUSED:
      drawFace("CONFUSED");
      break;

    case ERROR_STATE:
      drawFace("ERROR");
      break;
  }
}

// =====================================================================
// 10. HTTP TASK  (runs on Core 0, never blocks loop())
// =====================================================================
void httpTask(void* param) {
  HTTPClient http;
  http.begin(url_ask);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(12000);

  JsonDocument outDoc;
  outDoc["query"] = pendingQuery;
  String body;
  serializeJson(outDoc, body);

  int code = http.POST(body);

  // Only treat a clean 200 as success.
  // 500 from Failure Path B is > 0 so the old check would've passed it through —
  // this ensures the red X ERROR_STATE actually triggers on a cloud blackout.
  if (code == 200) {
    String response = http.getString();
    JsonDocument inDoc;
    DeserializationError err = deserializeJson(inDoc, response);

    if (!err) {
      httpResult.success = true;
      strncpy(httpResult.speech,      inDoc["speech"]      | "",     sizeof(httpResult.speech));
      strncpy(httpResult.expression,  inDoc["expression"]  | "IDLE", sizeof(httpResult.expression));
    } else {
      Serial.println("[HTTP Task] JSON parse failed.");
      httpResult.success = false;
    }
  } else {
    // Covers 500 (cloud blackout), 404, timeouts returning negative codes, etc.
    Serial.printf("[HTTP Task] Server returned non-200: %d\n", code);
    httpResult.success = false;
  }

  http.end();
  httpResult.done = true;
  vTaskDelete(NULL);
}

// =====================================================================
// 11. FIRE CANCEL  (best-effort POST to /cancel — no retry needed)
// =====================================================================
void fireCancel() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(url_cancel);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);
  http.POST("{}");
  http.end();
  Serial.println("[Cancel] /cancel notified.");
}

// =====================================================================
// 12. SENSOR & ANIMATION HELPERS
// =====================================================================
int getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  return (duration == 0) ? 999 : (int)(duration / 58);
}

void handleIdleAnimation() {
  unsigned long now = millis();
  if (now - lastBlinkTime > BLINK_INTERVAL_MS && !isSlouching) {
    // Quick blink — erase and redraw eyes
    tft.fillCircle(80,  100, 30, GC9A01A_BLACK);
    tft.fillCircle(160, 100, 30, GC9A01A_BLACK);
    tft.fillRect(50, 95, 60, 10, GC9A01A_CYAN);
    tft.fillRect(130, 95, 60, 10, GC9A01A_CYAN);
    delay(150);   // only blocking call — 150ms blink is acceptable
    drawFace("IDLE");
    lastBlinkTime = now;
  }
}

void handleErgonomics(int distance) {
  unsigned long now = millis();
  // Between 5cm (wake zone) and 30cm (normal sitting) = slouching
  if (distance > 5 && distance < 30) {
    if (!isSlouching) {
      postureViolationStart = now;
      isSlouching = true;
    } else if (now - postureViolationStart > 5000) {
      drawFace("CONCERNED");
    }
  } else {
    if (isSlouching) {
      isSlouching = false;
      drawFace("IDLE");
    }
  }
}

// =====================================================================
// 13. DISPLAY — expression strings match server's VALID_EXPRESSIONS
// =====================================================================
void drawFace(const char* expression) {
  tft.fillScreen(GC9A01A_BLACK);
  int lx = 80, rx = 160, ey = 100, r = 30;

  if (strcmp(expression, "IDLE") == 0) {
    tft.fillCircle(lx, ey, r, GC9A01A_CYAN);
    tft.fillCircle(rx, ey, r, GC9A01A_CYAN);
  }
  else if (strcmp(expression, "HAPPY") == 0) {
    tft.fillCircle(lx, ey, r, GC9A01A_CYAN);
    tft.fillCircle(rx, ey, r, GC9A01A_CYAN);
    tft.fillRect(50, ey, 140, 40, GC9A01A_BLACK);  // arch eyes
  }
  else if (strcmp(expression, "CONCERNED") == 0) {
    tft.fillCircle(lx, ey, r, GC9A01A_RED);
    tft.fillCircle(rx, ey, r, GC9A01A_RED);
    tft.fillTriangle(40, 60, 110, 60, 110, 100, GC9A01A_BLACK);
    tft.fillTriangle(200, 60, 130, 60, 130, 100, GC9A01A_BLACK);
  }
  else if (strcmp(expression, "THINKING") == 0) {
    tft.fillCircle(lx, ey - 20, r - 10, GC9A01A_ORANGE);
    tft.fillCircle(rx, ey - 20, r - 10, GC9A01A_ORANGE);
  }
  // Below are local-only faces — server never sends these
  // but the ESP32 uses them for its own error/sleep states
  else if (strcmp(expression, "CONFUSED") == 0) {
    tft.fillCircle(lx, ey, r,      GC9A01A_MAGENTA);
    tft.fillCircle(rx, ey, r - 15, GC9A01A_MAGENTA);
  }
  else if (strcmp(expression, "ERROR") == 0) {
    tft.drawLine(lx-20, ey-20, lx+20, ey+20, GC9A01A_RED);
    tft.drawLine(lx-20, ey+20, lx+20, ey-20, GC9A01A_RED);
    tft.drawLine(rx-20, ey-20, rx+20, ey+20, GC9A01A_RED);
    tft.drawLine(rx-20, ey+20, rx+20, ey-20, GC9A01A_RED);
  }
  else if (strcmp(expression, "SLEEP") == 0) {
    // Closed eyes — thin horizontal lines
    tft.fillRect(50, 98, 60, 4, GC9A01A_CYAN);
    tft.fillRect(130, 98, 60, 4, GC9A01A_CYAN);
  }
}

// =====================================================================
// 14. INIT HELPERS
// =====================================================================
void connectToWiFi() {
  tft.setTextColor(GC9A01A_CYAN);
  tft.setTextSize(2);
  tft.setCursor(60, 110);
  tft.print("LINKING...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  tft.fillScreen(GC9A01A_BLACK);
  Serial.println("\n[WiFi] Connected.");
}

void initMicrophone() {
  i2s_config_t i2s_config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = 16000,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 64,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num   = I2S_SCK,
    .ws_io_num    = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD
  };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  Serial.println("[I2S] Microphone ready.");
}