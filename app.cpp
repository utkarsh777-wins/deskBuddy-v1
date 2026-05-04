// V_1.11

// #include <WiFi.h>
// #include <HTTPClient.h>
// #include <ArduinoJson.h>
// #include <SPI.h>
// #include <Adafruit_GFX.h>
// #include <Adafruit_GC9A01A.h>
// #include <driver/i2s.h>

// // =====================================================================
// // 1. PIN DEFINITIONS
// // =====================================================================
// #define TFT_CS    5
// #define TFT_DC    2
// #define TFT_RST   4

// #define TRIG_PIN  13
// #define ECHO_PIN  12

// #define PIR_PIN   33   // PIR OUT pin — HIGH when presence detected

// #define I2S_WS    15
// #define I2S_SCK   14
// #define I2S_SD    32

// // =====================================================================
// // 2. NETWORK CONFIGURATION
// // =====================================================================
// const char* ssid             = "YOUR_WIFI_NAME";
// const char* password         = "YOUR_WIFI_PASSWORD";
// const char* url_ask          = "http://YOUR_LAPTOP_IP:5000/ask";
// const char* url_cancel       = "http://YOUR_LAPTOP_IP:5000/cancel";
// const char* url_health       = "http://YOUR_LAPTOP_IP:5000/health";

// // =====================================================================
// // 3. TIMING CONSTANTS  (all millis-based, zero blocking delays in loop)
// // =====================================================================
// #define LISTENING_TIMEOUT_MS     10000   // cancel if no audio captured in 10s
// #define THINKING_TIMEOUT_MS      15000   // cancel if server doesn't respond in 15s
// #define SPEAKING_DURATION_MS      4000   // simulated TTS playback duration
// #define ERROR_HOLD_MS             3000   // how long to show error face before idle
// #define BLINK_INTERVAL_MS         4000   // idle eye blink cadence
// #define ULTRASONIC_DEBOUNCE_MS     800   // min gap between two valid wave gestures
// #define PIR_DEBOUNCE_MS           2000   // ignore PIR chatter under 2s
// #define SLEEP_TIMEOUT_MS         60000   // go to sleep if no PIR for 60s

// // =====================================================================
// // 4. STATE MACHINE
// // =====================================================================
// enum BuddyState {
//   SLEEP,          // PIR sees nobody — display off, low power
//   IDLE,           // Resting, blinking, ergonomics active
//   LISTENING,      // Hand wave detected — capturing audio
//   THINKING,       // Audio sent — waiting for server response
//   SPEAKING,       // Response received — playing back TTS
//   CONFUSED,       // Bad input streak
//   ERROR_STATE     // Network / server failure
// };

// BuddyState currentState     = SLEEP;
// BuddyState previousState    = SLEEP;   // for transition side-effects

// // =====================================================================
// // 5. GLOBALS
// // =====================================================================
// Adafruit_GC9A01A tft = Adafruit_GC9A01A(TFT_CS, TFT_DC, TFT_RST);

// // Timers
// unsigned long stateEnteredAt        = 0;   // when we entered the current state
// unsigned long lastBlinkTime         = 0;
// unsigned long lastUltrasonicTrigger = 0;   // debounce for wave gesture
// unsigned long lastPirActivity       = 0;   // tracks last PIR HIGH for sleep timeout

// // Ergonomics
// unsigned long postureViolationStart = 0;
// bool          isSlouching           = false;

// // FreeRTOS HTTP task (keeps loop() unblocked during THINKING)
// struct HttpResult {
//   bool   done;
//   bool   success;
//   char   speech[256];
//   char   expression[16];
// };

// volatile HttpResult httpResult;
// TaskHandle_t        httpTaskHandle  = NULL;
// char                pendingQuery[512];   // query passed into the HTTP task

// // =====================================================================
// // 6. FORWARD DECLARATIONS
// // =====================================================================
// void drawFace(const char* expression);
// void handleIdleAnimation();
// void handleErgonomics(int distance);
// void handleSleepCheck();
// int  getDistance();
// void connectToWiFi();
// void initMicrophone();
// void fireCancel();
// void httpTask(void* param);
// void enterState(BuddyState next);

// // =====================================================================
// // 7. SETUP
// // =====================================================================
// void setup() {
//   Serial.begin(115200);

//   pinMode(TRIG_PIN, OUTPUT);
//   pinMode(ECHO_PIN, INPUT);
//   pinMode(PIR_PIN,  INPUT);      // PIR output directly into GPIO

//   tft.begin();
//   tft.setRotation(0);
//   tft.fillScreen(GC9A01A_BLACK);

//   connectToWiFi();
//   initMicrophone();

//   enterState(SLEEP);
// }

// // =====================================================================
// // 8. LOOP  —  owns ALL state transitions, no helper sets currentState
// // =====================================================================
// void loop() {
//   int  distance      = getDistance();
//   bool pirActive     = digitalRead(PIR_PIN) == HIGH;
//   unsigned long now  = millis();

//   // Track PIR activity for sleep timeout
//   if (pirActive) lastPirActivity = now;

//   switch (currentState) {

//     // ------------------------------------------------------------------
//     case SLEEP:
//       // Wake on PIR presence — debounced
//       if (pirActive && (now - lastPirActivity > PIR_DEBOUNCE_MS)) {
//         Serial.println("[PIR] Presence detected — waking up.");
//         enterState(IDLE);
//       }
//       break;

//     // ------------------------------------------------------------------
//     case IDLE:
//       handleErgonomics(distance);
//       handleIdleAnimation();

//       // Sleep if nobody around for SLEEP_TIMEOUT_MS
//       if (now - lastPirActivity > SLEEP_TIMEOUT_MS) {
//         Serial.println("[PIR] No presence — entering sleep.");
//         enterState(SLEEP);
//         break;
//       }

