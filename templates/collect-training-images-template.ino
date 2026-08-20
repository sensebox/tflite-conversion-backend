#include "esp_camera.h"
#include "img_converters.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "Arduino.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/rtc_io.h"
#include "USB.h"
#include "USBMSC.h"

// USB Mass Storage object
USBMSC msc;

// ---------------------------------------------------------
// USB MSC callbacks — raw sector read/write on the SD card
// ---------------------------------------------------------

static int32_t sd_msc_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  (void)offset;
  uint8_t *buf = (uint8_t *)buffer;
  for (uint32_t i = 0; i < bufsize / 512; i++) {
    if (!SD.readRAW(buf + i * 512, lba + i)) return -1;
  }
  return (int32_t)bufsize;
}

static int32_t sd_msc_write(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  (void)offset;
  for (uint32_t i = 0; i < bufsize / 512; i++) {
    if (!SD.writeRAW(buffer + i * 512, lba + i)) return -1;
  }
  return (int32_t)bufsize;
}

static bool sd_msc_start_stop(uint8_t power_condition, bool start, bool load_eject) {
  (void)power_condition;
  (void)start;
  (void)load_eject;
  return true;
}

// ---------- camera ----------
#define PWDN_GPIO_NUM  46
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5

#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM    8
#define Y3_GPIO_NUM    9
#define Y2_GPIO_NUM    11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

// ---------- OLED ----------
#define PIN_QWIIC_SDA 2
#define PIN_QWIIC_SCL 1

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Layout: camera image on the left (64x64), text panel on the right
#define IMG_WIDTH 64
#define IMG_HEIGHT 64
#define TEXT_X 68
#define TEXT_WIDTH (SCREEN_WIDTH - TEXT_X)
#define LINE_HEIGHT 12
#define CHAR_WIDTH 6 // width in px of one character at text size 1

// ---------- SD Card ----------
#define VSPI_MISO   40 // DAT
#define VSPI_MOSI   38 // CMD
#define VSPI_SCLK   39 // SCK/CLK
#define VSPI_SS     41 // CS
#define SD_ENABLE   48
SPIClass sdspi = SPIClass();

// ---------- Buttons ----------
#define BO_BUTTON   0  // switches to next class
#define SW_BUTTON  21 // takes a picture
#define SW_ALT_BUTTON  47 // takes a picture
const unsigned long DEBOUNCE_DELAY = 50;

bool lastClassBtnState = HIGH;
bool lastCaptureBtnState = HIGH;
unsigned long lastClassDebounceTime = 0;
unsigned long lastCaptureDebounceTime = 0;

// ---------- Classes ----------
const char *classNames[] = {"Rasen", "strasse", "anderes"};
const int NUM_CLASSES = 3;
int selectedClass = 0;
int sampleCount[NUM_CLASSES]; // next sample number to use, per class

bool sdReady = false;
bool usbMode = false;

// ---------------------------------------------------------
// Helpers
// ---------------------------------------------------------

String classFolder(const char *className) {
  String f = String(className);
  f.replace(" ", "_");
  return "/" + f;
}

// Counts the number of .jpg files in the class folder
int countFilesInFolder(const String &folderPath) {
  int count = 0;
  File dir = SD.open(folderPath);
  if (dir) {
    File entry = dir.openNextFile();
    while (entry) {
      if (!entry.isDirectory()) {
        String fname = String(entry.name());
        int slashIdx = fname.lastIndexOf('/');
        if (slashIdx >= 0) fname = fname.substring(slashIdx + 1);
        if (fname.endsWith(".jpg")) {
          count++;
        }
      }
      entry.close();
      entry = dir.openNextFile();
    }
    dir.close();
  }
  return count+1;
}

// Debounced check if all pins in the list are pressed simultaneously (active LOW)
bool checkButtonPressed(const int *pins, int numPins, bool &lastCombinedState, unsigned long &lastDebounceTime) {
  // Check if all pins are currently pressed (LOW)
  bool allPressed = true;
  for (int i = 0; i < numPins; i++) {
    if (digitalRead(pins[i]) != LOW) {
      allPressed = false;
      break;
    }
  }

  bool pressed = false;

  // Debounce: if enough time has passed since last state change
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    // Detect transition from HIGH (not pressed) to LOW (pressed)
    if (allPressed && lastCombinedState == HIGH) {
      pressed = true;
    }
  }

  // Track state changes for debouncing
  if (allPressed != (lastCombinedState == LOW)) {
    lastDebounceTime = millis();
  }

  lastCombinedState = allPressed ? LOW : HIGH;
  return pressed;
}

// Draws the live/frozen camera frame (grayscale, downscaled 96x96 -> 64x64)
// into the left half of the display.
void drawCameraImage(camera_fb_t *fb) {
  const int srcWidth = 96;
  const int srcHeight = 96;

  for (int y = 0; y < IMG_HEIGHT; y++) {
    for (int x = 0; x < IMG_WIDTH; x++) {
      int sx = x * srcWidth / IMG_WIDTH;
      int sy = y * srcHeight / IMG_HEIGHT;
      uint8_t pixel = fb->buf[sy * srcWidth + sx];
      display.drawPixel(x, y, pixel > 128 ? WHITE : BLACK);
    }
  }
}

