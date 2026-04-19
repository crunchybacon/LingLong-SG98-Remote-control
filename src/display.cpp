#include "display.h" 
#include "main.h"   
#include "gbw.h"
#include "definitions.h"

#include <Adafruit_GFX.h>    // Core graphics library
#include <Adafruit_ST7789.h> // Hardware-specific library for ST7789
#include <SPI.h>             // Arduino SPI library
#include <math.h>

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
#include "comm.h"

#ifdef RT_DRIVE 
#include "motorcontrol_rt.h"
#endif 
#ifdef JMC_DRIVE
#include "motorcontrol_jmc.h"
#endif

#define COLOR_NAVY 0x0010
#define COLOR_GREEN 0x4fe9
#define COLOR_RED 0xe803
#define COLOR_YELLOW 0xe7e0

// Modern Palette
#define COLOR_BG 0x0000
#define COLOR_CARD 0x18C3
#define COLOR_CARD_BORDER 0x3186
#define COLOR_ACCENT 0x6675
#define COLOR_TEXT_DIM 0x8410

const int SCREEN_WIDTH = 280;
const int SCREEN_HEIGHT = 240;
Adafruit_ST7789 *tft = NULL;

void drawCard(int x, int y, int w, int h, uint16_t color, uint16_t borderColor, const char* label) {
    tft->fillRoundRect(x, y, w, h, 12, color);
    tft->drawRoundRect(x, y, w, h, 12, borderColor);
    if (label) {
        tft->setFont(&FreeSans9pt7b);
        tft->setTextColor(COLOR_TEXT_DIM);
        tft->setCursor(x + 10, y + 20);
        tft->print(label);
    }
}

#define SCREEN_IDLE      0
#define SCREEN_GBW_IDLE  1
#define SCREEN_GRINDING  2
#define SCREEN_PURGING   3
#define SCREEN_MENU         4
#define SCREEN_GRINDING_GBW 5
#define SCREEN_CALIBRATING  6
#define SCREEN_ERROR        7

int initializedScreenId = -1;
bool dimmed = false;
bool disp_updateRequired = true;
unsigned long drawnErrorAt;


void display_init() { 
  SPIClass *spi = new SPIClass(HSPI);
  spi->begin(DISP_SCL, -1, DISP_SDA, DISP_CS);
  tft = new Adafruit_ST7789(spi, DISP_CS, DISP_DC, DISP_RST);
  // 80MHz should work, but may need lower speeds if attached to the motor
  tft->setSPISpeed(80000000);
  // this will vary depending on display type
  tft->init(SCREEN_HEIGHT, SCREEN_WIDTH, SPI_MODE0);
  tft->setRotation(1); // Landscape

  // Set backlight pin as output and turn on backlight
  //pinMode(DISP_BL, OUTPUT);
  ledcSetup(1,4000,8);
  ledcAttachPin(DISP_BL, 1);
  ledcWrite(1, 0);
  update_display();
}

void setBrightness() { 
  static uint8_t lastBrightness;
  uint8_t newBrightness;
  static int dimTime = 15; 

  if(lastActivity + dimTime * 60000 < millis()) { 
    newBrightness = 1;
    dimmed = true;
  } else {
      newBrightness = Menu1[DISP_BRIGHTNESS].value;
      dimmed = false;
  }

  if(lastBrightness != newBrightness) { 
    ledcWrite(1, newBrightness);
    lastBrightness = newBrightness;
  }
}

void update_display() { 
  static unsigned long lastUpdate;
  if(newData == true) disp_updateRequired = true;

  if(disp_updateRequired != true) return; //This is mostly functional for testing.
  if(lastUpdate + 1000/DISP_REFRESH_RATE > millis()) return;

  lastUpdate = millis();
  disp_updateRequired = false;
  setBrightness();

    if(state == IDLE) { 
      drawIdleScreen();
    }

    if(state == IDLE_GBW) { 
      drawGbWIdleScreen();
    }

    if(state == IDLE || state == IDLE_GBW) { 
      drawErrorOverlay(); 
    } 

    if(state == GRINDING) { 
      drawGrindingOrPurgingScreen();
    }

    if(state == PURGING) { 
      drawGrindingOrPurgingScreen();
    }

    if(state == GRINDING_GBW) { 
      drawGrindingGbwScreen();
    }

    if(state == MENU1) { 
      enterMenu == true ? drawMenuValueScreen(1, menu1Selected) : drawMenuScreen(1, NUM_MENU1_ITEMS, menu1Selected);
    }

    if(state == MENU2) { 
      enterMenu == true ? drawMenuValueScreen(2, menu2Selected) : drawMenuScreen(2, NUM_MENU2_ITEMS, menu2Selected);
    }

    if(state == MENU3) { 
      enterMenu == true ? drawMenuValueScreen(3, menu3Selected) : drawMenuScreen(3, NUM_MENU3_ITEMS, menu3Selected);
    }

    if(state == CALIBRATING) { 
      drawCalibratingScreen();
    }
}

