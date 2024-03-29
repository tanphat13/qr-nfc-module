
#include <Esp32MQTTClient.h>
#include <FS.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <SoftwareSerial.h>
#include <Wire.h>
#include <WiFi.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>

extern "C"
{
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
}

#if CONFIG_FREERTOS_UNICORE
#define ARDUINO_RUNNING_CORE 0
#else
#define ARDUINO_RUNNING_CORE 1
#endif

#ifdef ESP32
#include <SPIFFS.h>
#endif

#define WDT_TIMEOUT 120

#define BUTTON 2

#define CO2_TX 1
#define CO2_RX 3

#define PN532_SCK (18)
#define PN532_MOSI (23)
#define PN532_SS (5)
#define PN532_MISO (19)

uint32_t chipId = 0;
char espChipId[10];

Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
WiFiManager wifiManager;
bool shouldSaveConfig = false;

char http_server[16];
char http_port[6];
char secret_key[50];
static char connection_string[255];
String serviceName;
String serviceType;
static char gate[1];
static char deviceId[10];

SoftwareSerial gtSerial(CO2_RX, CO2_TX);

void saveConfigCallback()
{
	Serial.println("Saving config...");
	shouldSaveConfig = true;
}

void setupSpiffs()
{

	//read configuration from FS json
	Serial.println("mounting FS...");

	if (SPIFFS.begin(true))
	{
		Serial.println("mounted file system");
		if (SPIFFS.exists("/config.json"))
		{
			//file exists, reading and loading
			Serial.println("reading config file");
			File configFile = SPIFFS.open("/config.json", "r");
			if (configFile)
			{
				Serial.println("opened config file");
				size_t size = configFile.size();
				// Allocate a buffer to store contents of the file.
				std::unique_ptr<char[]> buf(new char[size]);

				configFile.readBytes(buf.get(), size);
				DynamicJsonDocument jsonBuffer(2048);
				DeserializationError error = deserializeJson(jsonBuffer, buf.get());
				serializeJsonPretty(jsonBuffer, Serial);
					Serial.println("\nparsed json");
					strcpy(connection_string, jsonBuffer["connection_string"]);
					strcpy(deviceId, jsonBuffer["device_id"]);
					if (jsonBuffer["service_type"] && jsonBuffer["service_name"])
					{
						serializeJson(jsonBuffer["service_name"], serviceName);
						serviceName = serviceName.substring(1, serviceName.length() - 1);
						serializeJson(jsonBuffer["service_type"], serviceType);
						serviceType = serviceType.substring(1, serviceType.length() - 1);
						strcpy(gate, jsonBuffer["gate"]);
					}
			}
			else
			{
				Serial.println("failed to load json config");
			}
		}
	}
	else
	{
		Serial.println("failed to mount FS");
	}
	//end read
}

/**
  This is section for integrating system with Azure IoTHub
*/
static void SendConfirmationCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result)
{
  if (result == IOTHUB_CLIENT_CONFIRMATION_OK)
  {
    Serial.println("Send Confirmation Callback finished.");
  }
}

static void SendMessage(const char *payload)
{
	int str_len = serviceType.length() + 1;
	char type[str_len];
	serviceType.toCharArray(type, str_len);
	Serial.println(type);
	str_len = serviceName.length() + 1;
	char name[str_len];
	serviceName.toCharArray(name, str_len);
	EVENT_INSTANCE *message = Esp32MQTTClient_Event_Generate(payload, MESSAGE);
	Esp32MQTTClient_Event_AddProp(message, "serviceType", type);
	Esp32MQTTClient_Event_AddProp(message, "serviceName", name);
	Esp32MQTTClient_Event_AddProp(message, "gate", gate);
	Esp32MQTTClient_SendEventInstance(message);
}

static void ReConnectWifi(char const *newSsid, char const *newPassword)
{
	Serial.print(newSsid);
	Serial.print(newPassword);
	wifiManager.connectWifi(newSsid, newPassword);
}

// DeviceTwinCallBack this function for receive config from server

