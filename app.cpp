// #include <WiFi.h>
// #include <HTTPClient.h>
// #include <ArduinoJson.h>
// #include <SPI.h>
// #include <Adafruit_GFX.h>
// #include <Adafruit_GC9A01A.h>
// #include <driver/i2s.h>

// // ==========================================
// // 1. HARDWARE PIN DEFINITIONS
// // ==========================================
// // GC9A01 Round Display Pins
// #define TFT_CS   5
// #define TFT_DC   2
// #define TFT_RST  4
// // SPI defaults: SCL/SCK = 18, SDA/MOSI = 23

// // HC-SR04 Ultrasonic Pins
// #define TRIG_PIN 13
// #define ECHO_PIN 12

// // INMP441 I2S Microphone Pins
// #define I2S_WS   15
// #define I2S_SCK  14
// #define I2S_SD   32

// // ==========================================
// // 2. NETWORK & API CONFIGURATION
// // ==========================================
// const char* ssid = "YOUR_WIFI_NAME";
// const char* password = "YOUR_WIFI_PASSWORD";
// const char* flask_server_url = "http://YOUR_LAPTOP_IP:5000/ask"; 

// // ==========================================
// // 3. STATE MACHINE & GLOBALS
// // ==========================================
// enum BuddyState { IDLE, LISTENING, THINKING, SPEAKING, CONFUSED, ERROR_STATE };
// BuddyState currentState = IDLE;

// Adafruit_GC9A01A tft = Adafruit_GC9A01A(TFT_CS, TFT_DC, TFT_RST);

// unsigned long lastBlinkTime = 0;
// unsigned long postureViolationTime = 0;
// bool isSlouching = false;
// String currentExpression = "IDLE";

// void setup() {
//   Serial.begin(115200);
  
//   // Init Sensor
//   pinMode(TRIG_PIN, OUTPUT);
//   pinMode(ECHO_PIN, INPUT);

//   // Init Display
//   tft.begin();
//   tft.setRotation(0); 
//   tft.fillScreen(GC9A01A_BLACK);
  
//   // Init Wi-Fi
//   connectToWiFi();

//   // Init I2S Microphone
//   initMicrophone();
  
//   // Boot Animation Complete
//   drawFace("IDLE");
// }

// void loop() {
//   // Non-blocking distance poll
//   int distance = getDistance();

//   // ==========================================
//   // THE FINITE STATE MACHINE
//   // ==========================================
//   switch (currentState) {
    
//     case IDLE:
//       handleErgonomics(distance);
//       handleIdleAnimation();
      
//       // Wake Gesture trigger (Hand wave under 5cm)
//       if (distance > 0 && distance < 5) {
//         currentState = LISTENING;
//         drawFace("THINKING"); // Look attentive
//         delay(500); // Debounce the hand wave
//       }
//       break;

//     case LISTENING:
//       Serial.println("Capturing Audio...");
//       // *Insert I2S DMA buffer read loop here*
//       // For now, we simulate capturing a voice query:
//       delay(2000); 
//       String testQuery = "What is a pointer in C++?";
      
//       pingCloud(testQuery); 
//       break;

//     case THINKING:
//       // In a multi-threaded RTOS build, you'd pulse the eyes here.
//       // Currently handled inside pingCloud() awaiting the HTTP response.
//       break;

//     case SPEAKING:
//       // The JSON has been parsed and the face is drawn.
//       // *Insert PAM8403 Audio Playback here*
//       delay(4000); // Simulate the time it takes to speak the answer
      
//       // Return to resting state
//       tft.fillScreen(GC9A01A_BLACK);
//       currentState = IDLE;
//       drawFace("IDLE");
//       break;

//     case CONFUSED:
//     case ERROR_STATE:
//       delay(3000); // Hold the error face for 3 seconds
//       tft.fillScreen(GC9A01A_BLACK);
//       currentState = IDLE;
//       drawFace("IDLE");
//       break;
//   }
// }

// // ==========================================
// // CORE FUNCTIONS
// // ==========================================

// void connectToWiFi() {
//   tft.setTextColor(GC9A01A_CYAN);
//   tft.setTextSize(2);
//   tft.setCursor(60, 110);
//   tft.print("LINKING...");
  
//   WiFi.begin(ssid, password);
//   while (WiFi.status() != WL_CONNECTED) {
//     delay(500);
//     Serial.print(".");
//   }
//   tft.fillScreen(GC9A01A_BLACK);
// }

// int getDistance() {
//   digitalWrite(TRIG_PIN, LOW);
//   delayMicroseconds(2);
//   digitalWrite(TRIG_PIN, HIGH);
//   delayMicroseconds(10);
//   digitalWrite(TRIG_PIN, LOW);
  
//   long duration = pulseIn(ECHO_PIN, HIGH, 30000); // 30ms timeout
//   if (duration == 0) return 999;
//   return duration / 58;
// }

// void pingCloud(String query) {
//   currentState = THINKING;
  
//   if (WiFi.status() == WL_CONNECTED) {
//     HTTPClient http;
//     http.begin(flask_server_url);
//     http.addHeader("Content-Type", "application/json");

//     // Build the JSON payload to send to Flask
//     JsonDocument outDoc;
//     outDoc["query"] = query;
//     String requestBody;
//     serializeJson(outDoc, requestBody);

