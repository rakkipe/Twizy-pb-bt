#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>

// =============================================================
//  CONFIGURATIE — pas hier aan voor jouw hardware
// =============================================================

#define DEVICE_NAME  "TwizyPB"   // BLE naam die de app ziet

// --- Sensor modus (kies er één) ---
#define MODE_DEMO    0   // Gesimuleerde testwaarden (standaard)
#define MODE_INA226  1   // INA226 via I2C op Grove-poort
#define MODE_ADC     2   // Spanningsdeler op ADC + ACS712/758

#define SENSOR_MODE  MODE_DEMO

// --- INA226 instellingen (alleen bij MODE_INA226) ---
// Sluit aan op Grove: SDA=G32, SCL=G33
// #define INA226_ADDR   0x40
// #define INA226_SHUNT  0.01f   // Shunt weerstand in Ohm (bv. 10mΩ)
// #define VOLT_SCALE    1.0f    // Correctiefactor spanning (1.0 = geen correctie)

// --- ADC instellingen (alleen bij MODE_ADC) ---
// Spanning: spanningsdeler op GPIO36
//   R1=100kΩ (naar batterij+), R2=5.6kΩ (naar GND)
//   Ratio = (R1+R2)/R2 ≈ 18.86 → voor 58V: 58/18.86 = 3.07V op ADC
// #define VOLT_PIN      36
// #define VOLT_RATIO    18.86f
// Stroom: ACS758 50B (40mV/A) op GPIO35
// #define CURR_PIN      35
// #define CURR_SENS     40.0f   // mV per Ampère
// #define CURR_OFFSET   1.65f   // Nulpunt spanning in V (halve VCC)

// --- GPIO voor relais ---
#define RELAY1_PIN  26   // Grove TX pin (of extern relais module)
#define RELAY2_PIN  0    // Pas aan naar jouw GPIO

// --- Batterijcapaciteit Twizy (voor SoC berekening) ---
#define BATT_FULL_V   58.8f   // Volledig geladen (V)
#define BATT_EMPTY_V  48.0f   // Leeg (V)

// =============================================================

// NUS UUIDs (Nordic UART Service)
#define NUS_SERVICE  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_CHAR  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_CHAR  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// --- Staat ---
static float    gVoltage = 0, gCurrent = 0, gPower = 0;
static int      gSoc = 0;
static bool     gRelay1 = false, gRelay2 = false;
static bool     gConnected = false;
static uint32_t gLastSend = 0;

static NimBLEServer*         pServer = nullptr;
static NimBLECharacteristic* pTxChar = nullptr;

// =============================================================
//  INA226 minimale implementatie (geen extra lib nodig)
// =============================================================
#if SENSOR_MODE == MODE_INA226
#include <Wire.h>

static void ina226_write(uint8_t addr, uint8_t reg, uint16_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val >> 8);
    Wire.write(val & 0xFF);
    Wire.endTransmission();
}

static uint16_t ina226_read(uint8_t addr, uint8_t reg) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)addr, (uint8_t)2);
    return ((uint16_t)Wire.read() << 8) | Wire.read();
}

static void ina226_init() {
    Wire.begin(32, 33);   // Grove SDA=G32, SCL=G33
    // Config: gemiddeld 16x, 1.1ms conversie, continu
    ina226_write(INA226_ADDR, 0x00, 0x4727);
    // Kalibratie voor 10mΩ shunt, max 10A:
    // Cal = 0.00512 / (CurrentLSB * Rshunt)
    // CurrentLSB = 10A / 32768 = 305µA
    ina226_write(INA226_ADDR, 0x05, 1677);
}

static void ina226_read_all() {
    int16_t raw_v = (int16_t)ina226_read(INA226_ADDR, 0x02);
    int16_t raw_i = (int16_t)ina226_read(INA226_ADDR, 0x04);
    gVoltage = raw_v * 1.25f / 1000.0f * VOLT_SCALE;  // 1.25mV/bit
    gCurrent = raw_i * 305e-6f;                         // 305µA/bit
    gPower   = gVoltage * gCurrent;
}
#endif

// =============================================================
//  Lees sensoren
// =============================================================
static void readSensors() {
#if SENSOR_MODE == MODE_DEMO
    // Gesimuleerde waarden die langzaam variëren
    static float phase = 0;
    phase += 0.05f;
    gVoltage = 52.0f + 2.0f * sinf(phase * 0.3f);
    gCurrent = 8.0f + 4.0f * sinf(phase);
    gPower   = gVoltage * gCurrent;

#elif SENSOR_MODE == MODE_INA226
    ina226_read_all();

#elif SENSOR_MODE == MODE_ADC
    // Spanning via spanningsdeler
    int raw_v = analogRead(VOLT_PIN);
    gVoltage  = (raw_v / 4095.0f) * 3.3f * VOLT_RATIO;

    // Stroom via ACS sensor
    int raw_i  = analogRead(CURR_PIN);
    float mv   = (raw_i / 4095.0f) * 3300.0f;
    gCurrent   = (mv / 1000.0f - CURR_OFFSET) / (CURR_SENS / 1000.0f);
    gPower     = gVoltage * gCurrent;
#endif

    // SoC lineaire benadering op basis van spanning
    float ratio = (gVoltage - BATT_EMPTY_V) / (BATT_FULL_V - BATT_EMPTY_V);
    gSoc = (int)constrain(ratio * 100.0f, 0.0f, 100.0f);
}

