// ESP32-C6 + 1.47" ST7789 MJPEG player (shared SPI with SD)
//
// Wiring (your working setup):
//   SD:   CS=4, SCK=1, MOSI=2, MISO=3
//   LCD:  DC=15, CS=14, RST=22, BL=23, SCK=1, MOSI=2  (shared SPI)
//   Button: (optional) set BTN_A to a GPIO if you want skip-on-press
//
// Build notes:
//   - Board: ESP32C6 Dev Module (Arduino core 3.2.x)
//   - You already use: -DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_ESP32C6_DEV=1

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <Arduino_GFX_Library.h>
#include "MjpegClass.h"

// ---------------- Pins ----------------
static const int PIN_SD_CS     = 4;
static const int PIN_SPI_SCK   = 1;
static const int PIN_SPI_MOSI  = 2;
static const int PIN_SPI_MISO  = 3;

static const int PIN_GFX_DC    = 15;
static const int PIN_GFX_CS    = 14;
static const int PIN_GFX_RST   = 22;
static const int PIN_GFX_BL    = 23;

// Optional skip button (active-low). Set to -1 to disable.
static const int BTN_A         = -1;

// ---------------- Display ----------------
// Matches your working example
Arduino_DataBus *bus = new Arduino_HWSPI(
  PIN_GFX_DC,        // dc
  PIN_GFX_CS,        // cs
  PIN_SPI_SCK,       // sck
  PIN_SPI_MOSI,      // mosi
  PIN_SPI_MISO,      // miso
  &SPI,              // SPIClass*
  true               // shared interface
);

Arduino_GFX *gfx = new Arduino_ST7789(
  bus, PIN_GFX_RST, /*rotation*/ 0, /*IPS*/ false,
  172, 320,         // panel "window"
  34, 0,            // col_offset1, row_offset1
  34, 0             // col_offset2, row_offset2
);

// The panel needs this vendor init (same as your working sketch)
static void lcd_reg_init() {
  static const uint8_t ops[] = {
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x11,
    END_WRITE,
    DELAY, 120,

    BEGIN_WRITE,
    WRITE_C8_D16, 0xDF, 0x98, 0x53,
    WRITE_C8_D8,  0xB2, 0x23,

    WRITE_COMMAND_8, 0xB7,
    WRITE_BYTES, 4, 0x00, 0x47, 0x00, 0x6F,

    WRITE_COMMAND_8, 0xBB,
    WRITE_BYTES, 6, 0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0,

    WRITE_C8_D16, 0xC0, 0x44, 0xA4,
    WRITE_C8_D8,  0xC1, 0x16,

    WRITE_COMMAND_8, 0xC3,
    WRITE_BYTES, 8, 0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77,

    WRITE_COMMAND_8, 0xC4,
    WRITE_BYTES, 12,
      0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82,

    WRITE_COMMAND_8, 0xC8,
    WRITE_BYTES, 32,
      0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
      0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
      0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
      0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,

    WRITE_COMMAND_8, 0xD0,
    WRITE_BYTES, 5, 0x04, 0x06, 0x6B, 0x0F, 0x00,

    WRITE_C8_D16, 0xD7, 0x00, 0x30,
    WRITE_C8_D8,  0xE6, 0x14,
    WRITE_C8_D8,  0xDE, 0x01,

    WRITE_COMMAND_8, 0xB7,
    WRITE_BYTES, 5, 0x03, 0x13, 0xEF, 0x35, 0x35,

    WRITE_COMMAND_8, 0xC1,
    WRITE_BYTES, 3, 0x14, 0x15, 0xC0,

    WRITE_C8_D16, 0xC2, 0x06, 0x3A,
    WRITE_C8_D16, 0xC4, 0x72, 0x12,
    WRITE_C8_D8,  0xBE, 0x00,
    WRITE_C8_D8,  0xDE, 0x02,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 3, 0x00, 0x02, 0x00,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 3, 0x01, 0x02, 0x00,

    WRITE_C8_D8,  0xDE, 0x00,
    WRITE_C8_D8,  0x35, 0x00,
    WRITE_C8_D8,  0x3A, 0x05,

    WRITE_COMMAND_8, 0x2A,
    WRITE_BYTES, 4, 0x00, 0x22, 0x00, 0xCD,

    WRITE_COMMAND_8, 0x2B,
    WRITE_BYTES, 4, 0x00, 0x00, 0x01, 0x3F,

    WRITE_C8_D8,  0xDE, 0x02,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 3, 0x00, 0x02, 0x00,

    WRITE_C8_D8,  0xDE, 0x00,
    WRITE_C8_D8,  0x36, 0x00,
    WRITE_COMMAND_8, 0x21,
    END_WRITE,

    DELAY, 10,

    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x29,
    END_WRITE
  };
  bus->batchOperation(ops, sizeof(ops));
}

