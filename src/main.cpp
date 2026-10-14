// Scale2MQTT: leser Senssun-vekt over BLE og publiserer målingen til MQTT.
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <time.h>
#include <WiFi.h>
#include "config.h"
#include "secrets.h"

static const uint32_t CONNECT_WINDOW_MS = 30000;
static const uint32_t GIVE_UP_PAUSE_MS = 3000; // vekta annonserer selv om ingen står på den
static const uint32_t SESSION_TIMEOUT_MS = 120000;
static const uint32_t COOLDOWN_AFTER_PUBLISH_MS = 30000;
static const uint32_t WIFI_TIMEOUT_MS = 20000;
static const uint32_t RETRY_PUBLISH_MS = 300000;
static const uint32_t PUBLISH_DELAY_MS = 120000; // Wi-Fi forstyrrer BLE, så vi sender først en stund etter veiingen
static const uint32_t PARTIAL_GRACE_MS = 20000; // vent så lenge på fett/muskel etter stabil vekt

struct Measurement {
  float weight = 0, fat = 0, water = 0, muscle = 0, bone = 0, kcal = 0;
  bool hasWeight = false, hasFat = false, hasMuscle = false, hasKcal = false;
};

static Measurement meas;
static volatile uint32_t weightAtMs = 0;
static NimBLEAddress targetAddr(SCALE_MAC);
static NimBLEAddress foundAddr;
static volatile bool found = false;
static volatile bool measurementReady = false;
static volatile bool sendUserCmd = false;
static bool userCmdSent = false;

static void blink(int times, int onMs = 200) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LED_ACTIVE_LOW ? LOW : HIGH);
    delay(onMs);
    digitalWrite(LED_PIN, LED_ACTIVE_LOW ? HIGH : LOW);
    delay(200);
  }
}

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *dev) override {
    if (dev->getAddress() == targetAddr && ADV_DEBUG) {
      char hex[100] = {0};
      size_t n = min(dev->getPayloadLength(), (size_t)32);
      for (size_t i = 0; i < n; i++) sprintf(hex + i * 3, "%02x ", dev->getPayload()[i]);
      Serial.printf("[%lu] ADV rssi=%d type=%d %s\n", millis(), dev->getRSSI(), dev->getAdvType(), hex);
      return;
    }
    if (dev->getAddress() == targetAddr && !found) {
      foundAddr = dev->getAddress();
      if (VERBOSE) Serial.printf("Fant vekt, RSSI %d\n", dev->getRSSI());
      found = true;
      NimBLEDevice::getScan()->stop();
    }
  }
};

// Ramme: FF A5 d1 d2 d3 d4 type, verdier er 16-bit big-endian i 0,1-enheter.
static void onNotify(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify) {
  if (len < 7 || data[0] != 0xFF || data[1] != 0xA5) return;
  uint16_t a = (data[2] << 8) | data[3];
  uint16_t b = (data[4] << 8) | data[5];
  switch (data[6]) {
    case 0xAA:
      if (a > 0 && !meas.hasWeight) {
        meas.weight = a / 10.0f;
        meas.hasWeight = true;
        weightAtMs = millis();
        sendUserCmd = true;
        if (VERBOSE) Serial.printf("Stabil vekt: %.1f kg\n", meas.weight);
      }
      break;
    case 0xB0:
      meas.fat = a / 10.0f;
      meas.water = b / 10.0f;
      meas.hasFat = true;
      break;
    case 0xC0:
      meas.muscle = a / 10.0f;
      meas.bone = ((data[5] << 8) | data[4]) / 10.0f; // ben-bytene er byttet om i protokollen
      meas.hasMuscle = true;
      break;
    case 0xD0:
      meas.kcal = a;
      meas.hasKcal = true;
      break;
    case 0xBE:
      Serial.println("Feil under fettmåling");
      break;
  }
  if (meas.hasWeight && meas.hasFat && meas.hasMuscle && meas.hasKcal) measurementReady = true;
}

static bool writeSenssun(NimBLEClient *client, uint8_t *msg) {
  uint8_t sum = 0;
  for (int i = 1; i < 7; i++) sum += msg[i];
  msg[7] = sum;
  NimBLERemoteService *svc = client->getService("FFF0");
  NimBLERemoteCharacteristic *chr = svc ? svc->getCharacteristic("FFF2") : nullptr;
  return chr && chr->writeValue(msg, 9, true);
}

static bool timeValid() { return time(nullptr) > 1700000000; }

