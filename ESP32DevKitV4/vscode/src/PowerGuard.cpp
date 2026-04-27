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
#define PHASE_12V 18            // Steuerpin für Phase 12V Versorgung zum ESP32 Heizstabsteuerung

// Definition der Zugangsdaten WiFi
WiFiClient myWiFiClient;

//Definition der Zugangsdaten MQTT
#define MQTT_CLIENTID "ESP32_PowerGuard" //Name muss eineindeutig auf dem MQTT-Broker sein!
#define PG_MQTT_KEEPALIVE 90
#define PG_MQTT_SOCKETTIMEOUT 30
#define MQTT_SERIAL_PUBLISH_STATUS "SmartHome/Keller/Heizung/ESP32_PowerGuard/status"
#define MQTT_SERIAL_RECEIVER_COMMAND "SmartHome/Keller/Heizung/ESP32_PowerGuard/command"
#define MQTT_SERIAL_PUBLISH_DS18B20 "SmartHome/Keller/Heizung/ESP32_PowerGuard/Temperatur/"
#define MQTT_SERIAL_PUBLISH_STATE "SmartHome/Keller/Heizung/ESP32_PowerGuard/state/"
#define MQTT_SERIAL_PUBLISH_CONFIG "SmartHome/Keller/Heizung/ESP32_PowerGuard/config/"
#define MQTT_SERIAL_PUBLISH_BASIS "SmartHome/Keller/Heizung/ESP32_PowerGuard/"
DeviceAddress myDS18B20Address;
unsigned long MQTTReconnect = 0;
#define MQTT_QUEUEDEPTH 50                // Tiefe der MQTT-Queue - 50 Botschaften
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
float deltaT = 2.0;               //Limit des Betrags von Differenz zwischen tempTop1 tempTop2 (|tempTop1-tempTop2|)
float minTemp = 10.0;             //untere Plausibilitätsgrenze für Temperatursignale. Bei Unterschreitung => Notabschaltung, da ggf. Sensor defekt
float maxTemp = 100.0;            //obere Plausibilitätsgrenze für Temperatursignale. Bei Überschreitung => Notabschaltung, da ggf. Sensor defekt
int volatile tempTSensorFail = 0; //Fehlercounter zur Temperaturmessung - Resilienz gegen gelegentliche Fehlauswertungen der Temperatursensoren
int maxTSensorFail = 3;           //maximal zulässige, hinereinander folgende Sensorfehler - danach panicStop
float DS18B20_minValue = -55.0;   //unterster Messwert im Messbereich [°C]
float DS18B20_maxValue = 125.0;   //unterster Messwert im Messbereich [°C]
#define DS18B20_RESOLUTION 10     // 9bit: ±0.5°C @ 93.75ms; 10bit: ±0.25°C @ 187.5ms; 11bit: ±0.125°C @ 375ms; 12bit: ±0.0625°C @ 750ms
#define DS18B20_DELAY 20          // Wartezeit nach angetriggerter Messung [ms]

//Initialisiere OneWire und Thermosensor(en)
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature myDS18B20(&oneWire);

//Mutexdefinitionen
static SemaphoreHandle_t mutexTemp;
static SemaphoreHandle_t mutexTempSensor;
static SemaphoreHandle_t mutexStatus;
static SemaphoreHandle_t mutexMQTT;

#define MQTT_STATE_REFRESH 60000  // Intervall des MQTT-Status-Tasks: 60.000 Ticks = 60s

//TaskHandler zur Verwendung mit ESP watchdog
static TaskHandle_t htempSensor;
static TaskHandle_t hMQTTwatchdog;

//PreDefinition Funktionen
void panicStop(void);
void thermalStop(void);
void Heizstab_reboot(void);

//erforderliche Funtions-Prototypen
bool mqttPublishQueue(const char*, const char*, bool);
String formatDS18B20Address(const DeviceAddress addr);

