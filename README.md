# PowerGuard
PowerGuard-Platine und Software zur Sicherheitsüberwachung einer Heizstabsteuerung

**zugehörige Repositories**<br>
- PowerGuard (dieses Repository)
- Heizstabsteuerung (Steuerungsplatine <a href="https://github.com/DieWaldfee/Heizstabsteuerung"> Link </a>)
- Heizstab_ioBroker (Ansteuerungs-Blockly - ein MQTT-Broker wird benötigt - Link)

**Funktion:** <br>
Der ESP32 steuert 2 Temperatursensoren (DS18B20) an und liest diese aus. Die beiden Sensoren sollen am höchsten Punkt des Pufferspeichers der Heizung angebracht werden, in dem auch der elektrische Heizstab eingeschraubt ist. 
Die beiden Werte werden einerseits verglichen und andererseits benutzt, um die Pufferspeichertemperatur zu überwachen. Liegt die Temperatur zu hoch, dann ist zu viel Energie im Pufferspeicher und vermutlich ist der Heizstab dafür ursächlich -> der Heizstabsteuerung wird der Strom abgeschalten. Dies stellt eine unabhängige Sicherungsfunktion dar, das jedweder Fehler im Heizstabsystem behandelt und die Anlage in den sicheren Zustand überführt (d.h. aus).<p>
Der PowerGuard hat 2 Möglichkeiten dies zu tun: <br>
(1) die 5V Versorgung des ESP32 der Heizstabsteuerung abzuschalten -> die Relais werden nicht mehr angesteuert - unabhängig vom Fehlerbild. <br>
(2) die 12V Versorgungsspannung der Lastschaltrelais (SSR siehe Heizstabsteuerung) wird abgeschaltet -> die Heizstabsteuerung bleibt aktiv, kann aber nichts mehr schalten. <p>
Die Kommunikation mit dem ESP32 wird über WiFi abgewickelt und über das Protokoll MQTT umgesetzt. Es stehen mehrere Befehle zur Verfügung, die ebenfalls via MQTT an den ESP gesendet werden kann. Neben "restart" / "reboot" (führt zu einem Neustart des ESP32) kann die ganze Konfiguration und der debug-Level angepasst werden. Alle Anpassungen sind nach einem Neustart verloren, da diese nicht permanent gespeichert werden. Befehle werden unter der MQTT-Hierarchie in folgendem Ort empfangen: `SmartHome/Keller/Heizung/ESP32_PowerGuard/command` (MQTT_SERIAL_RECEIVER_COMMAND)

**Entwicklungsumgebungen:** <br>
Die aktuelle Software (V2.x) für den ESP32 wird mit **PlatformIO** entwickelt (z. B. in VS Code mit dem PlatformIO-Plugin). Die Konfiguration des Projekts befindet sich in der Datei `platformio.ini`; Compilieren und Flashen erfolgen direkt über PlatformIO.<br>
Die historische Erstversion (V1.0) wurde in der **Arduino-IDE** erstellt und ist im Unterordner `V1.0/` archiviert.<br>
Die Platine (PCB) habe ich in Eagle von AutoCAD modelliert. Für Maker gibt es eine kostenfreie Version. Die CAM-Daten (PCB-Produktionsdaten), sowie die BOM (Stückliste) sind ebenfalls aus Eagle ausgegeben.
Eine Übersichtsversion der Platine ist mit Fritzing umgesetzt und enthält auch einige Notizen zur Pinbelegung.<br>

**benötigte Umgebung:** <br>
- ioBroker zur Ansteuerung (geht natürlich auch mit anderen SmartHome-Systemen, solange diese MQTT sprechen können)
- ioBroker MGTT-Adapter
- MQTT-Broker: z.B. mosquitto unter Linux/Debian/Raspian...
- 230V Stromversorgung oder alternativ eine 5V Versorgung über z.B. ein USB-Ladegerät (muss 2 ESP32 sicher versorgen können -> 2A reicht völlig aus) oder über die Schraubklemmen anderweitig versorgt.

