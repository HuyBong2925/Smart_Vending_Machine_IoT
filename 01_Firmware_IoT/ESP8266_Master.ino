#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "I2CKeyPad.h" // SỬ DỤNG THƯ VIỆN CHUYÊN DỤNG (Rob Tillaart)
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>

//  Thêm để gọi HTTPS (Render / onrender.com) 
#include <WiFiClientSecureBearSSL.h>

// ======= I2C pins & Address =======
#define SDA_PIN D2
#define SCL_PIN D1
#define LCD_ADDR 0x27
#define PCF_ADDR 0x20  

// ======= Relay pins =======
#define R1_PIN D5
#define R2_PIN D6
#define R3_PIN D7
#define R4_PIN D8

// ======= Relay logic =======
const bool RELAY_ACTIVE_LOW = true;
#define R_ON  (RELAY_ACTIVE_LOW ? HIGH : LOW)
#define R_OFF (RELAY_ACTIVE_LOW ? LOW  : HIGH)

// ======= Business config =======
const uint32_t PUMP_TIME[4] = { 300, 300, 300, 300 };
const uint32_t PRICE[4] = { 10000, 10000, 10000, 10000 };

// ======= WiFi & Server =======
const char* ssid = "Bongg";
const char* password = "00000001";
const char* serverUrl = "http://192.168.1.X";  // AI/local server

//  Backend URL để trừ số lượng trên server (POST) 
const char* beUrl = "YOUR_BACKEND_API_URL_HERE/api/transactions";
// ======= Objects =======
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
I2CKeyPad keyPad(PCF_ADDR); // Dùng thư viện thay vì thủ công

// ======= Variables =======
long balance = 200000;
int  selectedSlot = -1;
enum State { IDLE, SELECTED, DISPENSING } state = IDLE;

unsigned long lastCheck = 0;
const unsigned long CHECK_INTERVAL = 5000; // 5 giây/lần
String lastTimestamp = "";

// ======= Helpers =======
void relaysAllOff() {
  digitalWrite(R1_PIN, R_OFF);
  digitalWrite(R2_PIN, R_OFF);
  digitalWrite(R3_PIN, R_OFF);
  digitalWrite(R4_PIN, R_OFF);
}

void setRelayBySlot(int slot, bool on) {
  int pin = (slot==0)?R1_PIN : (slot==1)?R2_PIN : (slot==2)?R3_PIN : R4_PIN;
  digitalWrite(pin, on ? R_ON : R_OFF);
}

String money(long v){
  String s = String(v);
  int n = s.length();
  for (int i=n-3; i>0; i-=3) s = s.substring(0,i) + "." + s.substring(i);
  return s;
}

void showIdle() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("So du: "); lcd.print(money(balance));
  lcd.setCursor(0,1); lcd.print("Chon 1-4, OK=5");
}

void showSelected() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Slot "); lcd.print(selectedSlot+1);
  lcd.print(" Gia "); lcd.print(money(PRICE[selectedSlot]));
  lcd.setCursor(0,1); lcd.print("OK=5  Huy=6");
}

void showMsg(const char* line1, const char* line2, uint16_t ms=1200) {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(line1);
  lcd.setCursor(0,1); if (line2) lcd.print(line2);
  delay(ms);
}

//  POST transaction lên BE để trừ số lượng trên server 
bool postTransactionToBE(int slot_id, int quantity) {
  if (WiFi.status() != WL_CONNECTED) return false;

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure(); // nhanh nhất để gọi HTTPS (không check cert)

  HTTPClient https;
  https.setTimeout(4000);

  if (!https.begin(*client, beUrl)) {
    https.end();
    return false;
  }

  https.addHeader("Content-Type", "application/json");

  StaticJsonDocument<128> doc;
  doc["slot_id"] = slot_id;
  doc["quantity"] = quantity;

  String body;
  serializeJson(doc, body);

  int code = https.POST(body);
  String resp = https.getString(); // debug nếu cần
  https.end();

  Serial.print("POST /api/transactions code: ");
  Serial.println(code);
  Serial.println(resp);

  return (code >= 200 && code < 300);
}

void tryDispense(int slot) {
  if (balance < (long)PRICE[slot]) {
    showMsg("Khong du tien!", "Huy giao dich", 1200);
    state = IDLE; selectedSlot = -1; showIdle();
    return;
  }

  // Trừ tiền trên máy trước
  balance -= PRICE[slot];

  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Dang cap nuoc...");
  lcd.setCursor(0,1); lcd.print("Con: "); lcd.print(money(balance));

  setRelayBySlot(slot, true);

  // Trong lúc bơm, vẫn giữ kết nối nhưng chặn việc khác
  unsigned long t0 = millis();
  while (millis() - t0 < PUMP_TIME[slot]) {
    yield(); // Giữ cho ESP không bị reset (WDT)
  }
  setRelayBySlot(slot, false);

  //  Sau khi cấp nước xong, gọi BE để trừ số lượng (slot_id: 1-4) 
  bool ok = postTransactionToBE(slot + 1, 1);

  if (!ok) {
    // vẫn coi là đã cấp nước rồi, nhưng báo để bạn biết server chưa cập nhật
    showMsg("Da cap nuoc", "BE chua cap nhat", 1200);
  } else {
    showMsg("Thanh cong!", ("So du " + money(balance)).c_str(), 1000);
  }

  state = IDLE; selectedSlot = -1; showIdle();
}

