// 
// Specific controller for Avatto i8 Thermostat
//
// s60sc 2022

#include "globals.h"

#define MS_HR (3600 * 1000) // millisecs in hour
#define KW 1.8 // heating mat kilowatts
#define TIME_SLOTS 8 // number of time slots in daily schedule
#define USED_SLOTS 6 // only first 6 slots used for work day
#define SECS_IN_DAY (24 * 60 * 60) // number of seconds in a day
#define TGT_TEMP 4 // schedule column containing target temp
#define SECS_COL 5 // schedule column containing seconds duration

static bool gotHeartbeat = false;
static uint32_t heatingElapsed = 0; // total time heating on since startup
static float currentTemp = 15.0; // initial value for smoothing
static float alpha = 1.0; // for exponential moving average filter
static int drift = 3; // greater than floor sensor temperature fluctuations
static bool ESPcontroller = false; // whether controller is ESP (true) or MCU (false)
static float tgtTemp = 19.0;
static float backLash = 0.0;
static float baseCal = 0.0;
static bool heatingOn = false;
// schedule array format: hours, mins, temp high byte, temp low byte, temp deg C * 10, seconds
static int schedule[TIME_SLOTS][6];

static void wsJsonSend(const char* keyStr, const char* valStr) {
  // output key val pair from MCU and send as json over websocket
  char jsondata[100];
  updateConfigVect(keyStr, valStr);
  sprintf(jsondata, "{\"cfgGroup\":\"-1\", \"%s\":\"%s\"}", keyStr, valStr);
  logPrint("%s\n", jsondata);
}

static void sendWifiStatus(bool demanded) {
  // check if wifi status changed
  static int newWifiStatus = 0;
  static int oldWifiStatus = -1;
  char wifiStr[6] = "M 3 0";
  // set wifi icon on / off
  newWifiStatus = (WiFi.status() == WL_CONNECTED) ? 4 : 0; 
  if ((newWifiStatus != oldWifiStatus) || demanded) {
    wifiStr[4] = newWifiStatus + '0'; // convert to char
    oldWifiStatus = newWifiStatus;
    processTuyaMsg(wifiStr);
  }
}

static void sendLocalTime(bool demanded) {
  // check if MCU has been sent local time
  static bool sentTime = false;
  if ((timeSynchronized && !sentTime) || demanded) {
    char currTime[30] = {"M 28 0 0 0 0 0 0 0 0"}; // time not available
    if (timeSynchronized) {
      // time available
      struct timeval tv;
      gettimeofday(&tv, NULL);
      time_t currEpoch = tv.tv_sec;
      strftime(currTime, 29, "M 28 1 %y %m %d %H %M %S %w", localtime(&currEpoch));
      sentTime = true;
    }
    processTuyaMsg(currTime);
  }
}

static void updateStats() {
  // called on heartbeat to update stats
  // calculate uptime
  char timeBuff[20];
  formatElapsedTime(timeBuff, millis());
  updateConfigVect("upTime", timeBuff);
  // format heating time
  formatElapsedTime(timeBuff, heatingElapsed);
  updateConfigVect("totalOn", timeBuff);
  // calc percentage time on
  float pcntOn = (float)(heatingElapsed) * 100.0 / millis();
  sprintf(timeBuff, "%0.1f%%", pcntOn); 
  updateConfigVect("pcntOn", timeBuff);
  // calc avg time on per day
  float avgOn = pcntOn * 864.0 * 1000;  // avg msecs per day 
  formatElapsedTime(timeBuff, (uint32_t)(avgOn));
  updateConfigVect("avgOn", timeBuff+2); // skip over day counter
  // calc avg power per day
  float kWh = (avgOn / MS_HR) * KW;
  sprintf(timeBuff, "%0.1fkWh", kWh);
  updateConfigVect("kWh", timeBuff);
}

