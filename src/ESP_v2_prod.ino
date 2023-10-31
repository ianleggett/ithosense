
#define DEBUG_ESP_HTTP_CLIENT true
#define DEBUG_ESP_PORT Serial

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h> //https://github.com/tzapu/WiFiManager
#include <PubSubClient.h>

#include <Adafruit_Sensor.h>
#include <SPI.h>
#include <FS.h>
#include <ArduinoJson.h>

#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>
#define INFLUXDB_URL "https://eu-central-1-1.aws.cloud2.influxdata.com *** change to URL assigned ****"
#define INFLUXDB_TOKEN "**** Insert your token *****"
#define INFLUXDB_ORG "** Insert Org ID*****"
#define INFLUXDB_BUCKET "sensorData"
// Time zone info
#define TZ_INFO "UTC0"

#define CFG_BUTT 10
#define CFG_LED 14
#define PIR_IN 4

// #define USE_2320 true

#ifdef USE_2320
#include <AM2320.h>
AM2320 am2320;
#else
#include <AM2321.h>
AM2321 am2321;
#endif

#include <BH1750.h>

#define BATT_ID "V"
#define TEMP_ID "T"
#define HUM_ID "H"
#define LUX_ID "L"
#define PIR_ID "P"
#define RST_ID "R" // reset event or crash
#define CFG_ID "CFG"

#define CFX_ID "CFX"
#define LUX_TRIGGER 20
#define HTTP_CODE_OK 200

String ESP_ID = String("ITHOS_") + String(ESP.getChipId(), HEX);

String ESP_FLASH = String("flash:") + String(ESP.getFlashChipId(), HEX);
String ESP_BOOT = String("bootmode:") + String(ESP.getBootMode(), HEX);
String ESP_BOOT_VER = String("bootver:") + ESP.getBootVersion();
String ESP_CORE_VER = String("coreversion:") + ESP.getCoreVersion();
String ESP_SDK_VER = String("sdkversion:") + ESP.getSdkVersion();
String ESP_MD5 = String("sketchmd5:") + ESP.getSketchMD5();
String ESP_GUID = WiFi.macAddress();

ADC_MODE(ADC_VCC);

WiFiServer server(80);

#define ITHOS_SERVER_STR "server_ip_str"
#define ITHOS_PORT_STR "server_port_str"

// DHT dht(DHTPIN, DHTTYPE); // 11 works fine for ESP8266, 6=DHT11
BH1750 lightMeter(0x23); // or 0x5C

static long loopCount = 0;
long scanCount = 0;
uint16_t lux, lux_prev;

boolean lastCfg, lastPir;
// flag for saving data
bool shouldSaveConfig = false;

// char server_url_str[128];
char server_ip_str[50];
char server_port_str[30];
// char node_id_str[20];

WiFiClient espClient;
// PubSubClient client(espClient);

// Declare InfluxDB client instance with preconfigured InfluxCloud certificate
InfluxDBClient infclient(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);

// Declare Data point
Point sensor(ESP_ID.c_str());

void sleepWiFi()
{
  Serial.println("Wifi Sleeping...");
  WiFi.mode(WIFI_OFF);
  delay(500);
}

void sendToEventServer(String type, double event)
{

  Serial.println("Sending Data..");
  sensor.addField(type, event);
  if (!infclient.writePoint(sensor))
  {
    Serial.print("InfluxDB write failed: ");
    Serial.println(infclient.getLastErrorMessage());
  }
  sensor.clearFields();

  DynamicJsonDocument jsonBuffer(1024);
  jsonBuffer["batchnum"] = ESP_ID.c_str();
  jsonBuffer["guid"] = ESP_GUID.c_str();
  jsonBuffer["type"] = type.c_str();
  jsonBuffer["value"] = event;
  char json_String[1024];
  serializeJson(jsonBuffer, json_String);
  Serial.print(ESP_ID.c_str());
  Serial.println(json_String);
}

// mqtt callback on topic
void callback(char *topic, byte *payload, unsigned int length)
{
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++)
  {
    Serial.print((char)payload[i]);
  }
  Serial.println();
}