// Bruker NTP-tid når den finnes, ellers byggetidspunktet.
static void writeDateTime(NimBLEClient *client) {
  int year, doy, h, m, s;
  if (timeValid()) {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    year = tm.tm_year + 1900;
    doy = tm.tm_yday + 1;
    h = tm.tm_hour;
    m = tm.tm_min;
    s = tm.tm_sec;
  } else {
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    static const int daysIn[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    char mon[4] = {0};
    int day;
    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &h, &m, &s);
    int mi = (strstr(months, mon) - months) / 3;
    doy = day;
    for (int i = 0; i < mi; i++) doy += daysIn[i];
    if (mi > 1 && year % 4 == 0) doy++;
  }

  uint8_t dateMsg[9] = {0xA5, 0x30, (uint8_t)(year % 100), (uint8_t)(doy >> 8), (uint8_t)(doy & 0xFF), 0, 0, 0, 0};
  uint8_t timeMsg[9] = {0xA5, 0x31, (uint8_t)h, (uint8_t)m, (uint8_t)s, 0, 0, 0, 0};
  writeSenssun(client, dateMsg);
  writeSenssun(client, timeMsg);
}

static void writeUserCmd(NimBLEClient *client) {
  uint8_t msg[9] = {0xA5, 0x10, (uint8_t)((USER_MALE ? 15 * 16 : 0) + USER_ID), USER_AGE, USER_HEIGHT_CM, 0, 0, 0, 0};
  bool ok = writeSenssun(client, msg);
  if (!ok) Serial.println("Brukerkommando feilet");
  userCmdSent = true;
}

static void publishDiscovery(PubSubClient &mqtt) {
  struct Sensor {
    const char *key, *name, *unit, *deviceClass;
  };
  static const Sensor sensors[] = {
      {"weight_kg", "Vekt", "kg", "weight"},   {"fat_pct", "Kroppsfett", "%", nullptr},
      {"water_pct", "Vann", "%", nullptr},      {"muscle_pct", "Muskler", "%", nullptr},
      {"bone_kg", "Bein", "kg", "weight"},      {"kcal", "Kalorier", "kcal", nullptr},
      {"measured_at", "Målt", nullptr, "timestamp"},
  };
  char topic[128], payload[640], extra[128];
  for (const Sensor &s : sensors) {
    snprintf(topic, sizeof(topic), "%s/sensor/%s_%s/config", HA_DISCOVERY_PREFIX, DEVICE_ID, s.key);
    int n = 0;
    extra[0] = 0;
    if (s.deviceClass) n += snprintf(extra + n, sizeof(extra) - n, ",\"device_class\":\"%s\"", s.deviceClass);
    if (s.unit)
      n += snprintf(extra + n, sizeof(extra) - n, ",\"unit_of_measurement\":\"%s\",\"state_class\":\"measurement\"", s.unit);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.%s }}\"%s,"
             "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\",\"manufacturer\":\"Senssun\",\"model\":\"Fat scale\"}}",
             s.name, DEVICE_ID, s.key, MQTT_TOPIC, s.key, extra, DEVICE_ID, DEVICE_NAME);
    mqtt.publish(topic, payload, true);
    mqtt.loop();
  }
}

// Målinger lagres i flash og sendes når Wi-Fi og MQTT er tilgjengelig.
struct Stored {
  float weight, fat, water, muscle, bone, kcal;
  uint8_t flags;
  uint32_t ts; // epoch-sekunder, 0 = ukjent
  uint32_t ms; // millis() da målingen ble tatt, 0xFFFFFFFF = fra tidligere oppstart
};
enum : uint8_t { F_WEIGHT = 1, F_FAT = 2, F_MUSCLE = 4, F_KCAL = 8 };
static const int MAX_PENDING = 20;
static Stored pending[MAX_PENDING];
static int pendingCount = 0;
static uint32_t nextPublishTry = 0;
static uint32_t lastEnqueueMs = 0;
static float lastEnqueueWeight = 0;

static void loadPending() {
  Preferences prefs;
  prefs.begin("scale", true);
  int n = prefs.getUChar("n", 0);
  if (n > MAX_PENDING || prefs.getBytesLength("q") != n * sizeof(Stored)) n = 0;
  if (n > 0) prefs.getBytes("q", pending, n * sizeof(Stored));
  pendingCount = n;
  prefs.end();
  for (int i = 0; i < n; i++)
    if (pending[i].ts == 0) pending[i].ms = 0xFFFFFFFF;
  if (n > 0 && VERBOSE) Serial.printf("%d lagret måling(er) i kø\n", n);
}