// =============================================================
//  Scherm update
// =============================================================
static void updateDisplay() {
    auto& lcd = M5.Display;
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextColor(TFT_WHITE);

    // Titel
    lcd.setTextSize(1);
    lcd.setCursor(2, 2);
    lcd.setTextColor(gConnected ? TFT_GREEN : TFT_RED);
    lcd.printf("%s  %s", DEVICE_NAME, gConnected ? "BT OK" : "wacht...");

    // Metingen
    lcd.setTextColor(TFT_CYAN);
    lcd.setTextSize(2);
    lcd.setCursor(2, 22);
    lcd.printf("%.1fV", gVoltage);

    lcd.setCursor(80, 22);
    lcd.printf("%.1fA", gCurrent);

    lcd.setTextColor(TFT_YELLOW);
    lcd.setCursor(2, 50);
    lcd.printf("%.0fW", gPower);

    // SoC met kleur
    uint16_t socColor = gSoc > 50 ? TFT_GREEN : (gSoc > 20 ? TFT_YELLOW : TFT_RED);
    lcd.setTextColor(socColor);
    lcd.setCursor(80, 50);
    lcd.printf("%d%%", gSoc);

    // Relais status
    lcd.setTextSize(1);
    lcd.setTextColor(TFT_WHITE);
    lcd.setCursor(2, 80);
    lcd.printf("R1:%s  R2:%s",
        gRelay1 ? "AAN" : "UIT",
        gRelay2 ? "AAN" : "UIT");
}

// =============================================================
//  BLE callbacks
// =============================================================
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s) override {
        gConnected = true;
    }
    void onDisconnect(NimBLEServer* s) override {
        gConnected = false;
        NimBLEDevice::startAdvertising();
    }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string raw = c->getValue();
        if (raw.empty()) return;

        JsonDocument doc;
        if (deserializeJson(doc, raw) != DeserializationError::Ok) return;

        const char* cmd = doc["cmd"];
        bool val        = doc["val"];
        if (!cmd) return;

        if (strcmp(cmd, "r1") == 0) {
            gRelay1 = val;
            digitalWrite(RELAY1_PIN, val ? HIGH : LOW);
        } else if (strcmp(cmd, "r2") == 0) {
            gRelay2 = val;
            digitalWrite(RELAY2_PIN, val ? HIGH : LOW);
        }
    }
};

// =============================================================
//  BLE setup
// =============================================================
static void setupBle() {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    NimBLEService* svc = pServer->createService(NUS_SERVICE);

    pTxChar = svc->createCharacteristic(NUS_TX_CHAR,
        NIMBLE_PROPERTY::NOTIFY);

    NimBLECharacteristic* pRxChar = svc->createCharacteristic(NUS_RX_CHAR,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    pRxChar->setCallbacks(new RxCallbacks());

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE);
    adv->setScanResponse(true);
    adv->start();
}

// =============================================================
//  Stuur JSON naar app
// =============================================================
static void sendJson() {
    if (!gConnected || !pTxChar) return;

    char buf[96];
    snprintf(buf, sizeof(buf),
        "{\"v\":%.2f,\"i\":%.2f,\"p\":%.1f,\"soc\":%d,\"r1\":%s,\"r2\":%s}",
        gVoltage, gCurrent, gPower, gSoc,
        gRelay1 ? "true" : "false",
        gRelay2 ? "true" : "false");

    pTxChar->setValue((uint8_t*)buf, strlen(buf));
    pTxChar->notify();
}

// =============================================================
//  Arduino setup / loop
// =============================================================
void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);

    M5.Display.setRotation(3);   // Liggend
    M5.Display.setBrightness(80);
    M5.Display.setTextFont(1);

    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(10, 40);
    M5.Display.print("Twizy PowerBox");
    M5.Display.setCursor(10, 60);
    M5.Display.print("BLE opstarten...");

    pinMode(RELAY1_PIN, OUTPUT);
    pinMode(RELAY2_PIN, OUTPUT);
    digitalWrite(RELAY1_PIN, LOW);
    digitalWrite(RELAY2_PIN, LOW);

#if SENSOR_MODE == MODE_INA226
    ina226_init();
#elif SENSOR_MODE == MODE_ADC
    analogSetAttenuation(ADC_11db);
    analogSetWidth(12);
#endif

    setupBle();

    Serial.begin(115200);
    Serial.println("Twizy PowerBox gestart");
}

void loop() {
    M5.update();

    // Button A — toggle relais 1
    if (M5.BtnA.wasPressed()) {
        gRelay1 = !gRelay1;
        digitalWrite(RELAY1_PIN, gRelay1 ? HIGH : LOW);
    }

    // Button B — toggle relais 2
    if (M5.BtnB.wasPressed()) {
        gRelay2 = !gRelay2;
        digitalWrite(RELAY2_PIN, gRelay2 ? HIGH : LOW);
    }

    // Lees sensoren en stuur data elke 500ms
    if (millis() - gLastSend >= 500) {
        gLastSend = millis();
        readSensors();
        updateDisplay();
        sendJson();
    }
}
