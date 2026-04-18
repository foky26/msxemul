
/*
 * Optional SD Card connections:
 *   MISO => GPIO 16  (2 for PICO-D4)
 *   MOSI => GPIO 17  (12 for PICO-D4)
 *   CLK  => GPIO 14
 *   CS   => GPIO 13
 *
 * To change above assignment fill other paramaters of FileBrowser::mountSDCard().
 */


#include <Preferences.h>

#include "fabgl.h"
#include "MSX.h"
#include <SPI.h>
#include <SD.h>
#include <vector> 

#include <stdio.h>

#include "fabui.h"


#define FORMAT_ON_FAIL     false
#define SDCARD_MOUNT_PATH  "/SD"

char directorio [80];


Preferences preferences;

fabgl::VGA16Controller VGAController;
fabgl::PS2Controller PS2Controller;
fabgl::Canvas        *Canvas;


#include "esp32-hal-psram.h"
extern "C" {
  #include "esp_spiram.h"
}

#ifndef LSB_FIRST
#define LSB_FIRST
#endif

// fMSX 전역 변수 연결
extern "C" {
  extern int Mode;
  extern int RAMPages;
  extern int VRAMPages;
  extern const char *ROMName[MAXCARTS];
  extern const char *ProgDir;
  extern byte Verbose;
  extern  int SetAudio(int, int);
  extern void StopAudio();
}






// ESP32-MSX 
#include "ESP32_MSX.h"

// PSLReg 
byte PSLReg = 0;


bool emuRunning = false;
bool selecting = false;


// SD 
#define SD_MISO 2
#define SD_MOSI 12
#define SD_CLK  14
#define SD_CS   13


class MyApp : public uiApp {

 
  uiFileBrowser * fileBrowser;
  uiFrame * frame;
  uiButton * formatBtn;

  void init() {


   // fMSX 
  Mode = MSX_MSX1 | MSX_NTSC | MSX_GUESSA | MSX_GUESSB;
  RAMPages = 4;   
  VRAMPages = 2;  
  Verbose = 1;
  ProgDir = "/"; 



    rootWindow()->frameStyle().backgroundColor = RGB888(0, 0, 64);

    frame = new uiFrame(rootWindow(), "MSXEmul Emulator", Point(15, 10), Size(375, 275));
    frame->frameProps().hasCloseButton = false;

    // file browser
    
    fileBrowser = new uiFileBrowser(frame, Point(10, 25), Size(355, 210));
    
    fileBrowser->setDirectory(SDCARD_MOUNT_PATH);
   
    char ruta [160];
    strcpy(ruta,"");
    strncat(ruta,directorio+4,strlen(directorio)-4);
    ruta[sizeof(ruta)-1] = '\0';
    

    if (fileBrowser->content().exists(ruta,true))
    {    fileBrowser->setDirectory(directorio); }
   
    
    fileBrowser->update();

    formatBtn = new uiButton(frame, "Run BASIC", Point(10, 240), Size(100, 25));

    formatBtn->onClick = [&]() {
     
    ROMName[0] = 0;   
    ROMName[1] = 0;
             
     Serial.println("Starting MSX BASIC");
    
  
    Canvas->clear(); 
    Canvas->waitCompletion();
    VGAController.setResolution(VGA_320x200_60HzD); // Resolution for MSX
    VGAController.setMouseCursor(nullptr);

    // SD for MSX.ROM AND DISK.ROM
    Serial.println("Mounting SD card...");
    
    SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);

    if (!SD.begin(SD_CS)) {
    Serial.println("ERROR: SD Card Mount Failed!");
    Canvas->setPenColor(Color::Red);
    Canvas->drawText(10, 30, "SD Card Mount Failed!");
    Canvas->waitCompletion(); // 화면 출력이 완료될 때까지 대기
    while(1) { delay(1000); }
  }
    
  File root = SD.open("/");

   
    // StartMSX 
    emuRunning=true;
    int result = StartMSX(Mode, RAMPages, VRAMPages);
    Serial.printf("StartMSX returned: %d\n", result);
    emuRunning=false;
 
    StopAudio(); 
  
    
    
    TrashMachine();
   
      
      
    quit(0);

    };


    fileBrowser->onClick = [&]() {
      if (!fileBrowser->isDirectory()) { RunEmulator();}
     
    };