//-------------------------------------
// Basisfunktion zum sicheren Reset
void safeReset() {
  Serial.println("ESP32 Reset wird vorbereitet...");
  // Guard: mutexMQTT existiert erst nach Mutex-Initialisierung in setup()
  // Ohne diese Prüfung: xSemaphoreTake(NULL) → Assert-Crash wenn safeReset()
  // aus mqttConnect() heraus aufgerufen wird, bevor setup() die Mutexe angelegt hat.
  if (mutexMQTT != nullptr) {
    Serial.println("MQTT Disconnect...");
    xSemaphoreTake(mutexMQTT, pdMS_TO_TICKS(1000));  // best-effort; kein assert - Reboot folgt
    mqttClient.disconnect();
    delay(50);
    Serial.println("Flush TCP-Buffer...");
    myWiFiClient.clear();
    delay(50);
    // bewusst kein xSemaphoreGive - blockiert alle MQTT-Ops anderer Tasks bis Reboot
  }
  Serial.println("ESP32 Reset!");
  ESP.restart();
}

//-------------------------------------
// Callback für MQTT
void mqttCallback(char* topic, byte* message, unsigned int length) {
  BaseType_t rc;
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
    mqttPublishQueue(mqttTopicAC.c_str(), "Test OK", false);
    tx_ac = 0;
  }
  // Mutex holen – schützt alle Schreibzugriffe auf volatile Status-Flags
  rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
  assert(rc == pdPASS);
  //debug-Modifikation
  if ((tx_ac) && (str.startsWith("debug="))) {
    if (str[6] >= '0' && str[6] <= '3') {
      debug = str[6] - '0';
      mqttPublishQueue(mqttTopicAC.c_str(), ("debug=" + String(debug) + " umgesetzt").c_str(), false);
      tx_ac = 0;
    }
  }
  //panicMode-Modifikation
  if ((tx_ac) && (str.startsWith("panicMode=0"))) {
    panicMode = 0;
    if (thermalLimit == 0) {
      rc = xSemaphoreGive(mutexStatus);  // panicStop/Heizstab_reboot nehmen mutex selbst
      assert(rc == pdPASS);
      Heizstab_reboot();
      rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
      assert(rc == pdPASS);
    }
    mqttPublishQueue(mqttTopicAC.c_str(), "panicMode=0 umgesetzt", false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("panicMode=1"))) {
    panicMode = 1;
    mqttPublishQueue(mqttTopicAC.c_str(), "panicMode=1 umgesetzt", false);
    rc = xSemaphoreGive(mutexStatus);
    assert(rc == pdPASS);
    panicStop();
    rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
    assert(rc == pdPASS);
    tx_ac = 0;
  }
  //hardwareError-Modifikation
  if ((tx_ac) && (str.startsWith("hardwareError=0"))) {
    hardwareError = 0;
    mqttPublishQueue(mqttTopicAC.c_str(), "hardwareError=0 umgesetzt", false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("hardwareError=1"))) {
    hardwareError = 1;
    mqttPublishQueue(mqttTopicAC.c_str(), "hardwareError=1 umgesetzt", false);
    rc = xSemaphoreGive(mutexStatus);
    assert(rc == pdPASS);
    panicStop();
    rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
    assert(rc == pdPASS);
    tx_ac = 0;
  }
  //thermalLimit-Modifikation
  if ((tx_ac) && (str.startsWith("thermalLimit=0"))) {
    if (thermalLimit == 1) {
      if ((panicMode == 0) && (hardwareError == 0)) {
        thermalLimit = 0;
        rc = xSemaphoreGive(mutexStatus);
        assert(rc == pdPASS);
        Heizstab_reboot();
        rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
        assert(rc == pdPASS);
      } else {
        thermalLimit = 0;
      }
    }
    mqttPublishQueue(mqttTopicAC.c_str(), "thermalLimit=0 umgesetzt", false);
    tx_ac = 0;
  }
  if ((tx_ac) && (str.startsWith("thermalLimit=1"))) {
    rc = xSemaphoreGive(mutexStatus);
    assert(rc == pdPASS);
    thermalStop();
    rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
    assert(rc == pdPASS);
    mqttPublishQueue(mqttTopicAC.c_str(), "thermalLimit=1 umgesetzt", false);
    tx_ac = 0;
  }
  //Float-Parameter via Lookup-Tabelle (prefix.length() verhindert Off-by-one-Bugs)
  struct FloatParam { const char* prefix; float* target; };
  static const FloatParam floatParams[] = {
    {"tempLimit=",     &tempLimit},
    {"deltaT=",        &deltaT},
    {"minTemp=",       &minTemp},
    {"maxTemp=",       &maxTemp},
    {"tempReconnect=", &tempReconnect},
    {"tempMaxLimit=",  &tempMaxLimit},
  };
  for (int i = 0; i < (int)(sizeof(floatParams) / sizeof(floatParams[0])) && tx_ac; i++) {
    String prefix = floatParams[i].prefix;
    if (str.startsWith(prefix)) {
      *floatParams[i].target = str.substring(prefix.length()).toFloat();
      mqttMessage = prefix + String(*floatParams[i].target) + " umgesetzt";
      if (debug > 2) Serial.println(mqttMessage);
      mqttPublishQueue(mqttTopicAC.c_str(), mqttMessage.c_str(), false);
      tx_ac = 0;
    }
  }
  if ((tx_ac) && ((str.startsWith("restart")) || (str.startsWith("reboot")))) {
    // Kein Mutex nötig: Callback wird von mqttSender aufgerufen, das mutexMQTT bereits hält
    mqttClient.publish(mqttTopicAC.c_str(), "reboot in einer Sekunde!");
    if (debug) Serial.println("für Restart: alles aus & restart in 1s!");
    digitalWrite(PHASE_5V, HIGH);
    digitalWrite(PHASE_12V, HIGH);
    digitalWrite(LED_OK, LOW);
    digitalWrite(LED_ERROR, HIGH);
    vTaskDelay(1000);
    if (debug) Serial.println("führe Restart aus!");
    safeReset();
  }
  // Mutex freigeben
  rc = xSemaphoreGive(mutexStatus);
  assert(rc == pdPASS);
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
    myDS18B20.getAddress(myDS18B20Address, i);
    String adresse = formatDS18B20Address(myDS18B20Address);
    float tempVal = myDS18B20.getTempCByIndex(i);  // einmalig lesen – JSON und Topic bleiben konsistent
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/JSON";
    mqttJson = "{\"ID\":\"" + String(i) + "\"";
    mqttJson += ",\"Temperatur\":\"" + String(tempVal) + "\"";
    mqttJson += ",\"Adresse\":\"(" + adresse + ")\"";
    mqttJson += ",\"Ort\":\"Temperatur Sensor " + String(i) + "\"}";
    if (debug > 2) Serial.println("MQTT_JSON: " + mqttJson);
    mqttPublishQueue(mqttTopic.c_str(), mqttJson.c_str(), false);
    //Temperatur
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/Temperatur";
    mqttPayload = String(tempVal);
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), false);
    if (debug > 2) Serial.print("MQTT ID: ");
    if (debug > 2) Serial.println(mqttPayload);
    //ID
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/ID";
    mqttPayload = String(i);
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), false);
    if (debug > 2) Serial.print("MQTT Temperatur: ");
    if (debug > 2) Serial.println(mqttPayload);
    //Adresse
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/Adresse";
    mqttPublishQueue(mqttTopic.c_str(), adresse.c_str(), false);
    if (debug > 2) Serial.print("MQTT Adresse: ");
    if (debug > 2) Serial.println(adresse);
    //Ort
    mqttTopic = MQTT_SERIAL_PUBLISH_DS18B20 + String(i) + "/Ort";
    mqttPayload = "Temperatur Sensor " + String(i);
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), false);
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
  mqttJson += ",\"WiFi_Signal_Strength\":\"" + ((WiFi.status() == WL_CONNECTED) ? String(WiFi.RSSI()) : String("--")) + "\"";
  mqttJson += ",\"WiFi_IP_Adress\":\"" + WiFi.localIP().toString() + "\"";
  mqttJson += ",\"WiFi_MAC_Adress\":\"" + WiFi.macAddress() + "\"}";
  if (debug > 2) Serial.println("MQTT_JSON: " + mqttJson);
  mqttPublishQueue(mqttTopic.c_str(), mqttJson.c_str(),false);
  //panicMode
  mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
  mqttTopic += "panicMode";
  mqttPayload = String(panicMode);
  mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(),false);
  if (debug > 2) Serial.print("MQTT panicMode: ");
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
  mqttPayload = (WiFi.status() == WL_CONNECTED) ? String(WiFi.RSSI()) : String("--");
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
  mqttJson += ",\"tempReconnect\":\"" + String(tempReconnect) + "\"";
  mqttJson += ",\"deltaT\":\"" + String(deltaT) + "\"";
  mqttJson += ",\"minTemp\":\"" + String(minTemp) + "\"";
  mqttJson += ",\"maxTemp\":\"" + String(maxTemp) + "\"";
  mqttJson += ",\"thermalLimit\":\"" + String(thermalLimit) + "\"}";
  if (debug > 2) Serial.println("MQTT_JSON: " + mqttJson);
  mqttPublishQueue(mqttTopic.c_str(), mqttJson.c_str(),false);
}
// LED-Blik-MSG
void LEDblinkMSG() {
#if HARDWARE_VERSION >= 2
  digitalWrite(LED_MSG, HIGH);
  delay(150);
  digitalWrite(LED_MSG, LOW);
#else
  digitalWrite(LED_OK, HIGH);  // V1.0: kein MSG-LED, Blink auf OK-LED
  delay(150);
  digitalWrite(LED_OK, LOW);
#endif
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
    rc = xSemaphoreTake(mutexMQTT, portMAX_DELAY);
    assert(rc == pdPASS);
    bool mqttConn = mqttClient.connected();
    xSemaphoreGive(mutexMQTT);
    if (mqttConn) {
      rc = xSemaphoreTake(mutexTempSensor, portMAX_DELAY);
      assert(rc == pdPASS);
      rc = xSemaphoreTake(mutexTemp, portMAX_DELAY);
      assert(rc == pdPASS);
        printDS18B20MQTT();
      rc = xSemaphoreGive(mutexTemp);
      assert(rc == pdPASS);
      rc = xSemaphoreGive(mutexTempSensor);
      assert(rc == pdPASS);

      rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
      assert(rc == pdPASS);
        printStateMQTT();
      rc = xSemaphoreGive(mutexStatus);
      assert(rc == pdPASS);

      printConfigMQTT();
    }

    if (debug > 1) Serial.println("Stack frei MQTTstate: " + String(uxTaskGetStackHighWaterMark(NULL) * 4) + " Bytes");

    // Task schlafen legen
    LEDblinkMSG();
    vTaskDelayUntil(&ticktime, MQTT_STATE_REFRESH);
  }
}