//       // Wake gesture: hand wave under 5cm, debounced
//       if (distance > 0 && distance < 5 &&
//           (now - lastUltrasonicTrigger > ULTRASONIC_DEBOUNCE_MS)) {
//         lastUltrasonicTrigger = now;
//         Serial.println("[Ultrasonic] Wake gesture detected.");
//         enterState(LISTENING);
//       }
//       break;

//     // ------------------------------------------------------------------
//     case LISTENING:
//       // CANCEL: second wave while listening → back to idle
//       if (distance > 0 && distance < 5 &&
//           (now - lastUltrasonicTrigger > ULTRASONIC_DEBOUNCE_MS)) {
//         lastUltrasonicTrigger = now;
//         Serial.println("[Cancel] Wave detected in LISTENING — returning to IDLE.");
//         enterState(IDLE);
//         break;
//       }

//       // CANCEL: timeout — nothing was said
//       if (now - stateEnteredAt > LISTENING_TIMEOUT_MS) {
//         Serial.println("[Timeout] LISTENING timed out — returning to IDLE.");
//         enterState(IDLE);
//         break;
//       }

//       // TODO: replace with real I2S DMA capture + Whisper/Google STT
//       // Simulated: after 2s pretend we captured a query
//       if (now - stateEnteredAt > 2000) {
//         strncpy(pendingQuery, "What is a pointer in C++?", sizeof(pendingQuery));
//         enterState(THINKING);
//       }
//       break;

//     // ------------------------------------------------------------------
//     case THINKING:
//       // CANCEL: second wave during thinking → abort, notify server
//       if (distance > 0 && distance < 5 &&
//           (now - lastUltrasonicTrigger > ULTRASONIC_DEBOUNCE_MS)) {
//         lastUltrasonicTrigger = now;
//         Serial.println("[Cancel] Wave detected in THINKING — cancelling.");
//         fireCancel();                  // POST /cancel so server resets its streak
//         if (httpTaskHandle) {
//           vTaskDelete(httpTaskHandle); // kill the in-flight HTTP task
//           httpTaskHandle = NULL;
//         }
//         enterState(IDLE);
//         break;
//       }

//       // CANCEL: server timeout — avoid hanging forever
//       if (now - stateEnteredAt > THINKING_TIMEOUT_MS) {
//         Serial.println("[Timeout] THINKING timed out — cancelling.");
//         fireCancel();
//         if (httpTaskHandle) {
//           vTaskDelete(httpTaskHandle);
//           httpTaskHandle = NULL;
//         }
//         enterState(ERROR_STATE);
//         break;
//       }

//       // HTTP task finished — read result
//       if (httpResult.done) {
//         httpTaskHandle = NULL;

//         if (httpResult.success) {
//           Serial.printf("[AI] %s | %s\n", httpResult.expression, httpResult.speech);
//           drawFace(httpResult.expression);
//           enterState(SPEAKING);
//         } else {
//           enterState(ERROR_STATE);
//         }
//       }
//       break;

//     // ------------------------------------------------------------------
//     case SPEAKING:
//       // TODO: replace delay with actual TTS playback completion flag
//       if (now - stateEnteredAt > SPEAKING_DURATION_MS) {
//         enterState(IDLE);
//       }
//       break;

//     // ------------------------------------------------------------------
//     case CONFUSED:
//     case ERROR_STATE:
//       if (now - stateEnteredAt > ERROR_HOLD_MS) {
//         enterState(IDLE);
//       }
//       break;
//   }
// }

// // =====================================================================
// // 9. STATE TRANSITION  —  single choke point, handles all side-effects
// // =====================================================================
// void enterState(BuddyState next) {
//   previousState = currentState;
//   currentState  = next;
//   stateEnteredAt = millis();

//   Serial.printf("[FSM] %d → %d\n", previousState, next);

//   switch (next) {

//     case SLEEP:
//       tft.fillScreen(GC9A01A_BLACK);   // display off
//       isSlouching = false;
//       break;

//     case IDLE:
//       isSlouching = false;
//       drawFace("IDLE");
//       break;

//     case LISTENING:
//       drawFace("THINKING");            // attentive look while capturing
//       break;

//     case THINKING:
//       drawFace("THINKING");

//       // Reset result struct and spin up HTTP task on Core 0
//       // (loop() runs on Core 1 — keeps UI fully responsive)
//       httpResult.done    = false;
//       httpResult.success = false;
//       xTaskCreatePinnedToCore(
//         httpTask,          // function
//         "httpTask",        // name
//         8192,              // stack (bytes) — ArduinoJson + HTTPClient need headroom
//         NULL,              // param (pendingQuery is global)
//         1,                 // priority
//         &httpTaskHandle,   // handle so we can kill it on cancel
//         0                  // Core 0
//       );
//       break;

//     case SPEAKING:
//       // Face already drawn in THINKING handler from httpResult.expression
//       // TODO: kick off PAM8403 TTS playback here
//       break;

//     case CONFUSED:
//       drawFace("CONFUSED");
//       break;

//     case ERROR_STATE:
//       drawFace("ERROR");
//       break;
//   }
// }

// // =====================================================================
// // 10. HTTP TASK  (runs on Core 0, never blocks loop())
// // =====================================================================
// void httpTask(void* param) {
//   HTTPClient http;
//   http.begin(url_ask);
//   http.addHeader("Content-Type", "application/json");
//   http.setTimeout(12000);

//   JsonDocument outDoc;
//   outDoc["query"] = pendingQuery;
//   String body;
//   serializeJson(outDoc, body);

//   int code = http.POST(body);

//   // Only treat a clean 200 as success.
//   // 500 from Failure Path B is > 0 so the old check would've passed it through —
//   // this ensures the red X ERROR_STATE actually triggers on a cloud blackout.
//   if (code == 200) {
//     String response = http.getString();
//     JsonDocument inDoc;
//     DeserializationError err = deserializeJson(inDoc, response);