### Abhängigkeiten (PlatformIO / `platformio.ini`):
OneWire:_______________PaulStoffregen/OneWire (git-master) – v2.3.7 inkompatibel mit ESP-IDF 5.x; v2.3.8 buggy<br>
DallasTemperature:_____milesburton/DallasTemperature v4.0.4<br>
PubSubClient:__________knolleary/PubSubClient v2.8<br>
WiFi.h / esp_task_wdt:_Espressif Arduino-Framework (via pioarduino platform-espressif32)<br>

### Board / Plattform (PlatformIO):
platform: pioarduino platform-espressif32 v55.03.38-1 (ESP-IDF 5.x)<br>
board: esp32dev<br>
framework: arduino<br>

**Hardware-Version (PCB V1.0 vs. V2.0):**<br>
In `platformio.ini` kann die Hardware-Version über ein Build-Flag gesetzt werden:<br>
```
build_flags = -DHARDWARE_VERSION=2   ; für PCB V2.0 (mit MSG-LED an Pin 22)
```
Ohne Build-Flag wird V1.0-Verhalten angenommen (kein MSG-LED, Blink auf OK-LED).<br>

| PIN | V1.0 | V2.0 |
|-----|------|------|
| 19  | LED OK (grün) | LED OK (grün) |
| 22  | – | LED MSG (gelb) |
| 23  | LED ERROR (rot) | LED ERROR (rot) |
| 25  | DS18B20 OneWire Bus | DS18B20 OneWire Bus |
| 32  | PHASE 5V (Heizstabsteuerung) | PHASE 5V (Heizstabsteuerung) |
| 18  | PHASE 12V (SSR-Relaisversorgung) | PHASE 12V (SSR-Relaisversorgung) |

**aktuelle Versionen:** <br>
- ESP-Software    V2.x (PlatformIO, `src/PowerGuard.cpp`)
- ESP-Software    V1.0 (Arduino IDE, archiviert in `V1.0/PowerGuard.ino`)
- PCB (Eagle) 	   V2.0
- CAM             V1.0
- BOM             V1.0
- Fritzing		      V1.1
 
Fokus der PCB V2.0 ist die Platine mit über einen PCB-Hersteller umzusetzen.
Die CAM-Datei ist passend für https://jlcpcb.com. Stand 03.11.2023 ist die Platine für 2$ (5 St.) bestellbar.
In der BOM findet sich die Stückliste für die Bestückung wieder.

<img src="https://github.com/DieWaldfee/PowerGuard/assets/66571311/1c389b38-95ca-4472-810e-c5202d421479" width="500">

Zugehöriges Projekt: https://github.com/users/DieWaldfee/projects/1

