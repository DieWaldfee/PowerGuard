//https://beelogger.de/sensoren/temperatursensor-ds18b20/ für Pinning und Anregung
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>
#include "secrets.h"

#define LED_ERROR 23
#define LED_MSG 22
#define LED_OK 19
#define ONE_WIRE_BUS 25
static byte debug = 0;
static String lastError = "";

//Sicherheitsfunktionen
int volatile hardwareError = 0; // Indikator, ob ein Sensordefekt erkannt wurde.
int volatile thermalLimit = 0;  // Indikator, das die maximale Temperatur am Top-Sensor. Phasen werden abgeschalten.
int volatile panicMode = 0;     // Indikator für die Zwangsabschaltung - ab jetzt wird nichts mehr zugeschaltet

//Schaltausgaenge für Phase 1-3 und Luefter
#define PHASE_5V 32             // Steuerpin für Phase 5V Versorgung zum ESP32 Heizstabsteuerung
#define PHASE_12V 18            // Steuerpin für Phase 5V Versorgung zum ESP32 Heizstabsteuerung

// Definition der Zugangsdaten WiFi
WiFiClient myWiFiClient;

//Definition der Zugangsdaten MQTT
#define MQTT_CLIENTID "ESP32_PowerGuard" //Name muss eineindeutig auf dem MQTT-Broker sein!
#define MQTT_KEEPALIVE 90
#define MQTT_SOCKETTIMEOUT 30
#define MQTT_SERIAL_PUBLISH_STATUS "SmartHome/Keller/Heizung/ESP32_PowerGuard/status"
#define MQTT_SERIAL_RECEIVER_COMMAND "SmartHome/Keller/Heizung/ESP32_PowerGuard/command"
#define MQTT_SERIAL_PUBLISH_DS18B20 "SmartHome/Keller/Heizung/ESP32_PowerGuard/Temperatur/"
#define MQTT_SERIAL_PUBLISH_STATE "SmartHome/Keller/Heizung/ESP32_PowerGuard/state/"
#define MQTT_SERIAL_PUBLISH_CONFIG "SmartHome/Keller/Heizung/ESP32_PowerGuard/config/"
#define MQTT_SERIAL_PUBLISH_BASIS "SmartHome/Keller/Heizung/ESP32_PowerGuard/"
DeviceAddress myDS18B20Address;
String Adresse;
unsigned long MQTTReconnect = 0;
#define MQTT_QUEUEDEPTH 50                // Tiefe der MQTT-Queue - 50 Botschaften
#define MQTT_QUEUEMAXWAITTIME 3           // Wartezeit für das Senden in eine Queue - danach Error!
#define QUEUEMAXWAITTIME 3                // Wartezeit für das Senden in eine Queue - danach Error!
struct MqttJob {                          // Struktur der MQTT-Queue
  char topic[128];                        // topic:   Topic auf den die Botschaft gesendet werden soll -> 180 Zeichen lang
  char payload[256];                      // payload: Botschaft, die an das Topic gesendet werden soll. -> 256 Zeichen max.
  bool retain;                            // retain:  true, wenn die Botschaft im Broker gespeichert bleibt und false, wenn
};                                        // nur die angemeldeten User die Botschaft erhalten - diese dann vergessen wird.
PubSubClient mqttClient(myWiFiClient);
static QueueHandle_t mqttQueue;           // Queuedefinition für die MQTT-Queue
static TaskHandle_t hmqtt;                // handler für den MQTT-Sender-Task

// Anzahl der angeschlossenen DS18B20 - Sensoren
int DS18B20_Count = 0;            //Anzahl der erkannten DS18B20-Sensoren
//Sensorsetting (Ausgabe im Debugmodus (debug = 3) auf dem serial Monitor)
float volatile temp1 = 0.0;       //Sensor in Slot 2
float volatile temp2 = 0.0;       //Sensor in Slot 3
float tempLimit = 90.0;           //Ab dieser Temperatur wir 12V abgeschalten und thermalLimit = 1
float tempReconnect = 80.0;       //Ab dieser Temperatur thermalLimit = 0 und panicMode = 0 zurückgesetzt -> ESP32 Heizstabsteuerung boot neu nach PanicMode / bei thermalLimit wird kurz 5V abgeschaltet und der Neustart erzwungen..
float tempMaxLimit = 95.0;        //Panik-Abschaltung ab dieser Temperatur = 5V und 12V abschalten und thermalLimit = 1 & panicMode = 1
float tempHysterese = 2.0;        //bei Unterschreitung von (tmpLimit-tempHysterese)  
float deltaT = 2.0;               //Limit des Betrags von Differenz zwischen tempTop1 tempTop2 (|tempTop1-tempTop2|)
float minTemp = 10.0;             //untere Plausibilitätsgrenze für Temperatursignale. Bei Unterschreitung => Notabschaltung, da ggf. Sensor defekt
float maxTemp = 100.0;            //obere Plausibilitätsgrenze für Temperatursignale. Bei Überschreitung => Notabschaltung, da ggf. Sensor defekt
int volatile tempTSensorFail = 0; //Fehlercounter zur Temperaturmessung - Resilienz gegen gelegentliche Fehlauswertungen der Temperatursensoren
int maxTSensorFail = 3;           //maximal zulässige, hinereinander folgende Sensorfehler - danach panicStop
float DS18B20_minValue = -55.0;   //unterster Messwert im Messbereich [°C]
float DS18B20_maxValue = 125.0;   //unterster Messwert im Messbereich [°C]

//Initialisiere OneWire und Thermosensor(en)
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature myDS18B20(&oneWire);

//Mutexdefinitionen
static SemaphoreHandle_t mutexTemp;
static SemaphoreHandle_t mutexTempSensor;
static SemaphoreHandle_t mutexStatus;

//TaskHandler zur Verwendung mit ESP watchdog
static TaskHandle_t htempSensor;
static TaskHandle_t hMQTTwatchdog;

