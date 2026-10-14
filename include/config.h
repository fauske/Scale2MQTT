#pragma once
// Innstillinger for Scale2MQTT. Wi-Fi- og MQTT-passord ligger i secrets.h.

// Vektens Bluetooth-adresse, med små bokstaver.
#define SCALE_MAC "18:7a:93:5b:ea:06"

// Brukerprofilen vekta bruker til å beregne kroppssammensetning.
#define USER_ID 1          // 1-12
#define USER_MALE true     // true = mann, false = kvinne
#define USER_AGE 43        // år
#define USER_HEIGHT_CM 183 // cm

// Innebygd LED gir tilbakemelding etter en veiing: 1 blink = måling lagret, 2 = ingen måling/BLE-feil, 3 = sending via Wi-Fi/MQTT feilet.
#define LED_PIN 8          // ESP32-C3 Super Mini
#define LED_ACTIVE_LOW true

// Ekstra feilsøkingslogg (BLE-forsøk, RSSI osv.)
#define VERBOSE true

// Skriver ut hver annonsering fra vekta og kobler ikke til. Brukes til å se når vekta blir tilkoblingsvillig.
#define ADV_DEBUG false

// MQTT
#define MQTT_CLIENT_ID "scale2mqtt"
#define MQTT_TOPIC "scale2mqtt/senssun/state"

// Home Assistant MQTT Discovery
#define HA_DISCOVERY true
#define HA_DISCOVERY_PREFIX "homeassistant"
#define DEVICE_ID "senssun_scale"
#define DEVICE_NAME "Senssun vekt"

// Tid (NTP). Brukes til tidsstempel på målingene og til å stille klokka på vekta.
#define NTP_SERVER "pool.ntp.org"
#define TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3" // Norge
