#include <Arduino.h>
#include <M5GFX.h>
#include <M5PM1.h>
#include <M5Unified.h>
#include <M5UnitENV.h>
#include <SD.h>
#include <SPI.h>

static constexpr int SHT_SDA_PIN = 3;
static constexpr int SHT_SCL_PIN = 2;

static constexpr uint8_t SD_SPI_CS_PIN = 47;
static constexpr uint8_t SD_SPI_SCK_PIN = 15;
static constexpr uint8_t SD_SPI_MOSI_PIN = 13;
static constexpr uint8_t SD_SPI_MISO_PIN = 14;

static constexpr std::size_t MAX_IMAGE_FILES = 32;
static constexpr uint32_t SENSOR_UPDATE_INTERVAL_MS = 5UL * 60UL * 1000UL;
static constexpr int SWIPE_THRESHOLD_PX = 80;

M5PM1 pm1;
SHT4X sht4;

String imagePaths[MAX_IMAGE_FILES];
std::size_t imageCount = 0;
std::size_t currentImageIndex = 0;

bool powerReady = false;
bool sdReady = false;
bool sensorReady = false;
bool rtcReady = false;
bool hasSensorReading = false;

float temperatureC = 0.0f;
float humidityPct = 0.0f;
int lastRtcSlot = -1;
uint32_t lastSensorUpdateMs = 0;

static String normalizeRootPath(const char* path)
{
  String normalized = path == nullptr ? "" : String(path);
  if (!normalized.startsWith("/")) {
    normalized = "/" + normalized;
  }
  return normalized;
}

static bool isSupportedImagePath(const String& path)
{
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".bmp");
}

static bool beginPowerControl()
{
  const m5pm1_err_t err = pm1.begin(&M5.In_I2C, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K);
  if (err != M5PM1_OK) {
    Serial.println("PM1 init failed.");
    return false;
  }

  pm1.setLdoEnable(true);
  pm1.pinMode(M5PM1_GPIO_NUM_0, OUTPUT);
  pm1.digitalWrite(M5PM1_GPIO_NUM_0, HIGH);
  pm1.pinMode(M5PM1_GPIO_NUM_4, OUTPUT);
  pm1.digitalWrite(M5PM1_GPIO_NUM_4, HIGH);
  pm1.pinMode(M5PM1_GPIO_NUM_3, OUTPUT);
  pm1.digitalWrite(M5PM1_GPIO_NUM_3, HIGH);
  pm1.pinMode(M5PM1_GPIO_NUM_1, INPUT_PULLUP);
  return true;
}

static bool beginSdCard()
{
  if (!powerReady) {
    Serial.println("Skipping SD init because PM1 is unavailable.");
    return false;
  }

  if (pm1.digitalRead(M5PM1_GPIO_NUM_1) != LOW) {
    Serial.println("SD card not inserted.");
    return false;
  }

  SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    Serial.println("SD init failed.");
    return false;
  }

  return true;
}

static void loadImageList()
{
  imageCount = 0;

  if (!sdReady) {
    return;
  }

  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    Serial.println("Failed to open SD root.");
    return;
  }

  File entry = root.openNextFile();
  while (entry && imageCount < MAX_IMAGE_FILES) {
    if (!entry.isDirectory()) {
      const String path = normalizeRootPath(entry.name());
      if (isSupportedImagePath(path)) {
        imagePaths[imageCount++] = path;
        Serial.printf("Found image: %s\r\n", path.c_str());
      }
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();

  if (imageCount == 0) {
    Serial.println("No supported image files found in SD root.");
  }
}

static bool drawCurrentImage(int x, int y, int width, int height)
{
  if (!sdReady || imageCount == 0) {
    return false;
  }

  const String& path = imagePaths[currentImageIndex];
  String lower = path;
  lower.toLowerCase();

  if (lower.endsWith(".png")) {
    return M5.Display.drawPngFile(SD, path.c_str(), x, y, width, height);
  }
  if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) {
    return M5.Display.drawJpgFile(SD, path.c_str(), x, y, width, height);
  }
  if (lower.endsWith(".bmp")) {
    return M5.Display.drawBmpFile(SD, path.c_str(), x, y, width, height);
  }
  return false;
}

static bool updateSensorReading()
{
  if (!sensorReady) {
    return false;
  }

  if (!sht4.update()) {
    Serial.println("SHT40 update failed.");
    return false;
  }

  temperatureC = sht4.cTemp;
  humidityPct = sht4.humidity;
  hasSensorReading = true;
  Serial.printf("Sensor updated: %.1f C / %.1f %%\r\n", temperatureC, humidityPct);
  return true;
}

template <typename TFont>
static void drawCenteredText(const String& text, int x, int y, const TFont* font, uint16_t color, textdatum_t datum)
{
  M5.Display.setFont(font);
  M5.Display.setTextColor(color);
  M5.Display.setTextDatum(datum);
  M5.Display.drawString(text, x, y);
}

