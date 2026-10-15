# Scale2MQTT

Firmware for ESP32-C3 som leser en Senssun-kompatibel kroppsanalysevekt over Bluetooth LE og sender målingene til MQTT. Home Assistant kan oppdage sensorene automatisk via MQTT Discovery.

Testet med en ESP32-C3 Super Mini og en MarQuant-vekt fra Jula som annonserer seg som `SENSSUN FAT`. Andre vekter med samme BLE-protokoll kan fungere, men er ikke verifisert.

## Hardware og krav

- ESP32-C3 Super Mini. PlatformIO-målet i prosjektet er `esp32-c3-devkitm-1`.
- MarQuant kroppsanalysevekt fra Jula, Senssun-basert modell som annonserer `SENSSUN FAT`.
- Stabil 5 V USB-strømforsyning, for eksempel en vanlig USB-mobillader. Enheten skanner kontinuerlig og er ikke laget for batteridrift.
- 2,4 GHz Wi-Fi-nettverk.
- MQTT-broker som ESP32-en kan nå, for eksempel Mosquitto. Home Assistant må ha MQTT-integrasjonen satt opp mot brokeren.
- VS Code med PlatformIO-utvidelsen, eller PlatformIO CLI.

## Funksjon

1. Skanner etter den konfigurerte vekta og forsøker å koble til når den annonserer.
2. Abonnerer på vektas BLE-varsler, stiller vektas klokke når ESP32-en har gyldig tid, og sender brukerprofilen.
3. Leser stabil vekt, fett, vann, muskler, beinmasse og kalorier. Kroppsanalyse krever normalt bar hudkontakt med elektrodene.
4. Lagrer målingen i ESP32-ens NVS-flash. Køen rommer opptil 20 målinger; når den er full, fjernes den eldste.
5. Venter omtrent to minutter etter veiingen før Wi-Fi aktiveres. Dette reduserer radioforstyrrelser mens BLE brukes.
6. Synkroniserer klokke ved behov og publiserer køen til MQTT. Wi-Fi slås av etter økten.

Ved manglende Wi-Fi eller MQTT beholdes køen, og et nytt sende-forsøk gjøres senere. MQTT-meldingen publiseres med retain.

## Oppsett

### 1. Opprett lokale nettverksinnstillinger

Kopier eksempelmalen:

```sh
cp include/secrets.example.h include/secrets.h
```

Rediger `include/secrets.h` med verdiene for ditt miljø:

| Innstilling | Beskrivelse |
|---|---|
| `WIFI_SSID` | SSID for 2,4 GHz-nettverket |
| `WIFI_PASS` | Wi-Fi-passord |
| `MQTT_HOST` | IP-adresse eller vertsnavn for MQTT-brokeren |
| `MQTT_PORT` | MQTT-port, vanligvis `1883` for ukryptert lokal MQTT |
| `MQTT_USER` | MQTT-brukernavn, eller tom streng hvis broker ikke bruker innlogging |
| `MQTT_PASS` | MQTT-passord |

`include/secrets.h` er oppført i `.gitignore`; ikke legg passord i `config.h` eller commit secrets-fila.

### 2. Tilpass vekt og Home Assistant

Rediger `include/config.h`:

| Innstilling | Hva brukeren setter |
|---|---|
| `SCALE_MAC` | BLE-adressen til vekta. Finn den med nRF Connect mens vekta annonserer. |
| `USER_ID` | Brukerprofilnummeret på vekta, 1–12. |
| `USER_MALE` | `true` eller `false`, etter profilen som er valgt i appen/vekta. |
| `USER_AGE` | Alder i år. |
| `USER_HEIGHT_CM` | Høyde i centimeter. |
| `MQTT_CLIENT_ID` | MQTT-klient-ID. Bruk en unik verdi hvis du har flere Scale2MQTT-enheter. |
| `MQTT_TOPIC` | Topic for målingsmeldingen. Bruk en egen topic per enhet ved flere vekter. |
| `HA_DISCOVERY` | `true` for automatisk opprettelse av Home Assistant-sensorer. |
| `HA_DISCOVERY_PREFIX` | Discovery-prefiks. Må samsvare med Home Assistant, vanligvis `homeassistant`. |
| `DEVICE_ID`, `DEVICE_NAME` | Stabil, unik enhetsidentifikator og navn som vises i Home Assistant. |
| `NTP_SERVER`, `TIMEZONE` | NTP-server og tidssone. Standard tidssone er satt for Norge. |
| `LED_PIN`, `LED_ACTIVE_LOW` | Endres bare hvis kortvarianten har status-LED på en annen GPIO eller med motsatt polaritet. |
| `VERBOSE`, `ADV_DEBUG` | Seriell feilsøking. `ADV_DEBUG` skriver annonseringsdata og kobler ikke til vekta. |

### 3. Bygg, last opp og åpne Serial Monitor

Lukk Serial Monitor før opplasting, slik at serieporten ikke er opptatt.

```sh
pio run
pio run -t upload
pio device monitor
```

Serial Monitor bruker `115200` baud. I VS Code kan de tilsvarende Build, Upload og Monitor-oppgavene kjøres fra PlatformIO.

### 4. Ta en måling

