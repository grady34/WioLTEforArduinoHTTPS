#include <Arduino.h>
#include <stdio.h>
#include "wiolte-driver.h"

#define MODULE_RESPONSE_MAX_SIZE  (100)

////////////////////////////////////////////////////////////////////////////////////////
// Helper functions

static void PinModeAndDefault(int pin, WiringPinMode mode)
{
  pinMode(pin, mode);
}

static void PinModeAndDefault(int pin, WiringPinMode mode, int value)
{
  pinMode(pin, mode);
  if (mode == OUTPUT) digitalWrite(pin, value);
}

static void DebugPrint(const char* data)
{
  char message[10];
  int length = strlen(data);
  
  SerialUSB.print(length);
  
  SerialUSB.print(":");
  
  for (int i = 0; i < length; i++) {
    if (data[i] >= 0x20)
      SerialUSB.print((char)data[i]);
    else
      SerialUSB.print('.');
  }
  
  SerialUSB.print(":");
  
  for (int i = 0; i < length; i++) {
    sprintf(message, "%02x ", data[i]);
    SerialUSB.print(message);
  }
  
  SerialUSB.println("");
}

////////////////////////////////////////////////////////////////////////////////////////
// WioLTE

void WioLTE::DiscardRead()
{
  while (_Serial->available()) _Serial->read();
}

void WioLTE::SetTimeout(long timeout)
{
  _Timeout = timeout;
  _Serial->setTimeout(_Timeout);
}

bool WioLTE::ReadLine(char* data, int dataSize)
{
  int dataIndex = 0;

  Stopwatch sw;
  while (dataIndex < dataSize - 1) {
    sw.Start();
    while (!_Serial->available()) {
      if (sw.ElapsedMilliseconds() > _Timeout) {
        data[dataIndex] = '\0';
        return false; // Timeout.
      }
    }
    int c = _Serial->read();
    if (c < 0) {
      data[dataIndex] = '\0';
      return false; // No data.
    }
    if (c == '\r') continue;
    
    if (c == '\n') {
      data[dataIndex] = '\0';
      return true;
    }

    data[dataIndex++] = c;
  }

  if (dataIndex < dataSize) {
    data[dataIndex] = '\0';
  }
  return false; // Overflow.
}

void WioLTE::WriteCommand(const char* command)
{
  SerialUSB.print("<- ");
  DebugPrint(command);
  
  _Serial->write(command);
  _Serial->write('\r');
}

bool WioLTE::WaitForResponse(const char* response)
{
  char data[MODULE_RESPONSE_MAX_SIZE];
  do {
    if (!ReadLine(data, sizeof (data))) return false;
    SerialUSB.print("-> ");
    DebugPrint(data);
  }
  while (strcmp(data, response) != 0);

  return true;
}

bool WioLTE::WriteCommandAndWaitForResponse(const char* command, const char* response)
{
  WriteCommand(command);
  return WaitForResponse(response);
}

WioLTE::WioLTE() : _Serial(&Serial1), _Timeout(2000)
{
}

void WioLTE::Init()
{
  // Turn on/off Pins
  PinModeAndDefault(PWR_KEY_PIN, OUTPUT, LOW);
  PinModeAndDefault(RESET_MODULE_PIN, OUTPUT, HIGH);
  // Status Indication Pins
  PinModeAndDefault(STATUS_PIN, INPUT);
  // GPIO Pins
  PinModeAndDefault(WAKEUP_IN_PIN, OUTPUT, LOW);
  PinModeAndDefault(WAKEUP_DISABLE_PIN, OUTPUT, HIGH);
  //PinModeAndDefault(AP_READY_PIN, OUTPUT);  // NOT use
  
  _Serial->begin(115200);
}

void WioLTE::Reset() 
{
  digitalWrite(RESET_MODULE_PIN, LOW);
  delay(200);
  digitalWrite(RESET_MODULE_PIN, HIGH);
  delay(300);
}

bool WioLTE::IsBusy() const
{
  return digitalRead(STATUS_PIN) ? true : false;
}

bool WioLTE::TurnOn()
{
  delay(100);
  digitalWrite(PWR_KEY_PIN, HIGH);
  delay(200);
  digitalWrite(PWR_KEY_PIN, LOW);

  Stopwatch sw;
  sw.Start();
  while (IsBusy()) {
    SerialUSB.print(".");
    if (sw.ElapsedMilliseconds() >= 5000) return false;
    delay(100);
  }
  SerialUSB.println("");

  return true;
}

////////////////////////////////////////////////////////////////////////////////////////