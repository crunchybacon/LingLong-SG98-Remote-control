#include "gbw.h"
#include "main.h"
#include "io.h"
#include "pdata.h"
#include "display.h"

#ifdef JMC_DRIVE
#include "motorcontrol_jmc.h"
#endif

#ifdef RT_DRIVE
#include "motorcontrol_rt.h"
#endif

// #define DEBUG_GBW
unsigned long startOfShot = 0;
float speedModifier = 1;
bool slow_phase = false;

//shared between tasks!
volatile int32_t currentWeight = 0; //in MG
volatile bool scaleConnected = false;
volatile SCALE_STATUS scaleStatus = INVALID_SCALE_STATUS;
volatile int16_t newWeightAvail;
//Only used in scaletask:
bool connect = false;
bool maintain = true;

//Used in main loop:
uint16_t slow_phase_at = 0;
bool grindingComplete = false;
bool gbw_started = false;
unsigned long shotStopped = 0;

namespace {
int16_t clamp_gbw_rpm(int16_t rpm) {
    if(rpm < 0) return 0;
    if(rpm > Menu1[SETMAX].value) return Menu1[SETMAX].value;
    return rpm;
}
}

// MUTEX >>
SemaphoreHandle_t scaleMutex;
String scale_connect_mac = "";
String scale_connect_name = "";
String scale_mac = "";
String scale_name = "";
// MUTEX <<

String mac_local = "";

BLEScale scale(DEBUG_SCALE);

GBW_WEIGHT _shot[300];
uint16_t last_shot_updated;

//BLEService weightService("0x0FFE"); // create service
//BLEByteCharacteristic weightCharacteristic("0xFF11",  BLEWrite | BLERead);

//SEPARATE TASK!!
void scales_init() { 
    NimBLEDevice::init("GbWService");
    if (xSemaphoreTake(scaleMutex, portMAX_DELAY)) {
        mac_local = scale_connect_mac;
        xSemaphoreGive(scaleMutex);
    } 
}

//SEPARATE TASK!!
void gbwVitals() 
{   
    static uint8_t prevScaleStatus;
    static unsigned long lastConnected;
    static int32_t prevWeight;

    if(scale.manage(connect, maintain, mac_local) == true) { 
        scaleStatus = SCALE_CONNECTED;
        if (xSemaphoreTake(scaleMutex, portMAX_DELAY)) {
            scale_mac = scale.connected_mac;
            scale_name = scale.connected_name;
            xSemaphoreGive(scaleMutex);
        } 
    }

    if(scale.isConnected() == true) { 
        scaleStatus = SCALE_CONNECTED;

    } else { 
        if(scale.isConnecting() == true) scaleStatus = SCALE_CONNECTING;
        else { 
            scaleStatus = SCALE_DISCONNECTED;
            currentWeight = 0;
        }
    }

    static unsigned long lastCheck;
    if(lastCheck + 5000 < millis()) { 
        lastCheck = millis();

        if (xSemaphoreTake(scaleMutex, portMAX_DELAY)) {
            mac_local = scale_connect_mac;
            xSemaphoreGive(scaleMutex);
        }
    }

    // always call newWeightAvailable to actually receive the datapoint from the scale,
    // otherwise getWeight() will return stale data
    if(scale.newWeightAvailable())
    {   
        currentWeight = scale.getWeight() * 1000; // we only deal in mg
        newWeightAvail = true;
        disp_updateRequired = true;
    } else newWeightAvail = false; //Here coz thats one round!

    // Manage connection states 
    if(state == IDLE_GBW || state == GRINDING_GBW) {
        connect = true;
        maintain = true;
    } else {
        if(state == IDLE || state == GRINDING || state == SLEEPING)  {
            maintain = false;
        }
        connect = false;
    }

    if(scaleStatus != prevScaleStatus) { 
        disp_updateRequired = true;
        prevScaleStatus = scaleStatus;
    }

    if(prevWeight != currentWeight) { 
        disp_updateRequired = true;
        prevWeight = currentWeight;
    }

}

void disconnect_for_sleep() { 
    maintain = false;
    connect = false;
    scale.disconnect();
}

