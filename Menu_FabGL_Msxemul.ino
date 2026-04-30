/*
 * Menu_FabGL_Msxemul.ino
 * MSX Emulator for ESP32 TTGO VGA32
 *
 * SD Card connections (SPI):
 *   MISO => GPIO  2  (16 for non PICO-D4 models)
 *   MOSI => GPIO 12  (17 for non PICO-D4 models)
 *   CLK  => GPIO 14
 *   CS   => GPIO 13
 *
 * To exit a game: press F12.
 * Compile with Arduino IDE + Espressif ESP32 v2.0.17 (PSRAM enabled)
 */

// ─── Includes ────────────────────────────────────────────────────────────────
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <stdio.h>
#include <string.h>

#include "fabgl.h"
#include "fabui.h"
#include "MSX.h"
#include "ESP32_MSX.h"
#include "esp32-hal-psram.h"

extern "C" {
  #include "esp_spiram.h"
}

// ─── Configuration constants ─────────────────────────────────────────────────
#define FORMAT_ON_FAIL    false
#define SDCARD_MOUNT_PATH "/SD"

// SPI pins for SD card
#define SD_MISO  2
#define SD_MOSI 12
#define SD_CLK  14
#define SD_CS   13

#ifndef LSB_FIRST
  #define LSB_FIRST
#endif

// ─── Global variables ────────────────────────────────────────────────────────
char directorio[80];

Preferences preferences;

fabgl::VGA16Controller VGAController;
fabgl::PS2Controller   PS2Controller;
fabgl::Canvas         *Canvas;

bool emuRunning = false;
bool selecting  = false;
byte PSLReg     = 0;

// ─── fMSX global variable bindings ──────────────────────────────────────────
extern "C" {
  extern int         Mode;
  extern int         RAMPages;
  extern int         VRAMPages;
  extern const char *ROMName[MAXCARTS];
  extern const char *ProgDir;
  extern byte        Verbose;
  extern int         SetAudio(int, int);
  extern void        StopAudio();
}

// ─── Helper: check for valid ROM file extension ──────────────────────────────
// Returns true for .rom / .ROM / .mx1 / .MX1
static bool isRomFile(const char *filename) {
  if (!filename) return false;
  return (strstr(filename, ".ROM") != NULL) ||
         (strstr(filename, ".rom") != NULL) ||
         (strstr(filename, ".MX1") != NULL) ||
         (strstr(filename, ".mx1") != NULL);
}

// ─── Helper: mount SD card via SPI ──────────────────────────────────────────
// Shows an error on screen and halts if mount fails.
static void mountSDAndHaltOnError() {
  Serial.println("Mounting SD card...");
  SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("ERROR: SD Card Mount Failed!");
    Canvas->setPenColor(Color::Red);
    Canvas->drawText(10, 30, "SD Card Mount Failed!");
    Canvas->waitCompletion();
    while (1) { delay(1000); }   // Halt on fatal SD error
  }
  SD.open("/");   // Open root — required by some fMSX internals
}

// ─── Helper: switch to MSX video, mount SD, run emulator ────────────────────
// Used by both "Run BASIC" and ROM-file launch paths (avoids code duplication).
static void mountAndStartMSX() {
  Canvas->clear();
  Canvas->waitCompletion();

  // Switch to MSX-compatible video resolution
  VGAController.setResolution(VGA_320x200_60HzD);
  VGAController.setMouseCursor(nullptr);

  mountSDAndHaltOnError();

  emuRunning = true;
  int result = StartMSX(Mode, RAMPages, VRAMPages);
  Serial.printf("StartMSX returned: %d\n", result);
  emuRunning = false;

  StopAudio();
  TrashMachine();
}

// ─── UI Application class ────────────────────────────────────────────────────
class MyApp : public uiApp {

  uiFileBrowser *fileBrowser;
  uiFrame       *frame;
  uiButton      *formatBtn;

