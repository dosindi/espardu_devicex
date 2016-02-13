#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <FS.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <ArduinoJson.h>
#include <Time.h>
#include <Timezone.h>
#include "Arduino.h"
ADC_MODE(ADC_VCC);
extern "C" {
#include "user_interface.h"
}

const String CONFIGFILE = "config.json";
const String WIFI_SSID = "ssid";
const String WIFI_PSK = "psk";
const String HOSTNAME = "hostname";
const String MQTT_SERVER = "mqttServer";
const String MQTT_PORT = "mqttPort";
const String MQTT_TOPIC = "mqttTopic";
const String FRIENDLYNAME = "friendlyName";

String config;
DHT_Unified dht(D4, DHT22);

ESP8266WebServer server(80);
WiFiClient wifiClient;
PubSubClient client(wifiClient);
char mqttTopic[20] = "";
unsigned long previousMillis = 0;

//US Eastern Time Zone (New York, Detroit)
TimeChangeRule myDST = {"CEDT", Last, Sun, Mar, 2, 60};    //Daylight time = UTC - 4 hours
TimeChangeRule mySTD = {"CET", Last, Sun, Oct, 2, 120};     //Standard time = UTC - 5 hours
Timezone myTZ(myDST, mySTD);

TimeChangeRule *tcr;        //pointer to the time change rule, use to get TZ abbrev
time_t utc, local;

const char * stateToJsonString(const uint8_t value) {
  return value == HIGH ? "on" : "off";
}

const char * espToNodePin(const uint8_t pin) {
  switch (pin) {
    case 0: return "D3"; break;
    case 1: return "D10"; break;
    case 2: return "D4"; break;
    case 3: return "D9"; break;
    case 4: return "D2"; break;
    case 5: return "D1"; break;
    case 12: return "D6"; break;
    case 13: return "D7"; break;
    case 14: return "D5"; break;
    case 15: return "D8"; break;
    case 16: return "D0"; break;
  }
}

String getDiagnostics() {
	char value[50] = "";
	WiFiClient client = server.client();
	float servolt1 = ESP.getVcc();
	unsigned long spdcount = ESP.getCycleCount();
	delay(1);
	unsigned long spdcount1 = ESP.getCycleCount();
	unsigned long speedcnt = spdcount1-spdcount;
	FlashMode_t ideMode = ESP.getFlashChipMode();
	String uptime = "";
	int hr,mn,st;
	st = millis() / 1000;
	mn = st / 60;
	hr = st / 3600;
	st = st - mn * 60;
	mn = mn - hr * 60;
	if (hr<10) {uptime += ("0");}
	uptime += (hr);
	uptime += (":");
	if (mn<10) {uptime += ("0");}
	uptime += (mn);
	uptime += (":");
	if (st<10) {uptime += ("0");}
	uptime += (st);
	String status = String("{");
	status.concat("\"hostname\":\"");
	status.concat(WiFi.hostname());
	status.concat("\"ip\":\"");
	status.concat(WiFi.localIP());
	status.concat("\",\"freeRam\":");
	status.concat(ESP.getFreeHeap() / 1024);	// KBytes
	status.concat(",\"sdkVersion\":\"");
	status.concat(ESP.getSdkVersion());
	status.concat("\",\"bootVersion\":");
	status.concat(ESP.getBootVersion());
	status.concat(",\"freeSketchSpace\":"); // KBytes
	status.concat(ESP.getFreeSketchSpace()/1024);
	status.concat(",\"sketchSize\":"); // KBytes
	status.concat(ESP.getSketchSize()/1024);
	status.concat(",\"flashChipId\":\"");
	sprintf(value, "%08x", ESP.getFlashChipId());
	status.concat(value);
	status.concat("\",\"flashChipMode\":\"");
	status.concat(ideMode == FM_QIO ? "QIO" : ideMode == FM_QOUT ? "QOUT" : ideMode == FM_DIO ? "DIO" : ideMode == FM_DOUT ? "DOUT" : "UNKNOWN");
	status.concat("\",\"flashSizeByID\":"); // KBytes
	status.concat(ESP.getFlashChipRealSize()/1024);
	status.concat(",\"flashSizeIDE\":"); // KBytes
	status.concat(ESP.getFlashChipSize()/1024);
	status.concat(",\"flashSpeed\":"); // MHz
	status.concat(ESP.getFlashChipSpeed()/1000000);
	status.concat(",\"cpuSpeed\":"); // MHz
	status.concat(ESP.getCpuFreqMHz());
	status.concat(",\"cpuChipId\":\"");
	sprintf(value, "%08x", ESP.getChipId());
	status.concat(value);
	status.concat("\",\"speedCnt\":"); // System Instruction Cycles Per Second
	status.concat(speedcnt*1000);
	status.concat(",\"resetInfo\":\"");
	status.concat(ESP.getResetInfo());
	status.concat("\",\"vcc\":");
	status.concat(servolt1/1000);
	status.concat(",\"uptime\":\"");
	status.concat(uptime);
	uptime = "";
	status.concat("\"}");
	return status;
}