//PreDefinition Funktionen
void panicStop(void);
void thermalStop(void);
void Heizstab_reboot(void);

//erforderliche Funtions-Prototypen
bool mqttPublishQueue(const char*, const char*, bool);

//-------------------------------------
// Basisfunktion zum sicheren Reset
void safeReset() {
  // MQTT disconnecten
  Serial.println("MQTT Disconnect...");
  if (mqttClient.connected()) mqttClient.disconnect();
  delay(50);
  // TCP-Puffer flushen - alle Pakete aus dem Speicher
  Serial.println("Flush TCP-Buffer...");
  myWiFiClient.flush();
  delay(50);
  // restart durchführen
  Serial.println("ESP32 Reset!");
  ESP.restart();
}

//-------------------------------------
// Callback für MQTT
void mqttCallback(char* topic, byte* message, unsigned int length) {
  String str;
  unsigned long mqttValue;
  String mqttMessage;
  String mqttTopicAC;
  byte tx_ac = 1;
  for (int i = 0; i < length; i++)
  {
    str += (char)message[i];
  }
  if (debug > 1) {
    Serial.print("Nachricht aus dem Topic: ");
    Serial.print(topic);
    Serial.print(". Nachricht: ");
    Serial.println(str);
  }
  //Test-Botschaften  
  mqttTopicAC = MQTT_SERIAL_PUBLISH_BASIS;
  mqttTopicAC += "ac";
  if (str.startsWith("Test")) {
    if (debug) Serial.println("Test -> Test OK");
    mqttPublishQueue(mqttTopicAC.c_str(), "Test OK",false);
    tx_ac = 0;
  }
  //debug-Modfikation  
  if ((tx_ac) && (str.startsWith("debug=0"))) {
    debug = 0;
    mqttPublishQueue(mqttTopicAC.c_str(), "debug=0 umgesetzt",false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("debug=1"))) {
    debug = 1;
    mqttPublishQueue(mqttTopicAC.c_str(), "debug=1 umgesetzt",false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("debug=2"))) {
    debug = 2;
    mqttPublishQueue(mqttTopicAC.c_str(), "debug=2 umgesetzt",false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("debug=3"))) {
    debug = 3;
    mqttPublishQueue(mqttTopicAC.c_str(), "debug=3 umgesetzt",false);
    tx_ac = 0;
  }
  //panicMode-Modifikation
  if ((tx_ac) && (str.startsWith("panicMode=0"))) {
    panicMode = 0;
    if (thermalLimit == 0) Heizstab_reboot();
    mqttPublishQueue(mqttTopicAC.c_str(), "panicMode=0 umgesetzt",false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("panicMode=1"))) {
    panicMode = 1;
    mqttPublishQueue(mqttTopicAC.c_str(), "panicMode=1 umgesetzt",false);
    panicStop();
    tx_ac = 0;
  }
  //hardwareError-Modifikation
  if ((tx_ac) && (str.startsWith("hardwareError=0"))) {
    hardwareError = 0;
    mqttPublishQueue(mqttTopicAC.c_str(), "hardwareError=0 umgesetzt",false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("hardwareError=1"))) {
    hardwareError = 1;
    mqttPublishQueue(mqttTopicAC.c_str(), "thermalError=1 umgesetzt",false);
    panicStop();
    tx_ac = 0;
  }
  //thermalLimit-Modifikation
  if ((tx_ac) && (str.startsWith("thermalLimit=0"))) {
    if (thermalLimit == 1) {
      if ((panicMode == 0) && (hardwareError == 0)){
        thermalLimit = 0;
        Heizstab_reboot();
      } else {
        thermalLimit = 0;
      }
    }
    mqttPublishQueue(mqttTopicAC.c_str(), "thermalLimit=0 umgesetzt",false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("thermalLimit=1"))) {
    thermalLimit = 1;
    thermalStop();
    mqttPublishQueue(mqttTopicAC.c_str(), "thermalLimit=1 umgesetzt",false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("tempLimit="))) {
    str.remove(0,10);
    tempLimit=str.toFloat();
    mqttMessage = "tempLimit=";
    mqttMessage += String(tempLimit);
    mqttMessage += " umgesetzt";
    if (debug > 2) Serial.println(mqttMessage);
    mqttPublishQueue(mqttTopicAC.c_str(), mqttMessage.c_str(),false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("deltaT="))) {
    str.remove(0,7);
    deltaT=str.toFloat();
    mqttMessage = "deltaT=";
    mqttMessage += String(deltaT);
    mqttMessage += " umgesetzt";
    if (debug > 2) Serial.println(mqttMessage);
    mqttPublishQueue(mqttTopicAC.c_str(), mqttMessage.c_str(),false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("minTemp="))) {
    str.remove(0,8);
    minTemp=str.toFloat();
    mqttMessage = "minTemp=";
    mqttMessage += String(minTemp);
    mqttMessage += " umgesetzt";
    if (debug > 2) Serial.println(mqttMessage);
    mqttPublishQueue(mqttTopicAC.c_str(), mqttMessage.c_str(),false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("maxTemp="))) {
    str.remove(0,8);
    maxTemp=str.toFloat();
    mqttMessage = "maxTemp=";
    mqttMessage += String(maxTemp);
    mqttMessage += " umgesetzt";
    if (debug > 2) Serial.println(mqttMessage);
    mqttPublishQueue(mqttTopicAC.c_str(), mqttMessage.c_str(),false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("tempReconnect="))) {
    str.remove(0,14);
    tempReconnect=str.toFloat();
    mqttMessage = "tempReconnect=";
    mqttMessage += String(tempReconnect);
    mqttMessage += " umgesetzt";
    if (debug > 2) Serial.println(mqttMessage);
    mqttPublishQueue(mqttTopicAC.c_str(), mqttMessage.c_str(),false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("tempMaxLimit="))) {
    str.remove(0,14);
    tempMaxLimit=str.toFloat();
    mqttMessage = "tempMaxLimit=";
    mqttMessage += String(tempMaxLimit);
    mqttMessage += " umgesetzt";
    if (debug > 2) Serial.println(mqttMessage);
    mqttPublishQueue(mqttTopicAC.c_str(), mqttMessage.c_str(),false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("tempHysterese="))) {
    str.remove(0,14);
    tempHysterese=str.toFloat();
    mqttMessage = "tempHysterese=";
    mqttMessage += String(tempHysterese);
    mqttMessage += " umgesetzt";
    if (debug > 2) Serial.println(mqttMessage);
    mqttPublishQueue(mqttTopicAC.c_str(), mqttMessage.c_str(),false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("restart"))) {
    mqttPublishQueue(mqttTopicAC.c_str(), "reboot in einer Sekunde!",false);
    if (debug) Serial.println("für Restart: alles aus & restart in 1s!");
    digitalWrite(PHASE_5V, HIGH);
    digitalWrite(PHASE_12V, HIGH);
    digitalWrite(LED_OK, LOW);
    digitalWrite(LED_ERROR, HIGH);
    vTaskDelay(1000);
    if (debug) Serial.println("führe Restart aus!");
    safeReset();
  }
  if ((tx_ac) && (str.startsWith("reboot"))) {
    mqttPublishQueue(mqttTopicAC.c_str(), "reboot in einer Sekunde!",false);
    if (debug) Serial.println("für Restart: alles aus & restart in 1s!");
    digitalWrite(PHASE_5V, HIGH);
    digitalWrite(PHASE_12V, HIGH);
    digitalWrite(LED_OK, LOW);
    digitalWrite(LED_ERROR, HIGH);
    vTaskDelay(1000);
    if (debug) Serial.println("führe Restart aus!");
    safeReset();
  }
}

