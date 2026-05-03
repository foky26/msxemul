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

#include "Hourglass.h"

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

// Double-click guard: clicks arriving within this window after a directory
// navigation are silently discarded (absorbs the "phantom" second click).
#define DBLCLICK_GUARD_MS  400

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
bool selecting  = false;   // true while a directory is being loaded (blocks events)
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



// fabgl::Cursor: hotspot + Bitmap. Filled in setup() so the data pointer
// is set after the array is placed in memory.
static fabgl::Cursor hourglassCursor;

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
static void mountSDAndHaltOnError() {
  Serial.println("Mounting SD card...");
  SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("ERROR: SD Card Mount Failed!");
    Canvas->setPenColor(Color::Red);
    Canvas->drawText(10, 30, "SD Card Mount Failed!");
    Canvas->waitCompletion();
    while (1) { delay(1000); }
  }
  SD.open("/");
}

// ─── Helper: switch to MSX video, mount SD, run emulator ────────────────────
static void mountAndStartMSX() {
  Canvas->clear();
  Canvas->waitCompletion();

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
  bool          lasteventclick=false;

  // Timestamp of the last directory navigation (ms).
  // Used to swallow the phantom second click of a double-click sequence.
  unsigned long lastDirNavMs = 0;

  // ── Busy state helpers ─────────────────────────────────────────────────────

  void setBusy() {
    selecting = true;
   
    VGAController.setMouseCursor(&hourglassCursor);
  }

  void setReady() {
    selecting = false;
    VGAController.setMouseCursor(fabgl::CursorName::CursorPointerSimpleReduced);
  }

  // ── Directory navigation with double-click guard ───────────────────────────
  // When the user double-clicks a folder:
  //   1st click  → onClick fires → we call enterDirectory()
  //   2nd click  → onClick fires again immediately after navigation completes
  //                → we MUST discard it, otherwise it acts on the new listing.
  //
  // Strategy: record the time when navigation finishes, and ignore any onClick
  // that arrives within DBLCLICK_GUARD_MS milliseconds of that moment.
  void enterDirectory() {
    setBusy();

    if(lasteventclick) {fileBrowser->changeDirectory(fileBrowser->filename());}

    lasteventclick=false;
    fileBrowser->update();

    // Record when we finished navigating so the next spurious click is ignored
    lastDirNavMs = millis();

    setReady();
  }

  // Returns true if a click should be discarded because it arrived too soon
  // after the last directory navigation (phantom double-click second event).
  bool isPhantomClick() {
    return (millis() - lastDirNavMs) < DBLCLICK_GUARD_MS;
  }

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
      if (selecting) return;
      ROMName[0] = 0;
      ROMName[1] = 0;
      Serial.println("Starting MSX BASIC");
      mountAndStartMSX();
      quit(0);
    };

    // ── onClick ──────────────────────────────────────────────────────────────
    // Guard order:
    //   1. selecting  → busy loading a directory, ignore everything
    //   2. isPhantomClick() → second click of a double-click after navigation,
    //                         ignore it so it doesn't act on the new listing
    //   3. directory  → navigate into it
    //   4. file       → launch emulator
    fileBrowser->onClick = [&]() {
      if (selecting)        return;
      if (isPhantomClick()) {
        Serial.println("Phantom double-click discarded");
        return;
      }
      if (fileBrowser->isDirectory()) {
        lasteventclick=true;
        enterDirectory();
      } else {
        RunEmulator();
      }
    };

    // ── onKeyType: Enter key ─────────────────────────────────────────────────
    // Same phantom-event guard as onClick: keys pressed while loading a directory
    // stay queued in FabGL and fire after setReady(), so we discard them with
    // the same isPhantomClick() timestamp window.
    fileBrowser->onKeyType = [&](fabgl::uiKeyEventInfo evt) {
      if (selecting)        return;   // still loading — drop immediately
      if (isPhantomClick()) {
        Serial.println("Phantom key event discarded");
        return;                       // queued key from previous navigation
      }
      if (evt.VK == fabgl::VirtualKey::VK_RETURN) {
        if (fileBrowser->isDirectory()) {
          VGAController.setMouseCursor(&hourglassCursor);
          enterDirectory();
        } else {
          RunEmulator();
        }
      }
    };

    setFocusedWindow(fileBrowser);
    VGAController.setMouseCursor(fabgl::CursorName::CursorPointerSimpleReduced);
  }

  // Refresh the file browser view
  void updateBrowser() {
    fileBrowser->update();
  }

  // Launch the ROM or MX1 image selected in the file browser
  void RunEmulator() {
    char  fichero[160];
    char *Sdir = "/";

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

  // Cursor configuration
  hourglassCursor.hotspotX      = 6;   // horizontal center
  hourglassCursor.hotspotY      = 11;  // vertical center
  hourglassCursor.bitmap.width  = 13;
  hourglassCursor.bitmap.height = 22;
  hourglassCursor.bitmap.format = fabgl::PixelFormat::RGBA2222;
  hourglassCursor.bitmap.data   = (uint8_t *)hourglassBitmap;

  // Retrieve last visited directory from NVS
  preferences.begin("Datos", false);
  String dir = preferences.getString("Dir", SDCARD_MOUNT_PATH);
  strcpy(directorio, dir.c_str());

  // PSRAM is mandatory
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
  app.run(&VGAController);

  Serial.println("Emulator finished");

  preferences.putString("Dir", String(directorio));

  ESP.restart();
}