void do_gbw() { 
    static unsigned long lastCall;
    static unsigned long lastUpdate;
    static unsigned long tareTime;
    static bool properShot;
    static bool learned;

    #ifndef DEBUG_GBW
    if(commStatus != COMM_CONNECTED) { 
        motorOff();
        state = IDLE_GBW;
    }
    #endif

    // --- Reset Route (First call or timeout) ---
    if(lastCall + GBW_RESET_DELAY < millis()) { 
        if(motor_setRPM != 0) motor_setRPM = 0;
        
        gbw_started = false;
        grindingComplete = false;
        slow_phase = false;
        startOfShot = 0;
        last_shot_updated = 0;
        slow_phase_at = 0;
        lastUpdate = millis();
        shotStopped = 0;
        properShot = false;
        learned = false;

        memset(_shot, 0, sizeof(_shot));

        if(scaleStatus != SCALE_CONNECTED) { 
            state = IDLE_GBW;
            error = 106;
            disp_updateRequired = true;
            return;
        }

        // --- Initial Tare Logic ---
        if(abs(currentWeight) > GBW_TARE_THRESHOLD) { 
            scale.tare();
            tareTime = millis();
            gbw_started = false;
        } else { 
            startOfShot = millis();
            last_shot_updated = 0;
            grindingComplete = false;
            gbw_started = true;
            tareTime = millis();
            disp_updateRequired = true;
        }
    }

    lastCall = millis();

    // --- Data Recording ---
    static int32_t lastWeight;
    bool recordingAllowed = (state == GRINDING_GBW && gbw_started);
    bool inCaptureWindow = (grindingComplete == false) || (lastUpdate + GBW_POST_GRIND_WINDOW > millis());

    if(recordingAllowed && inCaptureWindow && lastWeight != currentWeight) {
        lastWeight = currentWeight;
        if (last_shot_updated < (sizeof(_shot)/sizeof(_shot[0])) - 1) {
            last_shot_updated++;
            _shot[last_shot_updated].weight = abs(currentWeight);
            _shot[last_shot_updated].time = millis() - startOfShot;
        }
    }

    // --- Delayed Start (Post-Tare) ---
    if(gbw_started == false && tareTime + GBW_TARE_DELAY < millis()) {
        if(abs(currentWeight) < GBW_STABLE_THRESHOLD) { 
            startOfShot = millis();
            last_shot_updated = 0;
            grindingComplete = false;
            gbw_started = true;
            disp_updateRequired = true;
            lastUpdate = millis();
        } else { 
            scale.tare();
            tareTime = millis();
        }
    }

    // --- Grinding Phase Control ---
    if(gbw_started && !grindingComplete) {
        // Normal Phase
        if (!slow_phase && (millis() - startOfShot > Menu3[GBW_BUTTON_DELAY].value)) {
            motor_setRPM = clamp_gbw_rpm(Menu3[GBW_RPM_SET].value);
        }

        // Slow Phase Check
        if(Menu3[GBW_SLOW_MG].value > 0 && !slow_phase) {
            if(abs(currentWeight) >= (setWeight - Menu3[GBW_SLOW_MG].value)) { 
                motor_setRPM = clamp_gbw_rpm(min(Menu3[GBW_SLOW_RPM].value, Menu3[GBW_RPM_SET].value));
                slow_phase = true;
                slow_phase_at = millis() - startOfShot; 
                disp_updateRequired = true;
            }
        }

        // --- Stop Logic ---
        // 1. Prediction Stop
        if(gbw_predict() < 0) { 
            grindingComplete = true;
            motor_setRPM = 0;
            lastUpdate = millis();
            shotStopped = millis() - startOfShot;
            properShot = true;
            disp_updateRequired = true;
        }
        // 2. Backup Stop (Overshoot)
        else if(abs(currentWeight) > (int32_t)setWeight + GBW_BACKUP_STOP_MG) { 
            grindingComplete = true;
            motor_setRPM = 0;
            lastUpdate = millis();
            shotStopped = millis() - startOfShot;
            properShot = false;
            disp_updateRequired = true;
        }
        // 3. Scale Lost Stop
        else if(!scale.isConnected()) { 
            motor_setRPM = 0;
            grindingComplete = true;
            gbw_started = false;
            state = IDLE_GBW;
            error = 104;
            disp_updateRequired = true;
        }
        // 4. Empty Hopper Detection
        else if(millis() - startOfShot > GBW_EMPTY_TIMEOUT && abs(currentWeight) < GBW_STABLE_THRESHOLD) { 
            motor_setRPM = 0;
            gbw_started = false;
            state = IDLE_GBW;
            error = 105;
            disp_updateRequired = true;
        }
    }

    // --- Post-Grind Learning & Reset ---
    if(grindingComplete) { 
        if(!learned && lastUpdate + GBW_LEARN_DELAY < millis()) { 
            scale.startTimer(); // BEEP
            delay(25);
            scale.stopTimer(); 
            if(motor_setRPM != 0) motorOff();
            
            if(properShot) gbw_learn();
            
            learned = true;
            lastActivity = millis();
        }

        if(lastUpdate + 2000 < millis()) { 
            state = IDLE_GBW;
            disp_updateRequired = true;
            grindingComplete = false;
            gbw_started = false;
            tareTime = 0;
        }
    }

    // --- LED Feedback ---
    if(!gbw_started) ledAction(0);
    else if(!grindingComplete) ledAction(100 + 50.f*(float(currentWeight)/float(setWeight)));
    else ledAction(1);
}


