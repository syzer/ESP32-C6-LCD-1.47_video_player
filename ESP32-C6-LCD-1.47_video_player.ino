// ESP32-C6 1.47" ST7789 MJPEG Player (shared SPI for LCD + SD)
// Brightness-max version (BL pin driven hard HIGH), with FPS tweaks + boot-time skip
//
// Wiring (your working setup):
//   SD:   CS=4, SCK=1, MOSI=2, MISO=3
//   LCD:  DC=15, CS=14, RST=22, BL=23, SCK=1, MOSI=2  (shared SPI)
//   Button: BTN_A (active-low) to skip videos (on-boot and during playback)
//
// Build notes:
//   - Board: ESP32C6 Dev Module (Arduino core 3.2.x)
//   - Common flags you used: -DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_ESP32C6_DEV=1

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

// User button (active-low). Change if your board uses another pin.
#ifndef BTN_A
#define BTN_A 9
#endif

// ---------------- Speeds (in MHz) ----------------
#define LCD_SPI_MHZ   80   // LCD bus (try 80, drop if unstable)

// Convert MHz to Hz for APIs
#define _MHZ(x) ((x) * 1000000UL)
#define LCD_SPI_HZ    _MHZ(LCD_SPI_MHZ)

// SD clock probes (fastest first)
static const uint32_t SD_SPEEDS_HZ[] = {
  _MHZ(42),   // 42 MHz (often fine with short wires)
  _MHZ(30),   // 30 MHz
  _MHZ(20),   // 20 MHz
  _MHZ(10),   // 10 MHz
  _MHZ(4)     //  4 MHz (safe)
};

// ---------------- Display geometry (1.47" ST7789, 172x320 with column offset 34) ----------------
#define LCD_W 172
#define LCD_H 320
#define LCD_ROTATION 0
#define LCD_COL_OFS 34
#define LCD_ROW_OFS 0

// ---------------- UI / Player ----------------
static const char *MJPEG_FOLDER = "/mjpeg";
#define MAX_FILES 40

// ---------------- Globals ----------------
String   mjpegFileList[MAX_FILES];
uint32_t mjpegFileSizes[MAX_FILES] = {0};
int      mjpegCount = 0;
static   int currentMjpegIndex = 0;

MjpegClass mjpeg;

int         total_frames;
uint32_t    total_read_video;
uint32_t    total_decode_video;
uint32_t    total_show_video;
uint32_t    start_ms, curr_ms;

int32_t     output_buf_pixels = 0; // number of pixels in DMA chunk
uint16_t   *output_buf = nullptr;  // LCD line buffer (DMA-capable)
uint8_t    *mjpeg_buf  = nullptr;  // decoder working buffer

volatile bool     skipRequested = false;   // set by ISR, consumed in loop
volatile uint32_t lastPressMs   = 0;

bool hasSD   = false;
bool msgShown= false;
uint32_t nextSDRetryMs = 0;

// ---------------- Bus / GFX ----------------
Arduino_DataBus *bus = new Arduino_HWSPI(
  PIN_GFX_DC,        // dc
  PIN_GFX_CS,        // cs
  PIN_SPI_SCK,       // sck
  PIN_SPI_MOSI,      // mosi
  PIN_SPI_MISO,      // miso
  &SPI,              // SPI class
  true               // shared interface (LCD + SD on same SPI)
);

Arduino_GFX *gfx = new Arduino_ST7789(
  bus, PIN_GFX_RST, LCD_ROTATION, false /*IPS*/,
  LCD_W, LCD_H,
  LCD_COL_OFS, LCD_ROW_OFS,
  LCD_COL_OFS, LCD_ROW_OFS
);

// ---------------- Vendor LCD init (same as your bright-working sketch) ----------------
static void lcd_reg_init() {
  static const uint8_t ops[] = {
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x11,        // Sleep out
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
    WRITE_COMMAND_8, 0x21,   // display inversion ON (adds perceived brightness/contrast)
    END_WRITE,

    DELAY, 10,

    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x29,  // display ON
    END_WRITE
  };
  bus->batchOperation(ops, sizeof(ops));
}