void display_off() {
    if (tft) {
        tft->fillScreen(ST77XX_BLACK);
        tft->enableDisplay(false); // turn off display output (if supported)
    }
    ledcWrite(1,0);
}

const char* getCommStatusText(SYSTEM_STATUS status) {
  switch (status) {
    case MOTOR_NOT_CONNECTED:return "Offline";
    case MOTOR_NOT_READY:    return "Starting..";
    case MOTOR_READY:        return "Online";
    case MOTOR_ENABLED:      return "Active";
    case MOTOR_FAULT:        return "Fault";
    default:           return "UNKNOWN";
  }
}

uint16_t getCommStatusColor(SYSTEM_STATUS status) {
  switch (status) {
    case MOTOR_NOT_CONNECTED: return COLOR_RED;
    case MOTOR_NOT_READY: return COLOR_YELLOW;
    case MOTOR_READY: return COLOR_GREEN;
    case MOTOR_ENABLED: return COLOR_GREEN;
    case MOTOR_FAULT: return COLOR_RED;
    default: return ST77XX_WHITE;
  }
}

const char* getScaleStatusText(SCALE_STATUS scaleStatus) {
  switch (scaleStatus) {
    case SCALE_DISCONNECTED: return "Offline";
    case SCALE_CONNECTED:        return "Online";
    case SCALE_CONNECTING:    return "Linking..";
    case SCALE_LOST:        return "Lost";
    default:           return "UNKNOWN";
  }
}

uint16_t getScaleStatusColor(SCALE_STATUS status) {
  switch (status) {
    case SCALE_CONNECTED: return COLOR_GREEN;
    case SCALE_DISCONNECTED: return COLOR_RED;
    case SCALE_CONNECTING: return COLOR_YELLOW;
    case SCALE_LOST: return COLOR_YELLOW;
    default: return ST77XX_WHITE;
  }
}

const char* getErrorChar(uint8_t error) {
  switch(error) {
    case 1: return "Drive connection failed";
    case 2: return "Drive connection lost";
    case 3: return "Drive fault:";
    case 4: return "drive packet loss > 100";
    case 5: return "Failed to create mutex";
    case 6: return "Motor stalled..";
    case 7: return "Write error, reboot drive";
    case 8: return "First use, reboot drive";
    case 100: return "Unexpected set RPM";
    case 101: return "No drive connection";
    case 102: return "Calibration cancelled";
    case 103: return "Could not load stored data";
    case 104: return "Scale connection lost";
    case 105: return "No output after 3s";
    case 106: return "No scale connected";
    case 107: return "Please Reboot Drive";
    default: return "Unknown Error";
  }
}


void drawErrorOverlay() {
  const int OVERLAY_Y = 5;
  static uint8_t prevError = 0xFF;
  static int prevScreenId = -1;
  static unsigned long errorDrawnAt;

  // Auto-clear minor errors after 5 seconds
  if(errorDrawnAt + 5000 < millis() && error > 100 && prevError == error) { 
    error = 0;
  }

  // If the error state changes, force a full UI redraw
  if (prevError != error) {
      prevError = error;
      initializedScreenId = -1; 
      disp_updateRequired = true;
      errorDrawnAt = millis();
      return; // Let the UI clear and redraw first; we'll draw the overlay on the next frame
  }

  // Check if the underlying screen was just redrawn
  bool needRedraw = false;
  if (prevScreenId != initializedScreenId) {
      prevScreenId = initializedScreenId;
      needRedraw = true;
  }

  if (!needRedraw) return;
  if (error == 0) return;

  // --- Draw floating error pill ---
  const char* errMsg = getErrorChar(error);
  tft->setFont(&FreeSans9pt7b);

  int16_t x1, y1; uint16_t w, h;
  tft->getTextBounds(errMsg, 0, 0, &x1, &y1, &w, &h);

  int textX = SCREEN_WIDTH/2 - w/2;
  int textY = OVERLAY_Y + h + 6; 

  int bgPad = 10;
  tft->fillRoundRect(textX - bgPad, OVERLAY_Y, w + 2*bgPad, h + 14, 8, COLOR_RED);
  tft->drawRoundRect(textX - bgPad, OVERLAY_Y, w + 2*bgPad, h + 14, 8, ST77XX_WHITE); // White border for contrast

  tft->setTextColor(ST77XX_WHITE);
  tft->setCursor(textX, textY);
  tft->print(errMsg);

  // Add hex error code if it's a drive fault
  if(error == 3) {
      char hexBuf[10];
      sprintf(hexBuf, "0x%02X", errorCode);
      tft->getTextBounds(hexBuf, 0, 0, &x1, &y1, &w, &h);
      int codeX = SCREEN_WIDTH/2 - w/2;
      int codeY = OVERLAY_Y + h + 20 + h + 6;
      tft->fillRoundRect(codeX - bgPad, OVERLAY_Y + h + 20, w + 2*bgPad, h + 14, 8, COLOR_RED);
      tft->drawRoundRect(codeX - bgPad, OVERLAY_Y + h + 20, w + 2*bgPad, h + 14, 8, ST77XX_WHITE);
      tft->setTextColor(ST77XX_WHITE);
      tft->setCursor(codeX, codeY);
      tft->print(hexBuf);
  }
}