String getStatus() {
	char buffer[1024];
	DynamicJsonBuffer jsonBuffer;
	JsonObject& root = jsonBuffer.createObject();

	JsonObject& state = root.createNestedObject("state");
	state["relay1"] = stateToJsonString(digitalRead(D1));
	state["relay2"] = stateToJsonString(digitalRead(D2));
	state["relay3"] = stateToJsonString(digitalRead(D3));

	float temp = dhtGetTemperature();
	float hum = dhtGetHumidity();
	if (!isnan(temp) || (!isnan(hum))) {
		JsonObject& env = root.createNestedObject("env");
		if (!isnan(temp)) { env["temp"] = temp; }
		if (!isnan(hum)) { env["hum"] = hum; }
	}

	JsonObject& conf = jsonBuffer.parseObject(config);
	conf.remove("psk");
	root["config"] = conf;

	root["diagnostics"] = jsonBuffer.parseObject(getDiagnostics());
	root.printTo(buffer, sizeof(buffer));
	return String(buffer);
}

String getConfig() {
	char buffer[1024];
	DynamicJsonBuffer jsonBuffer;
	JsonObject& root = jsonBuffer.parseObject(config);
	root.remove("psk");
	root.printTo(buffer, sizeof(buffer));
	return String(buffer);
}

void configSetup()
{
	SPIFFS.begin();
	Serial.print("Reading configuration from ");
	Serial.print(CONFIGFILE);
	File configFile = SPIFFS.open(CONFIGFILE, "r");
	if (configFile) {
		Serial.println(". success");
		config = configFile.readString();
		config.trim();
  		configFile.close();
	} else {
		char buffer[1024];
		char hostname[50] = "";
		char mqtt_topic[50] = "";
		sprintf(hostname, "devicex-%08x", ESP.getChipId());
		sprintf(mqtt_topic, "/devicex/%08x", ESP.getChipId());
		DynamicJsonBuffer jsonBuffer;
		JsonObject& root = jsonBuffer.createObject();
		root[HOSTNAME] = hostname;
		root[FRIENDLYNAME] = "DeviceX";
		root[MQTT_PORT] = "1883";
		root[MQTT_TOPIC] = mqtt_topic;
		root.printTo(buffer, sizeof(buffer));
		config = String(buffer);
		Serial.println(". fail");
	}
}

String configGet(String id, String defaultValue) {
	String ret = "";
	DynamicJsonBuffer jsonBuffer;
  	JsonObject& root = jsonBuffer.parseObject(config);
	if (root.containsKey(id)) {
		const char* value = root[id];
		return String(value);
	} else {
		return String(defaultValue);
	}
}

String configGet(String id) {
	return configGet(id, "");
}

void configPut(String id, String value) {
	DynamicJsonBuffer jsonBuffer;
	JsonObject& root = jsonBuffer.parseObject(config);
	root[id] = value;
	char buffer[256];
	root.printTo(buffer, sizeof(buffer));
	config = String(buffer);
}

