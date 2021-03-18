#include <FS.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <SoftwareSerial.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>


extern "C" {
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
#define PN532_SS   (5)
#define PN532_MISO (19)

uint32_t chipId = 0;
char espChipId[16];

Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
WiFiManager wifiManager;

char http_server[40];
char http_port[6];
char secret_key[32];

String qrCode;
long lastReconnectAttempt = 0;
bool shouldSaveConfig = false;
bool buttonState = LOW;

SoftwareSerial gtSerial(CO2_RX, CO2_TX);

void saveConfigCallback() {
  Serial.println("Saving config...");
  shouldSaveConfig = true;
}

void checkButton() {
  if (debounceButton(buttonState) == HIGH && buttonState == LOW) {
    Serial.println("Starting config portal");
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    wifiManager.setConfigPortalTimeout(120);
    buttonState = HIGH;      
    if (!wifiManager.startConfigPortal(espChipId,"12345678")) {
      Serial.println("failed to connect or hit timeout");
      ESP.restart();
    } else {
      wifiManager.autoConnect();
    }
  }
  else if (debounceButton(buttonState) == LOW && buttonState == HIGH) {
    buttonState = LOW;
  }
}

bool debounceButton(bool state) {
  bool stateNow = digitalRead(BUTTON);
  if (state != stateNow) {
    delay(10);
    stateNow = digitalRead(BUTTON);
  }
  return stateNow;
}

void setupSpiffs(){

  //read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin(true)) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonDocument jsonBuffer(1024);
        DeserializationError error = deserializeJson(jsonBuffer, buf.get());
        serializeJson(jsonBuffer, Serial);
        if (!error) {
          Serial.println("\nparsed json");

          strcpy(http_server, jsonBuffer["http_server"]);
          strcpy(http_port, jsonBuffer["http_port"]);
          strcpy(secret_key, jsonBuffer["secret_key"]);
          
          }
        } else {
          Serial.println("failed to load json config");
        }
      }
  } else {
    Serial.println("failed to mount FS");
  }
  //end read
}

void setup() {
  // put your setup code here, to run once:
  WiFi.mode(WIFI_STA);
  Serial.begin(115200);
  
  // Software serial port
  gtSerial.begin(9600);
  while (!Serial) delay(10);
  pinMode(BUTTON, INPUT);

  Serial.println("Hello!");

  setupSpiffs();
  

  for (int i = 0; i < 17; i = i + 8) {
    chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
  }
  Serial.print("ESP Chip ID: ");
  Serial.println(chipId);

  nfc.begin();
  
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (! versiondata) {
    Serial.print("Didn't find PN53x board");
    while (1);
  }

  Serial.print("Found chip PN53x"); Serial.println((versiondata >> 24) & 0xFF, HEX);
  Serial.print("Firmware ver. "); Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.'); Serial.println((versiondata >> 8) & 0xFF, DEC);

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
  
  if (!wifiManager.autoConnect(espChipId, "12345678")) {
    Serial.println("failed to connect and hit timeout");
    ESP.restart();
  }

  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);
  
  strcpy(http_server, custom_http_server.getValue());
  strcpy(http_port, custom_http_port.getValue());
  strcpy(secret_key, custom_secret_key.getValue());

  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonDocument jsonBuffer(1024);
    jsonBuffer["http_server"] = http_server;
    jsonBuffer["http_port"]   = http_port;
    jsonBuffer["secret_key"]   = secret_key;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }
    
    serializeJsonPretty(jsonBuffer, Serial);
    serializeJson(jsonBuffer, configFile);
    configFile.close();
    // End save
    shouldSaveConfig = false; 
  }
  Serial.println(WiFi.localIP());
}

void loop() {
  // put your main code here, to run repeatedly:
  if (WiFi.status() == WL_CONNECTED) {
    
    HTTPClient http;
    String payload;
    int httpResponseCode;
    boolean success;
    uint8_t uid[] = {0, 0, 0, 0, 0, 0, 0};
    uint8_t uidLength;

    String serverIp = String(http_server);
    uint16_t serverPort = atoi(http_port);
    String secretKey = String(secret_key);
    
    bool serverStatus = http.begin(serverIp, serverPort, "/api/v1/stats");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("secret_key", secretKey);
    
    while (gtSerial.available() > 0) {
      char str = gtSerial.read();
      if (str == 9) {
        qrCode.trim();
        Serial.println(qrCode);
        payload = "{\"id\":" + qrCode + ", \"wins\": 3, \"losses\": 1, \"points_scored\": 12}";
        httpResponseCode = http.POST(payload);
        qrCode = "";
        esp_task_wdt_reset();
        break;
      } else {
        qrCode += str;
      }
    }
  
    success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, &uid[0], &uidLength, 500);
    if (success) {
      Serial.println("Found an ISO14443A card");
      Serial.print("  UID Length: "); Serial.print(uidLength, DEC); Serial.println(" bytes");
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
        payload = "{\"id\":" + cardIdAsString + ", \"wins\": 3, \"losses\": 1, \"points_scored\": 12}";      
        httpResponseCode = http.POST(payload);
       }
        delay(500);
      }
      // Free resources
      http.end();  
    } else {
      Serial.println("WiFi Disconnected");  
    }
    checkButton();
}
