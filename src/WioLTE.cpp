#include "WioLTEConfig.h"
#include "WioLTE.h"

#include "Internal/Debug.h"
#include "Internal/StringBuilder.h"
#include "Internal/ArgumentParser.h"
#if defined ARDUINO_ARCH_STM32F4
#include "Internal/CMSIS/cmsis_gcc.h"
#include "Internal/CMSIS/core_cm4.h"
#elif defined ARDUINO_ARCH_STM32
#include <stm32f4xx_hal.h>
#endif
#include "WioLTEHardware.h"
#include <stdio.h>
#include <limits.h>

#define RET_OK(val)					(ReturnOk(val))
#define RET_ERR(val,err)			(ReturnError(__LINE__, val, err))

#define CONNECT_ID_NUM				(12)
#define POLLING_INTERVAL			(100)

#define HTTP_USER_AGENT				"QUECTEL_MODULE"
#define HTTP_CONTENT_TYPE			"application/json"

#define LINEAR_SCALE(val, inMin, inMax, outMin, outMax)	(((val) - (inMin)) / ((inMax) - (inMin)) * ((outMax) - (outMin)) + (outMin))

////////////////////////////////////////////////////////////////////////////////////////
// Helper functions

#if defined ARDUINO_ARCH_STM32
typedef uint32_t WiringPinMode;
#endif // ARDUINO_ARCH_STM32

static void PinModeAndDefault(int pin, WiringPinMode mode)
{
	pinMode(pin, mode);
}

static void PinModeAndDefault(int pin, WiringPinMode mode, int value)
{
	pinMode(pin, mode);
	if (mode == OUTPUT) digitalWrite(pin, value);
}

static int HexToInt(char c)
{
	if ('0' <= c && c <= '9') return c - '0';
	else if ('a' <= c && c <= 'f') return c - 'a' + 10;
	else if ('A' <= c && c <= 'F') return c - 'A' + 10;
	else return -1;
}

static bool ConvertHexToBytes(const char* hex, byte* data, int dataSize)
{
	int high;
	int low;

	for (int i = 0; i < dataSize; i++) {
		high = HexToInt(hex[i * 2]);
		low = HexToInt(hex[i * 2 + 1]);
		if (high < 0 || low < 0) return false;
		data[i] = high * 16 + low;
	}

	return true;
}

static int Convert2DigitsToInt(const char* digits)
{
	return (digits[0] - '0') * 10 + (digits[1] - '0');
}

static bool SplitUrl(const char* url, const char** host, int* hostLength, const char** uri, int* uriLength)
{
	if (strncmp(url, "http://", 7) == 0) {
		*host = &url[7];
	}
	else if (strncmp(url, "https://", 8) == 0) {
		*host = &url[8];
	}
	else {
		return false;
	}

	const char* ptr;
	for (ptr = *host; *ptr != '\0'; ptr++) {
		if (*ptr == '/') break;
	}
	*hostLength = ptr - *host;
	*uri = ptr;
	*uriLength = strlen(ptr);

	return true;
}

static bool SmAddressFieldToString(const byte* addressField, char* str, int strSize)
{
	byte addressLength = addressField[0];
	//byte typeOfAddress = addressField[1];
	const byte* addressValue = &addressField[2];

	if (addressLength + 1 > strSize) return false;

	for (int i = 0; i < addressLength; i++)
	{
		if (i % 2 == 0)
		{
			str[i] = '0' + (addressValue[i / 2] & 0x0f);
		}
		else
		{
			str[i] = '0' + (addressValue[i / 2] >> 4);
		}
	}
	str[addressLength] = '\0';

	return true;
}

static double GnssCoordinateToDecimal(double dddmm)
{
	int deg = (int)dddmm / 100;
	double min = dddmm - deg * 100.0;
	return deg + min / 60.0;
}

static void DelayArduino(int milliseconds)
{
	delay(milliseconds);
}

////////////////////////////////////////////////////////////////////////////////////////
// WioLTE

bool WioLTE::ReturnError(int lineNumber, bool value, WioLTE::ErrorCodeType errorCode)
{
	_LastErrorCode = errorCode;

	char str[100];
	sprintf(str, "%d", lineNumber);
	DEBUG_PRINT("ERROR! ");
	DEBUG_PRINTLN(str);

	return value;
}

int WioLTE::ReturnError(int lineNumber, int value, WioLTE::ErrorCodeType errorCode)
{
	_LastErrorCode = errorCode;

	char str[100];
	sprintf(str, "%d", lineNumber);
	DEBUG_PRINT("ERROR! ");
	DEBUG_PRINTLN(str);

	return value;
}

bool WioLTE::IsRespond()
{
	Stopwatch sw;
	sw.Restart();
	while (!_AtSerial.WriteCommandAndReadResponse("AT", "^OK$", 500, NULL)) {
		if (sw.ElapsedMilliseconds() >= 2000) return false;
	}

	return true;
}

bool WioLTE::Reset(long timeout)
{
	digitalWrite(WioLTEHardwarePin::RESET_MODULE_PIN, LOW);
	_Delay(200);
	digitalWrite(WioLTEHardwarePin::RESET_MODULE_PIN, HIGH);
	_Delay(300);

	Stopwatch sw;
	sw.Restart();
	while (!_AtSerial.ReadResponse("^RDY$", 100, NULL)) {
		DEBUG_PRINT(".");
		if (sw.ElapsedMilliseconds() >= timeout) return false;
	}
	DEBUG_PRINTLN("");

#ifdef WIO_DEBUG
	char dbg[100];
	sprintf(dbg, "Elapsed time is %lu[msec.].", sw.ElapsedMilliseconds());
	DEBUG_PRINTLN(dbg);
#endif // WIO_DEBUG

	return true;
}

bool WioLTE::TurnOn(long timeout)
{
	_Delay(100);
	digitalWrite(WioLTEHardwarePin::PWR_KEY_PIN, HIGH);
	_Delay(200);
	digitalWrite(WioLTEHardwarePin::PWR_KEY_PIN, LOW);

	Stopwatch sw;
	sw.Restart();
	while (!_AtSerial.ReadResponse("^RDY$", 100, NULL)) {
		DEBUG_PRINT(".");
		if (sw.ElapsedMilliseconds() >= timeout) return false;
	}
	DEBUG_PRINTLN("");

#ifdef WIO_DEBUG
	char dbg[100];
	sprintf(dbg, "Elapsed time is %lu[msec.].", sw.ElapsedMilliseconds());
	DEBUG_PRINTLN(dbg);
#endif // WIO_DEBUG

	return true;
}

int WioLTE::GetFirstIndexOfReceivedSMS()
{
	std::string response;
	ArgumentParser parser;

	if (!_AtSerial.WriteCommandAndReadResponse("AT+CMGF=0", "^OK$", 500, NULL)) return -1;

	_AtSerial.WriteCommand("AT+CMGL=4");	// ALL

	int messageIndex = -1;
	while (true) {
		if (!_AtSerial.ReadResponse("^(OK|\\+CMGL: .*)$", 500, &response)) return -1;
		if (response == "OK") break;
		if (messageIndex < 0) {
			parser.Parse(&response.c_str()[7]);
			if (parser.Size() != 4) return -1;
			messageIndex = atoi(parser[0]);
		}

		if (!_AtSerial.ReadResponse("^.*$", 500, NULL)) return -1;
	}

	return messageIndex < 0 ? -2 : messageIndex;
}