static void savePending() {
  Preferences prefs;
  prefs.begin("scale", false);
  prefs.putUChar("n", pendingCount);
  if (pendingCount > 0) prefs.putBytes("q", pending, pendingCount * sizeof(Stored));
  else prefs.remove("q");
  prefs.end();
}

static void enqueue(const Measurement &m) {
  // Vekta sender samme måling på nytt hvis vi kobler til igjen mens du fortsatt står på den.
  if (lastEnqueueMs && millis() - lastEnqueueMs < 120000 && fabsf(m.weight - lastEnqueueWeight) < 0.3f) {
    if (VERBOSE) Serial.println("Ignorerer duplikat");
    return;
  }
  Stored s = {m.weight, m.fat, m.water, m.muscle, m.bone, m.kcal,
              (uint8_t)((m.hasWeight ? F_WEIGHT : 0) | (m.hasFat ? F_FAT : 0) | (m.hasMuscle ? F_MUSCLE : 0) |
                        (m.hasKcal ? F_KCAL : 0)),
              timeValid() ? (uint32_t)time(nullptr) : 0, millis()};
  if (pendingCount == MAX_PENDING) {
    memmove(pending, pending + 1, (MAX_PENDING - 1) * sizeof(Stored));
    pendingCount--;
  }
  pending[pendingCount++] = s;
  savePending();
  lastEnqueueMs = millis();
  lastEnqueueWeight = m.weight;
  if (VERBOSE) Serial.printf("Måling lagret (%d i kø)\n", pendingCount);
}

static void formatPayload(const Stored &s, char *buf, size_t size) {
  size_t n = snprintf(buf, size, "{");
  bool first = true;
  auto add = [&](uint8_t flag, const char *key, float v, int decimals) {
    if (!(s.flags & flag)) return;
    n += snprintf(buf + n, size - n, "%s\"%s\":%.*f", first ? "" : ",", key, decimals, v);
    first = false;
  };
  add(F_WEIGHT, "weight_kg", s.weight, 1);
  add(F_FAT, "fat_pct", s.fat, 1);
  add(F_FAT, "water_pct", s.water, 1);
  add(F_MUSCLE, "muscle_pct", s.muscle, 1);
  add(F_MUSCLE, "bone_kg", s.bone, 1);
  add(F_KCAL, "kcal", s.kcal, 0);
  if (s.ts) {
    char iso[24];
    time_t t = s.ts;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
    n += snprintf(buf + n, size - n, ",\"measured_at\":\"%s\"", iso);
  }
  snprintf(buf + n, size - n, "}");
}

static uint32_t nextTimeSync = 0;
static bool timeSyncDue() { return !timeValid() || (int32_t)(millis() - nextTimeSync) >= 0; }

// Etter første NTP-synk kan målinger tatt før synk få tidspunkt ut fra tid siden målingen.
static void syncTime() {
  bool had = timeValid();
  configTzTime(TIMEZONE, NTP_SERVER);
  uint32_t t = millis();
  if (had) delay(2000);
  else while (!timeValid() && millis() - t < 8000) delay(200);
  if (!timeValid()) {
    Serial.println("NTP feilet");
    return;
  }
  nextTimeSync = millis() + 86400000UL;
  if (VERBOSE) Serial.println("Tid synkronisert");
  time_t now = time(nullptr);
  for (int i = 0; i < pendingCount; i++)
    if (pending[i].ts == 0 && pending[i].ms != 0xFFFFFFFF) pending[i].ts = now - (millis() - pending[i].ms) / 1000;
}

// Wi-Fi slås på bare mens vi publiserer, siden Wi-Fi og BLE deler radio.
static bool publishPending() {
  nextPublishTry = millis() + RETRY_PUBLISH_MS;
  if (pendingCount == 0 && !timeSyncDue()) return true;
  if (VERBOSE) Serial.printf("Wi-Fi-økt (%d måling(er) i kø) ...\n", pendingCount);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm); // C3 Super Mini har ofte dårlig antenne/strømforsyning ved full effekt
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_TIMEOUT_MS) delay(200);

  bool ok = WiFi.status() == WL_CONNECTED;
  if (!ok) {
    Serial.printf("Wi-Fi feilet (status=%d), målingen blir liggende i køen\n", (int)WiFi.status());
  } else {
    syncTime();
    if (pendingCount == 0) {
      WiFi.mode(WIFI_OFF);
      return true;
    }
    WiFiClient net;
    PubSubClient mqtt(net);
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(768);
    ok = strlen(MQTT_USER) > 0 ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)
                               : mqtt.connect(MQTT_CLIENT_ID);
    if (ok) {
      if (HA_DISCOVERY) publishDiscovery(mqtt);
      char payload[256];
      while (pendingCount > 0) {
        formatPayload(pending[0], payload, sizeof(payload));
        if (!mqtt.publish(MQTT_TOPIC, payload, true)) {
          ok = false;
          break;
        }
        mqtt.loop();
        delay(100);
        if (VERBOSE) Serial.printf("MQTT publisert: %s\n", payload);
        memmove(pending, pending + 1, (pendingCount - 1) * sizeof(Stored));
        pendingCount--;
      }
      savePending();
      mqtt.disconnect();
    } else {
      Serial.printf("MQTT-tilkobling feilet, rc=%d\n", mqtt.state());
    }
  }
  WiFi.mode(WIFI_OFF);
  return ok;
}