static void checkSchedule() {
  // check if time for next scheduled slot, assumes slots ordered by time
  static int currentSlot = -1;
  static int32_t slotDuration = 0;
  static uint32_t startTime = 0;
  static bool changedSlot = false;

  if (!timeSynchronized) {
//    LOG_WRN("Unable to use ESP control as time not synchronised");
    // uses default temp for home setting
    return;
  }

  if (currentSlot < 0) {
    // use current time of day secs on first call to determine which slot to use
    struct timeval tv;
    struct tm timeinfo;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &timeinfo);
    int32_t currentSecs = (((timeinfo.tm_hour * 60) + timeinfo.tm_min) * 60) + timeinfo.tm_sec;
    // determine slot for current time
    currentSlot =  USED_SLOTS - 1;
    while (schedule[currentSlot][SECS_COL] > currentSecs) {
      currentSlot--;
      if (currentSlot < 0) break; // current time is before first slot
    }
     // determine time remaining for current slot
    if (currentSlot < 0 || currentSlot == USED_SLOTS - 1) {
      // remaining time crosses day boundary
      currentSlot = USED_SLOTS - 1;
      slotDuration = schedule[0][SECS_COL] - currentSecs;
      // if currentSecs > time of last slot, then update for crossing day boundary
      if (slotDuration < 0) slotDuration += SECS_IN_DAY; 
    } else slotDuration = schedule[currentSlot+1][SECS_COL] - currentSecs;
    slotDuration *= 1000;
    changedSlot = true;

  } else {
    // slot active, check if time for next slot
    if (millis() - startTime > slotDuration) {
      // set up duration of next slot
      if (++currentSlot >= USED_SLOTS) currentSlot = 0;
      slotDuration = currentSlot < USED_SLOTS - 1 ? schedule[currentSlot+1][SECS_COL] : SECS_IN_DAY + schedule[0][SECS_COL]; 
      slotDuration = (slotDuration - schedule[currentSlot][SECS_COL]) * 1000;
      changedSlot = true;
    }
  }

  if (changedSlot) {
    // send new target temp to MCU
    changedSlot = false;
    startTime = millis();
    char formatted[10];
    sprintf(formatted, "%0.1f", (float)(schedule[currentSlot][TGT_TEMP] / 10.0));
    LOG_INF("Activate schedule W%u: Temp %s for %u mins", currentSlot + 1, formatted, slotDuration / 1000 / 60);
    updateAppStatus("tgtTemp", formatted);    
  }
}

void heartBeat() {
  if (!USE_SNIFFER) {
    // send heartbeats, time and wifi status changes
    processTuyaMsg("M 0"); // heartbeat
    // heartbeat interval is 1 per sec until acked then 1 per 15 secs
    int hbInterval = gotHeartbeat ? 15 : 1;  
    if (gotHeartbeat) {
      gotHeartbeat = false;
      sendWifiStatus(false);
      sendLocalTime(false);
      updateStats();
      checkSchedule();
    } else LOG_WRN("Missed heartbeat");
    delay(hbInterval * 1000);
  }
}

static void setSchedule() {
  // set up time slots with data received from MCU
  // first 6 slots are work days, final 2 slots are rest days
  char formatted[MAX_PWD_LEN];
  char slot[20];
  for (int i = 0; i < TIME_SLOTS; i++) {
    sprintf(slot, "slotTime%u", i + 1);
    sprintf(formatted, "%02u:%02u", mcuTuya.tuyaData[i * 4], mcuTuya.tuyaData[(i * 4) + 1]); // hours:mins
    wsJsonSend(slot, formatted);
    sprintf(slot, "slotTemp%u", i + 1);
    int16_t slotTemp = ((mcuTuya.tuyaData[(i * 4) + 2] << 8) | mcuTuya.tuyaData[(i * 4) + 3]) / 10;
    sprintf(formatted, "%hu", slotTemp); 
    wsJsonSend(slot, formatted);
  } 
}

static void controlHeating(float mcuTemp) {
  // controls heating from ESP 
  char formatted[10];
  float floorTemp = heatingOn ? baseCal + drift : baseCal - drift; // derive floor temp
  floorTemp += mcuTemp;
  currentTemp = smooth(floorTemp, currentTemp, alpha); // smooth out floor sensor fluctuation
 
  if (heatingOn) {
    // heating on, switch off if target reached
    if (currentTemp > tgtTemp)  { 
      // force heating off by setting calibration to overstate floor temp   
      LOG_INF("set OFF: current %0.1f, mcu %0.1f, floor %0.1f, calib %0.1f, target %0.1f", currentTemp, mcuTemp, floorTemp, baseCal + drift, tgtTemp); 
      sprintf(formatted, "%d", (int32_t)((baseCal + drift) * 10));
      updateAppStatus("espCal", formatted);
    }
  } else {
    // heating off, switch on if dropped below target - backlash
    if (currentTemp + backLash < tgtTemp)  {
      // force heating on by setting calibration to understate floor temp    
      LOG_INF("set ON: current + backlash %0.1f, mcu %0.1f, floor %0.1f, calib %0.1f, target %0.1f", currentTemp + backLash, mcuTemp, floorTemp, baseCal - drift, tgtTemp);     
      sprintf(formatted, "%d", (int32_t)((baseCal - drift) * 10));
      updateAppStatus("espCal", formatted);
    }
  }
 }