static void DeviceTwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char *payLoad, int size)
{
	DynamicJsonDocument doc(1024);
	char *result = (char *)malloc(size + 1);
	if (result == NULL)
	{
		return;
	}
	memcpy(result, payLoad, size);
	result[size] = '\0';
	// Display Twin message
	DeserializationError error = deserializeJson(doc, result);
	if (error)
	{
		Serial.println(error.f_str());
		return;
	}
	File configFile = SPIFFS.open("/config.json", "r");
	size_t buf_size = configFile.size();
	std::unique_ptr<char[]> buf(new char[buf_size]);
	configFile.readBytes(buf.get(), buf_size);
	DynamicJsonDocument jsonBuffer(1024);
	error = deserializeJson(jsonBuffer, buf.get());
	if (error) {
		Serial.println(error.f_str());
		return;
	}
	if (doc["desired"]) {
		if (doc["desired"]["wifiConfig"])
		{
			Serial.println("Have new config");
			char const *newSsid = doc["desired"]["wifiConfig"]["ssid"];
			char const *newPassword = doc["desired"]["wifiConfig"]["password"];

			ReConnectWifi(newSsid, newPassword);
		}
		if (doc["desired"]["serviceConfig"])
		{
			jsonBuffer["service_name"] = doc["desired"]["serviceConfig"]["service_name"];
			jsonBuffer["gate"] = doc["desired"]["serviceConfig"]["gate"];
			jsonBuffer["service_type"] = doc["desired"]["serviceConfig"]["service_type"];
			serializeJson(doc["desired"]["serviceConfig"]["service_type"], serviceType);
			serviceType = serviceType.substring(1, serviceType.length() - 1);
			serializeJson(doc["service_name"], serviceName);
			serviceName = serviceName.substring(1, serviceName.length() - 1);
			strcpy(gate, doc["desired"]["serviceConfig"]["gate"]);
		}
	} else {
		if (doc["wifiConfig"])
		{
			Serial.println("Have new config");
			char const *newSsid = doc["wifiConfig"]["ssid"];
			char const *newPassword = doc["wifiConfig"]["password"];

			ReConnectWifi(newSsid, newPassword);
		}
		if (doc["serviceConfig"])
		{
			jsonBuffer["service_name"] = doc["serviceConfig"]["service_name"];
			jsonBuffer["gate"] = doc["serviceConfig"]["gate"];
			jsonBuffer["service_type"] = doc["serviceConfig"]["service_type"];
			serializeJson(doc["serviceConfig"]["service_type"], serviceType);
			serviceType = serviceType.substring(1, serviceType.length() - 1);
			serializeJson(doc["service_name"], serviceName);
			serviceName = serviceName.substring(1, serviceName.length() - 1);
			strcpy(gate, doc["serviceConfig"]["gate"]);
		}
	}
	configFile = SPIFFS.open("/config.json", "w");
	serializeJson(jsonBuffer, configFile);
	configFile.close();
	free(result);
}

// MessageCallback log the feedback after sent message to server

static void MessageCallBack(const char *payload, int size)
{
	Serial.println(payload);
	//  char *result = (char *)malloc(size + 1);
	//  if (result == NULL)
	//  {
	//    return;
	//  }
	//  memcpy(result, payload, size);
	//  result[size] = '\0';
	//  Serial.println(result);
	//  free(result);
}

//end

String getProvisioningConnectionString(String serverIp, uint16_t serverPort, String secretKey)
{
	HTTPClient http;
	bool serverStatus = http.begin(serverIp, serverPort, "/api/modules/iot-hub-registration");
	http.addHeader("secret_key", secretKey);

	String connectionString = "";

	int httpResponse = http.GET();
	if (httpResponse > 0)
	{
		Serial.println("[HTTP] Status Code: " + httpResponse);

		if (httpResponse == HTTP_CODE_OK)
		{
			connectionString = http.getString();
		}
	}
	else
	{
		Serial.println("[HTTP] GET... Failed, Error Code: " + httpResponse);
	}
	http.end();
	return connectionString;
}