//-------------------------------------
//Subfunktionen für MQTT-Status-Task
// MQTT DS18B20 Status senden
void printDS18B20MQTT() {
  String mqttTopic;
  String mqttJson;
  String mqttPayload;
  for (int i = 0; i < DS18B20_Count; i++) {
    //MQTT-Botschaften
    //JSON        
    myDS18B20.getAddress(myDS18B20Address,i);
    Adresse="";
    for (uint8_t j = 0; j < 8; j++)
    {
      Adresse += "0x";
      if (myDS18B20Address[j] < 0x10) Adresse += "0";
      Adresse += String(myDS18B20Address[j], HEX);
      if (j < 7) Adresse += ", ";
    }
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/JSON"; 
    mqttJson = "{\"ID\":\"" + String(i) + "\"";
    mqttJson += ",\"Temperatur\":\"" + String(myDS18B20.getTempCByIndex(i)) + "\"";
    mqttJson += ",\"Adresse\":\"(" + Adresse + ")\"";
    mqttJson += ",\"Ort\":\"Temperatur Sensor " + String(i) + "\"}";
    if (debug > 2) Serial.println("MQTT_JSON: " + mqttJson);
    mqttPublishQueue(mqttTopic.c_str(), mqttJson.c_str(),false);
    //Temperatur
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/Temperatur";
    mqttPayload = String(myDS18B20.getTempCByIndex(i));
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
    if (debug > 2) Serial.print("MQTT ID: ");
    if (debug > 2) Serial.println(mqttPayload);
    //ID
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/ID";
    mqttPayload = String(i);
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
    if (debug > 2) Serial.print("MQTT Temperatur: ");
    if (debug > 2) Serial.println(mqttPayload);
    //Adresse
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/Adresse";
    mqttPublishQueue(mqttTopic.c_str(), Adresse.c_str(),false);
    if (debug > 2) Serial.print("MQTT Adresse: ");
    if (debug > 2) Serial.println(Adresse);
    //Ort
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/Ort";
    mqttPayload = "Temperatur Sensor " + String(i);
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
    if (debug > 2) Serial.print("MQTT Ort: ");
    if (debug > 2) Serial.println(mqttPayload);
  }
}
// MQTT Status Betrieb senden
void printStateMQTT() {
  String mqttTopic;
  String mqttJson;
  String mqttPayload;
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "JSON";
  mqttJson = "{\"panicMode\":\"" + String(panicMode) + "\"";
  mqttJson += ",\"thermalLimit\":\"" + String(thermalLimit) + "\"";
  mqttJson += ",\"hardwareError\":\"" + String(hardwareError) + "\"";
  mqttJson += ",\"lastError\":\"" + String(lastError) + "\"";
  mqttJson += ",\"WiFi_Signal_Strength\":\"" + String(WiFi.RSSI()) + "\"";
  mqttJson += ",\"WiFi_IP_Adress\":\"" + WiFi.localIP().toString() + "\"";
  mqttJson += ",\"WiFi_MAC_Adress\":\"" + WiFi.macAddress() + "\"}";
  if (debug > 2) Serial.println("MQTT_JSON: " + mqttJson);
  mqttPublishQueue(mqttTopic.c_str(), mqttJson.c_str(),false);
  //panicMode
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "panicmode";
  mqttPayload = String(panicMode);
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
  if (debug > 2) Serial.print("MQTT panicmode: ");
  if (debug > 2) Serial.println(mqttPayload);
  //thermalLimit
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "thermalLimit";
  mqttPayload = String(thermalLimit);
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
  if (debug > 2) Serial.print("MQTT thermalLimit: ");
  if (debug > 2) Serial.println(mqttPayload);
  //Hardware Error
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "hardwareError";
  mqttPayload = String(hardwareError);
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
  if (debug > 2) Serial.print("MQTT hardwareError: ");
  if (debug > 2) Serial.println(mqttPayload);
  //lastError
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "lastError";
  mqttPayload = String(lastError);
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
  if (debug > 2) Serial.print("LastError: ");
  if (debug > 2) Serial.println(mqttPayload);
  //WiFi Signalstärke
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "WiFi_Signal_Strength";
  mqttPayload = WiFi.RSSI();
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
  if (debug > 2) Serial.print("WiFi Signalstärke: ");
  if (debug > 2) Serial.println(mqttPayload);
  //WiFi IP-Adresse
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "WiFi_IP_Adress";
  mqttPayload = WiFi.localIP().toString();
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
  if (debug > 2) Serial.print("WiFi IP-Adresse: ");
  if (debug > 2) Serial.println(mqttPayload);
  //WiFi MAC-Adresse
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "WiFi_MAC_Adress";
  mqttPayload = WiFi.macAddress();
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
  if (debug > 2) Serial.print("WiFi MAC-Adresse: ");
  if (debug > 2) Serial.println(mqttPayload);
}
// MQTT Config und Parameter senden
void printConfigMQTT() {
  String mqttTopic;
  String mqttJson;
  String mqttPayload;
  //Teil 1
  mqttTopic = MQTT_SERIAL_PUBLISH_CONFIG;
  mqttTopic += "JSON_0";
  mqttJson = "{\"tempLimit\":\"" + String(tempLimit) + "\"";
  mqttJson += ",\"tempMaxLimit\":\"" + String(tempMaxLimit) + "\"";
  mqttJson += ",\"tempHysterese\":\"" + String(tempHysterese) + "\"";
  mqttJson += ",\"tempReconnect\":\"" + String(tempReconnect) + "\"";
  mqttJson += ",\"deltaT\":\"" + String(deltaT) + "\"";
  mqttJson += ",\"minTemp\":\"" + String(minTemp) + "\"";
  mqttJson += ",\"maxTemp\":\"" + String(maxTemp) + "\"";
  mqttJson += ",\"thermalLimit\":\"" + String(thermalLimit) + "\"}";
  if (debug > 2) Serial.println("MQTT_JSON: " + mqttJson);
  mqttPublishQueue(mqttTopic.c_str(), mqttJson.c_str(),false);
}
//LED-Blik-OK
void LEDblinkOK(){
  digitalWrite(LED_MSG, HIGH);
  delay(150);
  digitalWrite(LED_MSG, LOW);
}
//-------------------------------------
//MQTT-Status-Task
static void MQTTstate (void *args){
  BaseType_t rc;
  TickType_t ticktime;

  //ticktime initialisieren
  ticktime = xTaskGetTickCount();

  for (;;){                        // Dauerschleife des Tasks
    //Lesen der Temperaturen
    if (debug > 1) Serial.print("TickTime: ");
    if (debug > 1) Serial.print(ticktime);
    if (debug > 1) Serial.println(" | MQTT-Status-Task gestartet");
    if (mqttClient.connected()) {
      rc = xSemaphoreTake(mutexTempSensor, portMAX_DELAY);
      assert(rc == pdPASS);
        printDS18B20MQTT();
      rc = xSemaphoreGive(mutexTempSensor);
      assert(rc == pdPASS);

      rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
      assert(rc == pdPASS);
        printStateMQTT();
      rc = xSemaphoreGive(mutexStatus);
      assert(rc == pdPASS);

      printConfigMQTT();
    }

    // Task schlafen legen - restart alle 60s = 60*1000 ticks = 60000 ticks
    LEDblinkOK();
    vTaskDelayUntil(&ticktime, 60000);
  }
}