static void processDP() {
  // process tuya datapoint response from MCU
  float floatTemp = (float)(mcuTuya.tuyaInt / 10.0); // where value is temperature * 10
  char formatted[MAX_PWD_LEN * 2];
  static uint32_t startTime = 0;
  switch (mcuTuya.tuyaDP) {
    case 1: // device display switched on / off
      sprintf(formatted, "%u", mcuTuya.tuyaData[0]);
      wsJsonSend("switchDisp", formatted);
      if (mcuTuya.tuyaData[0]) {
        sendLocalTime(true);
        sendWifiStatus(true);
      }
    break; 
    case 2: // target temperature
      tgtTemp = floatTemp;
      sprintf(formatted, "%0.1f", floatTemp);
      wsJsonSend("tgtTemp", formatted);
    break;
    case 3: // current temperature 
      sprintf(formatted, "%0.1f", floatTemp);      
      wsJsonSend("rawTemp", formatted);
      // use smoothing for current temp if ESP is controller
      if (ESPcontroller) controlHeating(floatTemp);
      else currentTemp = floatTemp;
      sprintf(formatted, "%0.1f", currentTemp);
      wsJsonSend("currTemp", formatted);
    break;
    case 4: // program mode - 0 = home (manual), 1 = program (auto), 2 = tmpry prog (away)
      sprintf(formatted, "%u", mcuTuya.tuyaData[0]);
      wsJsonSend("progMode", formatted);
    break;
    case 5: // heating output - 0 = not heating, 1 = output (heating) on 
      sprintf(formatted, "%u", mcuTuya.tuyaData[0]);
      wsJsonSend("outputOn", formatted);
      heatingOn = (bool)mcuTuya.tuyaData[0];
      if (heatingOn) startTime = millis();
      if (!heatingOn && startTime > 0) {
        // add this heating time to total
        heatingElapsed += (millis() - startTime);   
        LOG_INF("Heating session lasted %u secs", (millis() - startTime) / 1000);
        startTime = 0;      
      }
    break;
    case 8: // child lock - 0 = off, 1 = on 
      sprintf(formatted, "%u", mcuTuya.tuyaData[0]);
      wsJsonSend("childLock", formatted);
    break;
    case 13: // sound - 0 = off, 1 = on
      sprintf(formatted, "%u", mcuTuya.tuyaData[0]);
      wsJsonSend("soundOn", formatted);
    break;
    case 16: // fault bitmap
      sprintf(formatted, "%u", mcuTuya.tuyaData[0]);
      wsJsonSend("fault", formatted);
      if (mcuTuya.tuyaData[0]) LOG_ERR("External temperature sensor not connected: %d", mcuTuya.tuyaData[0]);
    break;
    case 20: // temperature calibration, offset in C 
      if (!ESPcontroller) {
        sprintf(formatted, "%0.1f", floatTemp);
        wsJsonSend("tempCal", formatted);
      } // ignore if ESP is controller
    break;
    case 21: // max room temperature setting [no multiple] 
      sprintf(formatted, "%u", mcuTuya.tuyaInt);
      wsJsonSend("roomMax", formatted);
    break;
    case 25: // temp sensor used: 0 = internal, 1 = external, 2 = both 
      sprintf(formatted, "%u", mcuTuya.tuyaData[0]);
      wsJsonSend("tempSensor", formatted);
    break;
    case 26: // frost protection - 0 = off, 1 = on
      sprintf(formatted, "%u", mcuTuya.tuyaData[0]);
      wsJsonSend("frost", formatted);
    break;
    case 31: // reset response - 0 = n/a, 1 = did reset
      if (mcuTuya.tuyaData[0]) processTuyaMsg("M 8"); // query datapoint status
    break;
    case 41: // backlight setting - 0 = off, 1 = low, 2 = middle, 3 = high
      sprintf(formatted, "%u", mcuTuya.tuyaData[0]);
      wsJsonSend("backLight", formatted);
    break;
    case 42: // week day setting - 0 = 5+2, 1 = 6+1, 2 = 7
      sprintf(formatted, "%u", mcuTuya.tuyaData[0]);
      wsJsonSend("daySetting", formatted);
    break;
    case 43: // schedule slots 6 + 2 (home + away) HH MM degC
      setSchedule();
    break;
    case 101: // output main reverse (reverses heating operation !) - 0 = off, 1 = on
      sprintf(formatted, "%u", mcuTuya.tuyaData[0]);
      wsJsonSend("opReverse", formatted);
    break;
    case 105: // backlash deg C 
      backLash = floatTemp;
      sprintf(formatted, "%0.1f", floatTemp);
      wsJsonSend("tempLash", formatted);
    break;
    case 107: // external sensor max temp [no multiple]
      sprintf(formatted, "%u", mcuTuya.tuyaInt);
      wsJsonSend("floorMax", formatted);
    break;
    default: LOG_ERR("Unknown datapoint id %u", mcuTuya.tuyaDP);
  }
}

