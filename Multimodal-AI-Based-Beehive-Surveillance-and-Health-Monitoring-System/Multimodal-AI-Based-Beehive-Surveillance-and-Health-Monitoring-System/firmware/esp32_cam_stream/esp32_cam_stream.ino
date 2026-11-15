/*
 * ESP32-CAM live video streaming for the hive entrance.
 * Streams MJPEG at http://<IP>:81/stream; YOLO inference runs on a laptop
 * (see threat_detection/detector.py).
 *
 * Needs the CameraWebServer example files (app_httpd.cpp, board_config.h,
 * camera_pins.h) from the Espressif esp32 Arduino core in the same sketch folder.
 * Copy secrets.h.example to secrets.h and fill in your Wi-Fi details.
 */

#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>

#include "board_config.h"
#include "secrets.h"   // WIFI_SSID, WIFI_PASSWORD

const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;

// Provided by the CameraWebServer example files
void startCameraServer();
void setupLedFlash();

bool initializeCamera() {
  camera_config_t config;

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;

  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_VGA;
  config.pixel_format = PIXFORMAT_JPEG;

  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 10;
  config.fb_count = 2;

  if (!psramFound()) {
    Serial.println("WARNING: PSRAM not found. Using DRAM and one frame buffer.");
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
    config.jpeg_quality = 12;
  } else {
    Serial.println("PSRAM found.");
  }

  Serial.println("Initializing camera...");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera initialization failed: 0x%x\n", err);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    Serial.println("Camera sensor not found.");
    return false;
  }

  sensor->set_framesize(sensor, FRAMESIZE_VGA);

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  sensor->set_vflip(sensor, 1);
  sensor->set_hmirror(sensor, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  sensor->set_vflip(sensor, 1);
#endif

#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  Serial.println("Camera initialized successfully.");
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  delay(500);

  Serial.println();
  Serial.println("ESP32-CAM Honeybee Monitoring - live video streaming only");

  if (!initializeCamera()) {
    Serial.println("Camera setup failed. Restarting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }

  Serial.println("Connecting to Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - wifiStart > 30000UL) {
      Serial.println("\nWi-Fi connection timed out. Restarting...");
      delay(1000);
      ESP.restart();
    }
  }

  Serial.println("\nWi-Fi connected successfully.");
  Serial.print("ESP32-CAM IP address: ");
  Serial.println(WiFi.localIP());

  startCameraServer();

  Serial.print("Camera webpage: http://");
  Serial.println(WiFi.localIP());
  Serial.print("Video stream URL: http://");
  Serial.print(WiFi.localIP());
  Serial.println(":81/stream");
  Serial.println("Python on the laptop handles detection, counting, alerts and Adafruit IO publishing.");
}

void loop() {
  // The camera web server streams video; Python handles all AI detection.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi disconnected. Reconnecting...");
    WiFi.reconnect();
    delay(1000);
  } else {
    delay(100);
  }
}