void readTemp()
{
  float t, h;
#ifdef USE_2320
  if (am2320.measure())
  {
    t = am2320.getTemperature();
    h = am2320.getHumidity();
    Serial.print("(");
    Serial.print(t);
    Serial.print(", ");
    Serial.print(h);
    Serial.println(')');
  }
  else
  {
    int errorCode = am2320.getErrorCode();
    switch (errorCode)
    {
    case 1:
      Serial.println("ERR: Sensor is offline");
      break;
    case 2:
      Serial.println("ERR: CRC validation failed.");
      break;
    }
  }

#else

  am2321.read();
  t = am2321.temperature / 10.0;
  h = am2321.humidity / 10.0;
  Serial.print("(");
  Serial.print(t);
  Serial.print(", ");
  Serial.print(h);
  Serial.println(')');

#endif

  sendToEventServer(TEMP_ID, t);
  sendToEventServer(HUM_ID, h);
}

uint16_t doLightMeter()
{
  // read and check its not zero
  uint16_t lux = lightMeter.readLightLevel();
  Serial.print(lux);
  Serial.println(" lux");
  return lux;
}

float checkBatt()
{
  float vcc = ESP.getVcc() / 1000.0; // readvdd33();
  Serial.print("Batt ");
  Serial.print(vcc);
  Serial.println("V");
  return vcc;
}

void doSleep(long sec)
{
  Serial.println("Sleep ");
  ESP.deepSleep((sec - 5) * 1000 * 1000); // 20 sec sleep-+
  delay(1000);
}

void loop(void)
{

  int pir = digitalRead(PIR_IN);
  digitalWrite(CFG_LED, !pir);
  int cfg = !digitalRead(CFG_BUTT);

  if (lastPir != pir)
  {
    Serial.print("PIR:");
    Serial.println(pir);
    sendToEventServer(PIR_ID, pir);
  }
  // do differential of lightMeter
  if ((scanCount % 5000) == 0)
  {
    lux = lightMeter.readLightLevel();
    if (abs(lux_prev - lux) > LUX_TRIGGER)
    {
      sendToEventServer(LUX_ID, lux);
    }
    lux_prev = lux;
  }

  if (cfg && !lastCfg)
  {
    boolean full = false;
    int c = 0;
    while (cfg && c < 5000)
    {
      c++;
      cfg = !digitalRead(CFG_BUTT);
      delay(1);
    }
    if (c >= 5000)
    {
      Serial.print("FULL CFG:");
      Serial.println(cfg);
      sendToEventServer(CFX_ID, cfg);
      setUpWifi(true, true);
    }
    else
    {
      Serial.print("CFG:");
      Serial.println(cfg);
      sendToEventServer(CFG_ID, cfg);
      setUpWifi(true, false);
    }
  }

  lastPir = pir;
  lastCfg = cfg;

  if (scanCount > 55000)
  { // was 60

    digitalWrite(CFG_LED, 1);
    delay(100);
    digitalWrite(CFG_LED, 0);

    // every minute send results to server
    float vcc = checkBatt();
    readTemp();
    lux = doLightMeter();
    sendToEventServer(LUX_ID, lux);
    sendToEventServer(BATT_ID, vcc);
    scanCount = 0;
  }

  scanCount++;
  delay(1);
}

void loadConfig()
{

  if (SPIFFS.exists("/config.json"))
  {
    // file exists, reading and loading
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
      auto errStatus = deserializeJson(jsonBuffer, buf.get());
      serializeJson(jsonBuffer, Serial);
      if (!errStatus)
      {
        strcpy(server_ip_str, jsonBuffer[ITHOS_SERVER_STR]);
        strcpy(server_port_str, jsonBuffer[ITHOS_PORT_STR]);
        Serial.printf("Settings mqtt:%s port:%s", server_ip_str, server_port_str);
      }
      else
      {
        Serial.println("failed to load json config");
      }
    }
  }
}