bool WioLTE::HttpSetUrl(const char* url)
{
	StringBuilder str;
	if (!str.WriteFormat("AT+QHTTPURL=%d", strlen(url))) return false;
	_AtSerial.WriteCommand(str.GetString());
	if (!_AtSerial.ReadResponse("^CONNECT$", 500, NULL)) return false;

	_AtSerial.WriteBinary((const byte*)url, strlen(url));
	if (!_AtSerial.ReadResponse("^OK$", 500, NULL)) return false;

	return true;
}

bool WioLTE::ReadResponseCallback(const char* response)
{
	return false;

	if (strncmp(response, "+CGREG: ", 8) == 0) {
		DEBUG_PRINT("### Response Callback +CGREG ### ");
		DEBUG_PRINTLN(response);

		//ArgumentParser parser;
		//parser.Parse(&response[8]);
		//int stat;
		//if (parser.Size() == 2) {
		//	stat = atoi(parser[1]);
		//}
		//else if (parser.Size() >= 1) {
		//	stat = atoi(parser[0]);
		//}
		//else {
		//	stat = -1;
		//}

		//switch (stat)
		//{
		//case 0:	// Not registered. MT is not currently searching
		//case 2:	// Not registered, but MT is currently trying to attach or searching
		//case 3:	// Registration denied.
		//	_PacketGprsNetworkRegistration = false;
		//	DEBUG_PRINTLN("NETWORK OFF");
		//	break;
		//case 1:	// Registered, home network.
		//case 5:	// Registered, roaming
		//	_PacketGprsNetworkRegistration = true;
		//	DEBUG_PRINTLN("NETWORK ON");
		//	break;
		//}

		return true;
	}
	else if (strncmp(response, "+CEREG: ", 8) == 0) {
		DEBUG_PRINT("### Response Callback +CEREG ### ");
		DEBUG_PRINTLN(response);

		//ArgumentParser parser;
		//parser.Parse(&response[8]);
		//int stat;
		//if (parser.Size() == 2) {
		//	stat = atoi(parser[1]);
		//}
		//else if (parser.Size() >= 1) {
		//	stat = atoi(parser[0]);
		//}
		//else {
		//	stat = -1;
		//}

		//switch (stat)
		//{
		//case 0:	// Not registered. MT is not currently searching
		//case 2:	// Not registered, but MT is currently trying to attach or searching
		//case 3:	// Registration denied
		//	_PacketEpsNetworkRegistration = false;
		//	DEBUG_PRINTLN("NETWORK OFF");
		//	break;
		//case 1:	// Registered, home network
		//case 5:	// Registered, roaming
		//	_PacketEpsNetworkRegistration = true;
		//	DEBUG_PRINTLN("NETWORK ON");
		//	break;
		//}

		return true;
	}

	return false;
}

#if defined ARDUINO_ARCH_STM32F4
WioLTE::WioLTE() :
	_SerialAPI(&SerialModule),
	_AtSerial(&_SerialAPI, this), 
	_Led(1, WioLTEHardwarePin::RGB_LED_PIN),
	_LastErrorCode(E_OK), 
	_Delay{ DelayArduino }
{
}
#elif defined ARDUINO_ARCH_STM32
WioLTE::WioLTE() : 
	_SerialAPI(&SerialModule), 
	_AtSerial(&_SerialAPI, this), 
	_Led(), 
	_LastErrorCode(E_OK), 
	_Delay{ DelayArduino }
{
}
#endif

WioLTE::ErrorCodeType WioLTE::GetLastError() const
{
	return _LastErrorCode;
}

void WioLTE::SetDelayFunction(std::function<void(int)> func)
{
	_Delay = func;
}

void WioLTE::SetDoWorkInWaitForAvailableFunction(std::function<void()> func)
{
	_AtSerial.SetDoWorkInWaitForAvailableFunction(func);
}

void WioLTE::Init()
{
	// Power supply
	if (WioLTEHardwarePin::MODULE_PWR_PIN >= 0) PinModeAndDefault(WioLTEHardwarePin::MODULE_PWR_PIN, OUTPUT, LOW);
	if (WioLTEHardwarePin::ANT_PWR_PIN >= 0) PinModeAndDefault(WioLTEHardwarePin::ANT_PWR_PIN, OUTPUT, LOW);
	if (WioLTEHardwarePin::ENABLE_VCCB_PIN >= 0) PinModeAndDefault(WioLTEHardwarePin::ENABLE_VCCB_PIN, OUTPUT, LOW);
	if (WioLTEHardwarePin::RGB_LED_PWR_PIN >= 0) PinModeAndDefault(WioLTEHardwarePin::RGB_LED_PWR_PIN, OUTPUT, HIGH);
	if (WioLTEHardwarePin::SD_POWR_PIN >= 0) PinModeAndDefault(WioLTEHardwarePin::SD_POWR_PIN, OUTPUT, LOW);

	// Turn on/off Pins
	PinModeAndDefault(WioLTEHardwarePin::PWR_KEY_PIN, OUTPUT, LOW);
	PinModeAndDefault(WioLTEHardwarePin::RESET_MODULE_PIN, OUTPUT, HIGH);

	// Status Indication Pins
	if (WioLTEHardwarePin::STATUS_PIN >= 0) PinModeAndDefault(WioLTEHardwarePin::STATUS_PIN, INPUT);

	// UART Interface
	if (WioLTEHardwarePin::DTR_PIN >= 0) PinModeAndDefault(WioLTEHardwarePin::DTR_PIN, OUTPUT, LOW);

	// GPIO Pins
	if (WioLTEHardwarePin::WAKEUP_IN_PIN >= 0) PinModeAndDefault(WioLTEHardwarePin::WAKEUP_IN_PIN, OUTPUT, LOW);
	if (WioLTEHardwarePin::W_DISABLE_PIN >= 0) PinModeAndDefault(WioLTEHardwarePin::W_DISABLE_PIN, OUTPUT, HIGH);
	//if (WioLTEHardwarePin::AP_READY_PIN >= 0) PinModeAndDefault(WioLTEHardwarePin::AP_READY_PIN, OUTPUT);  // NOT use
  
	_SerialAPI.Begin(115200);
#if defined ARDUINO_ARCH_STM32F4
	_Led.begin();
#elif defined ARDUINO_ARCH_STM32
	if (WioLTEHardwarePin::RGB_LED_PIN >= 0) PinModeAndDefault(WioLTEHardwarePin::RGB_LED_PIN, OUTPUT, HIGH);
#endif
	_LastErrorCode = E_OK;

	_PacketGprsNetworkRegistration = false;
	_PacketEpsNetworkRegistration = false;
}

void WioLTE::PowerSupplyLTE(bool on)
{
	PowerSupplyCellular(on);
}

void WioLTE::PowerSupplyCellular(bool on)
{
	if (WioLTEHardwarePin::MODULE_PWR_PIN >= 0) digitalWrite(WioLTEHardwarePin::MODULE_PWR_PIN, on ? HIGH : LOW);

	_LastErrorCode = E_OK;
}