//-------------------------------------
//Subfunktionen für MQTTwatchdog-Task
// MQTT Verbindung herstellen (wird auch von setup verwendet!)
void mqttConnect () {
  int i = 0;
  Serial.print("Verbindungsaubfau zu MQTT Server ");
  Serial.print(MQTT_SERVER);
  Serial.print(" Port ");  
  Serial.print(MQTT_PORT);
  Serial.print(" wird aufgebaut ");  
  while (!mqttClient.connected()) {
    Serial.print(".");
    if (mqttClient.connect(MQTT_CLIENTID, MQTT_USER, MQTT_PASSWORD, MQTT_SERIAL_PUBLISH_STATUS, 0, true, "false")) {
      mqttClient.publish(MQTT_SERIAL_PUBLISH_STATUS, "true", true);
      Serial.println("");
      Serial.print("MQTT verbunden!");
    } 
    else {
      if (++i > 20) {
        Serial.println("MQTT scheint nicht mehr erreichbar! Reboot!!");
        safeReset();
      }
      Serial.print("fehlgeschlagen rc=");
      Serial.print(mqttClient.state());
      Serial.println(" erneuter Versuch in 5 Sekunden.");
      delay(5000);      
    }    
  }
  mqttClient.subscribe(MQTT_SERIAL_RECEIVER_COMMAND);
}
// MQTT Verbindungsprüfung 
void checkMQTTconnetion() {
  String mqttTopic;
  String mqttPayload;
  BaseType_t rc;
  if (!mqttClient.connected()) {
    if (debug) Serial.println("MQTT Server Verbindung verloren...");
    if (debug) Serial.print("Disconnect Errorcode: ");
    if (debug) Serial.println(mqttClient.state());  
    //Vorbereitung errorcode MQTT (https://pubsubclient.knolleary.net/api#state)
    mqttTopic = MQTT_SERIAL_PUBLISH_BASIS + String("error");
    mqttPayload = String(String(++MQTTReconnect) + ". reconnect: ") + String("; MQTT disconnect rc=" + String(mqttClient.state()));
    // 0	MQTT_CONNECTED	        Erfolgreich verbunden.
    // 1	MQTT_CONNECTION_TIMEOUT	Verbindung zum Broker hat zu lange gedauert (Timeout).
    // 2	MQTT_CONNECTION_LOST	  Verbindung ging verloren (nach dem Connect).
    // 3	MQTT_CONNECT_FAILED	    Verbindung konnte nicht hergestellt werden (Socket fehlerhaft).
    // 4	MQTT_DISCONNECTED	      Client ist aktuell nicht verbunden.
    // 5	MQTT_CONNECTED_FAILED	  Broker hat die Verbindung abgelehnt (z. B. Authentifizierung)
    //reconnect
    mqttConnect();
    //sende Fehlerstatus
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
    //thermalLimits wieder einschalten
     rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
    assert(rc == pdPASS);
      thermalLimit = 0;
    rc = xSemaphoreGive(mutexStatus);
    assert(rc == pdPASS);
    //reconnect zurückmelden
    mqttTopic = MQTT_SERIAL_PUBLISH_BASIS + String("ac");
    mqttPayload = String("MQTT reconnect durchgeführt!");
    mqttPublishQueue(mqttTopic.c_str(),mqttPayload.c_str(),false);
  }
  mqttClient.loop();
}
//-------------------------------------
//MQTT-MQTTwatchdog-Task
static void MQTTwatchdog (void *args){
  BaseType_t rc;
  esp_err_t er;
  TickType_t ticktime;

  //ticktime initialisieren
  ticktime = xTaskGetTickCount();

  er = esp_task_wdt_add(NULL);   // Task zur Überwachung hinzugefügt  
  assert(er == ESP_OK); 

  for (;;){                        // Dauerschleife des Tasks
    // Watchdog zurücksetzen
    esp_task_wdt_reset();
    //Lesen der Temperaturen
    if (debug > 1) Serial.print("TickTime: ");
    if (debug > 1) Serial.print(ticktime);
    if (debug > 1) Serial.println(" | MQTTonlinePrüf-Task gestartet");
    checkMQTTconnetion();

    // Task schlafen legen - restart alle 2s = 2*1000 ticks = 2000 ticks
    // mit mqttClient.loop() wird auch der MQTTcallback ausgeführt!
    vTaskDelayUntil(&ticktime, 2000);
  }
}

