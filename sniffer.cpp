// Tuya serial protocol sniffer
//
// ESP receives and forwards messages from the MCU module to the Wifi module and prints
// the message content to the web monitor
//
// The Web monitor can also be used to send commands to either module
// User is responsible for ensuring input data is valid else ESP32-C3 may crash
// User input of Tuya message must consist of:
// - Uart number, where W is wifi and M is mcu
// - Tuya command number in 2 digit hex format
// - optional Tuya data in 2 digit hex format per byte separated by spaces
// - the rest of the message is generated by the sketch
// Eg to send heartbeat to wifi enter: 1 0
//   this generates: 55 aa 00 00 00 00 FF
// Eg to send wifi status 1 to mcu enter: 2 3 00
//   this generates: 55 aa 00 03 00 01 00 03
// Tuya serial protocol defined here: 
//   https://developer.tuya.com/en/docs/iot/tuya-cloud-universal-serial-port-access-protocol?id=K9hhi0xxtn9cb
//
// Tuya command consists of:
// - header: 55 aa (fixed)
// - version: 00 (wifi) 03 (mcu) (derived)
// - command: xx (input)
// - length: xx xx (derived)
// - datapoint: xx (input)
// - data type: xx (input) 
// - data len: xx xx (input)
// - value: xx ... (variable length input)
// - checksum: xx (derived)

// s60sc 2022

#include "appGlobals.h"
#include "driver/uart.h"

static uint8_t uOffset = 0;  // if UART0 not used for MCU connection e.g ESP32, then UART1 used for MCU and UART2 used for Wifi
bool useIOextender = false;
#define QUEUE_SIZE 50

static TaskHandle_t wifiHandle = NULL;
static TaskHandle_t mcuHandle = NULL;
static SemaphoreHandle_t readMutex = NULL; // prevent uart contention
SemaphoreHandle_t writeMutex = NULL; // prevent Web monitor / heartbeat contention
static QueueHandle_t uartQueue[2];
static uart_event_t uartEvent[2];
tuyaStruct mcuTuya;

struct uartStruct {
  char uartId;
  const char* uartName;
  int txPin;
  int rxPin;
  const char* destName;
};
static uartStruct uart[2];