//     if (!err) {
//       httpResult.success = true;
//       strncpy(httpResult.speech,      inDoc["speech"]      | "",     sizeof(httpResult.speech));
//       strncpy(httpResult.expression,  inDoc["expression"]  | "IDLE", sizeof(httpResult.expression));
//     } else {
//       Serial.println("[HTTP Task] JSON parse failed.");
//       httpResult.success = false;
//     }
//   } else {
//     // Covers 500 (cloud blackout), 404, timeouts returning negative codes, etc.
//     Serial.printf("[HTTP Task] Server returned non-200: %d\n", code);
//     httpResult.success = false;
//   }

//   http.end();
//   httpResult.done = true;
//   vTaskDelete(NULL);
// }

// // =====================================================================
// // 11. FIRE CANCEL  (best-effort POST to /cancel — no retry needed)
// // =====================================================================
// void fireCancel() {
//   if (WiFi.status() != WL_CONNECTED) return;

//   HTTPClient http;
//   http.begin(url_cancel);
//   http.addHeader("Content-Type", "application/json");
//   http.setTimeout(3000);
//   http.POST("{}");
//   http.end();
//   Serial.println("[Cancel] /cancel notified.");
// }

// // =====================================================================
// // 12. SENSOR & ANIMATION HELPERS
// // =====================================================================
// int getDistance() {
//   digitalWrite(TRIG_PIN, LOW);
//   delayMicroseconds(2);
//   digitalWrite(TRIG_PIN, HIGH);
//   delayMicroseconds(10);
//   digitalWrite(TRIG_PIN, LOW);
//   long duration = pulseIn(ECHO_PIN, HIGH, 30000);
//   return (duration == 0) ? 999 : (int)(duration / 58);
// }

// void handleIdleAnimation() {
//   unsigned long now = millis();
//   if (now - lastBlinkTime > BLINK_INTERVAL_MS && !isSlouching) {
//     // Quick blink — erase and redraw eyes
//     tft.fillCircle(80,  100, 30, GC9A01A_BLACK);
//     tft.fillCircle(160, 100, 30, GC9A01A_BLACK);
//     tft.fillRect(50, 95, 60, 10, GC9A01A_CYAN);
//     tft.fillRect(130, 95, 60, 10, GC9A01A_CYAN);
//     delay(150);   // only blocking call — 150ms blink is acceptable
//     drawFace("IDLE");
//     lastBlinkTime = now;
//   }
// }

// void handleErgonomics(int distance) {
//   unsigned long now = millis();
//   // Between 5cm (wake zone) and 30cm (normal sitting) = slouching
//   if (distance > 5 && distance < 30) {
//     if (!isSlouching) {
//       postureViolationStart = now;
//       isSlouching = true;
//     } else if (now - postureViolationStart > 5000) {
//       drawFace("CONCERNED");
//     }
//   } else {
//     if (isSlouching) {
//       isSlouching = false;
//       drawFace("IDLE");
//     }
//   }
// }

// // =====================================================================
// // 13. DISPLAY — expression strings match server's VALID_EXPRESSIONS
// // =====================================================================
// void drawFace(const char* expression) {
//   tft.fillScreen(GC9A01A_BLACK);
//   int lx = 80, rx = 160, ey = 100, r = 30;

//   if (strcmp(expression, "IDLE") == 0) {
//     tft.fillCircle(lx, ey, r, GC9A01A_CYAN);
//     tft.fillCircle(rx, ey, r, GC9A01A_CYAN);
//   }
//   else if (strcmp(expression, "HAPPY") == 0) {
//     tft.fillCircle(lx, ey, r, GC9A01A_CYAN);
//     tft.fillCircle(rx, ey, r, GC9A01A_CYAN);
//     tft.fillRect(50, ey, 140, 40, GC9A01A_BLACK);  // arch eyes
//   }
//   else if (strcmp(expression, "CONCERNED") == 0) {
//     tft.fillCircle(lx, ey, r, GC9A01A_RED);
//     tft.fillCircle(rx, ey, r, GC9A01A_RED);
//     tft.fillTriangle(40, 60, 110, 60, 110, 100, GC9A01A_BLACK);
//     tft.fillTriangle(200, 60, 130, 60, 130, 100, GC9A01A_BLACK);
//   }
//   else if (strcmp(expression, "THINKING") == 0) {
//     tft.fillCircle(lx, ey - 20, r - 10, GC9A01A_ORANGE);
//     tft.fillCircle(rx, ey - 20, r - 10, GC9A01A_ORANGE);
//   }
//   // Below are local-only faces — server never sends these
//   // but the ESP32 uses them for its own error/sleep states
//   else if (strcmp(expression, "CONFUSED") == 0) {
//     tft.fillCircle(lx, ey, r,      GC9A01A_MAGENTA);
//     tft.fillCircle(rx, ey, r - 15, GC9A01A_MAGENTA);
//   }
//   else if (strcmp(expression, "ERROR") == 0) {
//     tft.drawLine(lx-20, ey-20, lx+20, ey+20, GC9A01A_RED);
//     tft.drawLine(lx-20, ey+20, lx+20, ey-20, GC9A01A_RED);
//     tft.drawLine(rx-20, ey-20, rx+20, ey+20, GC9A01A_RED);
//     tft.drawLine(rx-20, ey+20, rx+20, ey-20, GC9A01A_RED);
//   }
//   else if (strcmp(expression, "SLEEP") == 0) {
//     // Closed eyes — thin horizontal lines
//     tft.fillRect(50, 98, 60, 4, GC9A01A_CYAN);
//     tft.fillRect(130, 98, 60, 4, GC9A01A_CYAN);
//   }
// }