//     Serial.println("Sending to Flask: " + requestBody);
//     int httpResponseCode = http.POST(requestBody);

//     if (httpResponseCode > 0) {
//       String responseStr = http.getString();
//       Serial.println("Received: " + responseStr);
      
//       // Parse the incoming JSON from Gemini
//       JsonDocument inDoc;
//       DeserializationError error = deserializeJson(inDoc, responseStr);

//       if (!error) {
//         String speech = inDoc["speech"].as<String>();
//         String expression = inDoc["expression"].as<String>();
        
//         Serial.println("Buddy says: " + speech);
//         drawFace(expression);
//         currentState = SPEAKING;
        
//       } else {
//         drawFace("CONFUSED");
//         currentState = CONFUSED;
//       }
//     } else {
//       drawFace("ERROR");
//       currentState = ERROR_STATE;
//     }
//     http.end();
//   } else {
//     drawFace("ERROR");
//     currentState = ERROR_STATE;
//   }
// }

// // ==========================================
// // UI & EMOTION ENGINE (Math for a Round Screen)
// // ==========================================

// void drawFace(String expression) {
//   tft.fillScreen(GC9A01A_BLACK);
//   int leftEyeX = 80;
//   int rightEyeX = 160;
//   int eyeY = 100;
//   int radius = 30;

//   if (expression == "IDLE" || expression == "HAPPY") {
//     tft.fillCircle(leftEyeX, eyeY, radius, GC9A01A_CYAN);
//     tft.fillCircle(rightEyeX, eyeY, radius, GC9A01A_CYAN);
//     if (expression == "HAPPY") {
//       // Cut the bottom half off to make arches
//       tft.fillRect(50, eyeY, 140, 40, GC9A01A_BLACK); 
//     }
//   } 
//   else if (expression == "CONCERNED" || expression == "ANGRY") {
//     tft.fillCircle(leftEyeX, eyeY, radius, GC9A01A_RED);
//     tft.fillCircle(rightEyeX, eyeY, radius, GC9A01A_RED);
//     // Draw angry inward eyebrows
//     tft.fillTriangle(40, 60, 110, 60, 110, 100, GC9A01A_BLACK); 
//     tft.fillTriangle(200, 60, 130, 60, 130, 100, GC9A01A_BLACK);
//   }
//   else if (expression == "THINKING") {
//     tft.fillCircle(leftEyeX, eyeY - 20, radius - 10, GC9A01A_ORANGE);
//     tft.fillCircle(rightEyeX, eyeY - 20, radius - 10, GC9A01A_ORANGE);
//   }
//   else if (expression == "CONFUSED") {
//     tft.fillCircle(leftEyeX, eyeY, radius, GC9A01A_MAGENTA);
//     tft.fillCircle(rightEyeX, eyeY, radius - 15, GC9A01A_MAGENTA); // One small eye
//   }
//   else if (expression == "ERROR") {
//     tft.drawLine(leftEyeX-20, eyeY-20, leftEyeX+20, eyeY+20, GC9A01A_RED);
//     tft.drawLine(leftEyeX-20, eyeY+20, leftEyeX+20, eyeY-20, GC9A01A_RED);
//     tft.drawLine(rightEyeX-20, eyeY-20, rightEyeX+20, eyeY+20, GC9A01A_RED);
//     tft.drawLine(rightEyeX-20, eyeY+20, rightEyeX+20, eyeY-20, GC9A01A_RED);
//   }
// }

// void handleIdleAnimation() {
//   if (millis() - lastBlinkTime > 4000 && !isSlouching) {
//     tft.fillCircle(80, 100, 30, GC9A01A_BLACK);
//     tft.fillCircle(160, 100, 30, GC9A01A_BLACK);
//     tft.fillRect(50, 95, 60, 10, GC9A01A_CYAN);
//     tft.fillRect(130, 95, 60, 10, GC9A01A_CYAN);
//     delay(150);
//     drawFace("IDLE");
//     lastBlinkTime = millis();
//   }
// }

// void handleErgonomics(int distance) {
//   // If user is slouching (< 30cm) but not triggering the wake gesture (> 5cm)
//   if (distance > 5 && distance < 30) {
//     if (!isSlouching) {
//       postureViolationTime = millis();
//       isSlouching = true;
//     } else if (millis() - postureViolationTime > 5000) { // 5 seconds of slouching
//       if (currentExpression != "CONCERNED") {
//         drawFace("CONCERNED");
//         currentExpression = "CONCERNED";
//       }
//     }
//   } else {
//     if (isSlouching) {
//       isSlouching = false;
//       drawFace("IDLE");
//       currentExpression = "IDLE";
//     }
//   }
// }

// void initMicrophone() {
//   i2s_config_t i2s_config = {
//     .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
//     .sample_rate = 16000,
//     .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
//     .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
//     .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
//     .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
//     .dma_buf_count = 8,
//     .dma_buf_len = 64,
//     .use_apll = false,
//     .tx_desc_auto_clear = false,
//     .fixed_mclk = 0
//   };
  
//   i2s_pin_config_t pin_config = {
//     .bck_io_num = I2S_SCK,
//     .ws_io_num = I2S_WS,
//     .data_out_num = I2S_PIN_NO_CHANGE,
//     .data_in_num = I2S_SD
//   };
  
//   i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
//   i2s_set_pin(I2S_NUM_0, &pin_config);
// }