//-------------------------------------
//MQTT-MQTTSender-Task
static void mqttSender (void *args){
  MqttJob job;
  BaseType_t rc;
  esp_err_t er;
  TickType_t ticktime;

  //ticktime initialisieren
  ticktime = xTaskGetTickCount();

  for (;;){                        // Dauerschleife des Tasks
    //Statusausgabe
    if (debug > 1) Serial.print("TickTime: ");
    if (debug > 1) Serial.print(ticktime);
    if (debug > 1) Serial.println(" | MQTT-Sender-Task gestartet");
    if (mqttClient.connected()) {
      // Sendebereit -> MQTT-Queue kann geleert werden
      while (xQueuePeek(mqttQueue, &job, 0) == pdPASS) {
        if (mqttClient.connected()) {
          rc = xQueueReceive(mqttQueue, &job, 0) == pdPASS;
          mqttClient.publish(job.topic, job.payload, job.retain);
          if (debug > 2) Serial.print("Topic: ");
          if (debug > 2) Serial.println(job.topic);
          if (debug > 2) Serial.print("Payload: ");
          if (debug > 2) Serial.println(job.payload);
          if (debug > 2) Serial.print("retain: ");
          if (debug > 2) Serial.println(job.retain);
          mqttClient.loop();  // Keepalive
        }
      }    
    }    
    // Task schlafen legen - restart alle 0.5s = 0.5*1000 ticks = 500 ticks
    // mit mqttClient.loop() wird auch der MQTTcallback ausgeführt!
    vTaskDelayUntil(&ticktime, 500);
  }
}
//MQTT-Queue befüllen
bool mqttPublishQueue(const char* topic, const char* payload, bool retain = false) {
  MqttJob job;
  strncpy(job.topic, topic, sizeof(job.topic) - 1);           // Absicherung gegen Buffer-Overflow
  strncpy(job.payload, payload, sizeof(job.payload) - 1);     // Absicherung gegen Buffer-Overflow
  job.retain = retain;
  return xQueueSend(mqttQueue, &job, QUEUEMAXWAITTIME) == pdPASS;
}