void WioLTE::PowerSupplyGNSS(bool on)
{
	if (WioLTEHardwarePin::ANT_PWR_PIN >= 0) digitalWrite(WioLTEHardwarePin::ANT_PWR_PIN, on ? HIGH : LOW);

	_LastErrorCode = E_OK;
}

void WioLTE::PowerSupplyLed(bool on)
{
	if (WioLTEHardwarePin::RGB_LED_PWR_PIN >= 0) digitalWrite(WioLTEHardwarePin::RGB_LED_PWR_PIN, on ? HIGH : LOW);

	_LastErrorCode = E_OK;
}

void WioLTE::PowerSupplyGrove(bool on)
{
	if (WioLTEHardwarePin::ENABLE_VCCB_PIN >= 0) digitalWrite(WioLTEHardwarePin::ENABLE_VCCB_PIN, on ? HIGH : LOW);

	_LastErrorCode = E_OK;
}

void WioLTE::PowerSupplySD(bool on)
{
	if (WioLTEHardwarePin::SD_POWR_PIN >= 0) digitalWrite(WioLTEHardwarePin::SD_POWR_PIN, on ? HIGH : LOW);

	_LastErrorCode = E_OK;
}

void WioLTE::LedSetRGB(byte red, byte green, byte blue)
{
#if defined ARDUINO_ARCH_STM32F4
	_Led.WS2812SetRGB(0, red, green, blue);
	_Led.WS2812Send();
#elif defined ARDUINO_ARCH_STM32
	if (WioLTEHardwarePin::RGB_LED_PIN >= 0)
	{
		_Led.Reset();
		_Led.SetSingleLED(red, green, blue);
	}
#endif

	_LastErrorCode = E_OK;
}