static float gbw_liveRateMgPerMs(uint16_t windowSize = 6, uint16_t reference = last_shot_updated) {
    uint16_t oldIdx;
    if (reference > last_shot_updated) reference = last_shot_updated;

    if(last_shot_updated <= windowSize) { 
        oldIdx = 0;
    } else { 
        oldIdx = reference - windowSize;
    }

    uint16_t numPoints = reference - oldIdx;
    if (numPoints < 2) return -1;

    // Linear regression: fit y = mx + b through all points
    // where x = time, y = weight
    float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
    
    for (uint16_t i = oldIdx; i <= reference; i++) {
        float x = _shot[i].time;
        float y = _shot[i].weight;
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumX2 += x * x;
    }
    
    float n = numPoints + 1; // Since we include reference
    float denom = (n * sumX2 - sumX * sumX);
    
    if (abs(denom) < 0.001f) return 0;  // All points identical in time
    
    float slope = (n * sumXY - sumX * sumY) / denom;
    if(slope > GBW_MAX_GRIND_RATE) slope = GBW_MAX_GRIND_RATE; 

    return slope;  // mg per ms
}

int32_t gbw_predict() {
    int32_t predictedTime = 5000;
    static int32_t lastKnownWeight;
    static unsigned long lastCall;
    static int32_t lastActualPrediction;
    static unsigned long lastPredictionAt;
    speedModifier = Menu3[GBW_SPEEDMOD].value / 10000.0f;

    if(lastCall + GBW_RESET_DELAY < millis()) { 
        lastActualPrediction = 5000;
        lastPredictionAt = millis();
        predictedTime = 5000;
    }

    lastCall = millis();

    if(currentWeight != lastKnownWeight && gbw_started && !grindingComplete) {
        lastKnownWeight = currentWeight;
        if(state == GRINDING_GBW && gbw_started == true) {  
            // Use the most recent shot samples to estimate current grind speed.
            float speed = gbw_liveRateMgPerMs(6, last_shot_updated);
            float stillToGrind = float(setWeight) - float(abs(currentWeight));
            
            float fallbackSpeed = ((Menu3[GBW_SPEEDMOD].value/10000.f) * (clamp_gbw_rpm(Menu3[GBW_RPM_SET].value) / 60.f));

            if(speed > 0.0f && speed < GBW_MAX_GRIND_RATE) {
                predictedTime = (int32_t)(stillToGrind / speed);
            } else if (fallbackSpeed > 0.001f) { 
                predictedTime =  (int32_t)(stillToGrind / fallbackSpeed);
            } else {
                predictedTime = 5000;
            }

            lastPredictionAt = millis();
            lastActualPrediction = predictedTime;

            #ifdef DEBUG_GBW
                Serial.print("New prediction: "), Serial.print(predictedTime), Serial.print(" ..at: "), Serial.print(lastPredictionAt - startOfShot), Serial.print("ms...mg: "),Serial.print(currentWeight), Serial.print("  to go:  "), Serial.print(stillToGrind), Serial.print(" speed: "), Serial.println(speed);
            #endif
        } 
    }

    //Give a continuous prediction value
    predictedTime = lastActualPrediction - (millis() - lastPredictionAt); 

    if(gbw_started == true && grindingComplete == true) predictedTime = 0;
    return (int32_t)(predictedTime - abs(Menu3[GBW_OFFSET].value));
}