Lukk Senssun-appen og andre apper som kan holde BLE-forbindelsen til vekta. Vekk vekta, vent til Serial Monitor viser `Abonnerer. Stå på vekta ...`, og gå deretter barføtt opp på den. Stå stille mens målingen stabiliseres. MQTT-sendingen kan komme omtrent to minutter etter målingen.

Typisk logg ved vellykket måling:

```text
Fant vekt, RSSI -50
Tilkoblet etter ...
Abonnerer. Stå på vekta ...
Stabil vekt: 85.1 kg
Måling lagret (1 i kø)
MQTT publisert: { ... }
```

## MQTT og Home Assistant

Standard state-topic er `scale2mqtt/senssun/state`. Eksempel:

```json
{"weight_kg":85.1,"fat_pct":19.9,"water_pct":55.0,"muscle_pct":41.2,"bone_kg":3.8,"kcal":1890,"measured_at":"2026-10-15T08:36:30Z"}
```

Felt som ikke ble mottatt fra vekta, utelates. `measured_at` er UTC og utelates når måletidspunktet ikke er kjent. MQTT Discovery oppretter sensorene Vekt, Kroppsfett, Vann, Muskler, Bein, Kalorier og Målt under `DEVICE_NAME`.

Home Assistant registrerer historikk når den mottar meldingen. `measured_at` viser når målingen ble tatt, men tilbakedaterer ikke Home Assistant-historikken for målinger som har ligget i kø.

## LED-status

På standardoppsettet er den blå status-LED-en koblet til GPIO 8 og aktiv lav:

- Ett kort blink: BLE-abonnementet er klart; det er nå du skal gå på vekta.
- Ett langt blink: målingen er lagret.
- To blink: ingen måling eller BLE-/abonnementsfeil.
- Tre lange blink: sending feilet; køen beholdes for nytt forsøk.

En rød LED på kortet kan være en fast strømindikator og er ikke nødvendigvis styrt av firmware.

## Feilsøking

- **Det tar lang tid før `Abonnerer` vises.** Skalaen kan avvise eller overse BLE-tilkoblingsforsøk. Firmwaren prøver i opptil 30 sekunder. Lukk Senssun-appen, plasser ESP32-en nær vekta og sjekk RSSI i loggen. Feilkode 13 betyr at forbindelsesforsøket fikk timeout.
- **`Avsluttet (frakoblet)` rett etter abonnement.** Vekta koblet fra før måledata kom. Vekk vekta på nytt, lukk andre BLE-apper og prøv igjen.
- **Bare vekt, ingen kroppsanalyse.** Gå barføtt slik at elektrodene får hudkontakt, og stå stille. Kontroller alder, høyde, kjønn og profilnummer.
- **Målingen finnes ikke i Home Assistant.** Se etter `MQTT publisert` i Serial Monitor. Kontroller brokeradresse, innlogging, `MQTT_TOPIC`, MQTT-integrasjonen og at Discovery-prefikset stemmer.
- **Opplasting feiler med «port busy».** Lukk Serial Monitor og prøv opplasting på nytt.
- **`wifi:timeout when WiFi un-init` vises.** Dette er en melding fra ESP32 Wi-Fi-driveren under nedstenging. Kontroller om NTP/MQTT faktisk feilet; meldingen alene beviser ikke at sendingen mislyktes.

## Begrensninger og release-status

- Testet med én ESP32-C3 Super Mini og én MarQuant/SENSSUN FAT-vekt. Andre kort og vektmodeller er ikke verifisert.
- Én vekt og én brukerprofil støttes per enhet.
- BLE-protokollen er ikke offisielt dokumentert. Kroppsanalyseverdiene er vektas beregninger, ikke medisinske målinger.
- Wi-Fi/MQTT bruker vanligvis ukryptert MQTT på det lokale nettverket. Ikke eksponer brokeren direkte mot internett.
- Prosjektet har ingen automatisert testpakke. En vellykket PlatformIO-build verifiserer kompilering, ikke den fysiske BLE-/Wi-Fi-/Home Assistant-flyten.

Før en bred release bør en ren oppstart og veiing testes med den tiltenkte ESP32-en og vekta, Home Assistant Discovery bekreftes, og kø/nytt forsøk testes når MQTT-broker eller Wi-Fi midlertidig er utilgjengelig.

## BLE-protokollnotater

Bekreftet GATT-oppsett for den testede vekta:

- Tjeneste `FFF0`.
- Notifications fra `FFF1`.
- Skriv kommandoer til `FFF2`.
- Varsler har formatet `FF A5 d1 d2 d3 d4 type checksum`. Verdiparene er 16-bit big-endian i 0,1-enheter. Typene er `A0` levende vekt, `AA` stabil vekt, `B0` fett og vann, `C0` muskler og bein, `D0` kalorier og `BE` målefeil. I `C0` er byte-rekkefølgen for beinverdien byttet.
- Dato og tid sendes i 9-byte kommandoer (`A5 30` og `A5 31`). Brukerprofilen sendes i en 8-byte kommando (`A5 10`); tilleggbytes og checksum følger rammen som ble observert i Senssun-appens HCI-opptak.

Protokollbakgrunn finnes i [openScale issue #309](https://github.com/oliexdev/openScale/issues/309) og openScales Senssun-støtte.