void setup()
{

	// put your setup code here, to run once:
	WiFi.mode(WIFI_STA);
	Serial.begin(115200);
	// Software serial port
	gtSerial.begin(9600);

	setupSpiffs();

	for (int i = 0; i < 17; i = i + 8)
	{
		chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
	}
	Serial.print("ESP Chip ID: ");
	Serial.println(chipId);

	nfc.begin();

	uint32_t versiondata = nfc.getFirmwareVersion();
	if (!versiondata)
	{
	 Serial.print("Didn't find PN53x board");
	 while (1)
	   ;
	}

	Serial.print("Found chip PN53x");
	Serial.println((versiondata >> 24) & 0xFF, HEX);
	Serial.print("Firmware ver. ");
	Serial.print((versiondata >> 16) & 0xFF, DEC);
	Serial.print('.');
	Serial.println((versiondata >> 8) & 0xFF, DEC);

	nfc.setPassiveActivationRetries(0xFF);
	nfc.SAMConfig();

	sprintf(espChipId, "%lu", chipId);

	wifiManager.setSaveConfigCallback(saveConfigCallback);
	wifiManager.setClass("invert");

	WiFiManagerParameter custom_http_server("server", "http server", http_server, 15);
	WiFiManagerParameter custom_http_port("port", "http port", http_port, 6);
	WiFiManagerParameter custom_secret_key("key", "secret key", secret_key, 50);

	wifiManager.addParameter(&custom_http_server);
	wifiManager.addParameter(&custom_http_port);
	wifiManager.addParameter(&custom_secret_key);
	wifiManager.setConnectTimeout(10);

	if (!wifiManager.autoConnect(espChipId, "12345678"))
	{
		Serial.println("failed to connect and hit timeout");
		ESP.restart();
	}

	esp_task_wdt_init(WDT_TIMEOUT, true);
	esp_task_wdt_add(NULL);

	strcpy(http_server, custom_http_server.getValue());
	strcpy(http_port, custom_http_port.getValue());
	strcpy(secret_key, custom_secret_key.getValue());

	if (shouldSaveConfig)
	{
		Serial.println("saving config");
		DynamicJsonDocument doc(1024);

		if (strlen(connection_string) == 0)
		{
			String serverIp = String(http_server);
			uint16_t serverPort = atoi(http_port);
			String secretKey = String(secret_key);
			String provisioning_connection_string = "";
			while (provisioning_connection_string == "")
				provisioning_connection_string = getProvisioningConnectionString(serverIp, serverPort, secretKey);
			Serial.print("Provisioning Connection String: ");
			Serial.println(provisioning_connection_string);
			DeserializationError error = deserializeJson(doc, provisioning_connection_string);
			strcpy(connection_string, doc["connection_string"]);

			File configFile = SPIFFS.open("/config.json", "w");
			if (!configFile)
			{
				Serial.println("failed to open config file for writing");
			}

			serializeJsonPretty(doc, Serial);
			serializeJson(doc, configFile);
			configFile.close();
		}
		// End save
		shouldSaveConfig = false;
	}

	// Set up Azure IoTHub
	Esp32MQTTClient_Init((const uint8_t *)connection_string, true);
	if (!Esp32MQTTClient_Init((const uint8_t *)connection_string, true))
	{
		Serial.println("inital iothub failed!!!");
	}

	Esp32MQTTClient_SetDeviceTwinCallback(DeviceTwinCallback);
	Esp32MQTTClient_SetMessageCallback(MessageCallBack);
	Esp32MQTTClient_SetSendConfirmationCallback(SendConfirmationCallback);
}

void loop()
{

	// put your main code here, to run repeatedly:
	if (WiFi.status() == WL_CONNECTED)
	{
		Esp32MQTTClient_Check();
		String qrCode;
		boolean success;
		uint8_t uid[] = {0, 0, 0, 0, 0, 0, 0};
		uint8_t uidLength;
		while (gtSerial.available() > 0)
		{
			char str = gtSerial.read();
			if (str == 9)
			{
				qrCode.trim();
				SendMessage(qrCode.c_str());
				Serial.println(qrCode);
				qrCode = "";
				esp_task_wdt_reset();
				break;
			}
			else
			{
				qrCode += str;
			}
		}

		success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, &uid[0], &uidLength, 500);
		if (success)
		{
			Serial.println("Found an ISO14443A card");
			Serial.print("  UID Length: ");
			Serial.print(uidLength, DEC);
			Serial.println(" bytes");
			Serial.print("  UID Value: ");
			nfc.PrintHex(uid, uidLength);
			if (uidLength == 4)
			{
				uint32_t cardid = uid[0];
				cardid <<= 8;
				cardid |= uid[1];
				cardid <<= 8;
				cardid |= uid[2];
				cardid <<= 8;
				cardid |= uid[3];
				Serial.print("Seems to be a Mifare Classic card #");
				Serial.println(cardid);
				String cardIdAsString = String(cardid);
				Serial.println(serviceType);
				SendMessage(cardIdAsString.c_str());
				esp_task_wdt_reset();
			}
		}
		delay(500);
	}
	else
	{
		Serial.println("WiFi Disconnected");
		delay(5000);
	}
}