static void runSession() {
  NimBLEClient *client = NimBLEDevice::createClient();
  client->setConnectTimeout(10);
  // Kort intervall gjør at mislykkede forsøk gir opp raskt i stedet for etter ~500 ms.
  client->setConnectionParams(6, 6, 0, 400);

  // Vekta annonserer sjelden og avviser mange tilkoblinger, så vi prøver kontinuerlig en stund.
  bool ok = false;
  uint32_t tryStart = millis();
  int attempt = 0;
  while (!ok && millis() - tryStart < CONNECT_WINDOW_MS) {
    attempt++;
    uint32_t t0 = millis();
    ok = client->connect(foundAddr, false);
    if (!ok) {
      if (VERBOSE && client->getLastError() != 574) Serial.printf("Forsøk %d feilet (kode %d, %lu ms)\n", attempt, client->getLastError(), millis() - t0);
      delay(10);
    }
  }
  if (VERBOSE || !ok) Serial.printf("%s etter %d forsøk, %lu ms\n", ok ? "Tilkoblet" : "Ga opp", attempt, millis() - tryStart);
  if (!ok) {
    NimBLEDevice::deleteClient(client);
    delay(GIVE_UP_PAUSE_MS);
    return;
  }

  meas = Measurement();
  measurementReady = false;
  sendUserCmd = false;
  userCmdSent = false;

  NimBLERemoteService *svc = client->getService("FFF0");
  NimBLERemoteCharacteristic *chr = svc ? svc->getCharacteristic("FFF1") : nullptr;
  if (!chr || !chr->subscribe(true, onNotify)) {
    Serial.println("Fant ikke eller kunne ikke abonnere på FFF1");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    blink(2);
    return;
  }
  if (VERBOSE) Serial.println("Abonnerer. Stå på vekta ...");
  blink(1, 100);
  writeDateTime(client);
  writeUserCmd(client);

  uint32_t start = millis();
  while (client->isConnected() && !measurementReady && millis() - start < SESSION_TIMEOUT_MS &&
         !(meas.hasWeight && millis() - weightAtMs > PARTIAL_GRACE_MS)) {
    if (sendUserCmd) {
      sendUserCmd = false;
      writeUserCmd(client);
    }
    delay(50);
  }

  bool ready = measurementReady;
  if (!ready) {
    Serial.printf("Avsluttet (%s): vekt=%d fett=%d muskel=%d kcal=%d\n",
                  client->isConnected() ? "tidsavbrudd" : "frakoblet", meas.hasWeight, meas.hasFat,
                  meas.hasMuscle, meas.hasKcal);
  }
  if (client->isConnected()) client->disconnect();
  NimBLEDevice::deleteClient(client);

  if (meas.hasWeight) {
    enqueue(meas);
    nextPublishTry = millis() + PUBLISH_DELAY_MS;
    blink(1, 600);
    delay(COOLDOWN_AFTER_PUBLISH_MS);
  } else {
    Serial.println("Ingen måling");
    blink(2);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_ACTIVE_LOW ? HIGH : LOW);
  delay(2000);
  if (VERBOSE) Serial.println("Scale2MQTT starter");
  loadPending();
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  if (ADV_DEBUG) scan->setDuplicateFilter(false);
}

void loop() {
  if ((pendingCount > 0 || timeSyncDue()) && (int32_t)(millis() - nextPublishTry) >= 0) {
    if (!publishPending() && pendingCount > 0) blink(3, 600);
    return;
  }
  found = false;
  NimBLEDevice::getScan()->start(10, false);
  NimBLEDevice::getScan()->clearResults();
  if (found) runSession();
}