// // =====================================================================
// // 14. INIT HELPERS
// // =====================================================================
// void connectToWiFi() {
//   tft.setTextColor(GC9A01A_CYAN);
//   tft.setTextSize(2);
//   tft.setCursor(60, 110);
//   tft.print("LINKING...");
//   WiFi.begin(ssid, password);
//   while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
//   tft.fillScreen(GC9A01A_BLACK);
//   Serial.println("\n[WiFi] Connected.");
// }

// void initMicrophone() {
//   i2s_config_t i2s_config = {
//     .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
//     .sample_rate          = 16000,
//     .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
//     .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
//     .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
//     .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
//     .dma_buf_count        = 8,
//     .dma_buf_len          = 64,
//     .use_apll             = false,
//     .tx_desc_auto_clear   = false,
//     .fixed_mclk           = 0
//   };
//   i2s_pin_config_t pin_config = {
//     .bck_io_num   = I2S_SCK,
//     .ws_io_num    = I2S_WS,
//     .data_out_num = I2S_PIN_NO_CHANGE,
//     .data_in_num  = I2S_SD
//   };
//   i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
//   i2s_set_pin(I2S_NUM_0, &pin_config);
//   Serial.println("[I2S] Microphone ready.");
// }

// V_1.2

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
#define PIR_PIN   33
#define I2S_WS    15
#define I2S_SCK   14
#define I2S_SD    32
// Speaker: GPIO25 (DAC1) → PAM8403 audio input

// =====================================================================
// 2. NETWORK CONFIGURATION
// =====================================================================
const char* ssid       = "//";
const char* password   = "***REDACTED***";
const char* url_ask    = "http://172.20.227.192:5000/ask";
const char* url_cancel = "http://172.20.227.192:5000/cancel";
const char* url_health = "http://172.20.227.192:5000/health";
const char* url_stt    = "http://172.20.227.192:5000/stt";
const char* url_tts    = "http://172.20.227.192:5000/tts";

// =====================================================================
// 3. TIMING & ZONE CONSTANTS
// =====================================================================
#define LISTENING_TIMEOUT_MS    12000   // 12s per recording attempt
#define CONV_FOLLOWUP_MS        15000   // 15s for follow-ups in conversation
#define THINKING_TIMEOUT_MS     30000   // covers STT + Gemini + TTS chain
#define ERROR_HOLD_MS            3000
#define BLINK_INTERVAL_MS        4000
#define ULTRASONIC_DEBOUNCE_MS    800
#define PIR_DEBOUNCE_MS          2000
#define SLEEP_TIMEOUT_MS        60000
#define POSTURE_WARNING_MS      60000   // 60s sustained slouch → CONCERNED
#define POMODORO_MS        (25*60*1000UL)

// Ultrasonic zones
#define WAKE_ZONE_CM              5     // < 5cm  : Jedi wave
#define POSTURE_ZONE_CM          30     // 5–30cm : slouch risk
#define TIMEKEEPER_MIN_CM        10     // 10–100cm: normal desk distance
#define TIMEKEEPER_MAX_CM       100

// Audio
#define MIC_SAMPLE_RATE         16000   // 16kHz — STT needs quality
#define SPK_SAMPLE_RATE          8000   // 8kHz — server TTS output
#define RECORD_SECONDS              2   // recording window per attempt
#define AUDIO_SAMPLES    (MIC_SAMPLE_RATE * RECORD_SECONDS)   // 32000
#define AUDIO_BYTES      (AUDIO_SAMPLES * 2)                  // 64KB (16-bit)
#define AUDIO_BUF_SIZE   (AUDIO_BYTES + 44)                   // +44 WAV header
#define VOLUME_GAIN              4      // digital amplification — tune if too loud/quiet
#define VAD_THRESHOLD          200      // avg amplitude to count as speech — tune up if noisy room

// =====================================================================
// 4. STATE MACHINE
// =====================================================================
enum BuddyState {
  SLEEP, IDLE, LISTENING, THINKING, SPEAKING, CONFUSED, ERROR_STATE
};
BuddyState currentState  = SLEEP;
BuddyState previousState = SLEEP;

// =====================================================================
// 5. SHARED AUDIO BUFFER (~64KB static DRAM)
// Recording and TTS playback are sequential — one buffer serves both.
// If you get heap errors, reduce RECORD_SECONDS to 1.
// =====================================================================
static uint8_t audioBuffer[AUDIO_BUF_SIZE];
static int     audioBufferBytes  = 0;
static bool    audioPlaybackDone = false;
static TaskHandle_t playbackTaskHandle = NULL;

// =====================================================================
// 6. GLOBALS
// =====================================================================
Adafruit_GC9A01A tft = Adafruit_GC9A01A(TFT_CS, TFT_DC, TFT_RST);

unsigned long stateEnteredAt        = 0;
unsigned long lastBlinkTime         = 0;
unsigned long lastUltrasonicTrigger = 0;
unsigned long lastPirActivity       = 0;
unsigned long postureViolationStart = 0;
bool          isSlouching           = false;

// Conversation mode — SPEAKING loops back to LISTENING automatically
bool inConversation = false;

// Study timer
unsigned long studySessionStart  = 0;
bool          studySessionActive = false;
bool          pomodoroAlerted    = false;

// HTTP task result struct
struct HttpResult {
  volatile bool done;
  volatile bool success;
  volatile bool hasAudio;
  char speech[256];
  char expression[16];
};
HttpResult   httpResult;
TaskHandle_t httpTaskHandle = NULL;
char         pendingQuery[512];

// Serial
String serialInputBuffer = "";
bool   serialQueryReady  = false;

// =====================================================================
// 7. FORWARD DECLARATIONS
// =====================================================================
void drawFace(const char* expression);
void handleIdleAnimation();
void handleErgonomics(int distance);
void handleTimekeeper(int distance);
void handleSerialInput();
int  getDistance();
void connectToWiFi();
void initMicrophone();
void initSpeaker();
void fireCancel();
void httpTask(void* param);
void playbackTask(void* param);
void playAudio();
bool recordAudio();
void writeWavHeader(uint8_t* buf, int numSamples, int sampleRate);
void enterState(BuddyState next);
void displayStudyTime(unsigned long elapsedMs);