// ---------------- Helpers ----------------
static inline float hz_to_mhz(uint32_t hz) { return hz / 1000000.0f; }

// Max-bright backlight: hard HIGH (no PWM)
static void setBacklightMax() {
  pinMode(PIN_GFX_BL, OUTPUT);
  digitalWrite(PIN_GFX_BL, HIGH);   // If your BL is active-LOW, flip to LOW
}

void IRAM_ATTR onButtonPressISR() {
  uint32_t now = millis();
  if (now - lastPressMs > 250) { // debounce
    skipRequested = true;
    lastPressMs = now;
  }
}

static bool mountSD_with_probe() {
  for (uint32_t hz : SD_SPEEDS_HZ) {
    if (SD.begin(PIN_SD_CS, SPI, hz)) {
      Serial.printf("SD mounted @ %.1f MHz\n", hz_to_mhz(hz));
      return true;
    }
  }
  Serial.println("No SD card – will retry...");
  return false;
}

static bool isPlayableName(const String &nameIn) {
  if (nameIn.length() == 0) return false;
  // strip directory, ignore dotfiles and AppleDouble "._"
  String name = nameIn;
  int slash = name.lastIndexOf('/');
  if (slash >= 0) name = name.substring(slash + 1);
  if (name[0] == '.') return false;
  if (name.startsWith("._")) return false;
  String lower = name; lower.toLowerCase();
  return lower.endsWith(".mjpeg");
}