void handleSerialTopup() {
  if (Serial.available()) {
    String s = Serial.readStringUntil('\n'); s.trim();
    if (s == "+5") { balance += 5000; Serial.println("Topup +5k"); showIdle(); }
  }
}

void checkServerMoney() {
  // Chỉ kiểm tra Wifi khi có mạng
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;

  // Timeout ngắn lại để tránh treo máy quá lâu nếu server lỗi
  http.setTimeout(2000);

  http.begin(client, serverUrl);
  int httpCode = http.GET(); // Lệnh này làm máy khựng lại

  if (httpCode > 0) {
    String payload = http.getString();
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      const char* status = doc["status"] | "";
      String denominationStr = doc["denomination"] | "0";
      String timestamp = doc["timestamp"] | "";

      if (strcmp(status, "OK") == 0 && timestamp.length() > 0 && timestamp != lastTimestamp) {
        long value = denominationStr.toInt();
        if (value > 0) {
          balance = value;
          lastTimestamp = timestamp;
          lcd.clear();
          lcd.setCursor(0,0); lcd.print("Nhan: "); lcd.print(money(value));
          lcd.setCursor(0,1); lcd.print("So du: "); lcd.print(money(balance));
          delay(1500);

          // Nếu đang chọn dở mà tiền về, vẫn quay lại màn hình chọn
          if (state == SELECTED) showSelected();
          else showIdle();
        }
      }
    }
  }
  http.end();
}

// ======= Setup =======
void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  lcd.init(); lcd.backlight();
  lcd.clear(); lcd.print("Khoi tao...");

  // --- START KEYPAD ---
  // Sử dụng thư viện I2CKeyPad đã test thành công
  if (keyPad.begin() == false) {
    Serial.println("ERROR: Keypad not found");
    lcd.setCursor(0,1); lcd.print("Loi Keypad!");
    while(1);
  }
  // Map phím: Hàng nối P0,P1 - Cột nối P4,P5,P6
  // Map tương ứng: 1,2,3 - 4,5,6
  keyPad.loadKeyMap("123N456NNNNNNNNN");

  pinMode(R1_PIN, OUTPUT);
  pinMode(R2_PIN, OUTPUT);
  pinMode(R3_PIN, OUTPUT);
  pinMode(R4_PIN, OUTPUT);
  relaysAllOff();

  WiFi.begin(ssid, password);
  lcd.clear(); lcd.print("Wifi Connecting");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500); retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear(); lcd.print("WiFi OK");
  } else {
    lcd.clear(); lcd.print("WiFi Failed");
  }
  delay(1000);
  showIdle();
}

// ======= Loop =======
void loop() {
  handleSerialTopup();

  unsigned long now = millis();

  // --- CHIẾN LƯỢC TRÁNH ĐƠ PHÍM ---
  // Chỉ kiểm tra Server khi máy đang RẢNH (IDLE)
  // Nếu khách đang chọn (SELECTED) thì KHÔNG check server để bàn phím mượt nhất
  if (state == IDLE && (now - lastCheck > CHECK_INTERVAL)) {
    lastCheck = now;
    checkServerMoney();
  }

  // --- QUÉT PHÍM DÙNG THƯ VIỆN ---
  if (keyPad.isPressed()) {
    char k = keyPad.getChar();

    if (k != 'N' && k != 0) {
      Serial.print("Key: "); Serial.println(k);

      // Reset thời gian check server để tránh vừa bấm xong máy lại đi check server ngay
      lastCheck = millis();

      if (k >= '1' && k <= '4') {
        selectedSlot = (k - '1');
        state = SELECTED;
        showSelected();
      }
      else if (k == '5') { // Phím 5 là OK (Xác nhận)
        if (selectedSlot == -1) {
          showMsg("Chua chon slot", "Chon 1-4", 800);
          showIdle();
        } else {
          state = DISPENSING;
          tryDispense(selectedSlot);
        }
      }
      else if (k == '6') { // Phím 6 là HỦY
        selectedSlot = -1;
        state = IDLE;
        showIdle();
      }
    }
    delay(100); // Chống dội phím nhẹ
  }
}