void gbw_learn() { 
    int32_t overshoot = (int32_t)currentWeight - (int32_t)setWeight;
    uint32_t stopWeight = 0;
    float stopSpeed = 0;
    bool isProper = true;
    uint16_t firstValIdx = 0;
    uint16_t secondValIdx = 0;

    // --- 1. Estimate Weight and Speed at the exact moment the motor stopped ---
    for (int i = 1; i <= last_shot_updated; i++) { 
        if (_shot[i].time > shotStopped) {
            uint32_t dt = _shot[i].time - _shot[i-1].time;
            if (dt == 0) continue;
            float progress = (float)(shotStopped - _shot[i-1].time) / (float)dt;
            stopWeight = _shot[i-1].weight + progress * (float)(_shot[i].weight - _shot[i-1].weight);
            stopSpeed = gbw_liveRateMgPerMs(6, i); // Speed just before stopping
            break;
        }
    }

    if (stopWeight == 0 || stopSpeed <= 0.05f) isProper = false;

    // --- 2. Calculate Bulk Grind Speed (20% to 80% of Target Weight) ---
    uint32_t minWeight = setWeight * 0.2f;
    uint32_t maxWeight = setWeight * 0.8f;

    for (int i = 0; i <= last_shot_updated; i++) {
        if (_shot[i].weight > minWeight && firstValIdx == 0) firstValIdx = i;
        if (_shot[i].weight > maxWeight && secondValIdx == 0) secondValIdx = i;
    }


    if (firstValIdx != 0 && secondValIdx != 0 && secondValIdx > firstValIdx) {
        int32_t dw = _shot[secondValIdx].weight - _shot[firstValIdx].weight;
        uint32_t dt = _shot[secondValIdx].time - _shot[firstValIdx].time;

        if (dt > 200 && dw > 500) { // Need at least 200ms and 0.5g of data
            float currentGrindSpeed = (float)dw / (float)dt;
            float rpm = clamp_gbw_rpm(Menu3[GBW_RPM_SET].value);
            if (rpm > 0) {
                float newGroundPerRot = currentGrindSpeed / (rpm / 60.0f);
                
                // --- Alpha Filtering for Speed Modifier ---
                float oldGPR = Menu3[GBW_SPEEDMOD].value / 10000.0f;
                // Initialize if it was default 0.5f (or very different)
                if (abs(newGroundPerRot - oldGPR) > 0.5f) speedModifier = newGroundPerRot;
                else speedModifier = (oldGPR * (1.0f - GBW_LEARN_ALPHA)) + (newGroundPerRot * GBW_LEARN_ALPHA);
                
                // Outlier Rejection: Speed shouldn't be crazy
                if (speedModifier < 0.03f || speedModifier > 0.8f) isProper = false;
            } else isProper = false;
        } else isProper = false;
    } else isProper = false;

    // --- 3. Calculate Ideal Offset ---
    // The offset is the time required for the "fallout" (weight that falls after stop)
    // Fallout = currentWeight - stopWeight
    int16_t currentOffset = Menu3[GBW_OFFSET].value;
    int32_t actualFallout = (int32_t)currentWeight - (int32_t)stopWeight;
    
    // Ideal Offset = actualFallout / stopSpeed + correction for overshoot
    if (isProper && stopSpeed > 0.05f && actualFallout > 0) {
        float timeToFall = (float)actualFallout / stopSpeed;
        float errorTime = (float)overshoot / stopSpeed;
        
        // We want: StopTime = TargetTime - Offset
        // If we overshot by errorTime, our offset was too small.
        int16_t idealOffset = (int16_t)(timeToFall + (errorTime * 0.25f)); // P-correction (25%)
        
        // Clamp and Filter Offset
        if (idealOffset < GBW_MIN_OFFSET) idealOffset = GBW_MIN_OFFSET;
        if (idealOffset > GBW_MAX_OFFSET) idealOffset = GBW_MAX_OFFSET;
        
        int16_t delta = idealOffset - currentOffset;
        if (abs(delta) > GBW_MAX_OFFSET_CHANGE) {
            delta = (delta > 0) ? GBW_MAX_OFFSET_CHANGE : -GBW_MAX_OFFSET_CHANGE;
        }
        
        // Apply alpha filter to offset
        int16_t newOffset = currentOffset + (int16_t)(delta * GBW_LEARN_ALPHA);
        if (abs(overshoot) < 100) newOffset = currentOffset; // Deadzone for stability

        Menu3[GBW_OFFSET].value = newOffset;
        Menu3[GBW_SPEEDMOD].value = (uint16_t)(speedModifier * 10000);
        
        #ifdef DEBUG_GBW
            Serial.print("Proper shot! New speedmod: "), Serial.print(speedModifier, 4);
            Serial.print(" New offset: "), Serial.println(newOffset);
        #endif
        pdata_write(4);
    } else {
        #ifdef DEBUG_GBW
            Serial.println("Learning rejected: Outlier or unstable data.");
        #endif
    }
}