// Word-wraps text into the right-hand text panel.
void drawWrappedText(const String &text, int x, int y, int maxWidth) {
  int maxChars = maxWidth / CHAR_WIDTH;
  int cursorY = y;
  String line = "";
  int start = 0;
  int len = text.length();

  while (start < len) {
    int spaceIdx = text.indexOf(' ', start);
    String word = (spaceIdx == -1) ? text.substring(start) : text.substring(start, spaceIdx);

    int prospectiveLen = line.length() + (line.length() ? 1 : 0) + word.length();
    if (prospectiveLen > maxChars && line.length() > 0) {
      display.setCursor(x, cursorY);
      display.println(line);
      cursorY += LINE_HEIGHT;
      line = word;
    } else {
      line += (line.length() ? " " : "") + word;
    }

    start = (spaceIdx == -1) ? len : spaceIdx + 1;
  }

  if (line.length()) {
    display.setCursor(x, cursorY);
    display.println(line);
  }
}

// Draws the list of classes into the right panel, marking the selected one.
void drawClassList() {
  int y = 0;
  for (int i = 0; i < NUM_CLASSES; i++) {
    display.setCursor(TEXT_X, y);
    if (i == selectedClass) {
      display.setTextSize(2);
      display.print(">");
      // size 2: each char is 12 px wide; ">" uses 12 px
      int maxChars = (TEXT_WIDTH - 12) / 12;
      String name = String(classNames[i]);
      if ((int)name.length() > maxChars) name = name.substring(0, maxChars);
      display.println(name);
      y += LINE_HEIGHT * 2;
      display.setTextSize(1);
    } else {
      display.print("  ");
      // size 1: each char is 6 px wide; "  " uses 12 px
      int maxChars = (TEXT_WIDTH - 12) / CHAR_WIDTH;
      String name = String(classNames[i]);
      if ((int)name.length() > maxChars) name = name.substring(0, maxChars);
      display.println(name);
      y += LINE_HEIGHT;
    }
  }
}

void renderLive(camera_fb_t *fb) {
  display.clearDisplay();
  drawCameraImage(fb);
  drawClassList();
  display.display();
}

void renderCaptured(camera_fb_t *fb, const String &statusText) {
  display.clearDisplay();
  drawCameraImage(fb);
  drawWrappedText(statusText, TEXT_X, 0, TEXT_WIDTH);
  display.display();
}

// Converts the current frame to JPEG and saves it under
// /<Class_Name>/sample_<n>.jpg, then freezes the display for 1.5s
// showing how many pictures of that class have been taken.
void saveAndShowCaptured(camera_fb_t *fb) {
  const char *className = classNames[selectedClass];
  String folder = classFolder(className);

  String status;

  if (!sdReady) {
    Serial.println("SD card not available, cannot save picture");
    status = "SD Karte nicht verf\x81gbar!";
    renderCaptured(fb, status);
    delay(1000);
    return;
  }

  if (!SD.exists(folder)) {
    SD.mkdir(folder);
  }

  int num = sampleCount[selectedClass];
  String path = folder + "/" + String(millis()) + ".jpg";

  uint8_t *jpgBuf = NULL;
  size_t jpgLen = 0;
  bool converted = frame2jpg(fb, 80, &jpgBuf, &jpgLen);

  if (converted) {
    File file = SD.open(path.c_str(), FILE_WRITE);
    if (file) {
      file.write(jpgBuf, jpgLen);
      file.close();
      Serial.printf("Saved: %s\n", path.c_str());
      sampleCount[selectedClass]++;
      // 0x81 is the CP437 code point for 'ü', used together with display.cp437(true)
      int maxClassChars = TEXT_WIDTH / CHAR_WIDTH;
      String truncatedName = String(className);
      if ((int)truncatedName.length() > maxClassChars) truncatedName = truncatedName.substring(0, maxClassChars);
      status = String(num) + " Bilder f" + String((char)0x81) + "r " + truncatedName + " aufgenommen";
    } else {
      Serial.println("Failed to open file for writing");
      status = "Fehler beim Speichern!";
    }
    free(jpgBuf);
  } else {
    Serial.println("JPEG conversion failed");
    status = "Fehler bei JPEG!";
  }

  renderCaptured(fb, status);
  delay(1500);
}

// ---------------------------------------------------------
// Setup
// ---------------------------------------------------------