static void doTuyaInit() {
  // if initial heartbeat response, set mode and get status
  LOG_INF("Initialise MCU (App Ver: %s)", APP_VER);
  initStatus(98, 100); // config group 98 is the DP settings
  if (ESPcontroller) processTuyaMsg("M 6 4 4 0"); // manual mode
  else processTuyaMsg("M 6 4 4 1"); // auto mode
  delay(100);
  processTuyaMsg("M 8"); // get updated DPs
  delay(100);
}

void processMCUcmd() {
  // process tuya command from MCU
  switch (mcuTuya.tuyaCmd) {
    case 0: 
      // heartbeat response
      gotHeartbeat = true;
      if (mcuTuya.tuyaData[0] == 0) doTuyaInit(); // for initial heartbeat response
    break;
    case 1: break; // product query response - view only
    case 2: break; // working mode query response - view only
    case 3: break; // wifi status ack
    case 4: break; // request wifi reset - ignore
    case 7: // datapoint status response
      processDP();
    break;  
    case 28: // request for local time
      sendLocalTime(true);
    break;
    default: LOG_ERR("Unhandled command number %u", mcuTuya.tuyaCmd);
  }
}

bool updateAppStatus(const char* variable, const char* value) {
  /* build MCU datapoint command string from input value
     into uint8_t* array and send to MCU using processTuyaMsg(array);
     eg processTuyaMsg("M 6 4 4 1"); M = MCU, 6 = DP cmd, 4 = DP id, 4 = data type, 1 = data
  */
  bool res = true; 
  if (!USE_SNIFFER) {
    static int slotCnt = 0;
    char formatted[MAX_PWD_LEN * 2] = "M 6 ";
    char* fp = formatted;
    fp += 4; // set pointer
    bool msgReady = true;
    int intVal = atoi(value);
    float fltVal = atof(value);
    if (!strcmp(variable, "tgtTemp")) sprintf(fp, "2 2 %d", intVal * 10); 
    else if (!strcmp(variable, "floorMax")) sprintf(fp, "107 2 %d", intVal);
    else if (!strcmp(variable, "tempSensor")) sprintf(fp, "25 4 %u", intVal);
    else if (!strcmp(variable, "progMode")) sprintf(fp, "4 4 %u", intVal);
    else if (!strcmp(variable, "frost")) sprintf(fp, "26 1 %u", intVal); 
    else if (!strcmp(variable, "switchDisp")) sprintf(fp, "1 1 %u", intVal);
    else if (!strcmp(variable, "childLock")) sprintf(fp, "8 1 %u", intVal);
    else if (!strcmp(variable, "roomMax")) sprintf(fp, "21 2 %d", intVal);
    else if (!strcmp(variable, "tempCal")) {
      sprintf(fp, "20 2 %d", (int32_t)(fltVal * 10));
      baseCal = fltVal; 
      // if under ESP control, ESP determines calibration setting
      if (ESPcontroller) msgReady = false;
    }
    else if (!strcmp(variable, "espCal")) sprintf(fp, "20 2 %d", intVal); // used for ESP controller
    else if (!strcmp(variable, "tempLash")) sprintf(fp, "105 2 %d", (int32_t)(fltVal * 10));  
    else if (!strcmp(variable, "daySetting")) sprintf(fp, "42 4 %u", intVal);  
    else if (!strcmp(variable, "backLight")) sprintf(fp, "41 4 %u", intVal);  
    else if (!strcmp(variable, "doReset")) sprintf(fp, "31 1 %u", intVal); 
    else if (!strcmp(variable, "doReverse")) sprintf(fp, "101 1 %u", intVal);
         
    // process updates associated with schedule
    else if (strstr(variable, "slotTime") != NULL) {
      uint8_t slot, hour, mins;
      slot = variable[8] - '0' - 1;
      if (slot < TIME_SLOTS) { 
        sscanf(value, "%hhu:%hhu", &hour, &mins);      
        schedule[slot][0] = hour;
        schedule[slot][1] = mins;
        schedule[slot][SECS_COL] = ((hour * 60) + mins) * 60; // seconds
        slotCnt++;
      } else LOG_ERR("Invalid schedule slot number %d", slot);
      msgReady = false;
    } 
    else if (strstr(variable, "slotTemp") != NULL) {
      uint8_t slot = variable[8] - '0' - 1;
      int16_t temp = fltVal * 10; // held in 2 bytes
      if (slot < TIME_SLOTS) { 
        schedule[slot][2] = (temp >> 8) & 0xFF;
        schedule[slot][3] = temp & 0xFF;
        schedule[slot][4] = temp;
        slotCnt++;
      } else LOG_ERR("Invalid schedule slot number %d", slot);
      msgReady = false;
    }
    // internal (non MCU) commands
    else if (!strcmp(variable, "alpha")) {
      alpha = fltVal;
      msgReady = false;
    }
    else if (!strcmp(variable, "drift")) {
      drift = intVal;
      msgReady = false;
    }
    else if (!strcmp(variable, "setCtrl")) {    
      ESPcontroller = (bool)intVal;
      sprintf(fp, "4 4 %u", !ESPcontroller); // set prog mode = 0 (manual) if ESPcontroller else 1 (auto)
      LOG_INF("Control mode switched to %s", ESPcontroller ? "ESP" : "MCU");
    }
    else msgReady = false; // ignore unmatched key

    if (slotCnt >= TIME_SLOTS * 2) {
      // send complete schedule to MCU
      sprintf(fp, "43 0 ");
      for (int i = 0; i < TIME_SLOTS; i++) {
        sprintf(formatted + strlen(formatted), "%hhu %hhu %hhu %hhu ", schedule[i][0], schedule[i][1], schedule[i][2], schedule[i][3]);
      }
      formatted[strlen(formatted) - 1] = 0; // lose last space
      slotCnt = 0;
      msgReady = true;
    } 
    if (configLoaded && msgReady) processTuyaMsg(formatted);
  }
  return res;
}

void buildAppJsonString(bool filter) {
  // build app specific part of json string for MCU status
  char* p = jsonBuff + 1;
  *p = 0;
}

/********* mandatory callbacks *********/

void wsAppSpecificHandler(const char* wsMsg) {
  // message from web socket
  int wsLen = strlen(wsMsg) - 1;
  switch ((char)wsMsg[0]) {
    case 'H': 
      // keepalive heartbeat - ignore
    break;
    case 'S': 
      // status request
      buildJsonString(wsLen); // required config number 
      logPrint("%s\n", jsonBuff);
    break;   
    case 'U': 
      // update or control request
      memcpy(jsonBuff, wsMsg + 1, wsLen); // remove 'U'
      parseJson(wsLen);
    break;
    case 'I': 
      // manual request MCU initialisation
      doTuyaInit();
    break;
    default: processTuyaMsg(wsMsg); // tuya cmd input
  }
}

esp_err_t webAppSpecificHandler(httpd_req_t *req, const char* variable, const char* value) {
  return ESP_OK;
}

void appDataFiles() {
  // callback from setupAssist.cpp, for any app specific files 
}

void OTAprereq() {} // dummy 