//-------------------------------------
//Subfunktionen für MQTTwatchdog-Task
// MQTT Verbindung herstellen (wird auch von setup verwendet!)
void mqttConnect () {
  int i = 0;
  int wifiWait = 0;
  while (WiFi.status() != WL_CONNECTED) {
    if (++wifiWait > 60) {
      Serial.println("WiFi nicht erreichbar! Reboot!!");
      safeReset();
    }
    Serial.print("W");
    esp_task_wdt_reset();
    delay(1000);
  }
  Serial.print("Verbindungsaufbau zu MQTT Server ");
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
      esp_task_wdt_reset();
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
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), true);
    // thermalLimit nur zurücksetzen wenn beide Sensoren unter tempReconnect liegen
    rc = xSemaphoreTake(mutexTemp, portMAX_DELAY);
    assert(rc == pdPASS);
    float t1snap = temp1;
    float t2snap = temp2;
    rc = xSemaphoreGive(mutexTemp);
    assert(rc == pdPASS);
    rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
    assert(rc == pdPASS);
    if ((thermalLimit == 1) && (hardwareError == 0) &&
        (t1snap < tempReconnect) && (t2snap < tempReconnect)) {
      thermalLimit = 0;
      if (debug) Serial.println("MQTT-Reconnect: thermalLimit zurückgesetzt (Temp im Normalbereich).");
    } else if (thermalLimit == 1) {
      if (debug) Serial.println("MQTT-Reconnect: thermalLimit bleibt gesetzt (Temp noch zu hoch oder hardwareError).");
    }
    rc = xSemaphoreGive(mutexStatus);
    assert(rc == pdPASS);
    //reconnect zurückmelden
    mqttTopic = MQTT_SERIAL_PUBLISH_BASIS + String("ac");
    mqttPayload = String("MQTT reconnect durchgeführt!");
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), false);
  }
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
    rc = xSemaphoreTake(mutexMQTT, portMAX_DELAY);
    assert(rc == pdPASS);
    checkMQTTconnetion();
    xSemaphoreGive(mutexMQTT);

    if (debug > 1) Serial.println("Stack frei MQTTwatchdog: " + String(uxTaskGetStackHighWaterMark(NULL) * 4) + " Bytes");

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

  er = esp_task_wdt_add(NULL);   // Task zur Überwachung hinzugefügt
  assert(er == ESP_OK);

  for (;;){                        // Dauerschleife des Tasks
    // WDT-sicheres Warten auf Mutex — verhindert WDT-Timeout wenn MQTTwatchdog
    // den Mutex während eines Reconnects hält
    while (xSemaphoreTake(mutexMQTT, pdMS_TO_TICKS(5000)) != pdPASS) {
      esp_task_wdt_reset();
    }
    esp_task_wdt_reset();
    //Statusausgabe
    if (debug > 1) Serial.print("TickTime: ");
    if (debug > 1) Serial.print(ticktime);
    if (debug > 1) Serial.println(" | MQTT-Sender-Task gestartet");
    if (mqttClient.connected()) {
      // Sendebereit -> MQTT-Queue kann geleert werden
      while (xQueueReceive(mqttQueue, &job, 0) == pdPASS) {
        mqttClient.publish(job.topic, job.payload, job.retain);
        if (debug > 2) Serial.print("Topic: ");
        if (debug > 2) Serial.println(job.topic);
        if (debug > 2) Serial.print("Payload: ");
        if (debug > 2) Serial.println(job.payload);
        if (debug > 2) Serial.print("retain: ");
        if (debug > 2) Serial.println(job.retain);
      }
    }
    mqttClient.loop();  // Keepalive
    xSemaphoreGive(mutexMQTT);

    if (debug > 1) Serial.println("Stack frei mqttSender: " + String(uxTaskGetStackHighWaterMark(NULL) * 4) + " Bytes");

    // Task schlafen legen - restart alle 0.5s = 0.5*1000 ticks = 500 ticks
    // mit mqttClient.loop() wird auch der MQTTcallback ausgeführt!
    vTaskDelayUntil(&ticktime, 500);
  }
}
//MQTT-Queue befüllen
bool mqttPublishQueue(const char* topic, const char* payload, bool retain = false) {
  MqttJob job;
  strncpy(job.topic, topic, sizeof(job.topic) - 1);           // Absicherung gegen Buffer-Overflow
  job.topic[sizeof(job.topic) - 1] = '\0';                    // garantierte Null-Terminierung
  strncpy(job.payload, payload, sizeof(job.payload) - 1);     // Absicherung gegen Buffer-Overflow
  job.payload[sizeof(job.payload) - 1] = '\0';                // garantierte Null-Terminierung
  job.retain = retain;
  return xQueueSend(mqttQueue, &job, QUEUEMAXWAITTIME) == pdPASS;
}

