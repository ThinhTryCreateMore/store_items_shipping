// ESP32-S3: CHỈ dùng vân tay R558S để LƯU (đăng ký) và XÁC NHẬN vân tay qua server
// Đã bỏ: servo, LED, HC-SR04, logic ngăn tủ. Giữ lại: buzzer (báo hiệu khi đăng ký/xác nhận).
// Thư viện: Adafruit Fingerprint Sensor Library (>= 2.1.0), ArduinoJson (v7)
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_Fingerprint.h>

// ====== SỬA ======
const char* WIFI_SSID = "The Sun L1";
const char* WIFI_PASS = "88888888";
const char* SERVER    = "http://192.168.1.46:8000";  // IP máy chạy server

// ====== CHÂN ======
#define FP_RX_PIN  17   // nối vào TX của cảm biến vân tay
#define FP_TX_PIN  16   // nối vào RX của cảm biến vân tay
#define BUZZER_PIN 11
#define BUZZER_ON  HIGH // module buzzer kích mức thấp thì đổi thành LOW

// ====== THÔNG SỐ ======
// Kết quả test R558S: chỉ lưu được template khi gộp 4 lần quét
// (GenChar buf 1..4 -> RegModel -> Store buf1). Gộp 2 lần quét thì Store trả 0x35.
#define ENROLL_SCANS 4
const uint32_t SYNC_MS = 700, FP_MS = 200;

// Đang đăng ký vân tay: hàm này chạy lâu (vài chục giây) nên phải giữ kết nối với server ở chế độ nền
bool enrolling = false;
String deferred[4];   // lệnh server gửi xuống lúc đang bận đăng ký, xử lý sau
int ndef = 0;
void background();

Adafruit_Fingerprint finger(&Serial1);

// ---------- Buzzer ----------
void beepN(int n, int ms = 120) {
  for (int k = 0; k < n; k++) {
    digitalWrite(BUZZER_PIN, BUZZER_ON); delay(ms);
    digitalWrite(BUZZER_PIN, !BUZZER_ON); delay(ms);
  }
}

// ---------- Giao tiếp server ----------
bool postJson(const String& path, const String& body, String& resp) {
  HTTPClient http;
  http.begin(String(SERVER) + path);
  http.setTimeout(3000);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (code == 200) resp = http.getString();
  http.end();
  return code == 200;
}

void waitRelease(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (finger.getImage() != FINGERPRINT_NOFINGER && millis() - t0 < timeoutMs) {
    if (enrolling) background();
    delay(50);
  }
}

// Trả về mã lỗi của cảm biến (0 = OK). 0xFF = hết giờ mà chưa thấy ngón tay
uint8_t captureTz(uint8_t slot, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  uint8_t err = 0xFF;
  while (millis() - t0 < timeoutMs) {
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      p = finger.image2Tz(slot);
      if (p == FINGERPRINT_OK) return FINGERPRINT_OK;
      err = p;                                   // ảnh xấu (nhòe/ướt/chạm hụt): cho thử lại
      Serial.printf("[VANTAY] anh xau, ma loi 0x%02X - nhac tay roi dat lai\n", p);
      beepN(1, 40);
      waitRelease(3000);
    } else if (p != FINGERPRINT_NOFINGER) {
      err = p;                                   // lỗi giao tiếp với cảm biến
    }
    if (enrolling) background();
    delay(30);
  }
  return err;
}

// Đăng ký vân tay: quét ENROLL_SCANS lần (1 bíp = đặt tay, 2 bíp = nhấc tay ra, rồi đặt lại cùng ngón,
// mỗi lần lệch nhẹ vị trí). Thử tối đa 2 lượt.
void enrollFinger(int userId, int fid) {
  const char* reason = "";
  bool ok = false;
  bool idBad = finger.capacity > 0 && fid >= (int)finger.capacity;   // ID phải nằm trong sức chứa của cảm biến
  if (idBad) { reason = "id_range"; Serial.printf("[VANTAY] ID %d vuot suc chua %d cua cam bien\n", fid, finger.capacity); }
  enrolling = true;
  for (int attempt = 0; !idBad && attempt < 2 && !ok; attempt++) {
    bool scansOk = true;
    for (uint8_t n = 1; n <= ENROLL_SCANS; n++) {
      beepN(1);
      Serial.printf("[VANTAY] lan %u/%u: dat tay len cam bien\n", n, ENROLL_SCANS);
      uint8_t e = captureTz(n, 15000);
      if (e != FINGERPRINT_OK) {
        reason = (e == 0xFF || e == 0x01) ? "no_finger" : "bad_image";
        Serial.printf("[VANTAY] lan %u that bai, ma 0x%02X (%s)\n", n, e, reason);
        scansOk = false;
        break;
      }
      if (n < ENROLL_SCANS) { beepN(2); waitRelease(8000); delay(500); }
    }
    if (!scansOk) continue;

    uint8_t e = finger.createModel();
    Serial.printf("[VANTAY] createModel ma 0x%02X\n", e);
    if (e != FINGERPRINT_OK) {
      reason = (e == FINGERPRINT_ENROLLMISMATCH) ? "mismatch" : "model_fail";
      Serial.printf("[VANTAY] tao mau that bai, ma 0x%02X (%s)\n", e, reason);
      beepN(2, 60);
      continue;
    }
    e = 0xFF;
    for (int t = 0; t < 3 && e != FINGERPRINT_OK; t++) {      // ghi flash đôi khi lỗi lần đầu: thử lại tối đa 3 lần
      e = finger.storeModel(fid);                              // lưu từ CharBuffer1
      if (e != FINGERPRINT_OK) { Serial.printf("[VANTAY] luu mau ID %d loi, ma 0x%02X (lan %d)\n", fid, e, t + 1); delay(300); }
    }
    if (e != FINGERPRINT_OK) {
      reason = (e == 0x0B || e == 0x0C) ? "id_range" : (e == 0x18) ? "flash_fail" : "store_fail";
      break;                                     // lỗi lưu: không cần quét lại
    }
    ok = true;
  }
  if (ok) waitRelease(8000);                     // chờ nhấc tay để không bị đọc thành lần xác nhận
  enrolling = false;
  if (ok) { Serial.printf("[VANTAY] dang ky OK, ID %d\n", fid); beepN(1, 400); }
  else beepN(3, 80);
  JsonDocument d; d["user_id"] = userId; d["finger_id"] = fid; d["ok"] = ok; d["reason"] = ok ? "" : reason;
  String b, r; serializeJson(d, b);
  postJson("/api/finger/enroll_result", b, r);
}