bool WioLTE::TurnOnOrReset(long timeout)
{
	std::string response;

	if (IsRespond()) {
		DEBUG_PRINTLN("Reset()");
		if (!Reset(timeout)) return RET_ERR(false, E_UNABLE_TO_RECEIVE_RDY);
	}
	else {
		DEBUG_PRINTLN("TurnOn()");
		if (!TurnOn(timeout)) return RET_ERR(false, E_UNABLE_TO_RECEIVE_RDY);
	}

	Stopwatch sw;
	sw.Restart();
	while (!_AtSerial.WriteCommandAndReadResponse("AT", "^OK$", 500, NULL)) {
		DEBUG_PRINT(".");
		if (sw.ElapsedMilliseconds() >= 10000) return RET_ERR(false, E_NO_AT_RESPONSE);
	}
	DEBUG_PRINTLN("");

	if (!_AtSerial.WriteCommandAndReadResponse("ATE0", "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
	if (!_AtSerial.WriteCommandAndReadResponse("AT+QURCCFG=\"urcport\",\"uart1\"", "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
	if (!_AtSerial.WriteCommandAndReadResponse("AT+QSCLK=1", "^(OK|ERROR)$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);

	// TODO ReadResponseCallback
	//if (!_AtSerial.WriteCommandAndReadResponse("AT+CGREG=2", "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
	//if (!_AtSerial.WriteCommandAndReadResponse("AT+CEREG=2", "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
	//if (!_AtSerial.WriteCommandAndReadResponse("AT+CGREG?", "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
	//if (!_AtSerial.WriteCommandAndReadResponse("AT+CEREG?", "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);

	sw.Restart();
	bool cpinReady;
	while (true) {
		_AtSerial.WriteCommand("AT+CPIN?");
		cpinReady = false;
		while (true) {
			if (!_AtSerial.ReadResponse("^(OK|\\+CPIN: READY|\\+CME ERROR: .*)$", 500, &response)) return RET_ERR(false, E_PIN);
			if (response == "+CPIN: READY") {
				cpinReady = true;
				continue;
			}
			break;
		}
		if (response == "OK" && cpinReady) break;

		if (sw.ElapsedMilliseconds() >= 10000) return RET_ERR(false, E_PIN);
		_Delay(POLLING_INTERVAL);
	}

	return RET_OK(true);
}

bool WioLTE::TurnOff(long timeout)
{
	std::string response;

	Stopwatch sw;
	sw.Restart();
	while (true) {
		_AtSerial.WriteCommand("AT+QPOWD");
		if (!_AtSerial.ReadResponse("^(OK|ERROR)$", 500, &response)) return RET_ERR(false, E_TIMEOUT);
		if (response == "OK") break;
		if (sw.ElapsedMilliseconds() >= (unsigned long)timeout) return RET_ERR(false, E_TIMEOUT);
		_Delay(POLLING_INTERVAL);
	}

	if (!_AtSerial.ReadResponse("^POWERED DOWN$", 60000, NULL)) return RET_ERR(false, E_UNABLE_TO_RECEIVE_POWERED_DOWN);

	return RET_OK(true);
}

bool WioLTE::Sleep()
{
	if (WioLTEHardwarePin::DTR_PIN >= 0) digitalWrite(WioLTEHardwarePin::DTR_PIN, HIGH);

	return RET_OK(true);
}

bool WioLTE::Wakeup()
{
	if (WioLTEHardwarePin::DTR_PIN >= 0) digitalWrite(WioLTEHardwarePin::DTR_PIN, LOW);

	Stopwatch sw;
	sw.Restart();
	while (!_AtSerial.WriteCommandAndReadResponse("AT", "^OK$", 500, NULL)) {
		DEBUG_PRINT(".");
		if (sw.ElapsedMilliseconds() >= 2000) return RET_ERR(false, E_NO_AT_RESPONSE);
	}
	DEBUG_PRINTLN("");

	return RET_OK(true);
}

int WioLTE::GetRevision(char* revision, int revisionSize)
{
	std::string response;
	std::string revisionStr;

	_AtSerial.WriteCommand("AT+CGMR");
	while (true) {
		if (!_AtSerial.ReadResponse("^(OK|[0-9A-Z_]+)$", 500, &response)) return RET_ERR(-1, E_TIMEOUT);
		if (response == "OK") break;
		revisionStr = response;
	}

	if ((int)revisionStr.size() + 1 > revisionSize) return RET_ERR(-1, E_NOT_ENOUGH_SIZE);
	strcpy(revision, revisionStr.c_str());

	return RET_OK((int)strlen(revision));
}

int WioLTE::GetIMEI(char* imei, int imeiSize)
{
	std::string response;
	std::string imeiStr;

	_AtSerial.WriteCommand("AT+GSN");
	while (true) {
		if (!_AtSerial.ReadResponse("^(OK|[0-9]+)$", 500, &response)) return RET_ERR(-1, E_TIMEOUT);
		if (response == "OK") break;
		imeiStr = response;
	}

	if ((int)imeiStr.size() + 1 > imeiSize) return RET_ERR(-1, E_NOT_ENOUGH_SIZE);
	strcpy(imei, imeiStr.c_str());

	return RET_OK((int)strlen(imei));
}

int WioLTE::GetIMSI(char* imsi, int imsiSize)
{
	std::string response;
	std::string imsiStr;

	_AtSerial.WriteCommand("AT+CIMI");
	while (true) {
		if (!_AtSerial.ReadResponse("^(OK|[0-9]+)$", 500, &response)) return RET_ERR(-1, E_TIMEOUT);
		if (response == "OK") break;
		imsiStr = response;
	}

	if ((int)imsiStr.size() + 1 > imsiSize) return RET_ERR(-1, E_NOT_ENOUGH_SIZE);
	strcpy(imsi, imsiStr.c_str());

	return RET_OK((int)strlen(imsi));
}

int WioLTE::GetICCID(char* iccid, int iccidSize)
{
	std::string response;

	_AtSerial.WriteCommand("AT+QCCID");
	if (!_AtSerial.ReadResponse("^\\+QCCID: (.*)$", 500, &response)) return RET_ERR(-1, E_TIMEOUT);
	if (!_AtSerial.ReadResponse("^OK$", 500, NULL)) return RET_ERR(-1, E_TIMEOUT);
	response.erase(response.size() - 1, 1);

	if ((int)response.size() + 1 > iccidSize) return RET_ERR(-1, E_NOT_ENOUGH_SIZE);
	strcpy(iccid, response.c_str());

	return RET_OK((int)strlen(iccid));
}

int WioLTE::GetPhoneNumber(char* number, int numberSize)
{
	std::string response;
	ArgumentParser parser;
	std::string numberStr;

	_AtSerial.WriteCommand("AT+CNUM");
	while (true) {
		if (!_AtSerial.ReadResponse("^(OK|\\+CNUM: .*)$", 500, &response)) return RET_ERR(-1, E_TIMEOUT);
		if (response == "OK") break;

		if (numberStr.size() >= 1) continue;

		parser.Parse(response.c_str());
		if (parser.Size() < 2) return RET_ERR(-1, E_INVALID_RESPONSE);
		numberStr = parser[1];
	}

	if ((int)numberStr.size() + 1 > numberSize) return RET_ERR(-1, E_NOT_ENOUGH_SIZE);
	strcpy(number, numberStr.c_str());

	return RET_OK((int)strlen(number));
}

int WioLTE::GetReceivedSignalStrength()
{
	std::string response;
	ArgumentParser parser;

	_AtSerial.WriteCommand("AT+CSQ");
	if (!_AtSerial.ReadResponse("^\\+CSQ: (.*)$", 500, &response)) return RET_ERR(INT_MIN, E_TIMEOUT);

	parser.Parse(response.c_str());
	if (parser.Size() != 2) return RET_ERR(INT_MIN, E_INVALID_RESPONSE);
	int rssi = atoi(parser[0]);

	if (!_AtSerial.ReadResponse("^OK$", 500, NULL)) return RET_ERR(INT_MIN, E_TIMEOUT);

	if (rssi == 0) return RET_OK(-113);
	else if (rssi == 1) return RET_OK(-111);
	else if (2 <= rssi && rssi <= 30) return RET_OK((int)LINEAR_SCALE((double)rssi, 2, 30, -109, -53));
	else if (rssi == 31) return RET_OK(-51);
	else if (rssi == 99) return RET_OK(-999);
	else if (rssi == 100) return RET_OK(-116);
	else if (rssi == 101) return RET_OK(-115);
	else if (102 <= rssi && rssi <= 190) return RET_OK((int)LINEAR_SCALE((double)rssi, 102, 190, -114, -26));
	else if (rssi == 191) return RET_OK(-25);
	else if (rssi == 199) return RET_OK(-999);
	
	return RET_OK(-999);
}

bool WioLTE::GetTime(struct tm* tim)
{
	std::string response;

	_AtSerial.WriteCommand("AT+CCLK?");
	if (!_AtSerial.ReadResponse("^\\+CCLK: (.*)$", 500, &response)) return RET_ERR(false, E_TIMEOUT);
	if (!_AtSerial.ReadResponse("^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);

	if (strlen(response.c_str()) != 22) return RET_ERR(false, E_INVALID_RESPONSE);
	const char* parameter = response.c_str();

	if (parameter[0] != '"') return RET_ERR(false, E_INVALID_RESPONSE);
	if (parameter[3] != '/') return RET_ERR(false, E_INVALID_RESPONSE);
	if (parameter[6] != '/') return RET_ERR(false, E_INVALID_RESPONSE);
	if (parameter[9] != ',') return RET_ERR(false, E_INVALID_RESPONSE);
	if (parameter[12] != ':') return RET_ERR(false, E_INVALID_RESPONSE);
	if (parameter[15] != ':') return RET_ERR(false, E_INVALID_RESPONSE);
	if (parameter[21] != '"') return RET_ERR(false, E_INVALID_RESPONSE);

	int yearOffset = atoi(&parameter[1]);
	tim->tm_year = (yearOffset >= 80 ? 1900 : 2000) + yearOffset - 1900;
	tim->tm_mon = atoi(&parameter[4]) - 1;
	tim->tm_mday = atoi(&parameter[7]);
	tim->tm_hour = atoi(&parameter[10]);
	tim->tm_min = atoi(&parameter[13]);
	tim->tm_sec = atoi(&parameter[16]);
	tim->tm_wday = 0;
	tim->tm_yday = 0;
	tim->tm_isdst = 0;

	// Update tm_wday and tm_yday
	mktime(tim);

	return RET_OK(true);
}

bool WioLTE::SendSMS(const char* dialNumber, const char* message)
{
	if (!_AtSerial.WriteCommandAndReadResponse("AT+CMGF=1", "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);

	StringBuilder str;
	if (!str.WriteFormat("AT+CMGS=\"%s\"", dialNumber)) return RET_ERR(false, E_UNKNOWN);
	_AtSerial.WriteCommand(str.GetString());
	if (!_AtSerial.ReadResponse("^> ", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
	_AtSerial.WriteBinary((const byte*)message, strlen(message));
	_AtSerial.WriteBinary((const byte*)"\x1a", 1);
	if (!_AtSerial.ReadResponse("^OK$", 120000, NULL)) return RET_ERR(false, E_TIMEOUT);

	return RET_OK(true);
}

int WioLTE::ReceiveSMS(char* message, int messageSize, char* dialNumber, int dialNumberSize)
{
	int messageIndex = GetFirstIndexOfReceivedSMS();
	if (messageIndex == -2) return RET_OK(0);
	if (messageIndex < 0) return RET_ERR(-1, E_INVALID_ARGUMENT);

	if (!_AtSerial.WriteCommandAndReadResponse("AT+CMGF=0", "^OK$", 500, NULL)) return RET_ERR(-1, E_TIMEOUT);

	StringBuilder str;
	if (!str.WriteFormat("AT+CMGR=%d", messageIndex)) return RET_ERR(-1, E_UNKNOWN);
	_AtSerial.WriteCommand(str.GetString());

	if (!_AtSerial.ReadResponse("^\\+CMGR: .*$", 500, NULL)) return RET_ERR(-1, E_TIMEOUT);

	std::string response;
	if (!_AtSerial.ReadResponse("^(.*)$", 500, &response)) return RET_ERR(-1, E_TIMEOUT);
	const char* hex = response.c_str();

	int hexSize = strlen(hex);
	if (hexSize % 2 != 0) return RET_ERR(-1, E_INVALID_RESPONSE);
	int dataSize = hexSize / 2;
	byte* data = (byte*)alloca(dataSize);
	if (!ConvertHexToBytes(hex, data, dataSize)) return RET_ERR(-1, E_INVALID_RESPONSE);
	byte* dataEnd = &data[dataSize];

	// 3GPP TS 23.040 https://www.etsi.org/deliver/etsi_ts/123000_123099/123040/09.03.00_60/ts_123040v090300p.pdf
	// 3GPP TS 23.038 https://www.etsi.org/deliver/etsi_ts/123000_123099/123038/10.00.00_60/ts_123038v100000p.pdf
	byte* smscInfoSize = data;
	byte* tpMti = smscInfoSize + 1 + *smscInfoSize;
	if (tpMti >= dataEnd) return RET_ERR(-1, E_INVALID_RESPONSE);
	if ((*tpMti & 0x03) != 0x00) return RET_ERR(-1, E_INVALID_RESPONSE);	// SMS-DELIVER
	bool tpUdhi = *tpMti & 0b01000000 ? true : false;
	byte* tpOaSize = tpMti + 1;
	if (tpOaSize >= dataEnd) return RET_ERR(-1, E_INVALID_RESPONSE);
	byte* tpPid = tpOaSize + 2 + *tpOaSize / 2 + *tpOaSize % 2;
	if (tpPid >= dataEnd) return RET_ERR(-1, E_INVALID_RESPONSE);
	byte* tpDcs = tpPid + 1;
	if (tpDcs >= dataEnd) return RET_ERR(-1, E_INVALID_RESPONSE);
	byte* tpScts = tpDcs + 1;
	if (tpScts >= dataEnd) return RET_ERR(-1, E_INVALID_RESPONSE);
	byte* tpUd = tpScts + 7;
	if (tpUd >= dataEnd) return RET_ERR(-1, E_INVALID_RESPONSE);

	if (dialNumber != NULL && dialNumberSize >= 1)
	{
		if (!SmAddressFieldToString(tpOaSize, dialNumber, dialNumberSize)) return RET_ERR(-1, E_INVALID_RESPONSE);
	}

	int smSize;
	byte* sm;
	if (!tpUdhi)
	{
		smSize = tpUd[0];
		sm = tpUd + 1;
	}
	else
	{
		if (&tpUd[1] >= dataEnd) return RET_ERR(-1, E_INVALID_RESPONSE);
		smSize = tpUd[0] - (1 + tpUd[1]);
		sm = tpUd + 2 + tpUd[1];
	}

	if (messageSize < smSize + 1) return RET_ERR(-1, E_INVALID_RESPONSE);
	for (int i = 0; i < smSize; i++) {
		int offset = i - i / 8;
		int shift = i % 8;
		if (shift == 0) {
			message[i] = sm[offset] & 0x7f;
		}
		else {
			message[i] = (sm[offset] * 256 + sm[offset - 1]) << shift >> 8 & 0x7f;
		}
	}
	message[smSize] = '\0';

	if (!_AtSerial.ReadResponse("^OK$", 500, NULL)) return RET_ERR(-1, E_TIMEOUT);

	return RET_OK(smSize);
}

bool WioLTE::DeleteReceivedSMS()
{
	int messageIndex = GetFirstIndexOfReceivedSMS();
	if (messageIndex == -2) return RET_ERR(false, E_UNKNOWN);
	if (messageIndex < 0) return RET_ERR(false, E_UNKNOWN);

	StringBuilder str;
	if (!str.WriteFormat("AT+CMGD=%d", messageIndex)) return RET_ERR(false, E_UNKNOWN);
	if (!_AtSerial.WriteCommandAndReadResponse(str.GetString(), "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);

	return RET_OK(true);
}

bool WioLTE::WaitForCSRegistration(long timeout)
{
	std::string response;
	ArgumentParser parser;

	Stopwatch sw;
	sw.Restart();
	while (true) {
		int status;

		_AtSerial.WriteCommand("AT+CREG?");
		if (!_AtSerial.ReadResponse("^\\+CREG: (.*)$", 500, &response)) return RET_ERR(false, E_TIMEOUT);
		parser.Parse(response.c_str());
		if (parser.Size() < 2) return RET_ERR(false, E_INVALID_RESPONSE);
		//resultCode = atoi(parser[0]);
		status = atoi(parser[1]);
		if (!_AtSerial.ReadResponse("^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
		if (status == 0) return RET_ERR(false, E_INVALID_RESPONSE);
		if (status == 1 || status == 5) break;

		if (sw.ElapsedMilliseconds() >= (unsigned long)timeout) return RET_ERR(false, E_TIMEOUT);
		_Delay(POLLING_INTERVAL);
	}

	return RET_OK(true);
}

bool WioLTE::WaitForPSRegistration(long timeout)
{
	std::string response;
	ArgumentParser parser;

	Stopwatch sw;
	sw.Restart();
	while (true) {
		int status;

		_AtSerial.WriteCommand("AT+CGREG?");
		if (!_AtSerial.ReadResponse("^\\+CGREG: (.*)$", 500, &response)) return RET_ERR(false, E_TIMEOUT);
		parser.Parse(response.c_str());
		if (parser.Size() < 2) return RET_ERR(false, E_INVALID_RESPONSE);
		//resultCode = atoi(parser[0]);
		status = atoi(parser[1]);
		if (!_AtSerial.ReadResponse("^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
		if (status == 0) return RET_ERR(false, E_INVALID_RESPONSE);
		if (status == 1 || status == 5) break;

		_AtSerial.WriteCommand("AT+CEREG?");
		if (!_AtSerial.ReadResponse("^\\+CEREG: (.*)$", 500, &response)) return RET_ERR(false, E_TIMEOUT);
		parser.Parse(response.c_str());
		if (parser.Size() < 2) return RET_ERR(false, E_INVALID_RESPONSE);
		//resultCode = atoi(parser[0]);
		status = atoi(parser[1]);
		if (!_AtSerial.ReadResponse("^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
		if (status == 0) return RET_ERR(false, E_INVALID_RESPONSE);
		if (status == 1 || status == 5) break;

		if (sw.ElapsedMilliseconds() >= (unsigned long)timeout) return RET_ERR(false, E_TIMEOUT);
		_Delay(POLLING_INTERVAL);
	}

	return RET_OK(true);
}

bool WioLTE::Activate(const char* accessPointName, const char* userName, const char* password, long waitForRegistTimeout)
{
	std::string response;
	ArgumentParser parser;
	Stopwatch sw;

	if (!WaitForPSRegistration(0)) {
		StringBuilder str;
		if (!str.WriteFormat("AT+QICSGP=1,1,\"%s\",\"%s\",\"%s\",3", accessPointName, userName, password)) return RET_ERR(false, E_UNKNOWN);
		if (!_AtSerial.WriteCommandAndReadResponse(str.GetString(), "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);

		sw.Restart();

		if (!WaitForPSRegistration(waitForRegistTimeout)) return RET_ERR(false, E_UNKNOWN);

		// for debug.
#ifdef WIO_DEBUG
		char dbg[100];
		sprintf(dbg, "Elapsed time is %lu[msec.].", sw.ElapsedMilliseconds());
		DEBUG_PRINTLN(dbg);

		_AtSerial.WriteCommandAndReadResponse("AT+CREG?", "^OK$", 500, NULL);
		_AtSerial.WriteCommandAndReadResponse("AT+CGREG?", "^OK$", 500, NULL);
		_AtSerial.WriteCommandAndReadResponse("AT+CEREG?", "^OK$", 500, NULL);
#endif // WIO_DEBUG
	}

	sw.Restart();
	while (true) {
		_AtSerial.WriteCommand("AT+QIACT=1");
		if (!_AtSerial.ReadResponse("^(OK|ERROR)$", 150000, &response)) return RET_ERR(false, E_TIMEOUT);
		if (response == "OK") break;
		if (!_AtSerial.WriteCommandAndReadResponse("AT+QIGETERROR", "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
		if (sw.ElapsedMilliseconds() >= 150000) return RET_ERR(false, E_TIMEOUT);
		_Delay(POLLING_INTERVAL);
	}

	// for debug.
#ifdef WIO_DEBUG
	if (!_AtSerial.WriteCommandAndReadResponse("AT+QIACT?", "^OK$", 150000, NULL)) return RET_ERR(false, E_TIMEOUT);
#endif // WIO_DEBUG

	return RET_OK(true);
}

bool WioLTE::Deactivate()
{
	if (!_AtSerial.WriteCommandAndReadResponse("AT+QIDEACT=1", "^OK$", 40000, NULL)) return RET_ERR(false, E_TIMEOUT);

	return RET_OK(true);
}

bool WioLTE::SyncTime(const char* host)
{
	StringBuilder str;
	std::string response;
	if (!str.WriteFormat("AT+QNTP=1,\"%s\"", host)) return RET_ERR(false, E_UNKNOWN);
	if (!_AtSerial.WriteCommandAndReadResponse(str.GetString(), "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
	if (!_AtSerial.ReadResponse("^\\+QNTP: (.*)$", 125000, &response)) return RET_ERR(false, E_TIMEOUT);
	if (strncmp(response.c_str(), "0,", 2) != 0) return RET_ERR(-1, E_INVALID_RESPONSE); // check whether the command finished successfully

	return RET_OK(true);
}

bool WioLTE::GetLocation(double* longitude, double* latitude)
{
	std::string response;
	ArgumentParser parser;

	if (!_AtSerial.WriteCommandAndReadResponse("AT+QLOCCFG=\"contextid\",1", "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);

	_AtSerial.WriteCommand("AT+QCELLLOC");
	if (!_AtSerial.ReadResponse("^(\\+QCELLLOC: .*|\\+CME ERROR: .*)$", 60000, &response)) return RET_ERR(false, E_TIMEOUT);
	if (strncmp(response.c_str(), "+QCELLLOC: ", 11) != 0) return RET_ERR(false, E_INVALID_RESPONSE);

	parser.Parse(&response.c_str()[11]);
	if (parser.Size() != 2) return RET_ERR(false, E_INVALID_RESPONSE);
	*longitude = atof(parser[0]);
	*latitude = atof(parser[1]);
	if (!_AtSerial.ReadResponse("^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);

	return RET_OK(true);
}

int WioLTE::SocketOpen(const char* host, int port, SocketType type)
{
	std::string response;
	ArgumentParser parser;

	if (host == NULL || host[0] == '\0') return RET_ERR(-1, E_INVALID_ARGUMENT);
	if (port < 0 || 65535 < port) return RET_ERR(-1, E_INVALID_ARGUMENT);

	const char* typeStr;
	switch (type) {
	case SOCKET_TCP:
		typeStr = "TCP";
		break;
	case SOCKET_UDP:
		typeStr = "UDP";
		break;
	default:
		return RET_ERR(-1, E_INVALID_ARGUMENT);
	}

	bool connectIdUsed[CONNECT_ID_NUM];
	for (int i = 0; i < CONNECT_ID_NUM; i++) connectIdUsed[i] = false;

	_AtSerial.WriteCommand("AT+QISTATE?");
	do {
		if (!_AtSerial.ReadResponse("^(OK|\\+QISTATE: .*)$", 10000, &response)) return RET_ERR(-1, E_TIMEOUT);
		if (strncmp(response.c_str(), "+QISTATE: ", 10) == 0) {
			parser.Parse(&response.c_str()[10]);
			if (parser.Size() >= 1) {
				int connectId = atoi(parser[0]);
				if (connectId < 0 || CONNECT_ID_NUM <= connectId) return RET_ERR(-1, E_INVALID_RESPONSE);
				connectIdUsed[connectId] = true;
			}
		}
	} while (response != "OK");

	int connectId;
	for (connectId = 0; connectId < CONNECT_ID_NUM; connectId++) {
		if (!connectIdUsed[connectId]) break;
	}
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(-1, E_NOT_ENOUGH_RESOURCE);

	StringBuilder str;
	if (!str.WriteFormat("AT+QIOPEN=1,%d,\"%s\",\"%s\",%d", connectId, typeStr, host, port)) return RET_ERR(-1, E_UNKNOWN);
	if (!_AtSerial.WriteCommandAndReadResponse(str.GetString(), "^OK$", 150000, NULL)) return RET_ERR(-1, E_TIMEOUT);
	str.Clear();
	if (!str.WriteFormat("^\\+QIOPEN: %d,0$", connectId)) return RET_ERR(-1, E_UNKNOWN);
	if (!_AtSerial.ReadResponse(str.GetString(), 150000, NULL)) return RET_ERR(-1, E_TIMEOUT);

	return RET_OK(connectId);
}

bool WioLTE::SocketSend(int connectId, const byte* data, int dataSize)
{
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(false, E_INVALID_ARGUMENT);
	if (dataSize > 1460) return RET_ERR(false, E_INVALID_ARGUMENT);

	StringBuilder str;
	if (!str.WriteFormat("AT+QISEND=%d,%d", connectId, dataSize)) return RET_ERR(false, E_UNKNOWN);
	_AtSerial.WriteCommand(str.GetString());
	if (!_AtSerial.ReadResponse("^>", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
	_AtSerial.WriteBinary(data, dataSize);
	if (!_AtSerial.ReadResponse("^SEND OK$", 5000, NULL)) return RET_ERR(false, E_TIMEOUT);

	return RET_OK(true);
}

bool WioLTE::SocketSend(int connectId, const char* data)
{
	return SocketSend(connectId, (const byte*)data, strlen(data));
}

int WioLTE::SocketReceive(int connectId, byte* data, int dataSize)
{
	std::string response;

	if (connectId >= CONNECT_ID_NUM) return RET_ERR(-1, E_INVALID_ARGUMENT);

	StringBuilder str;
	if (!str.WriteFormat("AT+QIRD=%d", connectId)) return RET_ERR(-1, E_UNKNOWN);
	_AtSerial.WriteCommand(str.GetString());
	if (!_AtSerial.ReadResponse("^\\+QIRD: (.*)$", 500, &response)) return RET_ERR(-1, E_TIMEOUT);
	int dataLength = atoi(response.c_str());
	if (dataLength >= 1) {
		if (dataLength > dataSize) return RET_ERR(-1, E_NOT_ENOUGH_SIZE);
		if (!_AtSerial.ReadBinary(data, dataLength, 500)) return RET_ERR(-1, E_TIMEOUT);
	}
	if (!_AtSerial.ReadResponse("^OK$", 500, NULL)) return RET_ERR(-1, E_TIMEOUT);

	return RET_OK(dataLength);
}

int WioLTE::SocketReceive(int connectId, char* data, int dataSize)
{
	int dataLength = SocketReceive(connectId, (byte*)data, dataSize - 1);
	if (dataLength >= 0) data[dataLength] = '\0';

	return dataLength;
}

int WioLTE::SocketReceive(int connectId, byte* data, int dataSize, long timeout)
{
	Stopwatch sw;
	sw.Restart();
	int dataLength;
	while ((dataLength = SocketReceive(connectId, data, dataSize)) == 0) {
		if (sw.ElapsedMilliseconds() >= (unsigned long)timeout) return 0;
		_Delay(POLLING_INTERVAL);
	}
	return dataLength;
}

int WioLTE::SocketReceive(int connectId, char* data, int dataSize, long timeout)
{
	Stopwatch sw;
	sw.Restart();
	int dataLength;
	while ((dataLength = SocketReceive(connectId, data, dataSize)) == 0) {
		if (sw.ElapsedMilliseconds() >= (unsigned long)timeout) return 0;
		_Delay(POLLING_INTERVAL);
	}
	return dataLength;
}

bool WioLTE::SocketClose(int connectId)
{
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(false, E_INVALID_ARGUMENT);

	StringBuilder str;
	if (!str.WriteFormat("AT+QICLOSE=%d", connectId)) return RET_ERR(false, E_UNKNOWN);
	if (!_AtSerial.WriteCommandAndReadResponse(str.GetString(), "^OK$", 10000, NULL)) return RET_ERR(false, E_TIMEOUT);

	return RET_OK(true);
}

int WioLTE::HttpGet(const char* url, char* data, int dataSize, long timeout)
{
	WioLTEHttpHeader header;
	header["Accept"] = "*/*";
	header["User-Agent"] = HTTP_USER_AGENT;
	header["Connection"] = "close";

	return HttpGet(url, data, dataSize, header, timeout);
}

int WioLTE::HttpGet(const char* url, char* data, int dataSize, const WioLTEHttpHeader& header, long timeout)
{
	std::string response;
	ArgumentParser parser;

	int timeoutSec = timeout / 1000;
	if (timeout % 1000 > 0) timeoutSec++;

	if (strncmp(url, "https:", 6) == 0) {
		if (!_AtSerial.WriteCommandAndReadResponse("AT+QHTTPCFG=\"sslctxid\",1"         , "^OK$", 500, NULL)) return RET_ERR(-1, E_TIMEOUT);
		if (!_AtSerial.WriteCommandAndReadResponse("AT+QSSLCFG=\"sslversion\",1,4"      , "^OK$", 500, NULL)) return RET_ERR(-1, E_TIMEOUT);
		if (!_AtSerial.WriteCommandAndReadResponse("AT+QSSLCFG=\"ciphersuite\",1,0X0005", "^OK$", 500, NULL)) return RET_ERR(-1, E_TIMEOUT);
		if (!_AtSerial.WriteCommandAndReadResponse("AT+QSSLCFG=\"seclevel\",1,0"        , "^OK$", 500, NULL)) return RET_ERR(-1, E_TIMEOUT);
	}

	if (!_AtSerial.WriteCommandAndReadResponse("AT+QHTTPCFG=\"requestheader\",1", "^OK$", 500, NULL)) return RET_ERR(-1, E_TIMEOUT);

	if (!HttpSetUrl(url)) return RET_ERR(-1, E_UNKNOWN);

	const char* host;
	int hostLength;
	const char* uri;
	int uriLength;
	if (!SplitUrl(url, &host, &hostLength, &uri, &uriLength)) return RET_ERR(false, E_INVALID_ARGUMENT);

	StringBuilder headerSb;
	headerSb.Write("GET ");
	if (uriLength <= 0) {
		headerSb.Write("/");
	}
	else {
		headerSb.Write(uri, uriLength);
	}
	headerSb.Write(" HTTP/1.1\r\n");
	headerSb.Write("Host: ");
	headerSb.Write(host, hostLength);
	headerSb.Write("\r\n");
	for (auto it = header.begin(); it != header.end(); it++) {
		headerSb.Write(it->first.c_str());
		headerSb.Write(": ");
		headerSb.Write(it->second.c_str());
		headerSb.Write("\r\n");
	}
	headerSb.Write("\r\n");
	DEBUG_PRINTLN("=== header");
	DEBUG_PRINTLN(headerSb.GetString());
	DEBUG_PRINTLN("===");

	StringBuilder str;
	if (!str.WriteFormat("AT+QHTTPGET=%d,%d", timeoutSec, headerSb.Length())) return RET_ERR(false, E_UNKNOWN);
	_AtSerial.WriteCommand(str.GetString());
	if (!_AtSerial.ReadResponse("^CONNECT$", 60000, NULL)) return RET_ERR(false, E_TIMEOUT);
	const char* headerStr = headerSb.GetString();
	_AtSerial.WriteBinary((const byte*)headerStr, strlen(headerStr));
	if (!_AtSerial.ReadResponse("^OK$", 1000, NULL)) return RET_ERR(false, E_TIMEOUT);
	if (!_AtSerial.ReadResponse("^\\+QHTTPGET: (.*)$", (timeoutSec + 1) * 1000, &response)) return RET_ERR(-1, E_TIMEOUT);

	parser.Parse(response.c_str());
	if (parser.Size() < 1) return RET_ERR(-1, E_INVALID_RESPONSE);
	if (strcmp(parser[0], "0") != 0) return RET_ERR(-1, E_INVALID_RESPONSE);
	int contentLength = parser.Size() >= 3 ? atoi(parser[2]) : -1;

	_AtSerial.WriteCommand("AT+QHTTPREAD");
	if (!_AtSerial.ReadResponse("^CONNECT$", 1000, NULL)) return RET_ERR(-1, E_TIMEOUT);
	if (contentLength >= 0) {
		if (contentLength + 1 > dataSize) return RET_ERR(-1, E_NOT_ENOUGH_SIZE);
		if (!_AtSerial.ReadBinary((byte*)data, contentLength, 60000)) return RET_ERR(-1, E_TIMEOUT);
		data[contentLength] = '\0';

		if (!_AtSerial.ReadResponse("^OK$", 1000, NULL)) return RET_ERR(-1, E_TIMEOUT);
	}
	else {
		if (!_AtSerial.ReadResponseQHTTPREAD(data, dataSize, 60000)) return RET_ERR(-1, E_TIMEOUT);
		contentLength = strlen(data);
	}
	if (!_AtSerial.ReadResponse("^\\+QHTTPREAD: 0$", 1000, NULL)) return RET_ERR(-1, E_TIMEOUT);

	return RET_OK(contentLength);
}

bool WioLTE::HttpPost(const char* url, const char* data, int* responseCode, long timeout)
{
	WioLTEHttpHeader header;
	header["Accept"] = "*/*";
	header["User-Agent"] = HTTP_USER_AGENT;
	header["Connection"] = "close";
	header["Content-Type"] = HTTP_CONTENT_TYPE;

	return HttpPost(url, data, responseCode, header, timeout);
}

bool WioLTE::HttpPost(const char* url, const char* data, int* responseCode, const WioLTEHttpHeader& header, long timeout)
{
	std::string response;
	ArgumentParser parser;

	int timeoutSec = timeout / 1000;
	if (timeout % 1000 > 0) timeoutSec++;

	if (strncmp(url, "https:", 6) == 0) {
		if (!_AtSerial.WriteCommandAndReadResponse("AT+QHTTPCFG=\"sslctxid\",1"         , "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
		if (!_AtSerial.WriteCommandAndReadResponse("AT+QSSLCFG=\"sslversion\",1,3"      , "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
		if (!_AtSerial.WriteCommandAndReadResponse("AT+QSSLCFG=\"ciphersuite\",1,0XFFFF", "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
		if (!_AtSerial.WriteCommandAndReadResponse("AT+QSSLCFG=\"seclevel\",1,0"        , "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);
	}

	if (!_AtSerial.WriteCommandAndReadResponse("AT+QHTTPCFG=\"requestheader\",1", "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);

	if (!HttpSetUrl(url)) return RET_ERR(false, E_UNKNOWN);

	const char* host;
	int hostLength;
	const char* uri;
	int uriLength;
	if (!SplitUrl(url, &host, &hostLength, &uri, &uriLength)) return RET_ERR(false, E_INVALID_ARGUMENT);

	StringBuilder headerSb;
	headerSb.Write("POST ");
	if (uriLength <= 0) {
		headerSb.Write("/");
	}
	else {
		headerSb.Write(uri, uriLength);
	}
	headerSb.Write(" HTTP/1.1\r\n");
	headerSb.Write("Host: ");
	headerSb.Write(host, hostLength);
	headerSb.Write("\r\n");
	if (!headerSb.WriteFormat("Content-Length: %d\r\n", strlen(data))) return RET_ERR(false, E_UNKNOWN);
	for (auto it = header.begin(); it != header.end(); it++) {
		headerSb.Write(it->first.c_str());
		headerSb.Write(": ");
		headerSb.Write(it->second.c_str());
		headerSb.Write("\r\n");
	}
	headerSb.Write("\r\n");
	DEBUG_PRINTLN("=== header");
	DEBUG_PRINTLN(headerSb.GetString());
	DEBUG_PRINTLN("===");

	StringBuilder str;
	if (!str.WriteFormat("AT+QHTTPPOST=%d,%d,%d", headerSb.Length() + strlen(data), timeoutSec, timeoutSec)) return RET_ERR(false, E_UNKNOWN);
	_AtSerial.WriteCommand(str.GetString());
	if (!_AtSerial.ReadResponse("^CONNECT$", 60000, NULL)) return RET_ERR(false, E_TIMEOUT);
	const char* headerStr = headerSb.GetString();
	_AtSerial.WriteBinary((const byte*)headerStr, strlen(headerStr));
	_AtSerial.WriteBinary((const byte*)data, strlen(data));
	if (!_AtSerial.ReadResponse("^OK$", 1000, NULL)) return RET_ERR(false, E_TIMEOUT);
	if (!_AtSerial.ReadResponse("^\\+QHTTPPOST: (.*)$", (timeoutSec + 1) * 1000, &response)) return RET_ERR(false, E_TIMEOUT);
	parser.Parse(response.c_str());
	if (parser.Size() < 1) return RET_ERR(false, E_INVALID_RESPONSE);
	if (strcmp(parser[0], "0") != 0) return RET_ERR(false, E_INVALID_RESPONSE);
	if (parser.Size() < 2) {
		*responseCode = -1;
	}
	else {
		*responseCode = atoi(parser[1]);
	}

	return RET_OK(true);
}

bool WioLTE::EnableGNSS(long timeout)
{
	std::string response;

	Stopwatch sw;
	sw.Restart();
	while (true) {
		_AtSerial.WriteCommand("AT+QGPS=1");
		if (!_AtSerial.ReadResponse("^(OK|ERROR)$", 500, &response)) return RET_ERR(false, E_TIMEOUT);
		if (response == "OK") break;
		if (sw.ElapsedMilliseconds() >= (unsigned long)timeout) return RET_ERR(false, E_TIMEOUT);
		_Delay(POLLING_INTERVAL);
	}

	return RET_OK(true);
}

bool WioLTE::DisableGNSS()
{
	if (!_AtSerial.WriteCommandAndReadResponse("AT+QGPSEND", "^OK$", 500, NULL)) return RET_ERR(false, E_TIMEOUT);

	return RET_OK(true);
}

bool WioLTE::GetGNSSLocation(double* longitude, double* latitude, double* altitude, struct tm* tim)
{
	std::string response;
	std::string locStr;

	_AtSerial.WriteCommand("AT+QGPS=1");
	delay(1000);
	_AtSerial.WriteCommand("AT+QGPSLOC=0");
	delay(1000);
	_AtSerial.WriteCommand("AT+QGPSLOC?");
	while (true) {
		if (!_AtSerial.ReadResponse("^(OK|\\+QGPSLOC: .*|\\+CME ERROR: .*)$", 500, &response)) return RET_ERR(false, E_TIMEOUT);
		if (response == "OK") break;
		if (strncmp(response.c_str(), "+CME ERROR: ", 12) == 0) {
			if (strcmp(&response.c_str()[12], "516") == 0) {	// Not fixed now
				return RET_ERR(false, E_GNSS_NOT_FIXED);
			}
			else {
				return RET_ERR(false, E_INVALID_RESPONSE);
			}
		}
		locStr = response;
	}

	// parse the response: utc time, latitude, longitude, horizontal precision, altitude
	if (strlen(locStr.c_str()) < 10) return RET_ERR(false, E_INVALID_RESPONSE);
	ArgumentParser parser;
	parser.Parse(&locStr.c_str()[10]);
	if (parser.Size() < 5) return RET_ERR(false, E_INVALID_RESPONSE);

	// latitude
	if (latitude != NULL) {
		*latitude = GnssCoordinateToDecimal(atof(parser[1]));

		if (parser[1][strlen(parser[1]) - 1] != 'N') {
			*latitude = - *latitude;
		}
	}

	// longitude
	if (longitude != NULL) {
		*longitude = GnssCoordinateToDecimal(atof(parser[2]));

		if (parser[2][strlen(parser[2]) - 1] != 'E') {
			*longitude = - *longitude;
		}
	}

	// altitude
	if (altitude != NULL) {
		*altitude = atof(parser[4]);
	}

	// utc time
	if (tim != NULL) {
		if (parser.Size() < 10) return RET_ERR(false, E_INVALID_RESPONSE);

		const char* ymd = parser[9];	// date. ddmmyy
		const char* hms = parser[0];	// time. hhmmss.s

		if (strlen(ymd) != 6) return RET_ERR(false, E_INVALID_RESPONSE);
		if (strlen(hms) < 6) return RET_ERR(false, E_INVALID_RESPONSE);

		int yearOffset = Convert2DigitsToInt(&ymd[4]);
		tim->tm_year = (yearOffset >= 80 ? 1900 : 2000) + yearOffset - 1900;
		tim->tm_mon = Convert2DigitsToInt(&ymd[2]) - 1;
		tim->tm_mday = Convert2DigitsToInt(&ymd[0]);
		tim->tm_hour = Convert2DigitsToInt(&hms[0]);
		tim->tm_min = Convert2DigitsToInt(&hms[2]);
		tim->tm_sec = Convert2DigitsToInt(&hms[4]);
		tim->tm_wday = 0;
		tim->tm_yday = 0;
		tim->tm_isdst = 0;

		// Update tm_wday and tm_yday
		mktime(tim);
	}

	return RET_OK(true);
}

void WioLTE::SystemReset()
{
	NVIC_SystemReset();
}

////////////////////////////////////////////////////////////////////////////////////////