void configSave() {
	Serial.println("Saving configuration");
	File configFile = SPIFFS.open(CONFIGFILE, "w");
	if (configFile) {
		configFile.print(config);
		configFile.close();
	}
}

void networkSetup() {
	String station_ssid = configGet(WIFI_SSID);
	String station_psk = configGet(WIFI_PSK);
	String station_hostname = configGet(HOSTNAME);
	bool mustSaveConfig = false;
	if (station_ssid.length() == 0) {
		station_ssid = WiFi.SSID();
		configPut(WIFI_SSID, station_ssid);
		mustSaveConfig = true;
	}
	if (station_psk.length() == 0) {
		station_psk = WiFi.psk();
		configPut(WIFI_PSK, station_psk);
		mustSaveConfig = true;
	}
	if (mustSaveConfig) configSave();
	WiFi.hostname(station_hostname);
	if (WiFi.getMode() != WIFI_STA)
	{
		WiFi.mode(WIFI_STA);
		delay(10);
	}
	Serial.print("Connecting to ");
	Serial.println(station_ssid);
	if (WiFi.SSID() != station_ssid || WiFi.psk() != station_psk) {
		WiFi.begin(station_ssid.c_str(), station_psk.c_str());
	} else {
		WiFi.begin();
	}
	if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    	Serial.println("Can not connect to WiFi. Going into AP mode.");
			WiFi.mode(WIFI_AP);
    	delay(10);
    	WiFi.softAP(station_hostname.c_str());
		Serial.print("SSID: ");
		Serial.println(station_hostname);
		Serial.print("IP address: ");
		Serial.println(WiFi.softAPIP());
	} else {
		Serial.print("IP address: ");
		Serial.println(WiFi.localIP());
	}
	Serial.print("Host name: ");
	Serial.println(station_hostname);
}