static void formatTuya(int uartNum, const byte* tuyaData, size_t tuyaDataLen, bool isProcessed) {
  // format message for readability on web monitor and command processing 
  // only input data is processed and formatted, output is only formatted
  if (USE_SNIFFER) isProcessed = false; // no processing in sniffer mode
  static const char* typeStr[] = {"raw", "bool", "int", "str", "enum", "bmap"};
  char formatted[BUFF_LEN] = {0, };
  bool DP = false;
  sprintf(formatted, "%s > ", uart[uartNum].destName);
  for (int i = 0; i < tuyaDataLen; i++) {
    if (i == 3) {
      // command number
      sprintf(formatted + strlen(formatted), "[%d] ", tuyaData[i]);
      if (tuyaData[3] == 6 || tuyaData[3] == 7) DP = true; // has datapoints
      if (isProcessed) {
        mcuTuya.tuyaCmd = tuyaData[3];
        mcuTuya.tuyaDP = tuyaData[6];
      }
    }
    
    else if (DP) {
      // commands with datapoints
      if (i == 6) sprintf(formatted + strlen(formatted), "DP %d: ", tuyaData[6]); // datapoint id
      // data type
      else if (i == 7) sprintf(formatted + strlen(formatted), "%s ", typeStr[tuyaData[7]]); 
 
      // data content, format depends on data type
      else if (i == 10 && i < tuyaDataLen - 1) {
        strcat(formatted, "( "); 
        // raw and bitmap as stream of numbers
        if (tuyaData[7] == 0 || tuyaData[7] == 5) {
          for (int y = i; y < tuyaDataLen - 1; y++) {
            sprintf(formatted + strlen(formatted), "%d ", tuyaData[y]); 
            if (isProcessed) mcuTuya.tuyaData[y - i] = tuyaData[y];
          }
        }
        // boolean (switch) type as status
        else if (tuyaData[7] == 1) {
          sprintf(formatted + strlen(formatted), "%s ", tuyaData[i] ? "ON" : "OFF");
          if (isProcessed) mcuTuya.tuyaData[0] = tuyaData[i];
        }
        // integer type as 4 byte signed
        else if (tuyaData[7] == 2) {
          int32_t intVal = (tuyaData[10] << 24) | (tuyaData[11] << 16) | (tuyaData[12] << 8) | tuyaData[13];
          sprintf(formatted + strlen(formatted), "%ld ", intVal);
          if (isProcessed) mcuTuya.tuyaInt = intVal;
        }
        // variable length string type
        else if (tuyaData[7] == 3) {
          snprintf(formatted + strlen(formatted), tuyaDataLen - i, "%s ", tuyaData + i);
          if (isProcessed) for (int y = i; y < tuyaDataLen - 1; y++) mcuTuya.tuyaData[y - i] = tuyaData[y];
        }    
        // enum as number
        else if (tuyaData[7] == 4) {
          sprintf(formatted + strlen(formatted), "%d ", tuyaData[i]);
          if (isProcessed) mcuTuya.tuyaData[0] = tuyaData[i];
        }
        strcat(formatted, ") ");
      }
    } else {
      // commands without datapoints
      if (i == 6 && i < tuyaDataLen - 1) { // only if data available
        strcat(formatted, "( "); 
        // product data is string 
        if (tuyaData[3] == 1) {
          snprintf(formatted + strlen(formatted), tuyaDataLen - i, "%s", tuyaData + i); 
          if (isProcessed) for (int y = i; y < tuyaDataLen - 1; y++) mcuTuya.tuyaData[y - i] = tuyaData[y];
        }
        // other commands' data are numbers
        else for (int y = i; y < tuyaDataLen - 1; y++) {
          sprintf(formatted + strlen(formatted), "%d ", tuyaData[y]); 
          if (isProcessed) mcuTuya.tuyaData[y - i] = tuyaData[y];
        }
        strcat(formatted, ") ");
      }
    }
  }
  LOG_INF("%s", formatted);
}

static void processTuyaByte(int uartNum, byte tuyaByte) {
  // build individual message from uart data
  static int tuyaIdx[2] = {0, 0};
  static bool haveHdr[2] = {false, false};
  static uint16_t msgLen[2] = {BUFF_LEN - 10, BUFF_LEN - 10};
  static const uint16_t header = 0x55aa; 
  static byte tuyaData[2][BUFF_LEN]; // data received from wifi and mcu
  tuyaData[uartNum][tuyaIdx[uartNum]++] = tuyaByte;
  if (tuyaIdx[uartNum] > 1 && !haveHdr[uartNum]) {
    // check for header
    uint16_t tuyaHdr = (tuyaData[uartNum][tuyaIdx[uartNum] - 2] << 8) | tuyaData[uartNum][tuyaIdx[uartNum] - 1];
    if (tuyaHdr == header) {
      // move header to start of buffer
      haveHdr[uartNum] = true;
      if (tuyaIdx[uartNum] > 2) LOG_VRB("Invalid msg of %u bytes from %s deleted", tuyaIdx[uartNum] - 2, uart[uartNum].uartName);
      memmove(tuyaData[uartNum], tuyaData[uartNum] + tuyaIdx[uartNum] - 2, 2);
      tuyaIdx[uartNum] = 2;
    }
  }
  // determine msg length
  if (tuyaIdx[uartNum] == 6 && haveHdr[uartNum]) 
    msgLen[uartNum] = (tuyaData[uartNum][tuyaIdx[uartNum] - 2] << 8) | tuyaData[uartNum][tuyaIdx[uartNum] - 1];
  // send message for formatting and processing when all data received
  if (tuyaIdx[uartNum] == min(msgLen[uartNum] + 7, BUFF_LEN - 10)) {
    formatTuya(uartNum, tuyaData[uartNum], tuyaIdx[uartNum], true);
    if (!USE_SNIFFER) processMCUcmd();
    // reset for next message
    haveHdr[uartNum] = false;
    tuyaIdx[uartNum] = 0;
    msgLen[uartNum] = BUFF_LEN - 10;
  }
}