void drawIdleScreen() {
  static int prevSetRPM = -1;
  static SYSTEM_STATUS prevSysStatus = MOTOR_INVALID;
  static bool prevAutoPurge = false;

  int16_t x1, y1;
  uint16_t w, h;

  if (initializedScreenId != SCREEN_IDLE) {
    tft->fillScreen(COLOR_BG);

    // Central RPM Card
    drawCard(20, 30, SCREEN_WIDTH - 40, 130, COLOR_CARD, COLOR_CARD_BORDER, "SET RPM");
    
    // Bottom Status Cards
    drawCard(20, 175, 115, 55, COLOR_CARD, COLOR_CARD_BORDER, "DRIVE");
    drawCard(145, 175, 115, 55, COLOR_CARD, COLOR_CARD_BORDER, "PURGE");

    tft->setFont(&FreeSans12pt7b);
    tft->setTextColor(COLOR_TEXT_DIM);
    // Position RPM unit
    tft->setCursor(SCREEN_WIDTH - 100, 110);
    tft->print("RPM");

    initializedScreenId = SCREEN_IDLE;
    prevSetRPM = -1;
    prevSysStatus = MOTOR_INVALID;
    prevAutoPurge = !Menu2[AUTO_PURGE_ENABLED].value;
  }

  // --- Update setRPM ---
  if (prevSetRPM != setRPM) {
    char newVal[10];
    sprintf(newVal, "%d", setRPM);

    tft->setFont(&FreeSans24pt7b);
    int valX = 45;
    int valY = 110;

    // Clear just the value area inside the card
    tft->fillRect(valX, valY - 40, 120, 50, COLOR_CARD);

    tft->setTextColor(ST77XX_WHITE);
    tft->setCursor(valX, valY);
    tft->print(newVal);

    prevSetRPM = setRPM;
  }

  // --- Update Drive Status ---
  if (prevSysStatus != currentStatus) {
    const char* newStatus = getCommStatusText(currentStatus);
    tft->setFont(&FreeSans9pt7b);
    tft->fillRect(22, 205, 111, 20, COLOR_CARD); // Clear status area in Drive card
    tft->setTextColor(getCommStatusColor(currentStatus));
    tft->setCursor(25, 220);
    tft->print(newStatus);
    prevSysStatus = currentStatus;
  }

  // --- Update Purge Status ---
  if (prevAutoPurge != Menu2[AUTO_PURGE_ENABLED].value) {
    const char* purgeStatus = Menu2[AUTO_PURGE_ENABLED].value ? "Auto" : "Off";
    tft->setFont(&FreeSans9pt7b);
    tft->fillRect(147, 205, 111, 20, COLOR_CARD); // Clear status area in Purge card
    tft->setTextColor(Menu2[AUTO_PURGE_ENABLED].value ? COLOR_GREEN : COLOR_TEXT_DIM);
    tft->setCursor(150, 220);
    tft->print(purgeStatus);
    prevAutoPurge = Menu2[AUTO_PURGE_ENABLED].value;
  }
}

