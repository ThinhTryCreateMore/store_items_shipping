// ESP32-S3 WROOM N16R8 CAM: xem trước trên TFT ST7789, chụp và gửi JPEG (kèm SHA-256) lên server
// Thư viện: ArduinoJson (v7), Adafruit GFX Library, Adafruit ST7789 Library, TJpg_Decoder (Bodmer)
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <TJpg_Decoder.h>
#include "esp_camera.h"
#include "mbedtls/sha256.h"
#include "types.h"

// ====== SỬA ======
const char* WIFI_SSID = "REDMI Turbo 4 Pro";
const char* WIFI_PASS = "12072006";
const char* SERVER_URL = "http://10.77.196.245:8000/api/recognize";
const char* DEVICE_ID  = "cam-01";

#define BTN_PIN 0  // nút BOOT (đánh thức/bắt đầu quét)

// ====== TFT 1.54" ST7789 240x240 (đổi chân nếu board bạn khác) ======
#define TFT_SCK  39
#define TFT_MOSI 40
#define TFT_DC   41
#define TFT_RST  42
#define TFT_HAS_CS 0   // 0: module 7 chân (không có CS) | 1: module có chân CS
#define TFT_CS   38
// BL (đèn nền) nối thẳng 3V3; nếu muốn tắt đèn nền thì nối vào 1 GPIO

// Chân camera kiểu ESP32-S3-EYE / Freenove. KIỂM TRA lại với sơ đồ của người bán!
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM   4
#define SIOC_GPIO_NUM   5
#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM     8
#define Y3_GPIO_NUM     9
#define Y2_GPIO_NUM    11
#define VSYNC_GPIO_NUM  6
#define HREF_GPIO_NUM   7
#define PCLK_GPIO_NUM  13

const uint32_t SESSION_MS = 12000;  // tối đa 12s cho mỗi lần quét
const uint32_t AIM_MS     = 2500;   // 2.5s đầu chỉ xem trước để người dùng căn mặt
const uint32_t RETRY_MS   = 1500;   // gửi lại mỗi 1.5s nếu chưa nhận ra

SPIClass tftSPI(HSPI);
#if TFT_HAS_CS
Adafruit_ST7789 tft(&tftSPI, TFT_CS, TFT_DC, TFT_RST);
#define TFT_MODE SPI_MODE0
#else
Adafruit_ST7789 tft(&tftSPI, -1, TFT_DC, TFT_RST);
#define TFT_MODE SPI_MODE3
#endif

// TJpgDec giải mã ra 320x240 (scale 1/2 từ VGA); cắt giữa còn 240 cột cho vừa màn hình
bool tftOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (y >= 240) return false;
  x -= 40;
  int16_t x0 = x < 0 ? 0 : x;
  int16_t x1 = (x + w) > 240 ? 240 : (x + w);
  if (x0 >= x1) return true;
  int16_t vw = x1 - x0;
  static uint16_t tmp[16 * 16];
  for (int r = 0; r < h; r++) memcpy(&tmp[r * vw], &bmp[r * w + (x0 - x)], vw * 2);
  tft.startWrite();
  tft.setAddrWindow(x0, y, vw, h);
  tft.writePixels(tmp, (uint32_t)vw * h);
  tft.endWrite();
  return true;
}

void showText(const String& l1, const String& l2, uint16_t color) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(color);
  tft.setTextSize(3); tft.setCursor(10, 90);  tft.print(l1);
  tft.setTextSize(2); tft.setCursor(10, 135); tft.print(l2);
}

bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = FRAMESIZE_VGA;   // CỐ ĐỊNH VGA + quality 12 cho cả đăng ký lẫn nhận diện
  c.jpeg_quality = 12;
  c.fb_count     = 2;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_LATEST;
  if (esp_camera_init(&c) != ESP_OK) return false;
  sensor_t* s = esp_camera_sensor_get();
  // s->set_vflip(s, 1);    // bỏ comment nếu ảnh bị ngược
  // s->set_hmirror(s, 1);  // bỏ comment nếu ảnh bị lật trái-phải
  return true;
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(300);
}



