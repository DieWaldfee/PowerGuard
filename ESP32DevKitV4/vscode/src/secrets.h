// secrets.h - Vorlage
// Kopiere diese Datei nach secrets.h und trage deine Zugangsdaten ein.
// secrets.h wird NICHT ins Git eingecheckt!

#ifndef SECRETS_H
#define SECRETS_H

// WiFi-Zugangsdaten
#define HOSTNAME "ESP32_PowerGuard"
const char* ssid = "DEIN_WLAN_SSID";
const char* password = "DEIN_WLAN_PASSWORT";

// MQTT-Broker
#define MQTT_SERVER "192.168.x.x"
#define MQTT_PORT 1883
#define MQTT_USER "mqttbroker"
#define MQTT_PASSWORD "DEIN_MQTT_PASSWORT"

#endif // SECRETS_H