void drawGbWIdleScreen() {
  static float prevSetWeight = -1000;
  static float prevCurrentWeight = -1000;
  static SYSTEM_STATUS prevSysStatus = MOTOR_INVALID;
  static SCALE_STATUS prevScaleStatus = INVALID_SCALE_STATUS;

  if (initializedScreenId != SCREEN_GBW_IDLE) {
    tft->fillScreen(COLOR_BG);

    // Large Target Weight Card
    drawCard(20, 20, SCREEN_WIDTH - 40, 110, COLOR_CARD, COLOR_CARD_BORDER, "TARGET WEIGHT");
    
    // Bottom Info Cards
    drawCard(20, 140, 115, 85, COLOR_CARD, COLOR_CARD_BORDER, "LIVE");
    drawCard(145, 140, 115, 40, COLOR_CARD, COLOR_CARD_BORDER, "DRIVE");
    drawCard(145, 185, 115, 40, COLOR_CARD, COLOR_CARD_BORDER, "SCALE");

    initializedScreenId = SCREEN_GBW_IDLE;
    prevSetWeight = -1.0;
    prevCurrentWeight = -1.0;
    prevSysStatus = MOTOR_INVALID;
    prevScaleStatus = INVALID_SCALE_STATUS;
  }

  // --- Target Weight ---
  if (prevSetWeight != setWeight) {
    char newVal[16];
    sprintf(newVal, "%.1fg", setWeight/1000.0);
    tft->setFont(&FreeSans24pt7b);
    int valX = 40;
    int valY = 95;
    tft->fillRect(valX, valY - 40, 180, 50, COLOR_CARD);
    tft->setTextColor(ST77XX_WHITE);
    tft->setCursor(valX, valY);
    tft->print(newVal);
    prevSetWeight = setWeight;
  }

  // --- Live Weight ---
  if (prevCurrentWeight != currentWeight) {
    char newVal[16];
    sprintf(newVal, "%.1fg", (currentWeight / 1000.0));
    tft->setFont(&FreeSans18pt7b);
    int valX = 30;
    int valY = 210;
    tft->fillRect(valX, valY - 30, 95, 40, COLOR_CARD);
    tft->setTextColor(getScaleStatusColor(scaleStatus));
    tft->setCursor(valX, valY);
    tft->print(newVal);
    prevCurrentWeight = currentWeight;
  }

  // --- Drive Status ---
  if (prevSysStatus != currentStatus) {
    const char* newStatus = getCommStatusText(currentStatus);
    tft->setFont(&FreeSans9pt7b);
    tft->fillRect(147, 161, 111, 18, COLOR_CARD); // Shifted from 158 to 161
    tft->setTextColor(getCommStatusColor(currentStatus));
    tft->setCursor(150, 176); // Shifted from 175 to 176
    tft->print(newStatus);
    prevSysStatus = currentStatus;
  }

  // --- Scale Status ---
  if (prevScaleStatus != scaleStatus) {
    const char* newScaleText = getScaleStatusText(scaleStatus);
    tft->setFont(&FreeSans9pt7b);
    tft->fillRect(147, 206, 111, 18, COLOR_CARD); // Shifted from 203 to 206
    tft->setTextColor(getScaleStatusColor(scaleStatus));
    tft->setCursor(150, 221); // Shifted from 220 to 221
    tft->print(newScaleText);
    prevScaleStatus = scaleStatus;
  }
}