// ---------------- Config ----------------
#define GFX_BRIGHTNESS 255
static const char *MJPEG_FOLDER = "/mjpeg";
#define MAX_FILES  32

// ---------------- Globals ----------------
String   mjpegFileList[MAX_FILES];
uint32_t mjpegFileSizes[MAX_FILES];
int      mjpegCount = 0;
int      currentMjpegIndex = 0;

MjpegClass mjpeg;

int         total_frames;
uint32_t    total_read_video, total_decode_video, total_show_video;
uint32_t    start_ms, curr_ms;
int32_t     output_buf_size = 0;
uint8_t    *mjpeg_buf = nullptr;
uint16_t   *output_buf = nullptr;

bool        hasSD = false;
uint32_t    nextSDRetryMs = 0;
bool        msgShown = false;

volatile bool skipRequested = false;
volatile uint32_t lastPress = 0;

// ---------------- Helpers ----------------
static void setBacklight(uint8_t val) {
  pinMode(PIN_GFX_BL, OUTPUT);
  digitalWrite(PIN_GFX_BL, val > 0 ? HIGH : LOW);
}

void IRAM_ATTR onButtonPress() {
  uint32_t now = millis();
  if (now - lastPress > 300) {
    skipRequested = true;
    lastPress = now;
  }
}

static void mountSD_orScheduleRetry() {
  const uint32_t speeds[] = { 30000000, 8000000, 4000000 };  // 10/8/4 MHz
  for (uint32_t hz : speeds) {
    if (SD.begin(PIN_SD_CS, SPI, hz)) {
      Serial.printf("SD mounted @ %u Hz\n", hz);
      hasSD = true;
      return;
    }
  }
  Serial.println("No SD card – will retry…");
  hasSD = false;
  nextSDRetryMs = millis() + 2000;
}

static void loadMjpegFilesList() {
  mjpegCount = 0;

  File dir = SD.open(MJPEG_FOLDER);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("Failed to open %s\n", MJPEG_FOLDER);
    return;
  }

  while (true) {
    File file = dir.openNextFile();
    if (!file) break;
    if (!file.isDirectory()) {
      String name = file.name();
      String lower = name; lower.toLowerCase();

      // Skip macOS AppleDouble sidecars
      if (name.startsWith("._")) { file.close(); continue; }

      if (lower.endsWith(".mjpeg")) {
        if (mjpegCount < MAX_FILES) {
          mjpegFileList[mjpegCount]  = name;
          mjpegFileSizes[mjpegCount] = (uint32_t)file.size();
          mjpegCount++;
        }
      }
    }
    file.close();
    if (mjpegCount >= MAX_FILES) break;
  }
  dir.close();

  Serial.printf("%d playable files\n", mjpegCount);
  for (int i = 0; i < mjpegCount; i++) {
    Serial.printf("%4d: %s (%u bytes)\n",
      i, mjpegFileList[i].c_str(), (unsigned)mjpegFileSizes[i]);
  }
}

static int jpegDrawCallback(JPEGDRAW *pDraw) {
  if (!pDraw || !pDraw->pPixels) return 0;
  int x = pDraw->x, y = pDraw->y, w = pDraw->iWidth, h = pDraw->iHeight;
  if (x >= gfx->width() || y >= gfx->height()) return 1;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > gfx->width())  w = gfx->width()  - x;
  if (y + h > gfx->height()) h = gfx->height() - y;
  if (w <= 0 || h <= 0) return 1;

  uint32_t s = millis();
  gfx->draw16bitBeRGBBitmap(x, y, pDraw->pPixels, w, h);
  total_show_video += millis() - s;
  return 1;
}