//-------------------------------------
//Subfunktionen für den TempSensor-Task
// Formatiert eine DS18B20-Geräteadresse als lesbaren Hex-String
String formatDS18B20Address(const DeviceAddress addr) {
  String result = "";
  for (uint8_t j = 0; j < 8; j++) {
    result += "0x";
    if (addr[j] < 0x10) result += "0";
    result += String(addr[j], HEX);
    if (j < 7) result += ", ";
  }
  return result;
}
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
  delay(DS18B20_DELAY);             // Wartezeit bis Messung abgeschlossen ist
  if (debug > 2) Serial.println("fertig");
  for (int i = 0; i < DS18B20_Count; i++) {
    if (i == 0) t1 = myDS18B20.getTempCByIndex(i);
    if (i == 1) t2 = myDS18B20.getTempCByIndex(i);
  }
  //Plausibilitätscheck
  if (checkDS18B20Value(t1)) {
    temp1 = t1;
    res = true;
  }
  else {
    tempTSensorFail = tempTSensorFail + 1;
    res = false;
    mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
    mqttTopic += "lastError";
    mqttPayload = "Temperatursensor T1 außerhalb des Messbereichts: " + String(t1) + "[C]; Wiederholung: " + String(tempTSensorFail);
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), true);
    if (debug > 2) Serial.print("LastError: ");
    if (debug > 2) Serial.println(mqttPayload); //(debug > 2)
  }
  if (checkDS18B20Value(t2)) {
    temp2 = t2;
    if (res) tempTSensorFail = 0;  //t1 und t2 sind korrekt => Fehlercounter auf 0 gesetzt
  }
  else {
    tempTSensorFail = tempTSensorFail + 1;
    res = false;
    mqttTopic = MQTT_SERIAL_PUBLISH_STATE;
    mqttTopic += "lastError";
    mqttPayload = "Temperatursensor T2 außerhalb des Messbereichts: " + String(t2) + "[C]; Wiederholung: " + String(tempTSensorFail);
    mqttPublishQueue(mqttTopic.c_str(), mqttPayload.c_str(), true);
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
      rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
      assert(rc == pdPASS);
      hardwareError = 1;
      lastError = "Zwangsabschaltung: unterschiedliche Sensorwerte (" + String(temp1) + "; " + String(temp2) + ")";
      rc = xSemaphoreGive(mutexStatus);
      assert(rc == pdPASS);
      panicStop();
    }
  }
  const float temps[2] = {temp1, temp2};
  const char* labels[2] = {"Top-Sensor #1", "Top-Sensor #2"};
  for (int i = 0; i < 2; i++) {
    if ((temps[i] < minTemp) || (temps[i] > maxTemp)) {
      if (hardwareError == 0) {
        if (debug) Serial.print("Zwangsabschaltung wegen einer Verletzung der thermischen Grenzen! (");
        if (debug) Serial.print(temps[i]);
        if (debug) Serial.print("°C am ");
        if (debug) Serial.print(labels[i]);
        if (debug) Serial.print(". Limits: ] ");
        if (debug) Serial.print(minTemp);
        if (debug) Serial.print("..");
        if (debug) Serial.print(maxTemp);
        if (debug) Serial.print("[");
        rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
        assert(rc == pdPASS);
        hardwareError = 1;
        lastError = "Zwangsabschaltung: Verletzung der thermischen Grenzen " + String(temps[i]) + " -> [" + String(minTemp) + " ... " + String(maxTemp) + "]";
        rc = xSemaphoreGive(mutexStatus);
        assert(rc == pdPASS);
        panicStop();
      }
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
      rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
      assert(rc == pdPASS);
      lastError = "Thermal Stop: 12V abgeschaltet (" + String(temp1) + "; " + String(temp2) + ")";
      rc = xSemaphoreGive(mutexStatus);
      assert(rc == pdPASS);
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
      rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
      assert(rc == pdPASS);
      lastError = "Thermal Stop: 5V und 12V abgeschaltet (" + String(temp1) + "; " + String(temp2) + ")";
      rc = xSemaphoreGive(mutexStatus);
      assert(rc == pdPASS);
      panicStop();
    }
  }
  if ((thermalLimit == 1) && (hardwareError == 0)) {
    if ((temp1 < tempReconnect) && (temp2 < tempReconnect)) {
      if (debug) Serial.print("Thermische Zuschalten nach ThermalLimit/PanicMode: ");
      if (debug) Serial.print(temp1);
      if (debug) Serial.print("°C am Top-Sensor #1 bzw. ");
      if (debug) Serial.print(temp2);
      if (debug) Serial.println("°C am Top-Sensor #2.");
      rc = xSemaphoreTake(mutexStatus, portMAX_DELAY);
      assert(rc == pdPASS);
      lastError = "zurück im Normalbereich der Temperatur: reset der Heizstabelektornik";
      rc = xSemaphoreGive(mutexStatus);
      assert(rc == pdPASS);
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
      myDS18B20.getAddress(myDS18B20Address, i);
      String adresse = formatDS18B20Address(myDS18B20Address);
      Serial.println(adresse + ")");
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

    if (debug > 1) Serial.println("Stack frei getTempFromSensor: " + String(uxTaskGetStackHighWaterMark(NULL) * 4) + " Bytes");

    // Task schlafen legen - restart alle 5s = 5*1000 ticks = 5000 ticks
    vTaskDelayUntil(&ticktime, 5000);
  }
}