**Installation:**
* Zugangsdaten in `/src/secrets.h` eintragen (Hostname, WLAN-SSID, WLAN-Passwort, MQTT-Server, MQTT-Port, MQTT-User, MQTT-Passwort)
* Hardware-Version in `platformio.ini` als Build-Flag setzen (`-DHARDWARE_VERSION=2` für PCB V2.0, weglassen für V1.0)
* MQTT-Pfade sind in `src/PowerGuard.cpp` als `#define` hinterlegt und können dort angepasst werden<br>
&nbsp;&nbsp;&nbsp;<img src="https://github.com/DieWaldfee/PowerGuard/assets/66571311/4f8c1ffd-b743-4ed1-b313-fc14fc3ef089" height="100">
* Debug-Level kann via MQTT-Befehl `debug=0` … `debug=3` zur Laufzeit gesetzt werden (0 = Boot only; 1 = Basic; 2 = Advanced; 3 = Absolut); alternativ im Quellcode in Zeile 14 von `src/PowerGuard.cpp`
* ESP-Software wird über PlatformIO compiliert und auf das „ESP32 Dev Kit V4" geflasht.
* Platine entweder via Eagle an PCB-Hersteller übermitteln, oder das fertige CAM-File über die Anbieter-Webseite senden. (z.B. an https://jlcpcb.com)
* Platine bestücken + ESP und Level-Shifter (3.3V <-> 5V) aufsetzen.
* Relais anschließen.
* 12V Versorgung der Heizstabsteuerung wird über LED-Treiber realisiert.

### MQTT-Steuerbefehle

Befehle werden auf dem Topic **`SmartHome/Keller/Heizung/ESP32_PowerGuard/command`** empfangen.<br>
Bestätigungen erscheinen auf **`SmartHome/Keller/Heizung/ESP32_PowerGuard/ac`**.

| Befehl | Beschreibung | Antwort (ac-Topic) |
|--------|-------------|---------------------|
| `Test` | Verbindungstest | `Test OK` |
| `debug=0` … `debug=3` | Debug-Level setzen (0=Boot only, 1=Basic, 2=Advanced, 3=Absolut) | `debug=X umgesetzt` |
| `panicMode=0` | Panic-Modus zurücksetzen; Heizstabsteuerung neu starten (wenn thermalLimit=0) | `panicMode=0 umgesetzt` |
| `panicMode=1` | Panic-Abschaltung auslösen (5V + 12V aus) | `panicMode=1 umgesetzt` |
| `hardwareError=0` | Hardware-Fehler-Flag zurücksetzen | `hardwareError=0 umgesetzt` |
| `hardwareError=1` | Hardware-Fehler setzen → Panic-Abschaltung | `hardwareError=1 umgesetzt` |
| `thermalLimit=0` | Thermisches Limit zurücksetzen; Heizstabsteuerung neu starten (wenn panicMode=0 und hardwareError=0) | `thermalLimit=0 umgesetzt` |
| `thermalLimit=1` | Thermische Abschaltung auslösen (12V aus) | `thermalLimit=1 umgesetzt` |
| `tempLimit=<Wert>` | Abschalttemperatur 12V [°C] (Standard: 90,0) | `tempLimit=<Wert> umgesetzt` |
| `tempMaxLimit=<Wert>` | Panic-Abschalttemperatur 5V+12V [°C] (Standard: 95,0) | `tempMaxLimit=<Wert> umgesetzt` |
| `tempReconnect=<Wert>` | Wiedereinschalttemperatur [°C] (Standard: 80,0) | `tempReconnect=<Wert> umgesetzt` |
| `deltaT=<Wert>` | Max. zulässige Differenz zwischen den beiden Sensoren [K] (Standard: 2,0) | `deltaT=<Wert> umgesetzt` |
| `minTemp=<Wert>` | Untere Plausibilitätsgrenze [°C] (Standard: 10,0) | `minTemp=<Wert> umgesetzt` |
| `maxTemp=<Wert>` | Obere Plausibilitätsgrenze [°C] (Standard: 100,0) | `maxTemp=<Wert> umgesetzt` |
| `restart` oder `reboot` | ESP32 neu starten (Ausgänge werden vorher abgeschaltet) | `reboot in einer Sekunde!` |

> **Hinweis:** Alle Parameteränderungen sind flüchtig und gehen nach einem Neustart verloren.

### MQTT-Statustopics (veröffentlicht, alle 60 s)

Basis-Pfad: `SmartHome/Keller/Heizung/ESP32_PowerGuard/`

| Topic (relativ zum Basis-Pfad) | Inhalt | Retain |
|-------------------------------|--------|--------|
| `status` | Online-Status: `true` / `false` (LWT) | ja |
| `ac` | Befehlsbestätigungen | nein |
| `error` | Fehlermeldungen / Reconnect-Infos | ja |
| `Temperatur/0/JSON` | Sensor 0 komplett als JSON | nein |
| `Temperatur/0/Temperatur` | Temperatur Sensor 0 [°C] | nein |
| `Temperatur/0/ID` | Sensor-Index 0 | nein |
| `Temperatur/0/Adresse` | 1-Wire-Adresse Sensor 0 | nein |
| `Temperatur/0/Ort` | Ortsbezeichnung Sensor 0 | nein |
| `Temperatur/1/…` | Gleiche Subtopics für Sensor 1 | nein |
| `state/JSON` | Systemzustand komplett als JSON | nein |
| `state/panicMode` | Panic-Mode Flag (0/1) | nein |
| `state/thermalLimit` | Thermal-Limit Flag (0/1) | nein |
| `state/hardwareError` | Hardware-Fehler Flag (0/1) | nein |
| `state/lastError` | Letzter Fehler als Klartext | nein |
| `state/WiFi_Signal_Strength` | WiFi-Signalstärke [dBm] | nein |
| `state/WiFi_IP_Adress` | IP-Adresse | nein |
| `state/WiFi_MAC_Adress` | MAC-Adresse | nein |
| `config/JSON_0` | Konfigurationsparameter als JSON | nein |

**Bezugsquellen:**
* Platinennetzteil AC-05-3    <a href="https://www.azdelivery.de/products/copy-of-220v-zu-5v-mini-netzteil"> AZ-Delivery </a>
* Levelshifter (3.3V <-> 5V)  <a href="https://www.amazon.de/RUNCCI-YUN-Pegelwandler-Converter-BiDirektional-Mikrocontroller/dp/B082F6BSB5/ref=sr_1_2?__mk_de_DE=%C3%85M%C3%85%C5%BD%C3%95%C3%91&crid=45TPZ9B8CUP9&keywords=level+shifter&qid=1699045033&sprefix=level+shifter%2Caps%2C103&sr=8-2"> Amazon </a>
* Relais <a href="https://www.amazon.de/gp/product/B0B5816YJ7/ref=ppx_yo_dt_b_search_asin_image?ie=UTF8&th=1"> Amazon </a> oder <a href="https://www.az-delivery.de/products/relais-modul"> AZ Delivery </a>
* 12V LED-Treiber als Schaltspannung der SSR-Relais  <a href="https://www.amazon.de/gp/product/B082NLNCSB/ref=ppx_yo_dt_b_search_asin_image?ie=UTF8&psc=1"> Amazon </a>
* JST-Buchse <a href="https://www.amazon.de/gp/product/B0B2R99X99/ref=ppx_yo_dt_b_search_asin_title?ie=UTF8&psc=1"> Amazon </a>
* Klemmbuchse <a href="https://www.amazon.de/gp/product/B087RN8FDZ/ref=ppx_yo_dt_b_search_asin_title?ie=UTF8&th=1"> Amazon </a>
* Widerstände 4,7kOhm, 220 Ohm, 330 Ohm, Led grün, gelb und rot Amazon / eBay / Conrad...
* Temperatursensoren DS18B20 <a href="https://www.az-delivery.de/products/2er-set-ds18b20-mit-3m-kabel"> AZ Delivery </a>

**fertige Platine:**

![grafik](https://github.com/DieWaldfee/PowerGuard/assets/66571311/2c3fc57b-6fb9-496c-9dd0-f728c895b6c9)

![grafik](https://github.com/DieWaldfee/PowerGuard/assets/66571311/cb0928ba-aea0-4322-a46b-0f89a735b46d)
(oberes Relais steuert +5V -> Heizstabsteuerung; unteres Relais steuert die 12V für die SSR-40-DA Lastschaltrelais)

**Haftungsausschluss**<br>
Deutsch: Dieses Projekt arbeitet mit Netzspannung (220 V) und darf ausschließlich von qualifiziertem Fachpersonal aufgebaut, installiert und betrieben werden. Durch die anliegende Spannung besteht Lebensgefahr! Fehler in Schaltung oder Software können zu Sachschäden (z. B. an Gebäude oder Heizung) oder zu gefährlichen Situationen für Leib und Leben führen. Nutzung auf eigene Gefahr – jegliche Haftung wird ausgeschlossen.
<br>
English: This project operates with mains voltage (220 V) and must only be assembled, installed, and operated by qualified professionals. The present voltage poses a risk of fatal electric shock! Errors in circuitry or software may cause property damage (e.g., to buildings or heating systems) or create life-threatening situations. Use at your own risk – any liability is disclaimed.