void handleCmd(JsonObject c) {
  String cmd = (const char*)(c["cmd"] | "");
  if (cmd == "enroll_finger") {
    enrollFinger(c["user_id"], c["finger_id"]);
  } else if (cmd == "delete_finger") {
    finger.deleteModel((int)c["finger_id"]);
  }
}

// Gửi trạng thái lên server và nhận lệnh (đăng ký / xoá vân tay) trả về
void syncServer() {
  JsonDocument d;
  d["lockers"].to<JsonArray>();                  // để trống: giữ nguyên định dạng cũ cho server
  d["events"].to<JsonArray>();
  String body, resp; serializeJson(d, body);
  if (!postJson("/api/device/sync", body, resp)) return;
  JsonDocument r;
  if (deserializeJson(r, resp)) return;
  for (JsonObject c : r["cmds"].as<JsonArray>()) {
    if (enrolling && ndef < 4) { String t; serializeJson(c, t); deferred[ndef++] = t; }   // đang bận: để sau
    else handleCmd(c);
  }
}

void background() {   // việc nền khi đang chờ vân tay: giữ kết nối với server
  static uint32_t tY = 0;
  if (millis() - tY >= SYNC_MS) { syncServer(); tY = millis(); }
}

// ---------- Xác nhận vân tay ----------
void checkFinger() {
  if (finger.getImage() != FINGERPRINT_OK) return;     // chưa có ngón tay
  if (finger.image2Tz(1) != FINGERPRINT_OK) return;
  int id = 0, conf = 0;
  uint8_t sr = finger.fingerSearch(1);                 // Search chuẩn (0x04) trên CharBuffer1
  Serial.printf("[VANTAY] search ma 0x%02X\n", sr);
  if (sr == FINGERPRINT_OK) { id = finger.fingerID; conf = finger.confidence; }
  JsonDocument d; d["finger_id"] = id; d["confidence"] = conf;
  String b, resp; serializeJson(d, b);
  bool match = false;
  if (postJson("/api/finger/verify", b, resp)) {
    JsonDocument r;
    if (!deserializeJson(r, resp)) {
      String status = (const char*)(r["status"] | "");
      match = (status == "match");
      Serial.printf("[VANTAY] server tra ve: %s (finger_id %d, do tin cay %d)\n", status.c_str(), id, conf);
    }
  } else {
    Serial.println("[VANTAY] khong gui duoc len server");
  }
  if (match) beepN(1); else beepN(3, 80);
  waitRelease(5000);
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, !BUZZER_ON);
  Serial1.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(57600);
  delay(1000);                                    // chờ module khởi động
  bool fpOk = false;
  for (int i = 1; i <= 5 && !fpOk; i++) {         // thử lại vài lần phòng module khởi động chậm
    fpOk = finger.verifyPassword();
    Serial.printf("[VANTAY] verifyPassword lan %d: %s\n", i, fpOk ? "OK" : "khong tra loi");
    if (!fpOk) delay(400);
  }
  if (fpOk) {
    Serial.println("Cam bien van tay OK");
    finger.getParameters();
    if (finger.capacity == 0) finger.capacity = 100;   // R558S: 100 mẫu
    finger.getTemplateCount();
    Serial.printf("[VANTAY] suc chua %d mau, dang luu %d mau\n", finger.capacity, finger.templateCount);
  } else {
    Serial.println("KHONG thay cam bien van tay (kiem tra TX/RX, nguon 3V3)");
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.printf("\nWiFi OK: %s\n", WiFi.localIP().toString().c_str());
}

void loop() {
  static uint32_t tSync = 0, tFp = 0;
  while (!enrolling && ndef > 0) {              // xử lý lệnh bị hoãn trong lúc đăng ký vân tay
    JsonDocument d;
    if (!deserializeJson(d, deferred[--ndef])) handleCmd(d.as<JsonObject>());
  }
  uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(500); }
  if (now - tFp >= FP_MS) { checkFinger(); tFp = millis(); }
  if (now - tSync >= SYNC_MS) { syncServer(); tSync = millis(); }
}