void firmwareSetup() {
  ArduinoOTA.onStart([]() {
    Serial.println("OTA firmware update initiated");
		mqttDisconnect();
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA firmware update finished");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.setHostname(configGet("hostname").c_str());
  ArduinoOTA.begin();
}

void firmwareLoop() {
	ArduinoOTA.handle();
}

time_t compileTime(void)
{
#define FUDGE 25        //fudge factor to allow for compile time (seconds, YMMV)

  char *compDate = __DATE__, *compTime = __TIME__, *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char chMon[3], *m;
  int d, y;
  tmElements_t tm;
  time_t t;

  strncpy(chMon, compDate, 3);
  chMon[3] = '\0';
  m = strstr(months, chMon);
  tm.Month = ((m - months) / 3 + 1);

  tm.Day = atoi(compDate + 4);
  tm.Year = atoi(compDate + 7) - 1970;
  tm.Hour = atoi(compTime);
  tm.Minute = atoi(compTime + 3);
  tm.Second = atoi(compTime + 6);
  t = makeTime(tm);
  return t + FUDGE;        //add fudge factor to allow for compile time
}

//Print an integer in "00" format (with leading zero).
//Input value assumed to be between 0 and 99.
void sPrintI00(int val)
{
  if (val < 10) Serial.print('0');
  Serial.print(val, DEC);
  return;
}

//Print an integer in ":00" format (with leading zero).
//Input value assumed to be between 0 and 99.
void sPrintDigits(int val)
{
  Serial.print(':');
  if(val < 10) Serial.print('0');
  Serial.print(val, DEC);
}

//Function to print time with time zone
void printTime(time_t t, char *tz)
{
  sPrintI00(hour(t));
  sPrintDigits(minute(t));
  sPrintDigits(second(t));
  Serial.print(' ');
  Serial.print(dayShortStr(weekday(t)));
  Serial.print(' ');
  sPrintI00(day(t));
  Serial.print(' ');
  Serial.print(monthShortStr(month(t)));
  Serial.print(' ');
  Serial.print(year(t));
  Serial.print(' ');
  Serial.print(tz);
}

void printLocalTime() {
  utc = now();
  local = myTZ.toLocal(utc, &tcr);
  printTime(local, tcr -> abbrev);
}

void timeSetup() {
  setTime(myTZ.toUTC(compileTime()));
  utc = now();
  // printTime(utc, "UTC");
  // Serial.println();
  local = myTZ.toLocal(utc, &tcr);
  Serial.print("Current time: ");
  printTime(local, tcr -> abbrev);
  Serial.println();
}

void dhtSetup() {
  dht.begin();
  Serial.println("Initialising DHT sensor");
  sensor_t sensor;
  dht.temperature().getSensor(&sensor);
  Serial.println("------------------------------------");
  Serial.println("Temperature");
  Serial.print  ("Sensor:       "); Serial.println(sensor.name);
  Serial.print  ("Driver Ver:   "); Serial.println(sensor.version);
  Serial.print  ("Unique ID:    "); Serial.println(sensor.sensor_id);
  Serial.print  ("Max Value:    "); Serial.print(sensor.max_value); Serial.println(" *C");
  Serial.print  ("Min Value:    "); Serial.print(sensor.min_value); Serial.println(" *C");
  Serial.print  ("Resolution:   "); Serial.print(sensor.resolution); Serial.println(" *C");
  // Print humidity sensor details.
  dht.humidity().getSensor(&sensor);
  Serial.println("------------------------------------");
  Serial.println("Humidity");
  Serial.print  ("Sensor:       "); Serial.println(sensor.name);
  Serial.print  ("Driver Ver:   "); Serial.println(sensor.version);
  Serial.print  ("Unique ID:    "); Serial.println(sensor.sensor_id);
  Serial.print  ("Max Value:    "); Serial.print(sensor.max_value); Serial.println("%");
  Serial.print  ("Min Value:    "); Serial.print(sensor.min_value); Serial.println("%");
  Serial.print  ("Resolution:   "); Serial.print(sensor.resolution); Serial.println("%");
  Serial.println("------------------------------------");
}

float dhtGetTemperature() {
  sensors_event_t event;
  dht.temperature().getEvent(&event);
  if (isnan(event.temperature)) {
    Serial.print("Error reading temperature! ");
  }
  else {
    Serial.print("Temperature: ");
    Serial.print(event.temperature);
    Serial.print(" *C, ");
  }
  return event.temperature;
}

float dhtGetHumidity() {
  sensors_event_t event;
  dht.humidity().getEvent(&event);
  if (isnan(event.relative_humidity)) {
    Serial.println("Error reading humidity!");
  }
  else {
    Serial.print("Humidity: ");
    Serial.print(event.relative_humidity);
    Serial.println("%");
  }
  return event.relative_humidity;
}

void buttonsSetup() {
  Serial.println("Initialising buttons");
  pinMode(D5, INPUT_PULLUP);
  pinMode(D6, INPUT_PULLUP);
  pinMode(D7, INPUT_PULLUP);
}

void buttonLoop(const uint8_t buttonPin, const uint8_t relayPin, const char* id) {
  static int val[16];
  static unsigned long t[16];
  int newval = digitalRead(buttonPin);
  if (newval != val[buttonPin]) {
    val[buttonPin] = newval;
    if (val[buttonPin] == 0) {
      unsigned long nt = millis();
      if (nt > t[buttonPin] + 200) {
        t[buttonPin] = nt;
        Serial.print("Button pressed: [");
        Serial.print(espToNodePin(buttonPin));
        Serial.print("/");
        Serial.print(buttonPin);
        Serial.print(" @ ");
        printLocalTime();
        Serial.println("] Press");
        relaySet(relayPin, digitalRead(relayPin) == HIGH ? LOW : HIGH, id);
      }
    }
  }
}

void buttonsLoop() {
  buttonLoop(D5, D1, "switch1");
  buttonLoop(D6, D2, "switch2");
  buttonLoop(D7, D3, "switch3");
}

void relaySetup() {
  Serial.println("Initialising relays");
  pinMode(D1, OUTPUT);
  pinMode(D2, OUTPUT);
  pinMode(D3, OUTPUT);
  digitalWrite(D1, LOW);
  digitalWrite(D2, LOW);
  digitalWrite(D3, LOW);
}

boolean relaySet(const uint8_t pin, const uint8_t value, const char* id, boolean sendMqttStatus) {
  if (digitalRead(pin) != value) {
    Serial.print("Switch relay ");
    Serial.print(id);
    Serial.print(" (");
    Serial.print(espToNodePin(pin));
    Serial.print("/");
    Serial.print(pin);
    Serial.print(") ");
    Serial.println(stateToJsonString(value));
    digitalWrite(pin, value);
		if (sendMqttStatus) mqttSendStatus();
		return true;
  } else {
		return false;
	}
}

boolean relaySet(const uint8_t pin, const uint8_t value, const char* id) {
	return relaySet(pin, value, id, true);
}

boolean relaySetJson(JsonObject& root, const char* id, const uint8_t pin) {
  if (root.containsKey(id)) {
    const char* state = root[id];
    return relaySet(pin, strcmp(state, "on") == 0 ? HIGH : LOW, id, false);
  }
}

void relaySetJson(JsonObject& root) {
	if (
		relaySetJson(root, "relay1", D1) ||
		relaySetJson(root, "relay2", D2) ||
		relaySetJson(root, "relay3", D3)
	) {
		mqttSendStatus();
	}
}

void mqttReceive(char* topic, byte* payload, unsigned int length) {
  Serial.print("Received MQTT message: [");
  Serial.print(topic);
  Serial.print(" @ ");
  printLocalTime();
  Serial.println("]");
  char json[length];
  for (int i=0;i<length;i++) {
    json[i] = (char)payload[i];
  }
  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(json);
  if (!root.success()) {
    Serial.println("Message does not seem to be JSON");
    return;
  }
  relaySetJson(root);
}

void mqttSendStatus() {
  client.publish(mqttTopic, getStatus().c_str());
	mqttResetCountdown();
}

boolean mqttConnected() {
	return client.connected();
}

void mqttReconnect() {
	static unsigned long t = 0;
	static unsigned long w = 0;
	if (!client.connected()) {
		unsigned long nt = millis();
		if (nt >= t + w) {
			t = nt;
			Serial.print("Attempting MQTT connection...");
			if (client.connect(configGet(HOSTNAME).c_str())) {
				Serial.println(" connected");
				Serial.printf("Subscribing to %s\n", mqttTopic);
				client.subscribe(mqttTopic);
				w = 0;
			} else {
				Serial.print("failed, rc=");
				Serial.print(client.state());
				Serial.println(" try again in 10 seconds");
				w = 10000;
			}
		}
	}
}

void mqttDisconnect() {
	Serial.println("Disconnecting MQTT client");
	client.disconnect();
	while (client.connected()) {
		delay(100);
	}
}

void mqttSetup() {
	char * mqtt_server = new char[configGet(MQTT_SERVER).length() + 1];
	configGet(MQTT_SERVER).toCharArray(mqtt_server, configGet(MQTT_SERVER).length() + 1);
	client.setServer(mqtt_server, configGet(MQTT_PORT).toInt());
	client.setCallback(mqttReceive);
	sprintf(mqttTopic, configGet(MQTT_TOPIC).c_str());
}

void mqttResetCountdown() {
	previousMillis = millis();
}

void mqttListenerLoop() {
	if (WiFi.getMode() != WIFI_AP && configGet(MQTT_SERVER).length() > 0) {
		if (!mqttConnected()) {
			mqttReconnect();
		} else {
			client.loop();
		}
	}
}

void mqttStatusLoop() {
	if (WiFi.getMode() != WIFI_AP && configGet(MQTT_SERVER).length() > 0) {
		if (!mqttConnected()) {
			mqttReconnect();
		} else {
			unsigned long currentMillis = millis();
			if(currentMillis - previousMillis >= 5000) {
				mqttSendStatus();
			}
		}
	}
}

void webServerLogRequest(String status) {
	Serial.print("HTTP request: [");
	Serial.print(server.uri());
	Serial.print(" @ ");
	printLocalTime();
	Serial.print("] - ");
	Serial.println(status);
}

bool webserverFromSpiffs(String path) {
	String dataType = "text/plain";
	if(path.endsWith("/")) path += "index.html";
	if(path.equals("/index.html") && WiFi.getMode() == WIFI_STA) path = "/index_sta.html";
	if(path.equals("/index.html") && WiFi.getMode() == WIFI_AP) path = "/index_ap.html";
	if(path.endsWith(".src")) path = path.substring(0, path.lastIndexOf("."));
	else if(path.endsWith(".html")) dataType = "text/html";
	else if(path.endsWith(".htm")) dataType = "text/html";
	else if(path.endsWith(".css")) dataType = "text/css";
	else if(path.endsWith(".js")) dataType = "application/javascript";
	else if(path.endsWith(".png")) dataType = "image/png";
	else if(path.endsWith(".gif")) dataType = "image/gif";
	else if(path.endsWith(".jpg")) dataType = "image/jpeg";
	else if(path.endsWith(".ico")) dataType = "image/x-icon";
	else if(path.endsWith(".xml")) dataType = "text/xml";
	else if(path.endsWith(".pdf")) dataType = "application/pdf";
	else if(path.endsWith(".zip")) dataType = "application/zip";
	if (path.equals("/resources/sap-ui-core.js")) path = "/ui5/sap-ui-core.js";
	else if (path.equals("/resources/sap-ui-version.json")) path = "/ui5/sap-ui-version.json";
	else if (path.equals("/resources/sap/ui/thirdparty/jquery-mobile-custom.js")) path = "/ui5/jquery-mobile-custom.js";
	else if (path.equals("/resources/sap/ui/core/library-preload.json")) path = "/ui5/c/library-preload.json";
	else if (path.equals("/resources/sap/ui/core/themes/sap_bluecrystal/library.css")) path = "/ui5/c/library.css";
	else if (path.equals("/resources/sap/ui/core/themes/sap_bluecrystal/library-parameters.json")) path = "/ui5/c/library-parameters.json";
	else if (path.equals("/resources/sap/m/library-preload.json")) path = "/ui5/m/library-preload.json";
	else if (path.equals("/resources/sap/m/themes/sap_bluecrystal/library.css")) path = "/ui5/m/library.css";
	else if (path.equals("/resources/sap/m/themes/sap_bluecrystal/library-parameters.json")) path = "/ui5/m/library-parameters.json";
	else if (path.equals("/resources/sap/ui/layout/library-preload.json")) path = "/ui5/l/library-preload.json";
	else if (path.equals("/resources/sap/ui/layout/themes/sap_bluecrystal/library.css")) path = "/ui5/l/library.css";
	else if (path.equals("/resources/sap/ui/layout/themes/sap_bluecrystal/library-parameters.json")) path = "/ui5/l/library-parameters.json";
	else if (path.equals("/resources/sap/ui/layout/library.js")) path = "/ui5/l/library.js";
	else if (path.equals("/resources/sap/ui/core/themes/base/fonts/SAP-icons.ttf")) path = "/ui5/c/SAP-icons.ttf";
	else path = "/www" + path;
	File dataFile = SPIFFS.open(path.c_str(), "r");
	if (dataFile) {
		if (server.hasArg("download")) dataType = "application/octet-stream";
		if (server.streamFile(dataFile, dataType) != dataFile.size()) {}
		dataFile.close();
		return true;
	}
	return false;
}

void webserverHandleStatus() {
	webServerLogRequest("OK");
	if (server.method() == HTTP_POST || server.method() == HTTP_PUT) {
		DynamicJsonBuffer jsonBuffer;
		JsonObject& root = jsonBuffer.parseObject(String(server.arg("plain")));
		if (!root.success()) {
			server.send(400, "text/plain", "Message does not seem to be JSON");
			return;
		} else {
			relaySetJson(root);
	    }
	}
	server.send(200, "application/json", getStatus());
}

void webserverHandleConfig(){
	webServerLogRequest("OK");
	if (server.method() == HTTP_POST || server.method() == HTTP_PUT) {
		DynamicJsonBuffer jsonBuffer;
		JsonObject& root = jsonBuffer.parseObject(String(server.arg("plain")));
		if (!root.success()) {
			server.send(400, "text/plain", "Message does not seem to be JSON");
		} else {
			if (root.containsKey(WIFI_SSID)) { configPut(WIFI_SSID, root[WIFI_SSID]); }
			if (root.containsKey(WIFI_SSID)) { configPut(WIFI_SSID, root[WIFI_SSID]); }
			if (root.containsKey(WIFI_PSK) && strlen(root[WIFI_PSK]) > 0) { configPut(WIFI_PSK, root[WIFI_PSK]); }
			if (root.containsKey(HOSTNAME)) { configPut(HOSTNAME, root[HOSTNAME]); }
			if (root.containsKey(MQTT_SERVER)) { configPut(MQTT_SERVER, root[MQTT_SERVER]); }
			if (root.containsKey(MQTT_PORT)) { configPut(MQTT_PORT, root[MQTT_PORT]); }
			if (root.containsKey(MQTT_TOPIC)) { configPut(MQTT_TOPIC, root[MQTT_TOPIC]); }
			if (root.containsKey(FRIENDLYNAME)) { configPut(FRIENDLYNAME, root[FRIENDLYNAME]); }
			server.send(200, "application/json", getConfig());
			configSave();
			ESP.restart();
		}
	} else {
		server.send(200, "application/json", getConfig());
	}
}

void webserverHandleNotFound(){
	Serial.print("HTTP request: [");
	Serial.print(server.uri());
	Serial.print(" @ ");
	printLocalTime();
	Serial.print("] - ");
	if (webserverFromSpiffs(server.uri()))
	{
		Serial.println("ok");
	} else {
		Serial.println("not found");
		String message = "Page Not found\n\n";
		message += "URI: ";
		message += server.uri();
		message += "\nMethod: ";
		message += (server.method() == HTTP_GET) ? "GET" : "POST";
		message += "\nArguments: ";
		message += server.args();
		message += "\n";
		for (uint8_t i=0; i<server.args(); i++){
			message += " NAME:"+server.argName(i) + "\n VALUE:" + server.arg(i) + "\n";
		}
		server.send(404, "text/plain", message);
	}
}

void webserverSetup() {
	Serial.println("Setting up http server");
	server.on("/data/config", webserverHandleConfig);
	server.on("/data/status", webserverHandleStatus);
	server.onNotFound(webserverHandleNotFound);
	server.begin();
}

void webserverLoop() {
	server.handleClient();
}

void mdnsSetup() {
	if (MDNS.begin(configGet(HOSTNAME).c_str())) {
		MDNS.addService("http", "tcp", 80);
		Serial.println("mDNS responder started");
	} else {
		Serial.println("Error setting up MDNS responder!");
	}
}

void motd() {
	Serial.println("\n\rBooting");
	Serial.println("------------------------------------");
	Serial.printf("SDK Version: %s\n", ESP.getSdkVersion());
	Serial.printf("Speed: %u Mhz\n", ESP.getCpuFreqMHz());
	Serial.printf("Chip ID: %08x\n", ESP.getChipId());
	Serial.printf("Flash chip size: %u KBytes\n", ESP.getFlashChipRealSize() / 1024);
	Serial.printf("Free heap: %u KBytes\n", ESP.getFreeHeap() / 1024);
	Serial.println("------------------------------------");
}

void setup() {
	system_update_cpu_freq(80);
	Serial.begin(115200);
	motd();
	configSetup();
	relaySetup();
	buttonsSetup();
	dhtSetup();
	networkSetup();
	timeSetup();
	firmwareSetup();
	mqttSetup();
	webserverSetup();
	mdnsSetup();
	Serial.println("------------------------------------");
}

void loop() {
	firmwareLoop();
	mqttListenerLoop();
	mqttStatusLoop();
	buttonsLoop();
	webserverLoop();
}