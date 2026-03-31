#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>

const char* ssid = "D's";
const char* password = "00000000";

const char* checkUrl = "http://192.168.73.178:5000/should_capture";
const char* uploadUrl = "http://192.168.73.178:5000/upload";

// AI Thinker pins
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

void startCamera() {
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
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    while (true) delay(1000);
  }
}

bool connectWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 30) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("Failed");
  return false;
}

bool shouldCapture() {
  HTTPClient http;
  http.begin(checkUrl);
  int code = http.GET();

  if (code == 200) {
    String res = http.getString();
    http.end();
    return res == "1";
  }

  http.end();
  return false;
}

void sendImage() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Capture failed");
    return;
  }

  HTTPClient http;
  http.begin(uploadUrl);
  http.addHeader("Content-Type", "image/jpeg");

  int code = http.POST(fb->buf, fb->len);

  Serial.print("HTTP code: ");
  Serial.println(code);

  if (code > 0) {
    String res = http.getString();
    Serial.println("Detected:");
    Serial.println(res);
  } else {
    Serial.print("POST failed: ");
    Serial.println(http.errorToString(code));
  }

  http.end();
  esp_camera_fb_return(fb);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  startCamera();
  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (shouldCapture()) {
    Serial.println("Capture requested...");
    sendImage();
  }

  delay(700);
}