// =====================================================================
// 8. SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PIR_PIN,  INPUT);

  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(GC9A01A_BLACK);

  connectToWiFi();
  initMicrophone();
  initSpeaker();

  Serial.println("=================================================");
  Serial.println(" DESK BUDDY ONLINE");
  Serial.println(" Mic:    Jedi wave → speak naturally");
  Serial.println(" Serial: type 'wake' → then your question");
  Serial.println(" Type 'cancel' anytime to abort / exit convo");
  Serial.println("=================================================");

  enterState(SLEEP);
}

// =====================================================================
// 9. SERIAL INPUT HANDLER (non-blocking)
// =====================================================================
void handleSerialInput() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialInputBuffer.length() > 0) {
        serialInputBuffer.trim();

        if (serialInputBuffer.equalsIgnoreCase("wake")) {
          Serial.println("[Serial] Wake simulated.");
          if (currentState == IDLE || currentState == SLEEP) {
            lastPirActivity = millis();
            enterState(LISTENING);
          }
        }
        else if (serialInputBuffer.equalsIgnoreCase("cancel")) {
          Serial.println("[Serial] Cancel.");
          inConversation = false;
          if (currentState == THINKING) {
            fireCancel();
            if (httpTaskHandle) { vTaskDelete(httpTaskHandle); httpTaskHandle = NULL; }
          }
          if (currentState == LISTENING || currentState == THINKING
              || currentState == SPEAKING)
            enterState(IDLE);
        }
        else if (currentState == LISTENING) {
          strncpy(pendingQuery, serialInputBuffer.c_str(), sizeof(pendingQuery));
          Serial.printf("[Serial] Query: %s\n", pendingQuery);
          serialQueryReady = true;
        }
        else {
          Serial.println("[Serial] Not listening. Type 'wake' first.");
        }
        serialInputBuffer = "";
      }
    } else {
      serialInputBuffer += c;
    }
  }
}

// =====================================================================
// 10. AUDIO RECORDING  (blocking 2-second window with amplitude VAD)
// Handles its own wave + serial cancel checks inside the loop.
// Returns true if usable speech was captured into audioBuffer.
// =====================================================================
bool recordAudio() {
  int16_t* pcm      = (int16_t*)(audioBuffer + 44);
  int      pcmSamps = 0;
  long     ampSum   = 0;

  Serial.println("[MIC] Recording 2s...");
  unsigned long start = millis();

  while (millis() - start < (unsigned long)(RECORD_SECONDS * 1000)
         && pcmSamps < AUDIO_SAMPLES) {

    handleSerialInput();
    if (serialQueryReady || currentState != LISTENING) return false;

    // Wave cancel check inside recording
    int d = getDistance();
    unsigned long now = millis();
    if (d > 0 && d < WAKE_ZONE_CM &&
        now - lastUltrasonicTrigger > ULTRASONIC_DEBOUNCE_MS) {
      lastUltrasonicTrigger = now;
      Serial.println("[MIC] Wave cancel during recording.");
      inConversation = false;
      enterState(IDLE);
      return false;
    }

    // Read I2S (INMP441: 32-bit left-justified 24-bit data)
    int32_t raw[64];
    size_t  bytesRead = 0;
    i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytesRead, pdMS_TO_TICKS(10));
    int n = bytesRead / 4;

    for (int i = 0; i < n && pcmSamps < AUDIO_SAMPLES; i++) {
      int16_t s = (int16_t)(raw[i] >> 16);
      pcm[pcmSamps++] = s;
      ampSum += abs(s);
    }
  }

  long avgAmp = (pcmSamps > 0) ? ampSum / pcmSamps : 0;
  Serial.printf("[MIC] Avg amplitude: %ld (threshold: %d)\n", avgAmp, VAD_THRESHOLD);

  if (avgAmp < VAD_THRESHOLD || pcmSamps < MIC_SAMPLE_RATE / 4) {
    Serial.println("[MIC] Too quiet — no speech.");
    return false;
  }

  writeWavHeader(audioBuffer, pcmSamps, MIC_SAMPLE_RATE);
  audioBufferBytes = 44 + pcmSamps * 2;
  Serial.printf("[MIC] Captured %.1fs\n", (float)pcmSamps / MIC_SAMPLE_RATE);
  return true;
}

// =====================================================================
// 11. WAV HEADER WRITER
// =====================================================================
void writeWavHeader(uint8_t* buf, int numSamples, int sampleRate) {
  int data  = numSamples * 2;
  int file  = data + 36;
  int brate = sampleRate * 2;

  memcpy(buf,    "RIFF", 4);
  buf[4]=(file)&0xFF;  buf[5]=(file>>8)&0xFF;
  buf[6]=(file>>16)&0xFF; buf[7]=(file>>24)&0xFF;
  memcpy(buf+8,  "WAVEfmt ", 8);
  buf[16]=16; buf[17]=0; buf[18]=0; buf[19]=0;
  buf[20]=1;  buf[21]=0;                           // PCM
  buf[22]=1;  buf[23]=0;                           // mono
  buf[24]=(sampleRate)&0xFF;    buf[25]=(sampleRate>>8)&0xFF;
  buf[26]=(sampleRate>>16)&0xFF; buf[27]=(sampleRate>>24)&0xFF;
  buf[28]=(brate)&0xFF;  buf[29]=(brate>>8)&0xFF;
  buf[30]=(brate>>16)&0xFF; buf[31]=(brate>>24)&0xFF;
  buf[32]=2; buf[33]=0;                            // block align
  buf[34]=16; buf[35]=0;                           // 16-bit
  memcpy(buf+36, "data", 4);
  buf[40]=(data)&0xFF;  buf[41]=(data>>8)&0xFF;
  buf[42]=(data>>16)&0xFF; buf[43]=(data>>24)&0xFF;
}

