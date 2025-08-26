// ESP32-C6 MJPEG player (SdFat/exFAT)
// Board: ESP32C6 Dev Module (Arduino-ESP32 3.2.0)
// Libs: GFX Library for Arduino, JPEGDEC (Larry Bank), SdFat (Bill Greiman)

#include "PINS_ESP32-C6-LCD-1_47.h"
#include "MjpegClass.h"
#include <SdFat.h>
#include "Arduino.h"

#define GFX_BRIGHTNESS 255
#define MAX_FILES 20
#define MJPEG_DIR "/mjpeg"

// --------- SD SPI PINS (NEW) ---------
// --- FORCE SD PINS (override header) ---
#ifdef SD_CS
  #undef SD_CS
#endif
#ifdef SPI_SCK
  #undef SPI_SCK
#endif
#ifdef SPI_MOSI
  #undef SPI_MOSI
#endif
#ifdef SPI_MISO
  #undef SPI_MISO
#endif
#define SD_CS    9
#define SPI_SCK  10
#define SPI_MOSI 11
#define SPI_MISO 12

// Compile-time guard: refuse to build if someone sets 0..7 (flash bus)
#if (SD_CS>=0 && SD_CS<=7) || (SPI_SCK>=0 && SPI_SCK<=7) || (SPI_MOSI>=0 && SPI_MOSI<=7) || (SPI_MISO>=0 && SPI_MISO<=7)
  #error "SD pins 0..7 collide with external flash on ESP32-C6. Move SD to GPIO >=8."
#endif

// ---------- SdFat ----------
SdFs  sd;
FsFile mjpegFile;
FsFile mjpegDir;

// ---------- Playlist ----------
String   mjpegFileList[MAX_FILES];
uint32_t mjpegFileSizes[MAX_FILES] = {0};
int      mjpegCount = 0;
int      currentMjpegIndex = 0;

// ---------- Video / JPEG ----------
MjpegClass    mjpeg;
int           total_frames;
unsigned long total_read_video;
unsigned long total_decode_video;
unsigned long total_show_video;
unsigned long start_ms, curr_ms;
long          output_buf_size, estimateBufferSize;
uint8_t      *mjpeg_buf = nullptr;
uint16_t     *output_buf = nullptr;

// ---------- Skip button ----------
volatile bool     skipRequested = false;
volatile uint32_t lastPress = 0;

// ---------- Forward decls ----------
void setDisplayBrigthness();
bool fsBegin();
void loadMjpegFilesList();
void playSelectedMjpeg(int mjpegIndex);
void mjpegPlayFromSDCard(char *mjpegFilename);
int  jpegDrawCallback(JPEGDRAW *pDraw);
void IRAM_ATTR onButtonPress();

// ---------- Storage init ----------
bool fsBegin() {
  // De-select any other SPI peripherals (if defines exist in your pins header)
  #ifdef TFT_CS
    pinMode(TFT_CS, OUTPUT); digitalWrite(TFT_CS, HIGH);
  #endif
  #ifdef TP_CS
    pinMode(TP_CS, OUTPUT);  digitalWrite(TP_CS, HIGH);
  #endif
  #ifdef FLASH_CS
    pinMode(FLASH_CS, OUTPUT); digitalWrite(FLASH_CS, HIGH);
  #endif

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  // Start SPI on the new pins
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
  SPI.setDataMode(SPI_MODE0);

  // Start slow and safe; increase later
  SdSpiConfig cfg(SD_CS, SHARED_SPI, SD_SCK_MHZ(4), &SPI);

  if (!sd.begin(cfg)) {
    Serial.printf("SdFat begin() FAILED  code=0x%X data=0x%X\n",
                  sd.card()->errorCode(), sd.card()->errorData());
    return false;
  }

  // Optionally bump speed once stable:
  // sd.end(); SdSpiConfig fast(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(25), &SPI); sd.begin(fast);

  return true;
}

// ---------- Brightness ----------
void setDisplayBrigthness() {
  ledcAttachChannel(GFX_BL, 1000, 8, 1);
  ledcWrite(GFX_BL, GFX_BRIGHTNESS);
}