     fileBrowser->onKeyType=[&](fabgl::uiKeyEventInfo evt) {
    fabgl::VirtualKey key = evt.VK;

    if (key == fabgl::VirtualKey::VK_RETURN)  {
      if (!fileBrowser->isDirectory()) { RunEmulator();}
      
    }
    };
    
         

    setFocusedWindow(fileBrowser);
  }

  void updateBrowser()
  {
    
    fileBrowser->update();
   
  }

  
  // Run Emulator MSX
  void RunEmulator() {
    int64_t total, used;
    char fichero [160];
   

    char *Sdir="/";
   
         
    strcpy(directorio,fileBrowser->content().directory());
    strcpy(fichero,"");
    strncat(fichero,directorio+3,strlen(directorio)-3);
    strncat(fichero, Sdir, 160);
    
    strncat(fichero, fileBrowser->filename(), 160);
    fichero[sizeof(fichero)-1] = '\0';
    
  
    if ((strstr(fichero, ".ROM") != NULL) | (strstr(fichero, ".rom") != NULL) | (strstr(fichero, ".MX1") != NULL)| (strstr(fichero, ".mx1") != NULL))
    {
    ROMName[0] = fichero;   
    ROMName[1] = 0;
             
    Serial.println("Starting MSX emulation...");

    Serial.printf("Loading ROM: %s\n", fichero);
  
    Canvas->clear(); 
    Canvas->waitCompletion();
    VGAController.setResolution(VGA_320x200_60HzD);
    VGAController.setMouseCursor(nullptr);
    
    // SD for MSX.ROM AND DISK.ROM
    Serial.println("Mounting SD card...");
    SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);

    if (!SD.begin(SD_CS)) {
    Serial.println("ERROR: SD Card Mount Failed!");
    Canvas->setPenColor(Color::Red);
    Canvas->drawText(10, 30, "SD Card Mount Failed!");
    Canvas->waitCompletion(); // 화면 출력이 완료될 때까지 대기
    while(1) { delay(1000); }
  }
  
    File root = SD.open("/");
   

     // StartMSX 
    emuRunning=true;
    int result = StartMSX(Mode, RAMPages, VRAMPages);
    Serial.printf("StartMSX returned: %d\n", result);
    emuRunning=false;
    
    StopAudio(); 
    
    
    TrashMachine();
    

    

    quit(0);

    
    }

}


} app;





void setup()
{
  
 

  

 
  setCpuFrequencyMhz(240);
  Serial.begin(115200);
   

  Serial.println("\n\n=== MSX Emulator Starting ===");

  preferences.begin("Datos", false);
  String dir=preferences.getString("Dir", SDCARD_MOUNT_PATH);
  strcpy(directorio,dir.c_str());
  
  

  // PSRAM 
  if (psramInit()) {
    Serial.printf("PSRAM initialized: %d bytes\n", ESP.getPsramSize());
    Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
  } else {
    Serial.println("ERROR: PSRAM initialization FAILED!");
    while(1) { delay(1000); }
  }
  

  PS2Controller.begin(PS2Preset::KeyboardPort0_MousePort1);

  Serial.println("PS2 Controller initialized");
  
 
  if(!InitMemorySystem()) {
    Serial.println("ERROR: InitMemorySystem() failed!");
    while(1) { delay(1000); }
  }

  if(!InitMachine()) {
    Serial.println("ERROR: InitMachine() failed!");
    while(1) { delay(1000); }
  }


  Serial.println("Initializing sound system...");
  if(!SetAudio(22050, 127)) {
    Serial.println("WARNING: Sound initialization failed!");
  } else {
    Serial.println("Sound system initialized");
  }

   while (!FileBrowser::mountSDCard(FORMAT_ON_FAIL, SDCARD_MOUNT_PATH)) {delay(1000);};

  Serial.println("SD card mounted successfully");

  VGAController.begin();
   
  
  Serial.println("VGA16 Controller initialized");
  Canvas=new fabgl::Canvas(&VGAController);
 
  
  VGAController.setResolution(VGA_400x300_60Hz);

  
  Canvas->clear();
  Canvas->waitCompletion();
 
}




void loop()
{
        
    app.run(&VGAController);
    Serial.println("Emulator finished");

    String dir = directorio;
     
    preferences.putString("Dir", dir);
    ESP.restart(); 
 
}