// ĐÓNG GÓI ẢNH: gửi nguyên JPEG nhị phân (không Base64) + header X-SHA256 để server kiểm tra toàn vẹn
Result postFrame(camera_fb_t* fb) {
  Result r;
  size_t n = fb->len;
  if (n < 1000 || fb->buf[0] != 0xFF || fb->buf[1] != 0xD8 || fb->buf[n - 2] != 0xFF || fb->buf[n - 1] != 0xD9) {
    r.status = "bad_frame";            // khung JPEG hỏng/cụt: bỏ, lấy khung khác
    return r;
  }
  uint8_t h[32];
  mbedtls_sha256(fb->buf, n, h, 0);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", h[i]);

  connectWiFi();
  HTTPClient http;
  http.begin(SERVER_URL);
  http.setTimeout(10000);
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Device-Id", DEVICE_ID);
  http.addHeader("X-SHA256", hex);
  int code = http.POST(fb->buf, n);
  if (code == 200) {
    JsonDocument d;
    if (!deserializeJson(d, http.getString())) {
      r.status = (const char*)(d["status"] | "error");
      r.name   = (const char*)(d["name_ascii"] | "");
      r.locker = d["locker"] | 0;
    }
  } else {
    r.status = "http_" + String(code);
  }
  http.end();
  return r;
}

void scanSession() {
  uint32_t t0 = millis(), last = 0;
  int misses = 0;
  while (millis() - t0 < SESSION_MS) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) continue;
    TJpgDec.drawJpg(0, 0, fb->buf, fb->len);          // stream xem trước lên TFT
    bool ready = millis() - t0 > AIM_MS && millis() - last > RETRY_MS;
    if (!ready) {
      tft.setTextSize(2); tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      tft.setCursor(4, 222); tft.print("Nhin vao camera");
      esp_camera_fb_return(fb);
      continue;
    }
    last = millis();
    Result r = postFrame(fb);
    esp_camera_fb_return(fb);                          // luôn trả khung ngay sau khi gửi
    Serial.printf("Ket qua: %s\n", r.status.c_str());
    if (r.status == "match") {
      showText("MO TU " + String(r.locker), r.name, ST77XX_GREEN); delay(3000); return;
    } else if (r.status == "enrolled") {
      showText("DA DANG KY", "Khuon mat moi", ST77XX_CYAN); delay(2000); return;
    } else if (r.status == "no_match" && ++misses >= 3) {
      showText("KHONG CO", "du lieu", ST77XX_RED); delay(3000); return;
    } else if (r.status.startsWith("http_") || r.status == "error") {
      showText("LOI KET NOI", "Kiem tra server", ST77XX_YELLOW); delay(2000); return;
    }
    // no_face / bad_image / bad_frame / no_match(<3 lần): thử lại ở vòng sau
  }
  showText("HET GIO", "Thu lai", ST77XX_YELLOW); delay(1500);
}

void idleScreen() { showText("SAN SANG", "Nhan nut de quet", ST77XX_WHITE); }

void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);
  tftSPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_HAS_CS ? TFT_CS : -1);
  tft.init(240, 240, TFT_MODE);
  tft.setSPISpeed(40000000);
  tft.setRotation(0);
  // Nếu màu bị đảo (đỏ <-> xanh): thử TJpgDec.setSwapBytes(true);
  TJpgDec.setJpgScale(2);
  TJpgDec.setCallback(tftOut);
  showText("KHOI DONG", "...", ST77XX_WHITE);
  if (!initCamera()) { showText("LOI CAMERA", "Kiem tra pin/cap", ST77XX_RED); while (1) delay(1000); }
  connectWiFi();
  idleScreen();
}

void loop() {
  if (digitalRead(BTN_PIN) == LOW) {
    delay(30);
    if (digitalRead(BTN_PIN) == LOW) {
      while (digitalRead(BTN_PIN) == LOW) delay(10);
      scanSession();
      idleScreen();
    }
  }
  delay(10);
}