static void readUart(uart_port_t uartNum) {
  // Read data from the given UART when available
  static const char* uartErr[] = {"FRAME_ERR", "PARITY_ERR", "UART_BREAK", "DATA_BREAK",
    "BUFFER_FULL", "FIFO_OVF", "UART_DATA", "PATTERN_DET", "EVENT_MAX"};
  if (xQueueReceive(uartQueue[uartNum], (void*)&uartEvent[uartNum], (TickType_t)portMAX_DELAY)) {
    xSemaphoreTake(readMutex, portMAX_DELAY); 
    if (uartEvent[uartNum].type != UART_DATA) {
      xQueueReset(uartQueue[uartNum]);
      uart_flush_input(uartNum + uOffset);
      LOG_ERR("%s uart unexpected event type: %s\n", uart[uartNum].uartName, uartErr[uartEvent[uartNum].type]);
    } else {
      // uart rx data available
      byte tuyaByte[1];
      while (uart_read_bytes(uartNum + uOffset, tuyaByte, 1, 20 / portTICK_PERIOD_MS)) {
        uart_port_t otherUart = uartNum ^ 0x01; // flip uart number
        // forward to other uart if in sniffer mode
        if (USE_SNIFFER) uart_write_bytes(otherUart + uOffset, tuyaByte, 1);
        // format for processing
        processTuyaByte(otherUart, tuyaByte[0]);
      }
    }
  }
  xSemaphoreGive(readMutex);
}

static void configureUart(uart_port_t uartNum) {
  // configure parameters of UART driver
  uart_config_t uart_config = {
    .baud_rate = TUYA_BAUD_RATE,
    .data_bits = UART_DATA_8_BITS,
    .parity    = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#if !(CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S3)
    .source_clk = UART_SCLK_REF_TICK,
#endif
  };
  
  // install the driver and configure pins
  uart_driver_install(uartNum + uOffset, BUFF_LEN, BUFF_LEN, QUEUE_SIZE, &uartQueue[uartNum], 0);
  uart_param_config(uartNum + uOffset, &uart_config);
  uart_set_pin(uartNum + uOffset, uart[uartNum].txPin, uart[uartNum].rxPin, UART_RTS, UART_CTS);
}

static void mcuTask(void *arg) {
  // controlling task for local device MCU
  while (true) readUart(0); // wait for data to arrive
}

static void wifiTask(void *arg) {
  // controlling task for optional external wifi module 
  while (true) readUart(1); // wait for data to arrive
}

void prepUarts() {
  esp_log_level_set("*", ESP_LOG_NONE);
  readMutex = xSemaphoreCreateMutex();
  writeMutex = xSemaphoreCreateMutex();
  uart[0].uartId = 'M';
  uart[0].uartName = "MCU";
  uart[0].txPin = MCU_TX_PIN;
  uart[0].rxPin = MCU_RX_PIN;
  uart[0].destName = "Wifi";
  uart[1].uartId = 'W';
  uart[1].uartName = "Wifi";
  uart[1].txPin = WIFI_TX_PIN;
  uart[1].rxPin = WIFI_RX_PIN;
  uart[1].destName = "MCU";
  if (USE_UART0) {
    LOG_INF("detach UART0 from serial monitor");
    delay(100);
    monitorOpen = false;
    uart_driver_delete(UART_NUM_0);
  } else uOffset = 1;
  
  configureUart(0);
  xTaskCreate(mcuTask, "mcuTask", 1024 * 8, NULL, 2, &mcuHandle);
  if (USE_SNIFFER) {
    configureUart(1);
    xTaskCreate(wifiTask, "wifiTask", 1024 * 4, NULL, 2, &wifiHandle);
  } 
  xSemaphoreGive(readMutex);
  xSemaphoreGive(writeMutex);
  uartReady = true;
}