static void loadMjpegFilesList() {
  mjpegCount = 0;

  File dir = SD.open(MJPEG_FOLDER);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("Failed to open %s\n", MJPEG_FOLDER);
    if (dir) dir.close();
    return;
  }

  while (true) {
    File file = dir.openNextFile();
    if (!file) break;
    if (!file.isDirectory()) {
      String name = file.name();
      if (isPlayableName(name)) {
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
    Serial.printf("  %2d: %s (%u bytes)\n", i,
      mjpegFileList[i].c_str(), (unsigned)mjpegFileSizes[i]);
  }
}

static void printAndConsumeButton(const char *where) {
  if (skipRequested) {
    Serial.printf("[Button] %s: skip requested\n", where);
    skipRequested = false;
  }
}

static int jpegDrawCallback(JPEGDRAW *pDraw) {
  if (!pDraw || !pDraw->pPixels) return 0;
  uint32_t s = millis();
  gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  total_show_video += millis() - s;
  return 1;
}

static void mjpegPlayFromSDCard(const char *mjpegPath) {
  File f = SD.open(mjpegPath, FILE_READ);
  if (!f || f.isDirectory()) {
    Serial.printf("ERROR: Failed to open %s for reading\n", mjpegPath);
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

  float fps = (time_used > 0) ? (1000.0f * total_frames / time_used) : 0.0f;
  total_decode_video -= total_show_video;

  Serial.printf("Total frames: %d\n", total_frames);
  Serial.printf("Time used: %d ms\n", time_used);
  Serial.printf("Average FPS: %0.1f\n", fps);
  Serial.printf("Read MJPEG: %lu ms (%0.1f %%)\n", total_read_video,
                time_used ? 100.0 * total_read_video / time_used : 0);
  Serial.printf("Decode video: %lu ms (%0.1f %%)\n", total_decode_video,
                time_used ? 100.0 * total_decode_video / time_used : 0);
  Serial.printf("Show video: %lu ms (%0.1f %%)\n", total_show_video,
                time_used ? 100.0 * total_show_video / time_used : 0);

  if (skipRequested) {
    Serial.println("[Button] Skip consumed at end of video");
    skipRequested = false; // consume to avoid double-advance
  }
}

static void playSelectedMjpeg(int idx) {
  String fullPath = String(MJPEG_FOLDER) + "/" + mjpegFileList[idx];
  Serial.printf("Playing %s\n", fullPath.c_str());
  mjpegPlayFromSDCard(fullPath.c_str());
}

// ---------------- Arduino ----------------
void setup() {
  Serial.begin(115200);
  delay(60);

  Serial.printf("\nBooting… SD_CS=%d  SCK=%d  MOSI=%d  MISO=%d\n",
                PIN_SD_CS, PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_MISO);

  // Both CS idle high before SPI start
  pinMode(PIN_SD_CS, OUTPUT);   digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_GFX_CS, OUTPUT);  digitalWrite(PIN_GFX_CS, HIGH);

  // Shared SPI bus
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);

  // Mount SD first (reduces contention during LCD init)
  hasSD = mountSD_with_probe();

  // Init display (high speed) and vendor register sequence
  if (!gfx->begin(LCD_SPI_HZ)) {
    Serial.println("Display init failed!");
    while (true) { delay(1000); }
  }
  lcd_reg_init();
  gfx->setRotation(LCD_ROTATION);
  gfx->fillScreen(RGB565_BLACK);
  setBacklightMax(); // <- MAX BRIGHTNESS here
  Serial.printf("LCD SPI @ %.1f MHz\n", (float)LCD_SPI_MHZ);

  // List videos if SD present
  if (hasSD) {
    loadMjpegFilesList();
  }

  // Allocate LCD DMA chunk (multiple lines per burst)
  output_buf_pixels = gfx->width() * 12; // 12 lines/chunk (tune if needed)
  output_buf = (uint16_t *)heap_caps_aligned_alloc(16,
                  output_buf_pixels * sizeof(uint16_t),
                  MALLOC_CAP_DMA);
  if (!output_buf) {
    Serial.println("output_buf aligned_alloc failed!");
    while (true) { delay(1000); }
  }

  // Allocate MJPEG work buffer (with fallbacks)
  size_t want = (size_t)gfx->width() * gfx->height() * 2 / 5; // ~40% frame
  const size_t tries[] = { want, 128*1024, 96*1024, 64*1024, 48*1024 };
  for (size_t sz : tries) {
    mjpeg_buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (mjpeg_buf) { Serial.printf("MJPEG buffer: %u bytes\n", (unsigned)sz); break; }
  }
  if (!mjpeg_buf) {
    Serial.println("FATAL: could not allocate MJPEG buffer");
    while (true) { delay(1000); }
  }

  // Button for skipping
  pinMode(BTN_A, INPUT); // Active-low external pull-down or pull-up? If floating, use INPUT_PULLUP
  attachInterrupt(digitalPinToInterrupt(BTN_A), onButtonPressISR, FALLING);

  // ---- Boot-time skip window (2s): each press advances start index ----
  uint32_t bootWindowEnd = millis() + 2000;
  int bootSkips = 0;
  bool prev = (digitalRead(BTN_A) == LOW);
  while (millis() < bootWindowEnd) {
    bool now = (digitalRead(BTN_A) == LOW);
    if (now && !prev) {
      bootSkips++;
      Serial.printf("[Button] Boot press detected (%d)\n", bootSkips);
      delay(120); // debounce
    }
    prev = now;
    delay(5);
  }
  if (hasSD && mjpegCount > 0 && bootSkips > 0) {
    currentMjpegIndex = (currentMjpegIndex + bootSkips) % mjpegCount;
    Serial.printf("Start index advanced by %d → %d\n", bootSkips, currentMjpegIndex);
  }

  if (!hasSD || mjpegCount == 0) {
    gfx->setCursor(6, 6);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(2);
    gfx->print(hasSD ? "No .mjpeg in /mjpeg" : "Insert SD card");
    msgShown = true;
  }
}

void loop() {
  // Retry SD if missing
  if (!hasSD && millis() >= nextSDRetryMs) {
    hasSD = mountSD_with_probe();
    if (hasSD) {
      loadMjpegFilesList();
      gfx->fillScreen(RGB565_BLACK);
      msgShown = false;
    } else {
      nextSDRetryMs = millis() + 2000;
    }
  }

  if (!hasSD || mjpegCount == 0) {
    delay(50);
    return;
  }

  // Handle skip request (from ISR)
  if (skipRequested) {
    printAndConsumeButton("loop");
    currentMjpegIndex = (currentMjpegIndex + 1) % mjpegCount;
  }

  playSelectedMjpeg(currentMjpegIndex);
  currentMjpegIndex = (currentMjpegIndex + 1) % mjpegCount;

  delay(2); // tiny yield to keep USB CDC happy
}