// =====================================================================
// 12. AUDIO PLAYBACK  (I2S built-in DAC → GPIO25 → PAM8403)
// =====================================================================
void playAudio() {
  if (audioBufferBytes < 44) {
    Serial.println("[Audio] No audio to play.");
    return;
  }

  uint8_t* pcmData  = audioBuffer + 44;
  int      pcmBytes = audioBufferBytes - 44;
  int      nSamples = pcmBytes / 2;
  int16_t* samples  = (int16_t*)pcmData;

  // I2S DAC mode expects stereo 16-bit words where upper 8 bits = unsigned DAC value.
  // Only right channel (GPIO25) is actually driven.
  uint16_t* dacBuf = (uint16_t*)malloc(nSamples * 4);
  if (!dacBuf) { Serial.println("[Audio] malloc fail."); return; }

  for (int i = 0; i < nSamples; i++) {
    int32_t  amp    = (int32_t)samples[i] * VOLUME_GAIN;
    int16_t  clipped = (int16_t)constrain(amp, -32768, 32767);
    uint16_t dacVal = (uint16_t)(((int32_t)clipped + 32768) >> 8) << 8;
    dacBuf[i * 2]     = dacVal;   // Right → GPIO25 → PAM8403
    dacBuf[i * 2 + 1] = dacVal;   // Left  → GPIO26 (unused)
  }

  size_t written = 0;
  i2s_write(I2S_NUM_1, dacBuf, nSamples * 4, &written, portMAX_DELAY);
  i2s_zero_dma_buffer(I2S_NUM_1);
  free(dacBuf);
  Serial.printf("[Audio] Played %d bytes (%.1fs)\n",
                written, (float)nSamples / SPK_SAMPLE_RATE);
}

void playbackTask(void* param) {
  playAudio();
  audioPlaybackDone = true;
  vTaskDelete(NULL);
}

// =====================================================================
// 13. LOOP
// =====================================================================
void loop() {
  int  distance  = getDistance();
  bool pirActive = digitalRead(PIR_PIN) == HIGH;
  unsigned long now = millis();

  if (pirActive) lastPirActivity = now;
  handleSerialInput();

  switch (currentState) {

    // ------------------------------------------------------------------
    case SLEEP:
      if (pirActive && (now - lastPirActivity > PIR_DEBOUNCE_MS)) {
        Serial.println("[PIR] Presence — waking.");
        enterState(IDLE);
      }
      break;

    // ------------------------------------------------------------------
    case IDLE:
      handleErgonomics(distance);
      handleTimekeeper(distance);
      handleIdleAnimation();

      if (now - lastPirActivity > SLEEP_TIMEOUT_MS) {
        studySessionActive = false;
        Serial.println("[PIR] No presence — sleeping.");
        enterState(SLEEP);
        break;
      }

      // Jedi Wave: strictly < 5cm
      if (distance > 0 && distance < WAKE_ZONE_CM &&
          now - lastUltrasonicTrigger > ULTRASONIC_DEBOUNCE_MS) {
        lastUltrasonicTrigger = now;
        Serial.println("[Wake] Jedi wave detected.");
        enterState(LISTENING);
      }
      break;

    // ------------------------------------------------------------------
    case LISTENING: {
      unsigned long listenTimeout = inConversation
                                    ? CONV_FOLLOWUP_MS
                                    : LISTENING_TIMEOUT_MS;

      // Serial query ready (set inside handleSerialInput)
      if (serialQueryReady) {
        serialQueryReady = false;
        enterState(THINKING);
        break;
      }

      // Wave cancel — also exits conversation
      if (distance > 0 && distance < WAKE_ZONE_CM &&
          now - lastUltrasonicTrigger > ULTRASONIC_DEBOUNCE_MS) {
        lastUltrasonicTrigger = now;
        Serial.println("[Cancel] Wave in LISTENING.");
        inConversation = false;
        enterState(IDLE);
        break;
      }

      // Outer timeout
      if (now - stateEnteredAt > listenTimeout) {
        Serial.printf("[Timeout] LISTENING expired (%s).\n",
                      inConversation ? "conv" : "fresh");
        inConversation = false;
        enterState(IDLE);
        break;
      }

      // Mic recording attempt (2s blocking, checks cancel internally)
      bool got = recordAudio();
      if (currentState != LISTENING) break;   // wave cancel fired inside

      if (serialQueryReady) {                  // serial won during recording
        serialQueryReady = false;
        enterState(THINKING);
        break;
      }

      if (got) {
        // Mic captured audio — httpTask will do STT via this sentinel
        strncpy(pendingQuery, "__MIC_AUDIO__", sizeof(pendingQuery));
        enterState(THINKING);
      }
      // If !got → loop iterates again until outer timeout
      break;
    }

    // ------------------------------------------------------------------
    case THINKING:
      // Wave cancel
      if (distance > 0 && distance < WAKE_ZONE_CM &&
          now - lastUltrasonicTrigger > ULTRASONIC_DEBOUNCE_MS) {
        lastUltrasonicTrigger = now;
        Serial.println("[Cancel] Wave in THINKING.");
        inConversation = false;
        fireCancel();
        if (httpTaskHandle) { vTaskDelete(httpTaskHandle); httpTaskHandle = NULL; }
        enterState(IDLE);
        break;
      }

      if (now - stateEnteredAt > THINKING_TIMEOUT_MS) {
        Serial.println("[Timeout] THINKING expired.");
        inConversation = false;
        fireCancel();
        if (httpTaskHandle) { vTaskDelete(httpTaskHandle); httpTaskHandle = NULL; }
        enterState(ERROR_STATE);
        break;
      }

      if (httpResult.done) {
        httpTaskHandle = NULL;
        if (httpResult.success) {
          Serial.printf("[AI] %s | %s\n", httpResult.expression, httpResult.speech);
          drawFace(httpResult.expression);
          enterState(SPEAKING);
        } else {
          inConversation = false;
          enterState(ERROR_STATE);
        }
      }
      break;

    // ------------------------------------------------------------------
    case SPEAKING:
      // Wait for playback task to signal done (min 500ms so face is visible)
      if (audioPlaybackDone && now - stateEnteredAt > 500) {
        audioPlaybackDone  = false;
        playbackTaskHandle = NULL;
        // Loop back to LISTENING — conversation continues
        enterState(LISTENING);
      }
      break;

    // ------------------------------------------------------------------
    case CONFUSED:
    case ERROR_STATE:
      inConversation = false;
      if (now - stateEnteredAt > ERROR_HOLD_MS) {
        enterState(IDLE);
      }
      break;
  }
}