void drawGrindingOrPurgingScreen() {
    static int16_t prevRPM = -32768;
    static int16_t prevTorque = 0xFFFF;
    static SYSTEM_STATUS prevSysStatus = MOTOR_INVALID;
    static bool prevPurgingDrawn = false;

    // Layout constants
    const int speedoCX = SCREEN_WIDTH - 75;
    const int speedoCY = 90;
    const int speedoR = 60;

    if (initializedScreenId != SCREEN_GRINDING && initializedScreenId != SCREEN_PURGING) {
        tft->fillScreen(COLOR_BG);
        
        // RPM Card
        drawCard(15, 20, 130, 130, COLOR_CARD, COLOR_CARD_BORDER, "LIVE RPM");
        
        // Status Card
        drawCard(15, 165, SCREEN_WIDTH - 30, 60, COLOR_CARD, COLOR_CARD_BORDER, "SYSTEM STATUS");

        initializedScreenId = SCREEN_GRINDING;
        prevRPM = -32768;
        prevTorque = 0xFFFF;
        prevSysStatus = MOTOR_INVALID;
        prevPurgingDrawn = false;
    }

    // --- RPM ---
    if (prevRPM != motor_currentRPM) {
      char newVal[10];
      sprintf(newVal, "%d", motor_currentRPM);
      tft->setFont(&FreeSans18pt7b);
      int valX = 25;
      int valY = 100;
      tft->fillRect(valX, valY - 30, 110, 40, COLOR_CARD);
      tft->setTextColor(ST77XX_WHITE);
      tft->setCursor(valX, valY);
      tft->print(newVal);
      prevRPM = motor_currentRPM;
    }

    // --- Torque Gauge ---
    #ifdef RT_DRIVE
      int16_t targetTorquePercent = ((100 * motor_currentTorque) / Menu1[SETMOTORTORQUE].value); 
    #else
      int16_t targetTorquePercent = ((10*motor_currentTorque)/Menu1[SETMOTORTORQUE].value) ; 
    #endif
    
    static float displayTorquePercent = 0;
    if (initializedScreenId != SCREEN_GRINDING && initializedScreenId != SCREEN_PURGING) {
        displayTorquePercent = targetTorquePercent;
    }

    if (abs(displayTorquePercent - targetTorquePercent) > 0.5f || prevTorque != motor_currentTorque) {
        displayTorquePercent += (targetTorquePercent - displayTorquePercent) * 0.2f;
        
        if (abs(displayTorquePercent - targetTorquePercent) > 0.5f) {
            disp_updateRequired = true;
        } else {
            displayTorquePercent = targetTorquePercent;
        }

        // Redraw gauge area
        tft->fillCircle(speedoCX, speedoCY, speedoR + 5, COLOR_BG);
        
        // Background dotted track
        for (int i = 0; i <= 270; i += 6) {
            float rad = (135 + i) * M_PI / 180.0;
            tft->fillCircle(speedoCX + cos(rad)*(speedoR - 5), speedoCY + sin(rad)*(speedoR - 5), 1, COLOR_TEXT_DIM);
        }

        float angle = (constrain(abs(displayTorquePercent), 0, 100) / 100.0) * 270.0;
        for (int i = 0; i < angle; i += 2) {
            float rad = (135 + i) * M_PI / 180.0;
            
            // Sweep color: Green -> Yellow -> Red
            float localPercent = (float)i / 270.0f;
            uint8_t red, green;
            if (localPercent <= 0.5f) {
                red = (uint8_t)(255.0f * (localPercent / 0.5f));
                green = 255;
            } else {
                red = 255;
                green = (uint8_t)(255.0f * (1.0f - (localPercent - 0.5f) / 0.5f));
            }
            uint16_t color = tft->color565(red, green, 0);

            // Draw thick arc with multiple lines
            for(int r = speedoR - 10; r <= speedoR; r++) {
                tft->drawPixel(speedoCX + cos(rad)*r, speedoCY + sin(rad)*r, color);
            }
        }

        tft->setFont(&FreeSans12pt7b);
        tft->setTextColor(ST77XX_WHITE);
        char percentStr[8];
        sprintf(percentStr, "%d%%", (int)displayTorquePercent);
        int16_t x1, y1; uint16_t w, h;
        tft->getTextBounds(percentStr, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor(speedoCX - w / 2, speedoCY + h/2);
        tft->print(percentStr);

        prevTorque = motor_currentTorque;
    }

    // --- Status Updates ---
    if (prevSysStatus != currentStatus || (state == PURGING && !prevPurgingDrawn)) {
        tft->setFont(&FreeSans9pt7b);
        tft->fillRect(17, 195, 246, 20, COLOR_CARD);
        
        const char* statusStr = (state == PURGING) ? "PURGING..." : getCommStatusText(currentStatus);
        tft->setTextColor((state == PURGING) ? COLOR_YELLOW : getCommStatusColor(currentStatus));
        tft->setCursor(20, 210);
        tft->print(statusStr);
        
        prevSysStatus = currentStatus;
        prevPurgingDrawn = (state == PURGING);
    }
}

void drawGrindingGbwScreen() {
    static int16_t prevRPM = -32768;
    static int32_t prevCurrentWeight = -999999;
    static SYSTEM_STATUS prevSysStatus = MOTOR_INVALID;
    static SCALE_STATUS prevScaleStatus = INVALID_SCALE_STATUS;

    // Layout constants
    const int speedoCX = SCREEN_WIDTH - 75;
    const int speedoCY = 90;
    const int speedoR = 60;

    if (initializedScreenId != SCREEN_GRINDING_GBW) {
        tft->fillScreen(COLOR_BG);
        
        // Target Card
        drawCard(15, 20, 130, 130, COLOR_CARD, COLOR_CARD_BORDER, "TARGET");
        
        // Status Card
        drawCard(15, 165, SCREEN_WIDTH - 30, 60, COLOR_CARD, COLOR_CARD_BORDER, "GRINDING STATUS");

        initializedScreenId = SCREEN_GRINDING_GBW;
        prevRPM = -32768;
        prevCurrentWeight = -999999;
        prevSysStatus = MOTOR_INVALID;
        prevScaleStatus = INVALID_SCALE_STATUS;
    }

    // --- Target Display ---
    if (prevRPM != motor_setRPM) {
      char targetStr[16];
      sprintf(targetStr, "%.1fg", setWeight/1000.0);
      tft->setFont(&FreeSans12pt7b);
      int valX = 25;
      int valY = 100;
      tft->fillRect(valX, valY - 20, 110, 30, COLOR_CARD);
      tft->setTextColor(ST77XX_WHITE);
      tft->setCursor(valX, valY);
      tft->print(targetStr);
      prevRPM = motor_setRPM;
    }

    // --- Progress Gauge ---
    int32_t weight = currentWeight;
    int32_t goal = setWeight;
    if (goal == 0) goal = 1; 
    float targetPercent = constrain(float(weight) / float(goal), 0.0, 1.0);

    static float displayPercent = 0.0f;
    if (initializedScreenId != SCREEN_GRINDING_GBW) {
        displayPercent = targetPercent;
    }

    if (abs(displayPercent - targetPercent) > 0.005f || prevCurrentWeight != weight) {
        displayPercent += (targetPercent - displayPercent) * 0.2f;
        
        if (abs(displayPercent - targetPercent) > 0.005f) {
            disp_updateRequired = true;
        } else {
            displayPercent = targetPercent;
        }

        tft->fillCircle(speedoCX, speedoCY, speedoR + 5, COLOR_BG);
        
        // Background dotted track
        for (int i = 0; i <= 270; i += 6) {
            float rad = (135 + i) * M_PI / 180.0;
            tft->fillCircle(speedoCX + cos(rad)*(speedoR - 5), speedoCY + sin(rad)*(speedoR - 5), 1, COLOR_TEXT_DIM);
        }

        float angle = displayPercent * 270.0;
        for (int i = 0; i < angle; i += 2) {
            float rad = (135 + i) * M_PI / 180.0;
            
            // Sweep color: Cyan -> Green
            float localPercent = (float)i / 270.0f;
            uint8_t red = 0;
            uint8_t green = 255;
            uint8_t blue = (uint8_t)(255.0f * (1.0f - localPercent));
            uint16_t color = tft->color565(red, green, blue);
            
            if (displayPercent >= 1.0f) color = COLOR_GREEN; // Solid green when complete

            // Draw thick arc with multiple lines
            for(int r = speedoR - 10; r <= speedoR; r++) {
                tft->drawPixel(speedoCX + cos(rad)*r, speedoCY + sin(rad)*r, color);
            }
        }

        tft->setFont(&FreeSans18pt7b);
        tft->setTextColor(ST77XX_WHITE);
        char weightStr[16];
        sprintf(weightStr, "%.1fg", (currentWeight / 1000.0));
        int16_t x1, y1; uint16_t w, h;
        tft->getTextBounds(weightStr, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor(speedoCX - w / 2, speedoCY + h/2);
        tft->print(weightStr);

        prevCurrentWeight = weight;
    }

    // --- Status Updates ---
    if (prevSysStatus != currentStatus || prevScaleStatus != scaleStatus) {
        tft->setFont(&FreeSans9pt7b);
        tft->fillRect(17, 195, 246, 20, COLOR_CARD);
        
        char statusStr[32];
        snprintf(statusStr, sizeof(statusStr), "D: %s | S: %s", 
                getCommStatusText(currentStatus), getScaleStatusText(scaleStatus));
        tft->setTextColor(ST77XX_WHITE);
        tft->setCursor(20, 210);
        tft->print(statusStr);
        
        prevSysStatus = currentStatus;
        prevScaleStatus = scaleStatus;
    }
}

void drawCalibratingScreen() {
    static int16_t prevRPM = -32768;
    static int16_t prevTorque = 0xFFFF;
    static SYSTEM_STATUS prevSysStatus = MOTOR_INVALID;
    static int prevStep = -1;

    int totalSteps = 1 + SIZEOFCALIBRATEARRAY / sizeof(uint16_t);
    int step = currentCal + 1; 

    if (initializedScreenId != SCREEN_CALIBRATING) {
        tft->fillScreen(COLOR_BG);
        
        // Header Card
        drawCard(20, 20, SCREEN_WIDTH - 40, 50, COLOR_CARD, COLOR_CARD_BORDER, nullptr);
        tft->setFont(&FreeSans12pt7b);
        tft->setTextColor(ST77XX_WHITE);
        tft->setCursor(SCREEN_WIDTH/2 - 65, 52);
        tft->print("CALIBRATING");

        // Live RPM Card
        drawCard(20, 80, 115, 80, COLOR_CARD, COLOR_CARD_BORDER, "LIVE RPM");
        
        // Bottom Progress Card
        drawCard(20, 170, SCREEN_WIDTH - 40, 60, COLOR_CARD, COLOR_CARD_BORDER, "PROGRESS");

        initializedScreenId = SCREEN_CALIBRATING;
        prevRPM = -32768;
        prevTorque = 0xFFFF;
        prevSysStatus = MOTOR_INVALID;
        prevStep = -1;
    }

    // --- RPM Update ---
    if (prevRPM != motor_currentRPM) {
      char newVal[10];
      sprintf(newVal, "%d", motor_currentRPM);
      tft->setFont(&FreeSans12pt7b);
      tft->fillRect(30, 120, 90, 30, COLOR_CARD);
      tft->setTextColor(ST77XX_WHITE);
      tft->setCursor(35, 145);
      tft->print(newVal);
      prevRPM = motor_currentRPM;
    }

    // --- Progress Update ---
    if (prevStep != step) {
        // Clear text and progress bar area
        tft->fillRect(30, 195, 220, 30, COLOR_CARD);
        
        // Progress Bar
        int barW = SCREEN_WIDTH - 100;
        int barH = 8;
        int barX = 35;
        int barY = 210;
        tft->drawRect(barX, barY, barW, barH, COLOR_CARD_BORDER);
        int progressW = (int)((float)step / (float)totalSteps * barW);
        tft->fillRect(barX, barY, progressW, barH, COLOR_GREEN);

        // Step Text
        char stepStr[16];
        sprintf(stepStr, "%d/%d", step, totalSteps);
        tft->setFont(&FreeSans9pt7b);
        tft->setTextColor(ST77XX_WHITE);
        tft->setCursor(barX + barW + 10, 218);
        tft->print(stepStr);

        prevStep = step;
    }
}

// Helper to get menu pointer by number
static const menuEntry* getMenuByNum(int menuNum) {
    switch(menuNum) {
        case 1: return Menu1;
        case 2: return Menu2;
        case 3: return Menu3;
        default: return nullptr;
    }
}

void drawMenuScreen(int menuNum, int numItems, int selectedIdx) {
    static int prevSelectedIdx = -1;
    static int prevMenuNum = -1;

    const menuEntry* menu = getMenuByNum(menuNum);

    // Layout constants
    const int itemHeight = 35;
    const int centerY = SCREEN_HEIGHT / 2;
    const int cardW = SCREEN_WIDTH - 40;
    const int cardH = 50;
    const int cardX = 20;
    const int cardY = centerY - cardH / 2;

    int thisScreenId = menuNum * 1000; 

    if (initializedScreenId != thisScreenId) {
        tft->fillScreen(COLOR_BG);
        initializedScreenId = thisScreenId;
        prevSelectedIdx = -1; 
        prevMenuNum = menuNum;
    }

    if (prevSelectedIdx != selectedIdx || prevMenuNum != menuNum) {
        // Redraw list items
        tft->fillRect(0, 0, SCREEN_WIDTH, cardY, COLOR_BG);
        tft->fillRect(0, cardY + cardH, SCREEN_WIDTH, SCREEN_HEIGHT - (cardY + cardH), COLOR_BG);

        tft->setFont(&FreeSans9pt7b);
        
        // Draw 2 items above and 2 below
        for (int i = -2; i <= 2; i++) {
            if (i == 0) continue; // Skip selected, handled by card
            
            int idx = (selectedIdx + i + numItems) % numItems;
            int y = centerY + (i * itemHeight) + 8; // Offset for font baseline
            
            // Fade items based on distance
            uint16_t color = (abs(i) == 1) ? COLOR_TEXT_DIM : COLOR_CARD_BORDER;
            tft->setTextColor(color);
            
            int16_t x1, y1; uint16_t w, h;
            tft->getTextBounds(menu[idx].name, 0, 0, &x1, &y1, &w, &h);
            tft->setCursor(SCREEN_WIDTH/2 - w/2, y);
            tft->print(menu[idx].name);
        }

        // Draw selected item in card
        drawCard(cardX, cardY, cardW, cardH, COLOR_CARD, COLOR_GREEN, nullptr);
        
        tft->setFont(&FreeSans12pt7b);
        tft->setTextColor(ST77XX_WHITE);
        int16_t x1, y1; uint16_t w, h;
        tft->getTextBounds(menu[selectedIdx].name, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor(SCREEN_WIDTH/2 - w/2, centerY + 8);
        tft->print(menu[selectedIdx].name);

        prevSelectedIdx = selectedIdx;
        prevMenuNum = menuNum;
    }
}

void drawMenuValueScreen(int menuNum, int selectedIdx) {
    static int prevSelectedIdx = -1, prevMenuNum = -1;
    static int16_t prevValue = 0xFFFF;
    int thisScreenId = (menuNum << 8) | selectedIdx;
    
    const menuEntry* menu = getMenuByNum(menuNum);
    const menuEntry& item = menu[selectedIdx];

    if (initializedScreenId != thisScreenId) {
        tft->fillScreen(COLOR_BG);
        
        // Header Card (Name of the setting)
        drawCard(20, 20, SCREEN_WIDTH - 40, 50, COLOR_CARD, COLOR_CARD_BORDER, nullptr);
        
        tft->setFont(&FreeSans12pt7b);
        tft->setTextColor(ST77XX_WHITE);
        int16_t x1, y1; uint16_t w, h;
        
        const char* headerText = item.name;
        if (menuNum == 1 && selectedIdx == Menu1Items::CALIBRATE) headerText = "Calibrate?";
        
        tft->getTextBounds(headerText, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor(SCREEN_WIDTH/2 - w/2, 52);
        tft->print(headerText);

        initializedScreenId = thisScreenId;
        prevValue = 0xFFFF; 
    }

    // Special case for CALIBRATE which doesn't have a binary toggle
    if (menuNum == 1 && selectedIdx == Menu1Items::CALIBRATE) {
        if (prevValue == 0xFFFF) { // Draw once
            tft->setFont(&FreeSans9pt7b);
            tft->setTextColor(ST77XX_WHITE);
            const char* msg1 = "Long press encoder to";
            const char* msg2 = "start calibration.";
            const char* msg3 = "Press Start to exit.";
            int16_t x1, y1; uint16_t w, h;
            tft->getTextBounds(msg1, 0, 0, &x1, &y1, &w, &h); tft->setCursor(SCREEN_WIDTH/2 - w/2, 110); tft->print(msg1);
            tft->getTextBounds(msg2, 0, 0, &x1, &y1, &w, &h); tft->setCursor(SCREEN_WIDTH/2 - w/2, 130); tft->print(msg2);
            tft->getTextBounds(msg3, 0, 0, &x1, &y1, &w, &h); tft->setCursor(SCREEN_WIDTH/2 - w/2, 170); tft->print(msg3);
            prevValue = item.value;
        }
        return;
    }

    if (prevValue != item.value) {
        // Clear value area
        tft->fillRect(0, 80, SCREEN_WIDTH, SCREEN_HEIGHT - 80, COLOR_BG);

        // --- Toggle Case (Binary) ---
        if (item.minValue == 0 && item.maxValue == 1 && item.scalar == 1) {
            bool isEnabled = (item.value == 1);
            
            const char* label0 = "DISABLED";
            const char* label1 = "ENABLED";
            
            if (menuNum == 1 && selectedIdx == Menu1Items::RESET) {
                label0 = "BACK";
                label1 = "RESET";
            } else if (menuNum == 3 && selectedIdx == Menu3Items::SAVE_SCALE) {
                label0 = "DON'T SAVE";
                label1 = "SAVE";
            }
            
            // Draw 2 Cards for toggle
            drawCard(40, 100, SCREEN_WIDTH - 80, 50, isEnabled ? COLOR_CARD : COLOR_BG, isEnabled ? COLOR_GREEN : COLOR_CARD_BORDER, nullptr);
            drawCard(40, 160, SCREEN_WIDTH - 80, 50, !isEnabled ? COLOR_CARD : COLOR_BG, !isEnabled ? COLOR_RED : COLOR_CARD_BORDER, nullptr);

            tft->setFont(&FreeSans12pt7b);
            int16_t x1, y1; uint16_t w, h;
            
            tft->setTextColor(isEnabled ? ST77XX_WHITE : COLOR_TEXT_DIM);
            tft->getTextBounds(label1, 0, 0, &x1, &y1, &w, &h);
            tft->setCursor(SCREEN_WIDTH/2 - w/2, 132); tft->print(label1);
            
            tft->setTextColor(!isEnabled ? ST77XX_WHITE : COLOR_TEXT_DIM);
            tft->getTextBounds(label0, 0, 0, &x1, &y1, &w, &h);
            tft->setCursor(SCREEN_WIDTH/2 - w/2, 192); tft->print(label0);

            // Special case logic for SAVE_SCALE MAC output
            if (menuNum == 3 && selectedIdx == Menu3Items::SAVE_SCALE && !isEnabled) {
                String macCopy = "", nameCopy = "";
                if (xSemaphoreTake(scaleMutex, portMAX_DELAY)) {
                  if(scale_mac != "") {
                    macCopy = scale_mac;
                    nameCopy = scale_name;
                  } else { 
                    macCopy = scale_connect_mac;
                    nameCopy = scale_connect_name;
                  }
                  xSemaphoreGive(scaleMutex);
                }
                
                tft->setFont(&FreeSans9pt7b);
                tft->setTextColor(COLOR_TEXT_DIM);
                String dispText = nameCopy + " (" + macCopy + ")";
                tft->getTextBounds(dispText.c_str(), 0, 0, &x1, &y1, &w, &h);
                tft->setCursor(SCREEN_WIDTH/2 - w/2, 230);
                tft->print(dispText.c_str());
            }

        } 
        // --- Value Case (Numeric) ---
        else {
            drawCard(40, 100, SCREEN_WIDTH - 80, 100, COLOR_CARD, COLOR_GREEN, nullptr);

            char valStr[16];
            snprintf(valStr, sizeof(valStr), "%d", item.value);
            
            tft->setFont(&FreeSans24pt7b);
            tft->setTextColor(ST77XX_WHITE);
            int16_t x1, y1; uint16_t w, h;
            tft->getTextBounds(valStr, 0, 0, &x1, &y1, &w, &h);
            tft->setCursor(SCREEN_WIDTH/2 - w/2, 160);
            tft->print(valStr);

            if (item.unit) {
                tft->setFont(&FreeSans9pt7b);
                tft->setTextColor(COLOR_TEXT_DIM);
                tft->getTextBounds(item.unit, 0, 0, &x1, &y1, &w, &h);
                tft->setCursor(SCREEN_WIDTH/2 - w/2, 185);
                tft->print(item.unit);
            }
        }
        prevValue = item.value;
    }
}