void setup() {
  // WDT sofort auf 5 Minuten setzen - verhindert WDT-Reset während langer Init-Phasen (WiFi, MQTT)
  const esp_task_wdt_config_t wdt_config = {.timeout_ms = 300000, .idle_core_mask = 0, .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdt_config);
  // Initialisierung und Plausibilitaetschecks
  Serial.begin(115200);
  delay(100);                    // Stabilisierungspause
  while (!Serial) Serial.println("Start Setup");
  pinMode(LED_ERROR, OUTPUT);
  digitalWrite(LED_ERROR, HIGH);
#if HARDWARE_VERSION >= 2
  pinMode(LED_MSG, OUTPUT);
  digitalWrite(LED_MSG, HIGH);
#endif
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
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(ssid, password);
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
  // Event-Handler erst nach erfolgreichem Connect binden — feuert nur bei späteren Verbindungsänderungen
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) Serial.println("WiFi: Verbindung verloren.");
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)       Serial.println("WiFi: Verbindung wiederhergestellt.");
  });
  //MQTT-Setup
  String mqttTopic;
  String mqttPayload;
  Serial.println("MQTT Server Initialisierung laeuft...");
  mqttClient.setServer(MQTT_SERVER,MQTT_PORT); 
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(PG_MQTT_KEEPALIVE);
  mqttClient.setSocketTimeout(PG_MQTT_SOCKETTIMEOUT);
  mqttConnect();
  mqttTopic = MQTT_SERIAL_PUBLISH_BASIS + String("error");
  mqttPayload = String(String(MQTTReconnect) + ".: keine Fehler seit Reboot!");
  mqttClient.publish(mqttTopic.c_str(), mqttPayload.c_str(), true);  // retain=true; direkt - mqttQueue existiert noch nicht
  Serial.println("");
  //DS18B20-Setup
  Serial.println("Auslesen der DS18B20-Sensoren...");
  myDS18B20.begin();
  myDS18B20.setResolution(DS18B20_RESOLUTION);
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
  mutexMQTT = xSemaphoreCreateMutex();
  assert(mutexMQTT);
  Serial.println("Mutex-Einrichtung erfolgreich.");
  //Queue für MQTT anlegen
  mqttQueue = xQueueCreate(MQTT_QUEUEDEPTH, sizeof(MqttJob));
  assert(mqttQueue);
  //Tasks vorbereiten
  int app_cpu = xPortGetCoreID();
  BaseType_t rc;
  //Tasks starten
  rc = xTaskCreatePinnedToCore(
    mqttSender,                 //Taskroutine
    "MQTTSenderTask",           //Taskname
    3072,                       //StackSize
    nullptr,                    //Argumente / Parameter
    4,                          //Priorität
    &hmqtt,                     //handler
    app_cpu);                   //CPU_ID
  assert(rc == pdPASS);
  Serial.println("MQTT Sendertask gestartet.");
  rc = xTaskCreatePinnedToCore(
    getTempFromSensor,         //Taskroutine
    "getTempSensorTask",       //Taskname
    6144,                      //StackSize
    nullptr,                   //Argumente / Parameter
    2,                         //Priorität
    &htempSensor,              //handler
    app_cpu);                  //CPU_ID
  assert(rc==pdPASS);
  Serial.println("TempSensor-Task gestartet.");
  rc = xTaskCreatePinnedToCore(
    MQTTwatchdog,              //Taskroutine
    "MQTTwatchdog",            //Taskname
    4096,                      //StackSize
    nullptr,                   //Argumente / Parameter
    1,                         //Priorität
    &hMQTTwatchdog,            //handler
    app_cpu);                  //CPU_ID
  assert(rc==pdPASS);
  Serial.println("MQTT-Watchdog-Task gestartet.");
  rc = xTaskCreatePinnedToCore(
    MQTTstate,                 //Taskroutine
    "MQTTstate",               //Taskname
    6144,                      //StackSize
    nullptr,                   //Argumente / Parameter
    1,                         //Priorität
    nullptr,                   //handler
    app_cpu);                  //CPU_ID
  assert(rc==pdPASS);
  Serial.println("MQTT-State-Task gestartet.");
  // OK-Blinker / alle LEDs nach erfolgreichem Boot ausschalten
  digitalWrite(LED_ERROR, LOW);
  digitalWrite(LED_OK, LOW);
  delay(250);
  digitalWrite(LED_OK, HIGH);
  delay(250);
  digitalWrite(LED_OK, LOW);
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