static void mjpegPlayFromSDCard(const char *path) {
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    Serial.printf("ERROR: Failed to open %s for reading\n", path);
    return;
  }

  Serial.println(F("MJPEG start"));
  gfx->fillScreen(RGB565_BLACK);

  start_ms = millis();
  curr_ms = start_ms;
  total_frames = 0;
  total_read_video = 0;
  total_decode_video = 0;
  total_show_video = 0;

  mjpeg.setup(&f, mjpeg_buf, jpegDrawCallback, true /* big-endian RGB565 */,
              0, 0, gfx->width(), gfx->height());

  while (!skipRequested && f.available() && mjpeg.readMjpegBuf()) {
    total_read_video += millis() - curr_ms;
    curr_ms = millis();

    mjpeg.drawJpg();
    total_decode_video += millis() - curr_ms;

    curr_ms = millis();
    total_frames++;
  }

  int time_used = millis() - start_ms;
  Serial.println(F("MJPEG end"));
  f.close();
  skipRequested = false;

  float fps = (time_used > 0) ? (1000.0f * total_frames / time_used) : 0.0f;
  total_decode_video -= total_show_video;

  Serial.printf("Total frames: %d\n", total_frames);
  Serial.printf("Time used: %d ms\n", time_used);
  Serial.printf("Average FPS: %0.1f\n", fps);
  Serial.printf("Read MJPEG: %u ms (%0.1f %%)\n", total_read_video,  time_used ? 100.0 * total_read_video  / time_used : 0);
  Serial.printf("Decode video: %u ms (%0.1f %%)\n", total_decode_video,time_used ? 100.0 * total_decode_video/ time_used : 0);
  Serial.printf("Show video: %u ms (%0.1f %%)\n", total_show_video,   time_used ? 100.0 * total_show_video   / time_used : 0);
}

static void playSelectedMjpeg(int idx) {
  String fullPath = String(MJPEG_FOLDER) + "/" + mjpegFileList[idx];
  Serial.printf("Playing %s\n", fullPath.c_str());
  mjpegPlayFromSDCard(fullPath.c_str());
}

// ---------------- Arduino ----------------
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.printf("\nBooting… SD_CS=%d  SCK=%d  MOSI=%d  MISO=%d\n",
                PIN_SD_CS, PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_MISO);

  // Ensure both CS lines idle high before SPI start
  pinMode(PIN_SD_CS, OUTPUT);   digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_GFX_CS, OUTPUT);  digitalWrite(PIN_GFX_CS, HIGH);

  // Bring up shared SPI
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);

  // Mount SD first (reduces bus contention)
  mountSD_orScheduleRetry();

  // Init display with your proven sequence
  if (!gfx->begin()) {
    Serial.println("Display init failed!");
    while (true) {}
  }
  lcd_reg_init();
  gfx->setRotation(0);
  gfx->fillScreen(RGB565_BLACK);
  setBacklight(GFX_BRIGHTNESS ? 1 : 0);  // simple ON/OFF for BL pin

  // Allocate frame buffers
  output_buf_size = gfx->width() * 4 * 2;
  output_buf = (uint16_t *)heap_caps_aligned_alloc(
      16, output_buf_size * sizeof(uint16_t), MALLOC_CAP_DMA);
  if (!output_buf) {
    Serial.println("output_buf aligned_alloc failed!");
    while (true) {}
  }

  // Safe MJPEG work buffer allocation with fallbacks
  size_t want = (size_t)gfx->width() * gfx->height() * 4 / 5; // ~40% of a frame
  const size_t tries[] = { want, 128*1024, 96*1024, 64*1024, 48*1024 };
  for (size_t sz : tries) {
    mjpeg_buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (mjpeg_buf) { Serial.printf("MJPEG buffer: %u bytes\n", (unsigned)sz); break; }
  }
  if (!mjpeg_buf) {
    Serial.println("FATAL: could not allocate MJPEG buffer");
    while (true) {}
  }

  // Optional skip button
  if (BTN_A >= 0) {
    pinMode(BTN_A, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BTN_A), onButtonPress, FALLING);
  }

  if (hasSD) {
    loadMjpegFilesList();
  }
}

void loop() {
  // Retry SD mount if missing
  if (!hasSD && millis() >= nextSDRetryMs) {
    mountSD_orScheduleRetry();
    if (hasSD) {
      loadMjpegFilesList();
      gfx->fillScreen(RGB565_BLACK);
      msgShown = false;
    }
  }

  if (!hasSD || mjpegCount == 0) {
    if (!msgShown) {
      gfx->fillScreen(RGB565_BLACK);
      gfx->setCursor(6, 6);
      gfx->setTextColor(RGB565_WHITE);
      gfx->setTextSize(2);
      gfx->print(hasSD ? "No .mjpeg in /mjpeg" : "Insert SD card");
      msgShown = true;
    }
    delay(50);
    return;
  }

  playSelectedMjpeg(currentMjpegIndex);
  currentMjpegIndex = (currentMjpegIndex + 1) % mjpegCount;
}