// =====================================================================
// 14. STATE TRANSITION
// =====================================================================
void enterState(BuddyState next) {
  previousState  = currentState;
  currentState   = next;
  stateEnteredAt = millis();

  Serial.printf("[FSM] %d → %d%s\n", previousState, next,
                inConversation ? " [CONV]" : "");

  switch (next) {
    case SLEEP:
      tft.fillScreen(GC9A01A_BLACK);
      drawFace("SLEEP");
      isSlouching = false;
      break;

    case IDLE:
      isSlouching = false;
      drawFace("IDLE");
      Serial.println("[Hint] Jedi wave or type 'wake' to ask a question.");
      break;

    case LISTENING:
      drawFace("THINKING");
      Serial.println(inConversation
        ? "[Conv] Follow-up? Speak or type. Wave or 'cancel' to exit."
        : "[Listening] Speak your question or type it.");
      break;

    case THINKING:
      drawFace("THINKING");
      httpResult.done     = false;
      httpResult.success  = false;
      httpResult.hasAudio = false;
      audioBufferBytes    = 0;
      xTaskCreatePinnedToCore(httpTask, "httpTask", 16384, NULL, 1, &httpTaskHandle, 0);
      break;

    case SPEAKING:
      // Face already set from httpResult.expression drawn in loop's THINKING case
      inConversation    = true;   // we're now in a back-and-forth
      audioPlaybackDone = false;
      xTaskCreatePinnedToCore(playbackTask, "playback", 4096, NULL, 1, &playbackTaskHandle, 0);
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
// 15. HTTP TASK (Core 0): STT → /ask (Gemini) → /tts
// =====================================================================
void httpTask(void* param) {

  // ── Step 1: STT (only if mic audio was captured) ────────────────────
  if (strncmp(pendingQuery, "__MIC_AUDIO__", 13) == 0) {
    Serial.printf("[STT] Sending %d bytes...\n", audioBufferBytes);

    HTTPClient stt;
    stt.begin(url_stt);
    stt.addHeader("Content-Type", "audio/wav");
    stt.setTimeout(10000);

    int    sttCode = stt.POST(audioBuffer, audioBufferBytes);
    String sttResp = (sttCode == 200) ? stt.getString() : "";
    stt.end();
    audioBufferBytes = 0;   // free buffer for TTS

    if (sttCode != 200 || sttResp.isEmpty()) {
      Serial.printf("[STT] Failed: %d\n", sttCode);
      httpResult.success = false; httpResult.done = true;
      vTaskDelete(NULL); return;
    }

    JsonDocument sttDoc;
    deserializeJson(sttDoc, sttResp);
    String transcribed = sttDoc["text"] | "";
    Serial.printf("[STT] '%s'\n", transcribed.c_str());

    if (transcribed.length() == 0) {
      Serial.println("[STT] Empty — nothing understood.");
      httpResult.success = false; httpResult.done = true;
      vTaskDelete(NULL); return;
    }
    strncpy(pendingQuery, transcribed.c_str(), sizeof(pendingQuery));
  }

  // ── Step 2: Ask Gemini via /ask ──────────────────────────────────────
  Serial.printf("[Ask] '%s'\n", pendingQuery);

  HTTPClient ask;
  ask.begin(url_ask);
  ask.addHeader("Content-Type", "application/json");
  ask.setTimeout(12000);

  JsonDocument reqDoc;
  reqDoc["query"] = pendingQuery;
  String reqBody;
  serializeJson(reqDoc, reqBody);

  int    askCode = ask.POST(reqBody);
  String askResp = (askCode == 200) ? ask.getString() : "";
  ask.end();

  if (askCode != 200 || askResp.isEmpty()) {
    Serial.printf("[Ask] Failed: %d\n", askCode);
    httpResult.success = false; httpResult.done = true;
    vTaskDelete(NULL); return;
  }

  JsonDocument respDoc;
  if (deserializeJson(respDoc, askResp)) {
    Serial.println("[Ask] JSON parse fail.");
    httpResult.success = false; httpResult.done = true;
    vTaskDelete(NULL); return;
  }

  strncpy(httpResult.speech,     respDoc["speech"]     | "",     sizeof(httpResult.speech));
  strncpy(httpResult.expression, respDoc["expression"] | "IDLE", sizeof(httpResult.expression));
  Serial.printf("[Gemini] %s | %s\n", httpResult.expression, httpResult.speech);

  // ── Step 3: Fetch TTS audio ──────────────────────────────────────────
  HTTPClient tts;
  tts.begin(url_tts);
  tts.addHeader("Content-Type", "application/json");
  tts.setTimeout(10000);

  JsonDocument ttsReq;
  ttsReq["text"] = httpResult.speech;
  String ttsBody;
  serializeJson(ttsReq, ttsBody);

  int ttsCode = tts.POST(ttsBody);
  if (ttsCode == 200) {
    int len = tts.getSize();
    if (len > 0 && len <= (int)sizeof(audioBuffer)) {
      WiFiClient* stream = tts.getStreamPtr();
      audioBufferBytes   = stream->readBytes(audioBuffer, len);
      httpResult.hasAudio = (audioBufferBytes >= 44);
      Serial.printf("[TTS] %d bytes received.\n", audioBufferBytes);
    } else {
      Serial.printf("[TTS] Bad size: %d\n", len);
      httpResult.hasAudio = false;
    }
  } else {
    Serial.printf("[TTS] Failed: %d — face only.\n", ttsCode);
    httpResult.hasAudio = false;
    audioBufferBytes    = 0;
  }
  tts.end();

  httpResult.success = true;
  httpResult.done    = true;   // signal loop() — must be the LAST write
  vTaskDelete(NULL);
}

// =====================================================================
// 16. FIRE CANCEL
// =====================================================================
void fireCancel() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(url_cancel);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);
  http.POST("{}");
  http.end();
  Serial.println("[Cancel] /cancel sent.");
}