static void renderScreen()
{
  const int screenW = M5.Display.width();
  const int screenH = M5.Display.height();
  const int margin = 18;
  const int gap = 14;
  const int cardH = 180;
  const int cardY = screenH - margin - cardH;
  const int imageX = margin;
  const int imageY = margin;
  const int imageW = screenW - (margin * 2);
  const int imageH = cardY - gap - imageY;
  const int cardW = (screenW - (margin * 2) - gap) / 2;

  M5.Display.fillScreen(WHITE);
  M5.Display.drawRoundRect(imageX, imageY, imageW, imageH, 10, BLACK);

  if (!drawCurrentImage(imageX + 6, imageY + 6, imageW - 12, imageH - 12)) {
    drawCenteredText("No SD nameplate image", screenW / 2, imageY + (imageH / 2) - 14, &fonts::FreeSansBold18pt7b,
                     RED, middle_center);
    drawCenteredText("Put PNG/JPG/BMP in /", screenW / 2, imageY + (imageH / 2) + 20, &fonts::Font4, BLACK,
                     middle_center);
  }

  drawCenteredText("Swipe left/right to change the image", screenW / 2, cardY - 6, &fonts::Font2, BLACK,
                   bottom_center);

  const int tempCardX = margin;
  const int humCardX = margin + cardW + gap;
  const int cardRadius = 14;

  M5.Display.fillRoundRect(tempCardX, cardY, cardW, cardH, cardRadius, WHITE);
  M5.Display.fillRoundRect(humCardX, cardY, cardW, cardH, cardRadius, WHITE);
  M5.Display.drawRoundRect(tempCardX, cardY, cardW, cardH, cardRadius, RED);
  M5.Display.drawRoundRect(humCardX, cardY, cardW, cardH, cardRadius, BLUE);

  drawCenteredText("TEMPERATURE", tempCardX + (cardW / 2), cardY + 28, &fonts::FreeSansBold12pt7b, RED,
                   top_center);
  drawCenteredText("HUMIDITY", humCardX + (cardW / 2), cardY + 28, &fonts::FreeSansBold12pt7b, BLUE,
                   top_center);

  if (hasSensorReading) {
    char tempText[24];
    char humText[24];
    snprintf(tempText, sizeof(tempText), "%.1f C", temperatureC);
    snprintf(humText, sizeof(humText), "%.1f %%", humidityPct);

    drawCenteredText(tempText, tempCardX + (cardW / 2), cardY + 108, &fonts::FreeSansBold24pt7b, BLACK,
                     middle_center);
    drawCenteredText(humText, humCardX + (cardW / 2), cardY + 108, &fonts::FreeSansBold24pt7b, BLACK,
                     middle_center);
  } else if (sensorReady) {
    drawCenteredText("Updating...", tempCardX + (cardW / 2), cardY + 108, &fonts::FreeSansBold18pt7b, BLACK,
                     middle_center);
    drawCenteredText("Updating...", humCardX + (cardW / 2), cardY + 108, &fonts::FreeSansBold18pt7b, BLACK,
                     middle_center);
  } else {
    drawCenteredText("Sensor", tempCardX + (cardW / 2), cardY + 92, &fonts::FreeSansBold18pt7b, BLACK,
                     middle_center);
    drawCenteredText("not found", tempCardX + (cardW / 2), cardY + 128, &fonts::FreeSansBold18pt7b, BLACK,
                     middle_center);
    drawCenteredText("Sensor", humCardX + (cardW / 2), cardY + 92, &fonts::FreeSansBold18pt7b, BLACK,
                     middle_center);
    drawCenteredText("not found", humCardX + (cardW / 2), cardY + 128, &fonts::FreeSansBold18pt7b, BLACK,
                     middle_center);
  }
}

static void selectRelativeImage(int step)
{
  if (imageCount == 0) {
    return;
  }

  const int count = static_cast<int>(imageCount);
  const int current = static_cast<int>(currentImageIndex);
  currentImageIndex = static_cast<std::size_t>((current + step + count) % count);
  Serial.printf("Selected image: %s\r\n", imagePaths[currentImageIndex].c_str());
  renderScreen();
}

static void handleSwipeSelection()
{
  const auto touch = M5.Touch.getDetail();
  if (!touch.wasReleased()) {
    return;
  }

  const int dx = touch.distanceX();
  const int dy = touch.distanceY();
  if (abs(dx) < SWIPE_THRESHOLD_PX || abs(dx) <= abs(dy)) {
    return;
  }

  if (dx < 0) {
    selectRelativeImage(1);
  } else {
    selectRelativeImage(-1);
  }
}

static void refreshSensorIfNeeded()
{
  bool shouldRefresh = false;

  if (rtcReady) {
    const auto now = M5.Rtc.getDateTime();
    const int slot = ((now.time.hours * 60) + now.time.minutes) / 5;
    if (lastRtcSlot != slot) {
      lastRtcSlot = slot;
      shouldRefresh = true;
    }
  } else {
    const uint32_t nowMs = millis();
    if (!hasSensorReading || (nowMs - lastSensorUpdateMs) >= SENSOR_UPDATE_INTERVAL_MS) {
      lastSensorUpdateMs = nowMs;
      shouldRefresh = true;
    }
  }

  if (!shouldRefresh) {
    return;
  }

  if (!rtcReady) {
    lastSensorUpdateMs = millis();
  }
  updateSensorReading();
  renderScreen();
}

void setup()
{
  auto cfg = M5.config();
  cfg.clear_display = false;
  M5.begin(cfg);
  Serial.begin(115200);

  M5.Display.setRotation(0);
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Display.fillScreen(WHITE);

  powerReady = beginPowerControl();
  sdReady = beginSdCard();
  loadImageList();

  sensorReady = sht4.begin(&Wire, SHT40_I2C_ADDR_44, SHT_SDA_PIN, SHT_SCL_PIN, 400000U);
  if (!sensorReady) {
    Serial.println("SHT40 not found.");
  }

  rtcReady = M5.Rtc.isEnabled();
  if (!rtcReady) {
    Serial.println("RTC not available. Falling back to millis().");
  }

  renderScreen();
  refreshSensorIfNeeded();
}

void loop()
{
  M5.update();
  handleSwipeSelection();
  refreshSensorIfNeeded();
  delay(50);
}
