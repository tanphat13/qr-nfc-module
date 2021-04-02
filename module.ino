
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
char espChipId[16];
const char *ssid = "iPhone";
const char *password = "chuoideptrai";


Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
WiFiManager wifiManager;

char http_server[40];
char http_port[6];
char secret_key[32];
char connection_string[255];
char* serviceId;

String qrCode;
long lastReconnectAttempt = 0;
bool shouldSaveConfig = false;
bool buttonState = LOW;
static const char *connectionString = "HostName=thesis-hcmut.azure-devices.net;DeviceId=MyNodeDevice;SharedAccessKey=ZZ5ogRqIadvRwMQO0h+bOSoypF3okyICDklnmVC8CxU=";

SoftwareSerial gtSerial(CO2_RX, CO2_TX);

void saveConfigCallback()
{
  Serial.println("Saving config...");
  shouldSaveConfig = true;
}

void checkButton()
{
  if (debounceButton(buttonState) == HIGH && buttonState == LOW)
  {
    Serial.println("Starting config portal");
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    wifiManager.setConfigPortalTimeout(120);
    buttonState = HIGH;
    if (!wifiManager.startConfigPortal(espChipId, "12345678"))
    {
      Serial.println("failed to connect or hit timeout");
      ESP.restart();
    }
    else
    {
      wifiManager.autoConnect();
    }
  }
  else if (debounceButton(buttonState) == LOW && buttonState == HIGH)
  {
    buttonState = LOW;
  }
}

bool debounceButton(bool state)
{
  bool stateNow = digitalRead(BUTTON);
  if (state != stateNow)
  {
    delay(10);
    stateNow = digitalRead(BUTTON);
  }
  return stateNow;
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
        DynamicJsonDocument jsonBuffer(1024);
        DeserializationError error = deserializeJson(jsonBuffer, buf.get());
        serializeJsonPretty(jsonBuffer, Serial);
        if (!error)
        {
          Serial.println("\nparsed json");

          strcpy(http_server, jsonBuffer["http_server"]);
          strcpy(http_port, jsonBuffer["http_port"]);
          strcpy(secret_key, jsonBuffer["secret_key"]);
          strcpy(connection_string, jsonBuffer["connection_string"]);
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


void ReConnectWifi(char const* newSsid, char const* newPassword) {
  Serial.print(newSsid);
  Serial.print(newPassword);
  wifiManager.connectWifi(newSsid,newPassword);
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

//  Serial.println(result);
  DeserializationError error = deserializeJson(doc, result);
  if (error) {
    Serial.println(error.f_str());
    return;
  }
  if (doc["wifiConfig"]) {
    Serial.println("Have new config");
    char const* newSsid = doc["wifiConfig"]["ssid"];
    char const* newPassword =doc["wifiConfig"]["password"];
    
    ReConnectWifi(newSsid,newPassword);
  }

  free(result);
}

// MessageCallback log the feedback after sent message to server


static void MessageCallBack(const char* patload,int size){
  // do some thing  after upload code
}



//end


String getProvisioningConnectionString(String serverIp, uint16_t serverPort, String secretKey) {
  HTTPClient http;
  bool serverStatus = http.begin(serverIp, serverPort, "/module/iot-hub-registration");
  http.addHeader("secret_key", secretKey);

  String connectionString = "";

  int httpResponse = http.GET();
  if (httpResponse > 0) {
    Serial.println("[HTTP] Status Code: " + httpResponse);

    if (httpResponse == HTTP_CODE_OK) {
      connectionString = http.getString();
    }
  } else {
    Serial.println("[HTTP] GET... Failed, Error Code: " + httpResponse);
  }
  http.end();
  return connectionString;
}

int postDataToServer(String serverIp, uint16_t serverPort, String secretKey, String data)
{

  HTTPClient http;
  bool serverStatus = http.begin(serverIp, serverPort, "/identification");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("secret_key", secretKey);

  DynamicJsonDocument doc(1024);
  doc["customerCode"] = data;
  doc["moduleId"] = "esp-32-86-31";

  String requestBody;
  serializeJson(doc, requestBody);
  int httpResponse = http.POST(requestBody);
  http.end();
  return httpResponse;
}

void setup()
{
  // put your setup code here, to run once:
 WiFi.mode(WIFI_STA);
  Serial.begin(115200);
  Software serial port
 gtSerial.begin(9600);
 while (!Serial)
   delay(10);
 pinMode(BUTTON, INPUT);

 Serial.println("Hello!");


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

 WiFiManagerParameter custom_http_server("server", "http server", http_server, 40);
 WiFiManagerParameter custom_http_port("port", "http port", http_port, 6);
 WiFiManagerParameter custom_secret_key("key", "secret key", secret_key, 40);

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

   if (connection_string[0] == '\0') {
     String serverIp = String(http_server);
     uint16_t serverPort = atoi(http_port);
     String secretKey = String(secret_key);
     String provisioning_connection_string = getProvisioningConnectionString(serverIp, serverPort, secretKey);
     Serial.print("Provisioning Connection String: ");
     Serial.println(provisioning_connection_string);
     DeserializationError error = deserializeJson(doc, provisioning_connection_string);
   }
   doc["http_server"] = http_server;
   doc["http_port"] = http_port;
   doc["secret_key"] = secret_key;

   File configFile = SPIFFS.open("/config.json", "w");
   if (!configFile)
   {
     Serial.println("failed to open config file for writing");
   }

   serializeJsonPretty(doc, Serial);
   serializeJson(doc, configFile);
   configFile.close();
   // End save
   shouldSaveConfig = false;
 }

  Serial.println(WiFi.localIP());

  // Set up Azure IoTHub
  
  Esp32MQTTClient_Init((const uint8_t *)connectionString, true);
  if ( !Esp32MQTTClient_Init((const uint8_t *)connectionString, true) ) {
    Serial.println("inital iothub failed!!!");
  }

  Esp32MQTTClient_SetDeviceTwinCallback(DeviceTwinCallback);
  Esp32MQTTClient_SetMessageCallback(MessageCallBack);
  
}

void loop()
{
  
  // put your main code here, to run repeatedly:
  Serial.print(WiFi.status());
  if (WiFi.status() == WL_CONNECTED)
  {
//    Serial.print(serviceId);
    Serial.print(WiFi.SSID());
    Serial.print(wifiManager.getWiFiSSID());
    Esp32MQTTClient_Check();
    String payload;
    int httpResponseCode;
    boolean success;
    uint8_t uid[] = {0, 0, 0, 0, 0, 0, 0};
    uint8_t uidLength;

    String serverIp = String(http_server);
    uint16_t serverPort = atoi(http_port);
    String secretKey = String(secret_key);

    while (gtSerial.available() > 0)
    {
      char str = gtSerial.read();
      if (str == 9)
      {
        qrCode.trim();
        Serial.println(qrCode);
        Serial.println("reach here");
        httpResponseCode = postDataToServer(serverIp, serverPort, secretKey, qrCode);
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

      esp_task_wdt_reset();
      if (uidLength == 4)
      {
        // We probably have a Mifare Classic card ...
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
        httpResponseCode = postDataToServer(serverIp, serverPort, secretKey, cardIdAsString);
      }
     
      delay(500);
    }
  }
  else
  {
    Serial.println("WiFi Disconnected");
    delay(5000);
  }
  checkButton();
  
}