static int32_t getNumber(const char* consoleCmd, bool start = false) {
  static char* endPtr;
  if (start) endPtr = const_cast<char*>(consoleCmd + 1); // set to start of data 
  char* savePtr = endPtr;
  int32_t dataItem = strtol(endPtr, &endPtr, 10); 
  if (endPtr == savePtr) {
    if (endPtr - consoleCmd < strlen(consoleCmd)) LOG_ERR("Non numeric characters found: %s", consoleCmd);
    return LONG_MIN;
  }
  return dataItem;
}

void processTuyaMsg(const char* wsMsg) {
  // receive external Tuya commands from Web monitor or heartbeat task and format then for output
  // DP based command input comprises: destination command DP_id data_type data (format depends on data_type)
  // Non DP command input comprises: destination command data_as_individual_bytes
  xSemaphoreTake(writeMutex, portMAX_DELAY);
  uint8_t tuyaCmd[BUFF_LEN]; // numeric conversion of console command string
  int uartNum;
  if ((char)wsMsg[0] == uart[0].uartId) uartNum = 0;
  else if ((char)wsMsg[0] == uart[1].uartId) uartNum = 1;
  else {
    if (strlen(wsMsg) > 1) LOG_ERR("Invalid command destination: %c, needs to be %c or %c\n", (char)wsMsg[0], uart[0].uartId, uart[1].uartId);
    xSemaphoreGive(writeMutex);
    return;
  }
  int idx = 5; // index to start of data section
  tuyaCmd[3] = (uint8_t)(getNumber((const char*)wsMsg, true) & 0xFF); // command id
  bool isDP = (tuyaCmd[3] == 6 || tuyaCmd[3] == 7) ? true : false;
  if (isDP) {
    tuyaCmd[6] = (uint8_t)(getNumber((const char*)wsMsg) & 0xFF); // datapoint id
    tuyaCmd[7] = (uint8_t)(getNumber((const char*)wsMsg) & 0xFF); // data type
    if (tuyaCmd[7] == 2) {
      // decompose integer into big endian 4 bytes
      int32_t intVal = getNumber((const char*)wsMsg);
      for (int i = 0; i < 4; i++) tuyaCmd[13 - i] = intVal >> (8 * i);
      idx = 13;
    } else idx = 9; // all other types
  }

  // data part
  int32_t thisNum;
  while ((thisNum = getNumber((const char*)wsMsg)) != LONG_MIN) {
    tuyaCmd[++idx] = (uint8_t)(thisNum & 0xFF);
  } 

  // derive rest of tuya command 
  tuyaCmd[0] = 0x55; // header
  tuyaCmd[1] = 0xaa; // header
  tuyaCmd[2] = uartNum == 0 ? 0x00 : 0x03; // wifi sends version 0, mcu sends version 3
  tuyaCmd[4] = (uint8_t)((idx - 5) >> 8); // high byte of data length
  tuyaCmd[5] = (uint8_t)((idx - 5) & 0xFF); // low byte of data length
  if (isDP) {
    // datapoint data length
    tuyaCmd[8] = (uint8_t)((idx - 9) >> 8); // high byte of data length
    tuyaCmd[9] = (uint8_t)((idx - 9) & 0xFF); // low byte of data length
  }
  tuyaCmd[++idx] = 0; // checksum is modulo 256 of command content summation 
  for (int i = 0; i < idx; i++) tuyaCmd[idx] += tuyaCmd[i]; 
  
  // send tuya command to selected uart
  int tuyaWrote = uart_write_bytes(uartNum + uOffset, tuyaCmd, idx + 1);
  if (tuyaWrote == idx + 1) formatTuya(uartNum, (const byte*)tuyaCmd, tuyaWrote, false);
  else LOG_WRN("Uart %d wrote %d, expected %d", uartNum, tuyaWrote, idx+1);
  xSemaphoreGive(writeMutex);
}
