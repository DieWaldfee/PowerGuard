# PowerGuard
PowerGuard-Platine und Software zur Sicherheitsüberwachung einer Heizstabsteuerung

- ESP-Software    V2.0
- PCB (Eagle) 	   V2.0
- CAM             V1.0
- BOM             V1.0
- Fritzing		      V1.1
 
Fokus der PCB V2.0 ist die Platine mit über einen PCB-Hersteller umzusetzen.
Die CAM-Datei ist passend für https://jlcpcb.com. Stand 03.11.2023 ist die Platine für 2$ (5 St.) bestellbar.
In der BOM findet sich die Stückliste für die Bestückung wieder.

<img src="https://github.com/DieWaldfee/PowerGuard/assets/66571311/27bb17e4-c70e-40e7-adc7-ea61fb46f8e1" width="500">

Zugehöriges Projekt: https://github.com/users/DieWaldfee/projects/1

**Installation:**
* Hostname, WLAN-SSID + WLAN-Passwort setzen in /ESP32DevKitV4/PowerGuard.ino <br>
&nbsp;&nbsp;&nbsp;<img src="https://github.com/DieWaldfee/PowerGuard/assets/66571311/8d82d4cc-97bd-45e2-9087-7bd85dd3fe94" width="300">
* MQTT-Brokereinstellungen setzen in /ESP32DevKitV4/PowerGuard.ino <br>
&nbsp;&nbsp;&nbsp;<img src="https://github.com/DieWaldfee/PowerGuard/assets/66571311/4bdda754-68f7-475a-8669-c389b18ff35a" height="100">
* MQTT-Pfade setzen in /ESP32DevKitV4/PowerGuard.ino <br>
&nbsp;&nbsp;&nbsp;<img src="https://github.com/DieWaldfee/PowerGuard/assets/66571311/e8671429-b80d-4a7d-809b-9bdb9c047c20" height="100">
* bei Fehlern kann in Zeile 13 der Debug-Level für die Ausgaben auf den serial Monitor eingestellt werden: 0 = BootUp only; 1 = Basic; 2 = Advanced; 3 = Absolut
* ESP-Software wird über die Arduino-IDE aus das "ESP32 Dev Kit V4" compiliert und übertragen.
* Platine entweder via Eagle an PCB-Hersteller übermitteln, oder das fertige CAM-File über die Anbieter-Webseite senden. (z.B. an https://jlcpcb.com)
* Platine bestücken + ESP und Level-Shifter (3.3V <-> 5V) aufsetzen.
* Relais anschließen.
* 12V Versorgung der Heizstabsteuerung wird über LED-Treiber realisiert.

**Bezugsquellen:**
* Platinennetzteil AC-05-3    <a href="https://www.azdelivery.de/products/copy-of-220v-zu-5v-mini-netzteil"> AZ-Delivery </a>
* Levelshifter (3.3V <-> 5V)  <a href="https://www.amazon.de/RUNCCI-YUN-Pegelwandler-Converter-BiDirektional-Mikrocontroller/dp/B082F6BSB5/ref=sr_1_2?__mk_de_DE=%C3%85M%C3%85%C5%BD%C3%95%C3%91&crid=45TPZ9B8CUP9&keywords=level+shifter&qid=1699045033&sprefix=level+shifter%2Caps%2C103&sr=8-2"> Amazon </a>
* Relais <a href="https://www.amazon.de/gp/product/B0B5816YJ7/ref=ppx_yo_dt_b_search_asin_image?ie=UTF8&th=1"> Amazon </a> oder <a href="https://www.az-delivery.de/products/relais-modul"> AZ Delivery </a>
* 12V LED-Treiber als Schaltspannung der SSR-Relais  <a href="https://www.amazon.de/gp/product/B082NLNCSB/ref=ppx_yo_dt_b_search_asin_image?ie=UTF8&psc=1"> Amazon </a>
* JST-Buchse <a href="https://www.amazon.de/gp/product/B0B2R99X99/ref=ppx_yo_dt_b_search_asin_title?ie=UTF8&psc=1"> Amazon </a>
* Klemmbuchse <a href="https://www.amazon.de/gp/product/B087RN8FDZ/ref=ppx_yo_dt_b_search_asin_title?ie=UTF8&th=1"> Amazon </a>
* Widerstände 4,7kOhm, 220 Ohm, 330 Ohm, Led grün, gelb und rot Amazon / eBay / Conrad...