// callback notifying us of the need to save config
void saveConfigCallback()
{
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void setUpWifi(boolean demand, boolean resetCfg)
{

  Serial.println("mounting FS...");
  if (SPIFFS.begin())
  {
    Serial.println("mounted file system");

    if (!resetCfg)
    {
      loadConfig();
    }
    WiFiManager wifiManager;
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    WiFiManagerParameter server_ip("ithos_server", "ITHOS server", server_ip_str, 40);
    WiFiManagerParameter server_port("ithos_port", "ITHOS port", server_port_str, 6);

    // wifiManager.addParameter(&node_id);
    wifiManager.addParameter(&server_ip);
    // wifiManager.addParameter(&server_url);
    wifiManager.addParameter(&server_port);

    // reset settings - for testing
    if (resetCfg)
    {
      wifiManager.resetSettings();
    }

    // sets timeout until configuration portal gets turned off
    // useful to make it all retry or go to sleep
    // in seconds
    wifiManager.setTimeout(180);

    // fetches ssid and pass and tries to connect
    // if it does not connect it starts an access point with the specified name
    // here using ESP chip ID
    // and goes into a blocking loop awaiting configuration
    if (!wifiManager.autoConnect(ESP_ID.c_str()))
    {
      Serial.println("failed to connect and hit timeout");
      delay(3000);
      // reset and try again, or maybe put it to deep sleep
      ESP.reset();
      delay(5000);
    }

    // if you get here you have connected to the WiFi
    Serial.printf("connected as %s \n", ESP_ID.c_str());

    strcpy(server_ip_str, server_ip.getValue());
    // strcpy(server_url_str, server_url.getValue());
    strcpy(server_port_str, server_port.getValue());
    // strcpy(node_id_str, node_id.getValue());

    if (shouldSaveConfig)
    {
      Serial.println("saving config");
      DynamicJsonDocument jsonBuffer(1024);
      jsonBuffer[ITHOS_SERVER_STR] = server_ip.getValue();
      jsonBuffer[ITHOS_PORT_STR] = server_port.getValue();

      File configFile = SPIFFS.open("/config.json", "w");
      if (!configFile)
      {
        Serial.println("failed to open config file for writing");
      }
      serializeJson(jsonBuffer, Serial);
      serializeJson(jsonBuffer, configFile);
      configFile.close();
      // end save
    }
  }
  else
  {
    Serial.println("failed to mount FS");
  }
}

void setup()
{

  timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");

  if (infclient.validateConnection())
  {
    Serial.print("Connected to InfluxDB: ");
    Serial.println(infclient.getServerUrl());
  }
  else
  {
    Serial.print("InfluxDB connection failed: ");
    Serial.println(infclient.getLastErrorMessage());
  }

  ESP_ID.toUpperCase();
  ESP_GUID.replace(":", "");

  Serial.begin(9600);
  Serial.println("Ithos.com init");

  Serial.println(ESP_ID);

  pinMode(CFG_BUTT, INPUT_PULLUP);
  pinMode(CFG_LED, OUTPUT);
  pinMode(PIR_IN, INPUT);

  // set up I2C bus
  Wire.begin(2, 5);
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  Serial.print("light setup");
  lightMeter.readLightLevel(); // dummy read zeros

  for (int x = 0; x < 10; x++)
  {
    digitalWrite(CFG_LED, 1);
    delay(100);
    digitalWrite(CFG_LED, 0);
    delay(100);
  }

  int cfg = !digitalRead(CFG_BUTT);
  if (cfg)
  {
    Serial.println("Config button pressed setting up wifi");
    setUpWifi(false, true); // use existing
  }
  else
  {
    setUpWifi(false, false); // use existing
  }
  String str_port(server_port_str);

  sendToEventServer(RST_ID, 1);

  for (int x = 0; x < 10; x++)
  {
    digitalWrite(CFG_LED, 1);
    delay(150);
    digitalWrite(CFG_LED, 0);
    delay(50);
  }

  sendToEventServer(RST_ID, 0);
  digitalWrite(CFG_LED, 1);

#ifdef USE_2320
  am2320.begin(2, 5);
#endif
}