//-------------------------------------
//Subfunktionen für den TempSensor-Task
// Temperatursensorenwerte auf die Limits prüfen
bool checkDS18B20Value (float t){
  bool res = true;     // true = im Messbereich; false = außerhalb des Messbereichs
  if ((t < DS18B20_minValue) || (t > DS18B20_maxValue)){
    //Sensorwert außerhalb des Messbereichs
    res = false;
  }
  if (debug > 2) Serial.print("Prüfe t-Wert auf Gültigkeit: ");
  if (debug > 2) Serial.print(t);
  if (debug > 2) Serial.print("°C [");
  if (debug > 2) Serial.print(DS18B20_minValue);
  if (debug > 2) Serial.print(",");
  if (debug > 2) Serial.print(DS18B20_maxValue);
  if (debug > 2) Serial.print("]; Ergebnis: ");
  if (debug > 2) Serial.println(res);
  return res;
}
// Temperatursensoren auslesen
void readDS18B20() {
  String mqttTopic;
  String mqttPayload;
  float t1 = 0.0;
  float t2 = 0.0;
  bool res = false;
  if (debug > 2) Serial.print("Anfrage der Temperatursensoren... ");
  myDS18B20.requestTemperatures();  //Anfrage zum Auslesen der Temperaturen
  if (debug > 2) Serial.println("fertig");
  for (int i = 0; i < DS18B20_Count; i++) {
    myDS18B20.getAddress(myDS18B20Address,i);
    Adresse="";
    for (uint8_t j = 0; j < 8; j++)
    {
      Adresse += "0x";
      if (myDS18B20Address[j] < 0x10) Adresse += "0";
      Adresse += String(myDS18B20Address[j], HEX);
      if (j < 7) Adresse += ", ";
    }
    if (i == 0) t1 = myDS18B20.getTempCByIndex(i);
    if (i == 1) t2 = myDS18B20.getTempCByIndex(i);
  }
  //Plausibilitätscheck
  if (checkDS18B20Value(t1)) {
    temp1 = t1;
    res = true;
  }
  else {
    ++tempTSensorFail;
    res = false;
    mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
    mqttTopic += "lastError";
    mqttPayload = "Temperatursensor T1 außerhalb des Messbereichts: " + String(t1) + "[C]; Wiederholung: " + String(tempTSensorFail);
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
    if (debug > 2) Serial.print("LastError: ");
    if (debug > 2) Serial.println(mqttPayload); //(debug > 2)
  }
  if (checkDS18B20Value(t2)) {
    temp2 = t2;
    if (res) tempTSensorFail = 0;  //t1 und t2 sind korrekt => Fehlercounter auf 0 gesetzt
  }
  else {
    ++tempTSensorFail;
    res = false;
    mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
    mqttTopic += "lastError";
    mqttPayload = "Temperatursensor T2 außerhalb des Messbereichts: " + String(t2) + "[C]; Wiederholung: " + String(tempTSensorFail);
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
    if (debug > 2) Serial.print("LastError: ");
    if (debug > 2) Serial.println(mqttPayload);
  }
  if (tempTSensorFail > maxTSensorFail) {
    Serial.println("zu viele Fehler (out of range) beim Auslesen der DS18B20! Reboot!!");
    safeReset();
  }
}
//Thermale Limits prüfen und ggf. reagieren
void termalLimits () {
  BaseType_t rc;
  //Plausibilitätscheck tempTop1 und tempTop2
  if ((abs(temp1 - temp2)) > deltaT) {
    //ggf. ist ein Sensor defekt, da die Temperaturen bei Top1 und Top2 sich unterscheiden
    if (hardwareError == 0) {
      if (debug) Serial.print("Zwangsabschaltung wegen Unterschied zwischen Sensor 1 und 2! (");
      if (debug) Serial.print(temp1);
      if (debug) Serial.print("°C am Top-Sensor #1 bzw. ");
      if (debug) Serial.print(temp2);
      if (debug) Serial.println("°C am Top-Sensor #2)");
      //Temperatursensoren liefern unplausible Werte gegeneinander -> Defekt!
      hardwareError = 1;
      lastError = "Zwangsabschaltung: unterschiedliche Sensorwerte (" + String(temp1) + "; " + String(temp2) + ")";
      panicStop();
    }
  }
  if ((temp1 < minTemp) || (temp1 > maxTemp)){
    if (hardwareError == 0) {
      if (debug) Serial.print("Zwangsabschaltung wegen einer Verletzung der thermischen Grenzen! (");
      if (debug) Serial.print(temp1);
      if (debug) Serial.print("°C am Top-Sensor #1. Limits: ] ");
      if (debug) Serial.print(minTemp);
      if (debug) Serial.print("..");
      if (debug) Serial.print(maxTemp);
      if (debug) Serial.print("[");
      //Thermosensor 1 ist außerhalb des eingestellten Normbereichs -> Annahme Defekt!
      hardwareError = 1;
      lastError = "Zwangsabschaltung: Verletzung der thermischen Grenzen " + String(temp1) + " -> [" + String(minTemp) +  " ... " + String(maxTemp) + "]";
      panicStop();
    }
  }
  if ((temp2 < minTemp) || (temp2 > maxTemp)){
    if (hardwareError == 0) {
      if (debug) Serial.print("Zwangsabschaltung wegen einer Verletzung der thermischen Grenzen! (");
      if (debug) Serial.print(temp2);
      if (debug) Serial.print("°C am Top-Sensor #2. Limits: ] ");
      if (debug) Serial.print(minTemp);
      if (debug) Serial.print("..");
      if (debug) Serial.print(maxTemp);
      if (debug) Serial.print("[");
      //Thermosensor 2 ist außerhalb des eingestellten Normbereichs -> Annahme Defekt!
      hardwareError = 1;
      lastError = "Zwangsabschaltung: Verletzung der thermischen Grenzen " + String(temp2) + " -> [" + String(minTemp) +  " ... " + String(maxTemp) + "]";
      panicStop();
    }
  }
  // Prüfung auf ThermoLimit
  if ((temp1 >= tempLimit) || (temp2 >= tempLimit)) {
    if (thermalLimit == 0) {
      if (debug) Serial.print("Thermische Abschaltung durch ");
      if (debug) Serial.print(temp1);
      if (debug) Serial.print("°C am Top-Sensor #1 bzw. ");
      if (debug) Serial.print(temp2);
      if (debug) Serial.println("°C am Top-Sensor #2.");
      //thermalstop = 1 -> 12V wird abgeschalten, damit die Relais keine Versorgung mehr haben können. 
      lastError = "Thermal Stop: 12V abgeschaltet (" + String(temp1) + "; " + String(temp2) + ")";
      thermalStop();
    }
  }
  if ((temp1 >= tempMaxLimit) || (temp2 >= tempMaxLimit)) {
    if (panicMode == 0) {
      if (debug) Serial.print("Thermische Abschaltung durch ");
      if (debug) Serial.print(temp1);
      if (debug) Serial.print("°C am Top-Sensor #1 bzw. ");
      if (debug) Serial.print(temp2);
      if (debug) Serial.println("°C am Top-Sensor #2.");
      //thermalstop = 1 -> 12V wird abgeschalten, damit die Relais keine Versorgung mehr haben können. 
      //panicMode = 1 -> 5V des ESP32 zur Steuerung des Heizstabs wird abgeschalten - damit alle Spannungsversorgungen
      lastError = "Thermal Stop: 5V und 12V abgeschaltet (" + String(temp1) + "; " + String(temp2) + ")";
      panicStop();
    }
  }
  if ((thermalLimit == 1) && (panicMode == 1) && (hardwareError == 0)) {
    //Prüfung, ob die Temperatur unter TempReconnect gefallen ist - falls ja, Reboot ESP32 Heizstabsteuerung zur 5V ein
    if ((temp1 < tempReconnect) && (temp2 < tempReconnect)) {
      if (debug) Serial.print("Thermische Zuschalten nach PanicMode: ");
      if (debug) Serial.print(temp1);
      if (debug) Serial.print("°C am Top-Sensor #1 bzw. ");
      if (debug) Serial.print(temp2);
      if (debug) Serial.println("°C am Top-Sensor #2.");
      lastError = "zurück im Normalbereich der Temperatur: reset der Heizstabelektornik";
      Heizstab_reboot();
    }
  }
  if ((thermalLimit == 1) && (panicMode == 0) && (hardwareError == 0)) {
    //Prüfung, ob die Temperatur unter TempReconnect gefallen ist - falls ja Reboot erzwingen die kurzzeitiges Ausschalten 5V
    // ...es liegt kein detektierter Sensorfehler vor.
    if ((temp1 < tempReconnect) && (temp2 < tempReconnect)) {
      if (debug) Serial.print("Thermische Zuschalten nach ThermalLinit: ");
      if (debug) Serial.print(temp1);
      if (debug) Serial.print("°C am Top-Sensor #1 bzw. ");
      if (debug) Serial.print(temp2);
      if (debug) Serial.println("°C am Top-Sensor #2.");
      lastError = "zurück im Normalbereich der Temperatur: reset der Heizstabelektornik";
      Heizstab_reboot();
    }
  }
}
//Debug-Ausgabe der Temp-Sensorwerte
void printDS18B20() {
  if (debug > 2) {
    for (int i = 0; i < DS18B20_Count; i++) {
      //print to Serial
      Serial.print("DS18B20[");
      Serial.print(i);
      Serial.print("]: ");
      Serial.print(myDS18B20.getTempCByIndex(i));
      Serial.print(" *C (");
      myDS18B20.getAddress(myDS18B20Address,i);
      Adresse="";
      for (uint8_t j = 0; j < 8; j++)
      {
        Adresse += "0x";
        if (myDS18B20Address[j] < 0x10) Adresse += "0";
        Adresse += String(myDS18B20Address[j], HEX);
        if (j < 7) Adresse += ", ";
      }
      Serial.println(Adresse + ")");
    }
  }
}
//Panicabschaltung fullStop
void panicStop() {
  BaseType_t rc;
  //sofort alles abschalten
  digitalWrite(PHASE_5V, HIGH);
  digitalWrite(PHASE_12V, HIGH);
  rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
  assert(rc == pdPASS);
    panicMode = 1;
    thermalLimit = 1;  
  rc = xSemaphoreGive(mutexStatus);
  assert(rc == pdPASS);
  if (debug) Serial.println("Notabschaltung durchgeführt - 5V und 12V abgeschalten!");
  digitalWrite(LED_OK, LOW);
  digitalWrite(LED_ERROR, HIGH);
}
//Termale abschaltung
void thermalStop() {
  BaseType_t rc;
  //sofort 12V abschalten
  digitalWrite(PHASE_12V, HIGH);
  rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
  assert(rc == pdPASS);
    thermalLimit = 1; 
  rc = xSemaphoreGive(mutexStatus);
  assert(rc == pdPASS);
  if (debug) Serial.println("Thermale Abschaltung durchgeführt - 12V abgeschalten!");
  digitalWrite(LED_OK, LOW);
  digitalWrite(LED_ERROR, HIGH);
}
//Heizstab wird zum reboot gezwungen und 5V + 12V zugeschaltet
void Heizstab_reboot(){
  BaseType_t rc;
  //alles kurz abschalten
  digitalWrite(PHASE_5V, HIGH);
  digitalWrite(PHASE_12V, HIGH);
  delay(3000);
  digitalWrite(PHASE_5V, LOW);
  digitalWrite(PHASE_12V, LOW);
  digitalWrite(LED_OK, HIGH);
  digitalWrite(LED_ERROR, LOW);
  delay(250);
  digitalWrite(LED_OK, LOW);
  rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
  assert(rc == pdPASS);
    panicMode = 0;
    thermalLimit = 0;  
  rc = xSemaphoreGive(mutexStatus);
  assert(rc == pdPASS);
  if (debug) Serial.println("Abschaltung aufgehoben - 5V und 12V sind zugeschaltet! -> ESP32 Heizstabsteuerung bootet!");
}
//-------------------------------------
//Task zur Ermittlung der Temperaturen
static void getTempFromSensor (void *args){
  BaseType_t rc;
  esp_err_t er;
  TickType_t ticktime;

  //ticktime initialisieren
  ticktime = xTaskGetTickCount();

  er = esp_task_wdt_add(NULL);   // Task zur Überwachung hinzugefügt  
  assert(er == ESP_OK); 

  for (;;){                        // Dauerschleife des Tasks
    // Watchdog zurücksetzen
    esp_task_wdt_reset();
    //Lesen der Temperaturen
    if (debug > 1) Serial.print("TickTime: ");
    if (debug > 1) Serial.print(ticktime);
    if (debug > 1) Serial.println(" | TempSensor-Task liest DS18B20-Sensoren aus");
    rc = xSemaphoreTake(mutexTempSensor, portMAX_DELAY);
    assert(rc == pdPASS);
      rc = xSemaphoreTake(mutexTemp, portMAX_DELAY);
      assert(rc == pdPASS);
        readDS18B20();                // Sensoren auslesen und den Variablen zuordnen
        printDS18B20();               // DebugInfo auf Serial (thermale Infos)
        termalLimits();               // Sensorwerte prüfen und ggf. Fehlermaßnahemn einleiten
      rc = xSemaphoreGive(mutexTemp);
      assert(rc == pdPASS);
    rc = xSemaphoreGive(mutexTempSensor);
    assert(rc == pdPASS);

    // Task schlafen legen - restart alle 5s = 5*1000 ticks = 5000 ticks
    vTaskDelayUntil(&ticktime, 5000);
  }
}