// =====================================================================
// 17. SENSOR & ANIMATION HELPERS
// =====================================================================
int getDistance() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long d = pulseIn(ECHO_PIN, HIGH, 30000);
  return (d == 0) ? 999 : (int)(d / 58);
}

void handleIdleAnimation() {
  unsigned long now = millis();
  if (now - lastBlinkTime > BLINK_INTERVAL_MS && !isSlouching) {
    tft.fillCircle(80,  100, 30, GC9A01A_BLACK);
    tft.fillCircle(160, 100, 30, GC9A01A_BLACK);
    tft.fillRect(50, 95, 60, 10, GC9A01A_CYAN);
    tft.fillRect(130, 95, 60, 10, GC9A01A_CYAN);
    delay(150);
    drawFace("IDLE");
    lastBlinkTime = now;
  }
}

// Posture: 60s sustained in 5–30cm range → CONCERNED face
void handleErgonomics(int distance) {
  unsigned long now = millis();
  if (distance > WAKE_ZONE_CM && distance < POSTURE_ZONE_CM) {
    if (!isSlouching) {
      postureViolationStart = now;
      isSlouching = true;
    } else if (now - postureViolationStart > POSTURE_WARNING_MS) {
      if (currentState == IDLE) drawFace("CONCERNED");
    }
  } else {
    if (isSlouching) {
      isSlouching = false;
      if (currentState == IDLE) drawFace("IDLE");
    }
  }
}

// Timekeeper: 10–100cm = user at normal desk distance → Pomodoro session
void handleTimekeeper(int distance) {
  unsigned long now = millis();
  if (distance >= TIMEKEEPER_MIN_CM && distance <= TIMEKEEPER_MAX_CM) {
    if (!studySessionActive) {
      studySessionStart  = now;
      studySessionActive = true;
      pomodoroAlerted    = false;
      Serial.println("[Timer] Study session started.");
    }
    unsigned long elapsed = now - studySessionStart;
    if (!pomodoroAlerted && elapsed >= POMODORO_MS) {
      pomodoroAlerted = true;
      Serial.println("[Timer] 25 min — time for a break!");
      if (currentState == IDLE) {
        tft.fillCircle(120, 175, 8, GC9A01A_ORANGE);
        delay(600);
        drawFace(isSlouching ? "CONCERNED" : "IDLE");
      }
    }
    displayStudyTime(elapsed);
  } else if (distance > TIMEKEEPER_MAX_CM && studySessionActive) {
    studySessionActive = false;
    Serial.printf("[Timer] Paused at %lu min.\n",
                  (now - studySessionStart) / 60000UL);
  }
}

void displayStudyTime(unsigned long elapsedMs) {
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 30000) return;    // redraw every 30s
  lastDraw = millis();
  tft.setTextColor(GC9A01A_GREEN, GC9A01A_BLACK);
  tft.setTextSize(1);
  tft.setCursor(95, 200);
  tft.printf("%02dm", (int)(elapsedMs / 60000));
}

// =====================================================================
// 18. DISPLAY
// =====================================================================
void drawFace(const char* expression) {
  tft.fillScreen(GC9A01A_BLACK);
  int lx = 80, rx = 160, ey = 100, r = 30;

  if      (strcmp(expression, "IDLE") == 0) {
    tft.fillCircle(lx, ey, r, GC9A01A_CYAN);
    tft.fillCircle(rx, ey, r, GC9A01A_CYAN);
  }
  else if (strcmp(expression, "HAPPY") == 0) {
    tft.fillCircle(lx, ey, r, GC9A01A_CYAN);
    tft.fillCircle(rx, ey, r, GC9A01A_CYAN);
    tft.fillRect(50, ey, 140, 40, GC9A01A_BLACK);
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
    tft.fillRect(50,  98, 60, 4, GC9A01A_CYAN);
    tft.fillRect(130, 98, 60, 4, GC9A01A_CYAN);
  }
}

// =====================================================================
// 19. INIT HELPERS
// =====================================================================
void connectToWiFi() {
  tft.setTextColor(GC9A01A_CYAN); tft.setTextSize(2);
  tft.setCursor(60, 110); tft.print("LINKING...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  tft.fillScreen(GC9A01A_BLACK);
  Serial.println("\n[WiFi] Connected.");
}

void initMicrophone() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = MIC_SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8, .dma_buf_len = 64,
    .use_apll = false, .tx_desc_auto_clear = false, .fixed_mclk = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK, .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = I2S_SD
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  Serial.println("[I2S] Mic ready (16kHz).");
}

void initSpeaker() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
    .sample_rate          = SPK_SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8, .dma_buf_len = 128,
    .use_apll = false, .tx_desc_auto_clear = true, .fixed_mclk = 0
  };
  i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
  i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN);   // GPIO25 → PAM8403
  Serial.println("[I2S] Speaker ready (8kHz, GPIO25).");
}