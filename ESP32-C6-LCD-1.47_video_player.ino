// Tutorial : https://youtu.be/JqQEG0eipic
// Board: ESP32C6 Dev Module (Arduino core 3.2.x)

#include "PINS_ESP32-C6-LCD-1_47.h"
#include "MjpegClass.h"
#include "SD.h"
#include "Arduino.h"

// ---------- Config ----------
#define GFX_BRIGHTNESS 255
const char *MJPEG_FOLDER = "/mjpeg";
#define MAX_FILES 20

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
long output_buf_size, estimateBufferSize;
uint8_t  *mjpeg_buf;
uint16_t *output_buf;

// hot-plug SD support
bool hasSD = false;
unsigned long nextSDRetryMs = 0;
bool msgShown = false;

// skip button state
volatile bool skipRequested = false;
volatile uint32_t lastPress = 0;

// ---------- Fwds ----------
void setDisplayBrigthness();
void loadMjpegFilesList();
void playSelectedMjpeg(int mjpegIndex);
void mjpegPlayFromSDCard(char *mjpegFilename);
int  jpegDrawCallback(JPEGDRAW *pDraw);

// ---------- ISR ----------
void IRAM_ATTR onButtonPress() {
  uint32_t now = millis();
  if (now - lastPress > 300) { // debounce
    skipRequested = true;
    lastPress = now;
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  DEV_DEVICE_INIT(); // sets up display pins/backlight per header

  // Keep both devices idle before SPI begins
  pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH);
#ifdef GFX_CS
  pinMode(GFX_CS, OUTPUT); digitalWrite(GFX_CS, HIGH);
#endif

  // Use the working SD pins: SCK=1, MISO=3, MOSI=2, CS=SD_CS (from header)
  SPI.begin(1, 3, 2, SD_CS);
  Serial.printf("\nBooting… SD_CS=%d  SCK=%d  MOSI=%d  MISO=%d\n", SD_CS, 1, 2, 3);

  // Mount SD (simple path that matched your working test)
  if (SD.begin(SD_CS)) {
    Serial.println("SD mounted.");
    hasSD = true;
    loadMjpegFilesList();
  } else {
    Serial.println("No SD card – will retry…");
    hasSD = false;
    nextSDRetryMs = millis() + 2000;
  }

  // Bring up the display (shares the same SPI object/pins)
  if (!gfx->begin(GFX_SPEED)) {
    Serial.println("Display initialization failed!");
    while (true) {}
  }
  gfx->setRotation(0);
  gfx->fillScreen(RGB565_BLACK);
  setDisplayBrigthness();

  // Frame buffers
  output_buf_size = gfx->width() * 4 * 2;
  output_buf = (uint16_t *)heap_caps_aligned_alloc(16, output_buf_size * sizeof(uint16_t), MALLOC_CAP_DMA);
  if (!output_buf) {
    Serial.println("output_buf aligned_alloc failed!");
    while (true) {}
  }
  estimateBufferSize = gfx->width() * gfx->height() * 2 / 5;
  mjpeg_buf = (uint8_t *)heap_caps_malloc(estimateBufferSize, MALLOC_CAP_8BIT);

  // Button to skip video
  pinMode(BTN_A, INPUT);                        // active-low
  attachInterrupt(digitalPinToInterrupt(BTN_A), onButtonPress, FALLING);
}

// ---------- Loop ----------
void loop() {
  // Retry mount if card absent (every 2s)
  if (!hasSD && millis() >= nextSDRetryMs) {
    if (SD.begin(SD_CS)) {
      Serial.println("SD mounted.");
      hasSD = true;
      loadMjpegFilesList();
      gfx->fillScreen(RGB565_BLACK);
      msgShown = false;
    } else {
      nextSDRetryMs = millis() + 2000;
    }
  }

  // Nothing to play yet? Show a simple message and idle.
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

  // Normal playback
  playSelectedMjpeg(currentMjpegIndex);
  currentMjpegIndex = (currentMjpegIndex + 1) % mjpegCount;
}

// ---------- Helpers ----------
void setDisplayBrigthness() {
  ledcAttachChannel(GFX_BL, 1000, 8, 1);
  ledcWrite(GFX_BL, GFX_BRIGHTNESS);
}

void playSelectedMjpeg(int mjpegIndex) {
  String fullPath = String(MJPEG_FOLDER) + "/" + mjpegFileList[mjpegIndex];
  char mjpegFilename[128];
  fullPath.toCharArray(mjpegFilename, sizeof(mjpegFilename));
  Serial.printf("Playing %s\n", mjpegFilename);
  mjpegPlayFromSDCard(mjpegFilename);
}

int jpegDrawCallback(JPEGDRAW *pDraw) {
  unsigned long s = millis();
  gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  total_show_video += millis() - s;
  return 1;
}

void mjpegPlayFromSDCard(char *mjpegFilename) {
  File f = SD.open(mjpegFilename, FILE_READ);
  if (!f || f.isDirectory()) {
    Serial.printf("ERROR: Failed to open %s for reading\n", mjpegFilename);
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
  Serial.printf("Read MJPEG: %lu ms (%0.1f %%)\n", total_read_video,  time_used ? 100.0 * total_read_video  / time_used : 0);
  Serial.printf("Decode video: %lu ms (%0.1f %%)\n", total_decode_video,time_used ? 100.0 * total_decode_video/ time_used : 0);
  Serial.printf("Show video: %lu ms (%0.1f %%)\n", total_show_video,   time_used ? 100.0 * total_show_video   / time_used : 0);
}

void loadMjpegFilesList() {
  File dir = SD.open(MJPEG_FOLDER);
  if (!dir) {
    Serial.printf("Failed to open %s\n", MJPEG_FOLDER);
    mjpegCount = 0;
    return;
  }

  mjpegCount = 0;
  while (true) {
    File file = dir.openNextFile();
    if (!file) break;
    if (!file.isDirectory()) {
      String name = file.name();
      String lower = name; lower.toLowerCase();
      if (lower.endsWith(".mjpeg")) {
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

  Serial.printf("%d mjpeg files read\n", mjpegCount);
  for (int i = 0; i < mjpegCount; i++) {
    Serial.printf("File %d: %s, Size: %lu bytes\n",
                  i, mjpegFileList[i].c_str(), (unsigned long)mjpegFileSizes[i]);
  }
}