void setup() {
  // WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disable brownout detector

  Serial.begin(115200);
  delay(100);

  pinMode(BO_BUTTON, INPUT_PULLUP);
  pinMode(SW_BUTTON , INPUT_PULLUP);
  pinMode(SW_ALT_BUTTON , INPUT_PULLUP);

  // Init OLED
  Wire.begin(PIN_QWIIC_SDA, PIN_QWIIC_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3D);
  display.setRotation(2);
  display.cp437(true); // enables correct rendering of extended chars like 'ü'
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("Initialisieren...");
  display.display();

  // Camera config (grayscale, small frame - fast live view and small training images)
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_GRAYSCALE;
  config.frame_size = FRAMESIZE_96X96;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  Serial.println("Init camera...");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x", err);
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Camera init failed!");
    display.display();
    return;
  }

  // Init SD card
  Serial.println("Starting SD Card");
  pinMode(SD_ENABLE, OUTPUT);
  digitalWrite(SD_ENABLE, LOW);

  sdspi.begin(VSPI_SCLK, VSPI_MISO, VSPI_MOSI, VSPI_SS);
  delay(100);
  if (!SD.begin(VSPI_SS, sdspi)) {
    Serial.println("Card Mount Failed");
    // Retry once after a longer delay
    delay(500);
    if (SD.begin(VSPI_SS, sdspi) && SD.cardType() != CARD_NONE) {
      sdReady = true;
    }
  } else if (SD.cardType() == CARD_NONE) {
    Serial.println("No SD card attached");
  } else {
    sdReady = true;
  }
  Serial.printf("SD ready: %s\n", sdReady ? "yes" : "no");

  if (!sdReady) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("SD-Karte nicht");
    display.println("erkannt.");
    display.println("");
    display.println("Bitte ueberpruefe");
    display.println("die SD-Karte und");
    display.println("starte dann neu.");
    display.display();
    return;
  }

  // Determine starting sample count per class from existing files on SD
  for (int i = 0; i < NUM_CLASSES; i++) {
    String folder = classFolder(classNames[i]);
    if (sdReady) {
      if (!SD.exists(folder)) {
        SD.mkdir(folder);
      }
      sampleCount[i] = countFilesInFolder(folder);
    } else {
      sampleCount[i] = 0;
    }
  }
}

// ---------------------------------------------------------
// USB Mass Storage
// ---------------------------------------------------------

void enterUsbMode() {
  if (!sdReady) {
    Serial.println("Cannot enter USB mode: SD card not ready");
    return;
  }

  Serial.println("Entering USB Mass Storage mode");
  uint32_t sectorCount = SD.cardSize() / 512;

  USB.productName("senseBox MCU Eye - Training Data");
  USB.manufacturerName("senseBox");

  msc.vendorID("senseBox");
  msc.productID("SD Card");
  msc.productRevision("1.0");
  msc.onRead(sd_msc_read);
  msc.onWrite(sd_msc_write);
  msc.onStartStop(sd_msc_start_stop);
  msc.mediaPresent(true);

  msc.begin(sectorCount, 512);
  usbMode = true;

  USB.begin();
  Serial.println("USB Mass Storage ready - SD card accessible as USB drive");

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("SD-Karte als USB-");
  display.println("Laufwerk verfuegbar.");
  display.println("");
  display.println("Kopiere deine");
  display.println("Trainingsbilder");
  display.println("auf den PC.");
  display.display();
}

// ---------------------------------------------------------
// Loop
// ---------------------------------------------------------

void loop() {
  if (!sdReady) {
    delay(1000);
    return;
  }

  // While the SD card is exposed as a USB drive, wait for both buttons
  // to be held simultaneously to reboot back into camera mode.
  if (usbMode) {
    if (digitalRead(BO_BUTTON) == LOW && digitalRead(SW_BUTTON) == LOW && digitalRead(SW_ALT_BUTTON) == LOW) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("Neustart...");
      display.display();
      esp_restart();
    }
    delay(100);
    return;
  }

  // Enter USB mode when both buttons are held simultaneously
  if (digitalRead(BO_BUTTON) == LOW && digitalRead(SW_BUTTON) == LOW && digitalRead(SW_ALT_BUTTON) == LOW) {
    enterUsbMode();
    // Wait until both buttons are released so the USB-mode exit combo
    // doesn't trigger immediately on the very first loop iteration.
    while (digitalRead(BO_BUTTON) == LOW || digitalRead(SW_BUTTON) == LOW && digitalRead(SW_ALT_BUTTON) == LOW) {
      delay(10);
    }
    return;
  }

  const int classPins[] = {BO_BUTTON};
  bool classPressed = checkButtonPressed(classPins, 1, lastClassBtnState, lastClassDebounceTime);

  const int capturePins[] = {SW_BUTTON, SW_ALT_BUTTON};
  bool capturePressed = checkButtonPressed(capturePins, 2, lastCaptureBtnState, lastCaptureDebounceTime);

  if (classPressed) {
    selectedClass = (selectedClass + 1) % NUM_CLASSES;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Capture failed");
    delay(10);
    return;
  }

  if (capturePressed) {
    saveAndShowCaptured(fb);
  } else {
    renderLive(fb);
  }

  esp_camera_fb_return(fb);
  delay(10);
}