// ---------- Button ISR ----------
void IRAM_ATTR onButtonPress() {
  uint32_t now = millis();
  if (now - lastPress > 300) { // debounce
    skipRequested = true;
    lastPress = now;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("SD pins  CS=%d SCK=%d MISO=%d MOSI=%d\n", SD_CS, SPI_SCK, SPI_MISO, SPI_MOSI);

  DEV_DEVICE_INIT();

  // Display
  if (!gfx->begin(GFX_SPEED)) {
    Serial.println("Display initialization failed!");
    while (true) {}
  }
  gfx->setRotation(0);
  gfx->fillScreen(RGB565_BLACK);
  setDisplayBrigthness();

  // Filesystem (exFAT / FAT32)
  if (!fsBegin()) {
    Serial.println("ERROR: exFAT/FAT mount failed (SdFat)!");
    while (true) {}
  }

  // Buffers for MJPEG
  output_buf_size   = gfx->width() * 4 * 2;
  output_buf        = (uint16_t *)heap_caps_aligned_alloc(16, output_buf_size * sizeof(uint16_t), MALLOC_CAP_DMA);
  if (!output_buf) {
    Serial.println("output_buf aligned_alloc failed!");
    while (true) {}
  }
  estimateBufferSize = gfx->width() * gfx->height() * 2 / 5;
  mjpeg_buf          = (uint8_t *)heap_caps_malloc(estimateBufferSize, MALLOC_CAP_8BIT);

  loadMjpegFilesList();

  // Skip button (active-low)
  pinMode(BTN_A, INPUT);
  attachInterrupt(digitalPinToInterrupt(BTN_A), onButtonPress, FALLING);
}

void loop() {
  if (mjpegCount == 0) {
    delay(500);
    return;
  }
  playSelectedMjpeg(currentMjpegIndex);
  currentMjpegIndex++;
  if (currentMjpegIndex >= mjpegCount) currentMjpegIndex = 0;
}

// ---------- Draw callback ----------
int jpegDrawCallback(JPEGDRAW *pDraw) {
  unsigned long s = millis();
  gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  total_show_video += millis() - s;
  return 1;
}

// ---------- Play chosen file ----------
void playSelectedMjpeg(int mjpegIndex) {
  String fullPath = String(MJPEG_DIR) + "/" + mjpegFileList[mjpegIndex];
  char   mjpegFilename[128];
  fullPath.toCharArray(mjpegFilename, sizeof(mjpegFilename));
  Serial.printf("Playing %s\n", mjpegFilename);
  mjpegPlayFromSDCard(mjpegFilename);
}

// ---------- Play from SdFat (exFAT OK) ----------
void mjpegPlayFromSDCard(char *mjpegFilename) {
  mjpegFile.close();
  if (!mjpegFile.open(mjpegFilename, O_RDONLY)) {
    Serial.printf("ERROR: Failed to open %s\n", mjpegFilename);
    return;
  }

  Serial.println(F("MJPEG start"));
  gfx->fillScreen(RGB565_BLACK);

  start_ms           = millis();
  curr_ms            = start_ms;
  total_frames       = 0;
  total_read_video   = 0;
  total_decode_video = 0;
  total_show_video   = 0;

  mjpeg.setup(
      &mjpegFile, mjpeg_buf, jpegDrawCallback, true,
      0, 0, gfx->width(), gfx->height());

  while (!skipRequested && mjpegFile.available() && mjpeg.readMjpegBuf()) {
    total_read_video += millis() - curr_ms;
    curr_ms = millis();

    mjpeg.drawJpg();

    total_decode_video += millis() - curr_ms;
    curr_ms = millis();
    total_frames++;
  }

  int time_used = millis() - start_ms;
  Serial.println(F("MJPEG end"));
  mjpegFile.close();
  skipRequested = false;

  float fps = (time_used > 0) ? (1000.0f * total_frames / time_used) : 0.0f;
  total_decode_video -= total_show_video;

  Serial.printf("Total frames: %d\n", total_frames);
  Serial.printf("Time used: %d ms\n", time_used);
  Serial.printf("Average FPS: %0.1f\n", fps);
  Serial.printf("Read MJPEG: %lu ms (%0.1f %%)\n", total_read_video,   time_used ? 100.0 * total_read_video   / time_used : 0.0);
  Serial.printf("Decode video: %lu ms (%0.1f %%)\n", total_decode_video, time_used ? 100.0 * total_decode_video / time_used : 0.0);
  Serial.printf("Show video: %lu ms (%0.1f %%)\n", total_show_video,   time_used ? 100.0 * total_show_video   / time_used : 0.0);
}

// ---------- Build playlist ----------
void loadMjpegFilesList() {
  mjpegDir.close();
  if (!mjpegDir.open(MJPEG_DIR)) {
    Serial.printf("Failed to open %s\n", MJPEG_DIR);
    while (true) {}
  }

  mjpegCount = 0;
  FsFile f;
  while (f.openNext(&mjpegDir, O_RDONLY)) {
    if (!f.isDir()) {
      char name[128] = {0};
      f.getName(name, sizeof(name));
      String s = String(name);
      s.toLowerCase();
      if (s.endsWith(".mjpeg")) {
        if (mjpegCount < MAX_FILES) {
          mjpegFileList[mjpegCount]  = String(name);   // keep original case
          mjpegFileSizes[mjpegCount] = f.size();
          mjpegCount++;
        }
      }
    }
    f.close();
    if (mjpegCount >= MAX_FILES) break;
  }
  mjpegDir.close();

  Serial.printf("%d mjpeg files read\n", mjpegCount);
  for (int i = 0; i < mjpegCount; i++) {
    Serial.printf("File %d: %s, Size: %lu bytes\n",
                  i, mjpegFileList[i].c_str(), (unsigned long)mjpegFileSizes[i]);
  }
}