/******** ESP32-CAM: IR -> Delay -> Bật LED ngoài -> Chụp -> Tắt LED -> Gửi server -> Motor ********/
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_camera.h"

// ==== AI-Thinker Camera Pin Map ====
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

// ==== CẤU HÌNH LED NGOÀI ====
// Lưu ý: Không dùng GPIO 4 nếu muốn tắt flash trên bo mạch
// Chọn GPIO 12 (nằm ngay cạnh GPIO 13, 14 trên header)
#define LED_NGOAI_PIN_1 12   // 1 LED
#define LED_NGOAI_PIN_2 16   // 2 LED chung chân

// ===== CONFIG =====
const char* WIFI_SSID = "Bongg";
const char* WIFI_PASS = "00000001";
String SERVER_URL = "http://192.168.1.X";

const int PIN_IR    = 13;   // IR sensor
const int PIN_RELAY = 14;   // Motor relay

const bool SENSOR_ACTIVE_HIGH = false;

// Thời gian motor & delay
const uint32_t MOTOR_ON_MS = 1000;
const uint32_t DELAY_STABLE_MONEY   = 1500;
const uint32_t DELAY_AFTER_CAPTURE  = 200;
const uint32_t DELAY_AFTER_POST     = 250;
const uint32_t DELAY_BETWEEN_CYCLES = 2500;
const uint32_t LED_ON_BEFORE_SHOT   = 150;   // Thời gian bật đèn trước khi chụp (ms)

inline bool sensorTriggered(int raw){ return SENSOR_ACTIVE_HIGH ? (raw==HIGH):(raw==LOW); }
inline void relayOn()  { digitalWrite(PIN_RELAY, HIGH); }
inline void relayOff() { digitalWrite(PIN_RELAY, LOW); }

bool initCamera() {
  camera_config_t c;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0=Y2_GPIO_NUM;  c.pin_d1=Y3_GPIO_NUM;  c.pin_d2=Y4_GPIO_NUM;  c.pin_d3=Y5_GPIO_NUM;
  c.pin_d4=Y6_GPIO_NUM;  c.pin_d5=Y7_GPIO_NUM;  c.pin_d6=Y8_GPIO_NUM;  c.pin_d7=Y9_GPIO_NUM;
  c.pin_xclk=XCLK_GPIO_NUM; 
  c.pin_pclk=PCLK_GPIO_NUM; 
  c.pin_vsync=VSYNC_GPIO_NUM; 
  c.pin_href=HREF_GPIO_NUM;
  c.pin_sscb_sda=SIOD_GPIO_NUM; 
  c.pin_sscb_scl=SIOC_GPIO_NUM; 
  c.pin_pwdn=PWDN_GPIO_NUM; 
  c.pin_reset=RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;

  if(psramFound()){ c.frame_size=FRAMESIZE_VGA; c.jpeg_quality=12; c.fb_count=2; }
  else            { c.frame_size=FRAMESIZE_QVGA; c.jpeg_quality=15; c.fb_count=1; }

  return esp_camera_init(&c) == ESP_OK;
}

void postJpegRaw(const uint8_t* buf, size_t len){
  if (WiFi.status()!=WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, SERVER_URL)) return;

  http.setTimeout(15000);
  http.addHeader("Content-Type","image/jpeg");
  http.POST((uint8_t*)buf, len);
  http.end();
}

void setup(){
  Serial.begin(115200);

  pinMode(PIN_IR, SENSOR_ACTIVE_HIGH ? INPUT : INPUT_PULLUP);
  pinMode(PIN_RELAY, OUTPUT);
  relayOff();

// SETUP LED NGOÀI
pinMode(LED_NGOAI_PIN_1, OUTPUT);
pinMode(LED_NGOAI_PIN_2, OUTPUT);

digitalWrite(LED_NGOAI_PIN_1, LOW);
digitalWrite(LED_NGOAI_PIN_2, LOW);


  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint8_t t=0; 
  while (WiFi.status()!=WL_CONNECTED && t<40){ delay(250); t++; }

  initCamera();
}

void loop(){
  if (sensorTriggered(digitalRead(PIN_IR))) {

    delay(DELAY_STABLE_MONEY);

    // === 1. BẬT LED NGOÀI TRƯỚC KHI CHỤP ===
    digitalWrite(LED_NGOAI_PIN_1, HIGH);
    digitalWrite(LED_NGOAI_PIN_2, HIGH);
    delay(LED_ON_BEFORE_SHOT); // Chờ đèn sáng ổn định

    // === 2. CHỤP ẢNH ===
    camera_fb_t* fb = esp_camera_fb_get();

    // === 3. TẮT LED NGAY SAU KHI CHỤP XONG ===
    digitalWrite(LED_NGOAI_PIN_1, LOW);
    digitalWrite(LED_NGOAI_PIN_2, LOW);

    if (fb) {
      // Có thể thêm delay nhỏ nếu cần xử lý ảnh
      delay(DELAY_AFTER_CAPTURE);

      // === 4. GỬI LÊN SERVER ===
      postJpegRaw(fb->buf, fb->len);

      esp_camera_fb_return(fb);

      delay(DELAY_AFTER_POST);
    }

    // === 5. CHẠY MOTOR THU TIỀN ===
    relayOn();
    delay(MOTOR_ON_MS);
    relayOff();

    // === 6. THỜI GIAN NGHỈ ===
    delay(DELAY_BETWEEN_CYCLES);
  }

  delay(10);
}