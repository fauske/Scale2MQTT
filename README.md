# Scale2MQTT

Firmware for ESP32-C3 (testet på «Super Mini») som leser målinger fra en Senssun Fat-baserte Bluetooth-vekt og sender dem til MQTT, med automatisk oppsett i Home Assistant.

Utviklet og testet med en MarQuant-vekt fra Jula som annonserer seg som `SENSSUN FAT`. Andre vekter med samme protokoll (tjeneste `0xFFF0`) kan fungere, men er ikke testet.

## Hva den gjør

1. Skanner kontinuerlig etter vekta (Bluetooth LE) og kobler til når den våkner.
2. Stiller klokka på vekta og sender brukerprofilen (kjønn, alder, høyde), slik at vekta beregner kroppssammensetning.
3. Leser vekt, fett, vann, muskler, bein og kalorier.
4. Lagrer målingen i flash (NVS), så den overlever strømbrudd.
5. Slår på Wi-Fi, synkroniserer klokka (NTP) og publiserer målingene til MQTT med tidsstempel.
6. Slår av Wi-Fi igjen, siden Wi-Fi og Bluetooth deler radio.

Er ikke Wi-Fi eller MQTT tilgjengelig, blir målingene liggende i køen (opptil 20) og sendes senere. Nytt forsøk skjer hvert femte minutt.

### MQTT-melding

Topic (standard): `scale2mqtt/senssun/state`, sendt med retain.

```json
{"weight_kg":85.0,"fat_pct":19.1,"water_pct":55.6,"muscle_pct":41.7,"bone_kg":3.8,"kcal":1889,"measured_at":"2026-10-10T08:12:49Z"}
```

Felt som vekta ikke rakk å sende, utelates. `measured_at` er UTC og mangler hvis klokka ikke var synkronisert og tidspunktet ikke kan regnes ut.

### Home Assistant

Med `HA_DISCOVERY` på publiseres MQTT Discovery-meldinger, og sensorene dukker opp av seg selv under enheten «Senssun vekt»: vekt, kroppsfett, vann, muskler, bein, kalorier og tidspunkt for måling.

## Krav

- ESP32-C3-kort (testet: Super Mini)
- Senssun-kompatibel Bluetooth-vekt
- MQTT-broker, for eksempel Mosquitto
- [PlatformIO](https://platformio.org/) (VS Code-utvidelsen eller CLI)
- Fast strømforsyning, for eksempel en USB-lader. Skanningen bruker ca. 50–80 mA, så batteridrift er ikke realistisk.

## Oppsett

1. **Klon prosjektet og åpne det i VS Code med PlatformIO.**

2. **Finn vektens Bluetooth-adresse.** Bruk nRF Connect på mobilen og se etter enheten `SENSSUN FAT`.

3. **Lag `include/secrets.h`** ved å kopiere `include/secrets.example.h`, og fyll inn:
   ```c
   #define WIFI_SSID "..."
   #define WIFI_PASS "..."
   #define MQTT_HOST "192.168.1.10"
   #define MQTT_PORT 1883
   #define MQTT_USER ""   // tom = uten innlogging
   #define MQTT_PASS ""
   ```
   Filen ligger i `.gitignore` og sjekkes ikke inn.

4. **Tilpass `include/config.h`:**

   | Innstilling | Betydning |
   |---|---|
   | `SCALE_MAC` | Vektens adresse, små bokstaver |
   | `USER_ID` | Bruker 1–12 på vekta |
   | `USER_MALE` | `true` = mann, `false` = kvinne |
   | `USER_AGE`, `USER_HEIGHT_CM` | Brukes til å beregne kroppssammensetning |
   | `MQTT_TOPIC`, `MQTT_CLIENT_ID` | MQTT-oppsett |
   | `HA_DISCOVERY`, `HA_DISCOVERY_PREFIX` | Home Assistant Discovery |
   | `NTP_SERVER`, `TIMEZONE` | Tid og tidssone |

5. **Bygg og last opp** (lukk serial monitor først, ellers er porten opptatt):
   ```sh
   pio run -t upload
   pio device monitor
   ```

6. **Test.** Vekk vekta og gå på den barføtt, og stå helt stille til vekten stabiliserer seg. Monitor viser omtrent:
   ```
   Fant vekt, RSSI -45
   Tilkoblet etter 12 forsøk, 3100 ms
   Stabil vekt: 85.0 kg
   Måling lagret (1 i kø)
   MQTT publisert: {...}
   ```

## Vanlige problemer

- **Koble til tar lang tid eller feiler med kode 574.** Vekta avviser mange tilkoblingsforsøk. Firmwaren prøver kontinuerlig i 30 sekunder, men plasser ESP-en nær vekta (innen en meter) og vekk vekta først. Svak antenne på billige kort kan forverre dette.
- **Bare vekt, ingen fett eller vann.** Gå på barføtt og stå stille til målingen er ferdig. Sjekk at `USER_AGE` og `USER_HEIGHT_CM` er riktige.
- **Vekta tar ikke imot tilkobling.** Slå av Bluetooth på mobilen eller lukk andre apper som er koblet til vekta. Vekta tar vanligvis imot én tilkobling av gangen.
- **Opplasting feiler med «port busy».** Lukk serial monitor.
- **Sensorer dukker ikke opp i Home Assistant.** Sjekk at MQTT-integrasjonen bruker samme prefiks som `HA_DISCOVERY_PREFIX`, og at en måling er fullført minst én gang.

## Begrensninger

- Støtter én vekt og én bruker.
- Home Assistant lagrer historikk på tidspunktet meldingen mottas. Forsinkede målinger får riktig tidspunkt bare i sensoren «Målt».
- Protokollet er reverse-engineeret av andre og ikke offisielt dokumentert, så andre modeller kan oppføre seg annerledes.

## Protokoll (kort)

Tjeneste `0xFFF0`. Skriv til `0xFFF2`, abonner på notifications fra `0xFFF1`.

- Notification: `FF A5 d1 d2 d3 d4 type`, verdier er 16-bit big-endian i 0,1-enheter. `type`: `A0` levende vekt, `AA` stabil vekt, `B0` fett og vann, `C0` muskler og bein (bein-bytene er byttet), `D0` kalorier, `BE` feil.
- Kommandoer (9 byte, sjekksum = sum av byte 1–6 i byte 7): `A5 30` dato, `A5 31` tid, `A5 10` bruker (kjønn+id, alder, høyde).

Protokollet er beskrevet i [openScale issue #309](https://github.com/oliexdev/openScale/issues/309) og i openScales Senssun-støtte.
