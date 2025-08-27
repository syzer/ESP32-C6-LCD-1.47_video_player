// ESP32-C6 1.47" LCD MJPEG Player (shared SPI for LCD + SD)
// - LCD pins: DC=15, CS=14, RST=22, BL=23
// - SD pins : CS=4
// - Shared SPI: SCK=1, MOSI=2, MISO=3
// - Button   : BTN_A (active-low) to skip videos (on-boot and during playback)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <SD.h>
#include <FS.h>
#include <SPI.h>
#include "MjpegClass.h"

// ---------- Pins ----------
static const int PIN_SPI_SCK  = 1;
static const int PIN_SPI_MOSI = 2;
static const int PIN_SPI_MISO = 3;

static const int PIN_SD_CS    = 4;

static const int PIN_GFX_DC   = 15;
static const int PIN_GFX_CS   = 14;
static const int PIN_GFX_RST  = 22;
static const int PIN_GFX_BL   = 23;   // backlight (PWM-able)

// Your board's user button (active-low). Change if different:
#ifndef BTN_A
#define BTN_A 9
#endif

// ---------- Speeds (Hz) ----------
// #define LCD_SPI_HZ   80000000   // 80 MHz for display
// Try these for SD (we’ll probe best stable one at boot)
// static const uint32_t SD_SPEEDS_HZ[] = { 25000000, 20000000, 10000000, 8000000, 4000000 };


#define LCD_SPI_MHZ   80    // Display SPI speed in MHz

// Convert MHz to Hz for APIs
#define _MHZ(x) ((x) * 1000000UL)

#define LCD_SPI_HZ    _MHZ(LCD_SPI_MHZ)

// Try these for SD (we’ll probe best stable one at boot)
static const uint32_t SD_SPEEDS_HZ[] = {
  _MHZ(42),
  _MHZ(30),
  _MHZ(20),
  _MHZ(10),
  _MHZ(4)
};


// ---------- Display geometry (1.47" ST7789, 172x320 with column offset 34) ----------
#define LCD_W 172
#define LCD_H 320
#define LCD_ROTATION 0
#define LCD_COL_OFS 34
#define LCD_ROW_OFS 0

// ---------- UI / Player ----------
#define GFX_BRIGHTNESS 255
static const char *MJPEG_FOLDER = "/mjpeg";
#define MAX_FILES 40

// ---------- Globals ----------
String   mjpegFileList[MAX_FILES];
uint32_t mjpegFileSizes[MAX_FILES] = {0};
int      mjpegCount = 0;
static   int currentMjpegIndex = 0;

MjpegClass mjpeg;

int total_frames;
unsigned long total_read_video;
unsigned long total_decode_video;
unsigned long total_show_video;
unsigned long start_ms, curr_ms;
int32_t  output_buf_pixels;
uint8_t  *mjpeg_buf = nullptr;
uint16_t *output_buf = nullptr;

volatile bool skipRequested = false;
volatile uint32_t lastPressMs = 0;

bool hasSD = false;
bool msgShown = false;

// ---------- Bus / GFX ----------
Arduino_DataBus *bus = new Arduino_HWSPI(
  PIN_GFX_DC, PIN_GFX_CS,           // DC, CS
  PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_MISO
);

Arduino_GFX *gfx = new Arduino_ST7789(
  bus, PIN_GFX_RST, LCD_ROTATION, false,
  LCD_W, LCD_H,
  LCD_COL_OFS, LCD_ROW_OFS,
  LCD_COL_OFS, LCD_ROW_OFS
);

// ---------- Helpers ----------
static inline float hz_to_mhz(uint32_t hz) { return hz / 1000000.0f; }

void IRAM_ATTR onButtonPressISR() {
  uint32_t now = millis();
  if (now - lastPressMs > 250) { // debounce
    skipRequested = true;
    lastPressMs = now;
  }
}

void setDisplayBrightness(uint8_t v) {
  // Simple backlight control via LEDC
  ledcAttachChannel(PIN_GFX_BL, 1000 /*Hz*/, 8 /*bits*/, 1 /*channel*/);
  ledcWrite(PIN_GFX_BL, v);
}

bool mountSD_with_probe() {
  for (uint32_t hz : SD_SPEEDS_HZ) {
    if (SD.begin(PIN_SD_CS, SPI, hz)) {
      Serial.printf("SD mounted @ %.1f MHz\n", hz_to_mhz(hz));
      return true;
    }
  }
  Serial.println("No SD card – will retry...");
  return false;
}

static bool isPlayableName(const String &name) {
  if (name.length() == 0) return false;
  // ignore macOS dotfiles like "._foo"
  if (name[0] == '.') return false;
  String lower = name; lower.toLowerCase();
  return lower.endsWith(".mjpeg");
}