void setup() {
  // Initialisierung und Plausibilitaetschecks
  Serial.begin(115200);
  while (!Serial)
  Serial.println("Start Setup");
  pinMode(LED_ERROR, OUTPUT);
  digitalWrite(LED_ERROR, LOW);
  pinMode(LED_MSG, OUTPUT);
  digitalWrite(LED_MSG, HIGH);
  pinMode(LED_OK, OUTPUT);
  digitalWrite(LED_OK, HIGH);
  //Initialisierung der Phasenschalter L1-3
  if (debug) Serial.println("Initialisierung der Phasenschalter.");
  pinMode(PHASE_5V, OUTPUT);
  pinMode(PHASE_12V, OUTPUT);
  digitalWrite(PHASE_5V, HIGH);     //fallende Flanke erforderlich
  digitalWrite(PHASE_12V, HIGH);    //fallende Flanke erforderlich
  delay(250);
  digitalWrite(PHASE_5V, LOW);     //angeschlossenes SolidStade Relais schaltet auf High
  digitalWrite(PHASE_12V, LOW);    //angeschlossenes SolidStade Relais schaltet auf High
 //WiFi-Setup
  int i = 0;
  Serial.print("Verbindungsaufbau zu ");
  Serial.print(ssid);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(ssid,password);
  while (WiFi.status() != WL_CONNECTED)
  {
    if (++i > 240) {
      // Reboot nach 2min der Fehlversuche
      Serial.println("WLAN scheint nicht mehr erreichbar! Reboot!!");
      ESP.restart();
    }
    delay(500);
    Serial.print(".");    
  }
  Serial.println("");
  Serial.println("WiFi verbunden.");
  Serial.print("IP Adresse: ");
  Serial.print(WiFi.localIP());
  Serial.println("");
  //MQTT-Setup
  String mqttTopic;
  String mqttPayload;
  Serial.println("MQTT Server Initialisierung laeuft...");
  mqttClient.setServer(MQTT_SERVER,MQTT_PORT); 
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(MQTT_KEEPALIVE);
  mqttClient.setSocketTimeout(MQTT_SOCKETTIMEOUT);
  mqttConnect();
  mqttTopic = MQTT_SERIAL_PUBLISH_BASIS + String("error");
  mqttPayload = String(String(MQTTReconnect) + ".: keine Fehler seit Reboot!");
  mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str());
  Serial.println("");
  //DS18B20-Setup
  Serial.println("Auslesen der DS18B20-Sensoren...");
  myDS18B20.begin();
  Serial.print("Anzahl gefundener 1-Wire-Geraete:  ");
  Serial.println(myDS18B20.getDeviceCount());
  DS18B20_Count = myDS18B20.getDS18Count();
  Serial.print("Anzahl gefundener DS18B20-Geraete: ");
  Serial.println(DS18B20_Count);
  if (DS18B20_Count < 2) {
    Serial.println("... Anzahl DB18B20 < 2 => zu wenig! ... System angehalten!");
    digitalWrite(LED_OK, LOW);
    while (true) {
      //blinke bis zur Unendlichkeit...
      digitalWrite(LED_ERROR, HIGH);
      delay(250);
      digitalWrite(LED_ERROR, LOW);
      delay(250);
    }
  }
  Serial.print("Globale Aufloesung (Bit):        ");
  Serial.println(myDS18B20.getResolution());
  //Mutex-Initialisierung
  mutexTemp = xSemaphoreCreateMutex();
  assert(mutexTemp);
  mutexTempSensor = xSemaphoreCreateMutex();
  assert(mutexTempSensor);
  mutexStatus = xSemaphoreCreateMutex();
  assert(mutexStatus);
  Serial.println("Mutex-Einrichtung erforlgreich.");
  //Queue für MQTT anlegen
  mqttQueue = xQueueCreate(MQTT_QUEUEDEPTH, sizeof(MqttJob));
  assert(mqttQueue);
  //Tasks vorbereiten
  int app_cpu = xPortGetCoreID();
  BaseType_t rc;
  esp_err_t er;
  //Watchdog initiieren
  er = esp_task_wdt_init(300, true);  // Timeout in Sekunden, true = trigger_panic
  assert(er == ESP_OK); 
  //Tasks starten
  rc = xTaskCreatePinnedToCore(
    mqttSender,                 //Taskroutine
    "MQTTSenderTask",           //Taskname
    2048,                       //StackSize
    nullptr,                    //Argumente / Parameter
    4,                          //Priorität
    &hmqtt,                     //handler
    app_cpu);                   //CPU_ID
  assert(rc == pdPASS);
  Serial.println("MQTT Sendertask gestartet.");
  rc = xTaskCreatePinnedToCore(
    getTempFromSensor,         //Taskroutine
    "getTempSensorTask",       //Taskname
    2048,                      //StackSize
    nullptr,                   //Argumente / Parameter
    2,                         //Priorität
    &htempSensor,              //handler
    app_cpu);                  //CPU_ID
  assert(rc=pdPASS);
  Serial.println("TempSensor-Task gestartet.");
  rc = xTaskCreatePinnedToCore(
    MQTTwatchdog,              //Taskroutine
    "MQTTwatchdog",            //Taskname
    2048,                      //StackSize
    nullptr,                   //Argumente / Parameter
    1,                         //Priorität
    &hMQTTwatchdog,            //handler
    app_cpu);                  //CPU_ID
  assert(rc=pdPASS);
  Serial.println("MQTT-Watchdog-Task gestartet.");
  rc = xTaskCreatePinnedToCore(
    MQTTstate,                 //Taskroutine
    "MQTTstate",               //Taskname
    2048,                      //StackSize
    nullptr,                   //Argumente / Parameter
    1,                         //Priorität
    nullptr,                   //handler
    app_cpu);                  //CPU_ID
  assert(rc=pdPASS);
  Serial.println("MQTT-State-Task gestartet.");
  //OK-Blinker
  digitalWrite(LED_MSG, HIGH);
  delay(250);
  digitalWrite(LED_MSG, LOW);
  Serial.println("Normalbetrieb gestartet...");
  //Startmeldung via MQTT
  String mqttTopicAC;
  mqttTopicAC = MQTT_SERIAL_PUBLISH_BASIS;
  mqttTopicAC += "ac";
  mqttPublishQueue(mqttTopicAC.c_str(), "Start durchgeführt.",false);
}

void loop() {
  vTaskDelete(nullptr);
}