  void init() {

    // fMSX default configuration
    Mode      = MSX_MSX1 | MSX_NTSC | MSX_GUESSA | MSX_GUESSB;
    RAMPages  = 4;
    VRAMPages = 2;
    Verbose   = 1;
    ProgDir   = "/";

    // Main window background
    rootWindow()->frameStyle().backgroundColor = RGB888(0, 0, 64);

    // Main frame
    frame = new uiFrame(rootWindow(), "MSXEmul Emulator", Point(15, 10), Size(375, 275));
    frame->frameProps().hasCloseButton = false;

    // File browser
    fileBrowser = new uiFileBrowser(frame, Point(10, 25), Size(355, 210));
    fileBrowser->setDirectory(SDCARD_MOUNT_PATH);

    // Restore last visited directory if it still exists on SD
    char ruta[160];
    strcpy(ruta, "");
    strncat(ruta, directorio + 4, strlen(directorio) - 4);
    ruta[sizeof(ruta) - 1] = '\0';

    if (fileBrowser->content().exists(ruta, true)) {
      fileBrowser->setDirectory(directorio);
    }
    fileBrowser->update();

    // "Run BASIC" button — boots MSX without any ROM cartridge
    formatBtn = new uiButton(frame, "Run BASIC", Point(10, 240), Size(100, 25));
    formatBtn->onClick = [&]() {
      ROMName[0] = 0;
      ROMName[1] = 0;
      Serial.println("Starting MSX BASIC");
      mountAndStartMSX();
      quit(0);
    };

    // Single click on a file launches it
    fileBrowser->onClick = [&]() {
      if (!fileBrowser->isDirectory()) { RunEmulator(); }
    };

    // Enter key on a file also launches it
    fileBrowser->onKeyType = [&](fabgl::uiKeyEventInfo evt) {
      if (evt.VK == fabgl::VirtualKey::VK_RETURN) {
        if (!fileBrowser->isDirectory()) { RunEmulator(); }
      }
    };

    setFocusedWindow(fileBrowser);
  }

  // Refresh the file browser view
  void updateBrowser() {
    fileBrowser->update();
  }

  // Launch the ROM or MX1 image selected in the file browser
  void RunEmulator() {
    char  fichero[160];
    char *Sdir = "/";

    // Save current browser directory and build full path for fMSX.
    // The browser prepends "/SD" (3 chars); fMSX needs the path without it.
    strcpy(directorio, fileBrowser->content().directory());
    strcpy(fichero, "");
    strncat(fichero, directorio + 3, strlen(directorio) - 3);
    strncat(fichero, Sdir, 160);
    strncat(fichero, fileBrowser->filename(), 160);
    fichero[sizeof(fichero) - 1] = '\0';

    if (!isRomFile(fichero)) {
      Serial.printf("Not a ROM file: %s\n", fichero);
      return;
    }

    ROMName[0] = fichero;
    ROMName[1] = 0;
    Serial.println("Starting MSX emulation...");
    Serial.printf("Loading ROM: %s\n", fichero);

    mountAndStartMSX();
    quit(0);
  }

} app;

// ─── Arduino setup ───────────────────────────────────────────────────────────
void setup() {
  setCpuFrequencyMhz(240);

  Serial.begin(115200);
  Serial.println("\n\n=== MSX Emulator Starting ===");

  // Retrieve last visited directory from NVS (non-volatile storage)
  preferences.begin("Datos", false);
  String dir = preferences.getString("Dir", SDCARD_MOUNT_PATH);
  strcpy(directorio, dir.c_str());

  // PSRAM is mandatory — halt if not detected
  if (!psramInit()) {
    Serial.println("ERROR: PSRAM initialization FAILED!");
    while (1) { delay(1000); }
  }
  Serial.printf("PSRAM initialized: %d bytes total, %d bytes free\n",
                ESP.getPsramSize(), ESP.getFreePsram());

  // PS/2 keyboard + mouse controller
  PS2Controller.begin(PS2Preset::KeyboardPort0_MousePort1);
  Serial.println("PS2 Controller initialized");

  // MSX memory and machine initialisation
  if (!InitMemorySystem()) {
    Serial.println("ERROR: InitMemorySystem() failed!");
    while (1) { delay(1000); }
  }
  if (!InitMachine()) {
    Serial.println("ERROR: InitMachine() failed!");
    while (1) { delay(1000); }
  }

  // Audio system
  Serial.println("Initializing sound system...");
  if (!SetAudio(22050, 127)) {
    Serial.println("WARNING: Sound initialization failed!");
  } else {
    Serial.println("Sound system initialized");
  }

  // Mount SD card for the FabGL file browser
  while (!FileBrowser::mountSDCard(FORMAT_ON_FAIL, SDCARD_MOUNT_PATH)) {
    Serial.println("Waiting for SD card...");
    delay(1000);
  }
  Serial.println("SD card mounted successfully");

  // VGA controller and canvas
  VGAController.begin();
  Serial.println("VGA16 Controller initialized");

  Canvas = new fabgl::Canvas(&VGAController);
  VGAController.setResolution(VGA_400x300_60Hz);
  Canvas->clear();
  Canvas->waitCompletion();
}

// ─── Arduino loop ────────────────────────────────────────────────────────────
void loop() {
  // Run the UI; returns when the emulator session ends
  app.run(&VGAController);

  Serial.println("Emulator finished");

  // Persist last visited directory before restarting
  preferences.putString("Dir", String(directorio));

  ESP.restart();
}