void loadMjpegFilesList() {
  mjpegCount = 0;

  File dir = SD.open(MJPEG_FOLDER);
  if (!dir) {
    Serial.printf("Failed to open %s\n", MJPEG_FOLDER);
    return;
  }
  if (!dir.isDirectory()) {
    Serial.printf("%s is not a directory\n", MJPEG_FOLDER);
    dir.close();
    return;
  }

  while (true) {
    File file = dir.openNextFile();
    if (!file) break;
    if (!file.isDirectory()) {
      String name = file.name();
      // Drop directory prefix if present (SPIFFS-like names can contain full path)
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (isPlayableName(name)) {
        if (mjpegCount < MAX_FILES) {
          mjpegFileList[mjpegCount]  = name;
          mjpegFileSizes[mjpegCount] = file.size();
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
    Serial.printf("  %2d: %s (%lu bytes)\n", i,
      mjpegFileList[i].c_str(), (unsigned long)mjpegFileSizes[i]);
  }
}

void printButtonIfPressedAndConsume(const char *where) {
  if (skipRequested) {
    Serial.printf("[Button] %s: skipping to next video\n", where);
    skipRequested = false; // consume
  }
}

int jpegDrawCallback(JPEGDRAW *pDraw) {
  unsigned long s = millis();
  gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y,
                            pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  total_show_video += millis() - s;
  return 1;
}

void mjpegPlayFromSDCard(const char *mjpegPath) {
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

  mjpeg.setup(&f, mjpeg_buf, jpegDrawCallback, true /* RGB565 BE */,
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
    skipRequested = false; // consume at end to avoid double-skip
  }
}

void playSelectedMjpeg(int idx) {
  String fullPath = String(MJPEG_FOLDER) + "/" + mjpegFileList[idx];
  Serial.printf("Playing %s\n", fullPath.c_str());
  mjpegPlayFromSDCard(fullPath.c_str());
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  delay(50);

  Serial.printf("\nBooting… SD_CS=%d  SCK=%d  MOSI=%d  MISO=%d\n",
                PIN_SD_CS, PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_MISO);

  // Make both devices idle before SPI.begin()
  pinMode(PIN_SD_CS, OUTPUT);   digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_GFX_CS, OUTPUT);  digitalWrite(PIN_GFX_CS, HIGH);

  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);

  // Mount SD (probe several speeds)
  hasSD = mountSD_with_probe();

  if (hasSD) {
    loadMjpegFilesList();
  }

  // Init display at high speed
  if (!gfx->begin(LCD_SPI_HZ)) {
    Serial.println("Display initialization failed!");
    while (true) { delay(1000); }
  }
  gfx->setRotation(LCD_ROTATION);
  gfx->fillScreen(RGB565_BLACK);
  setDisplayBrightness(GFX_BRIGHTNESS);
  Serial.printf("LCD SPI @ %.1f MHz\n", hz_to_mhz(LCD_SPI_HZ));

  // Frame buffers
  output_buf_pixels = gfx->width() * 4; // 4 lines per chunk
  // DMA-capable 16-byte aligned
  output_buf = (uint16_t *)heap_caps_aligned_alloc(16, output_buf_pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
  if (!output_buf) {
    Serial.println("output_buf aligned_alloc failed!");
    while (true) { delay(1000); }
  }
  int32_t estimateBufferSize = gfx->width() * gfx->height() * 2 / 5;
  mjpeg_buf = (uint8_t *)heap_caps_malloc(estimateBufferSize, MALLOC_CAP_8BIT);
  if (!mjpeg_buf) {
    Serial.println("mjpeg_buf malloc failed!");
    while (true) { delay(1000); }
  }

  // Button (active-low)
  pinMode(BTN_A, INPUT);
  attachInterrupt(digitalPinToInterrupt(BTN_A), onButtonPressISR, FALLING);

  // ---- Boot-time “next video” selection window (2s) ----
  // Each press advances by +1 before starting playback.
  uint32_t bootWindowEnd = millis() + 2000;
  int bootSkips = 0;
  bool prev = digitalRead(BTN_A) == LOW;
  while (millis() < bootWindowEnd) {
    bool now = digitalRead(BTN_A) == LOW;
    if (now && !prev) {            // rising press (active-low)
      bootSkips++;
      Serial.printf("[Button] Boot press detected (%d)\n", bootSkips);
      // simple debounce
      delay(120);
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
  if (!hasSD || mjpegCount == 0) {
    // Idle screen already drawn in setup()
    delay(50);
    return;
  }

  // Button pressed? (from ISR)
  if (skipRequested) {
    printButtonIfPressedAndConsume("loop");
    currentMjpegIndex = (currentMjpegIndex + 1) % mjpegCount;
  }

  playSelectedMjpeg(currentMjpegIndex);
  currentMjpegIndex = (currentMjpegIndex + 1) % mjpegCount;

  // Tiny yield to keep CDC happy
  delay(2);
}