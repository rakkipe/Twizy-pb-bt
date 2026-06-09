#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <driver/twai.h>   // ESP32 ingebouwde CAN-controller (TWAI)

// =============================================================
//  CONFIGURATIE
// =============================================================

#define DEVICE_NAME  "TwizyPB"

// --- Modus ---
#define MODE_CAN   0   // Renault Twizy CAN-bus (standaard)
#define MODE_DEMO  1   // Gesimuleerde testdata (geen hardware nodig)

#define SENSOR_MODE  MODE_CAN

// --- CAN pinnen ---
// M5Stack CAN Unit op Grove-poort: TX=G32, RX=G33
// Of eigen SN65HVD230/TJA1050 module
#define CAN_TX_PIN  GPIO_NUM_32
#define CAN_RX_PIN  GPIO_NUM_33

// --- GPIO voor relais (pas aan naar jouw bedrading) ---
#define RELAY1_PIN  26
#define RELAY2_PIN  0

// =============================================================
//  TWIZY CAN FRAME IDs
//  Bron: OVMS project + community reverse engineering
//  500 kbps, standaard 11-bit identifiers
// =============================================================

// 0x424  — SoC, laadstatus (elke ~100ms)
//   byte 0 bits[5:0] = SoC 0-100%
//   byte 0 bit[6]    = ready (contactsleutel aan)
//   byte 0 bit[7]    = fout
#define CAN_ID_SOC      0x424

// 0x155  — Rijsnelheid (elke ~10ms)
//   bytes[0:1] bits[11:0] = snelheid in 0.01 m/s
#define CAN_ID_SPEED    0x155

// 0x59E  — Batterijspanning + stroom (elke ~100ms)
//   bytes[0:1] int16 big-endian = stroom in 0.25A (negatief = ontladen)
//   bytes[2:3] uint16 big-endian = spanning in 0.5V
//
//   OPMERKING: byte-volgorde varieert per bouwjaar.
//   Gebruik de sniffer hieronder (zie Serial output) om te verifiëren.
#define CAN_ID_BATT     0x59E

// 0x3F2  — Batterijtemperatuur (optioneel)
//   byte 0 = temperatuur in °C + 40 offset
#define CAN_ID_TEMP     0x3F2

// =============================================================

// NUS UUIDs (Nordic UART Service — zelfde als Android app)
#define NUS_SERVICE  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_CHAR  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_CHAR  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// --- Globale staat ---
static float    gVoltage = 0, gCurrent = 0, gPower = 0;
static int      gSoc = 0;
static float    gSpeed = 0;
static int      gTemp = 0;
static bool     gReady = false;
static bool     gRelay1 = false, gRelay2 = false;
static bool     gBleConnected = false;
static bool     gCanOk = false;
static uint32_t gLastSend = 0;
static uint32_t gLastCanMsg = 0;

static NimBLEServer*         pServer  = nullptr;
static NimBLECharacteristic* pTxChar  = nullptr;

// =============================================================
//  CAN-bus initialisatie
// =============================================================
static bool canInit() {
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_LISTEN_ONLY);
    // LISTEN_ONLY = leest alleen, verstuurt geen ACK → veilig op OBD2-poort
    // Verander naar TWAI_MODE_NORMAL als je ook wil schrijven (relais via CAN)

    twai_timing_config_t  t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t  f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
    if (twai_start() != ESP_OK) return false;
    return true;
}

// =============================================================
//  CAN frame verwerking
// =============================================================
static void processCanFrame(uint32_t id, uint8_t* d, uint8_t len) {
    gLastCanMsg = millis();
    gCanOk = true;

    switch (id) {

        case CAN_ID_SOC:
            if (len >= 1) {
                gSoc   = d[0] & 0x3F;        // bits 5:0 = SoC%
                gReady = (d[0] >> 6) & 0x01;  // bit 6 = contactsleutel aan
            }
            break;

        case CAN_ID_BATT:
            if (len >= 4) {
                // Stroom: int16 big-endian, schaal 0.25A
                int16_t raw_i = (int16_t)((d[0] << 8) | d[1]);
                gCurrent = raw_i * 0.25f;

                // Spanning: uint16 big-endian, schaal 0.5V
                uint16_t raw_v = ((uint16_t)d[2] << 8) | d[3];
                gVoltage = raw_v * 0.5f;

                gPower = gVoltage * gCurrent;
            }
            break;

        case CAN_ID_SPEED:
            if (len >= 2) {
                uint16_t raw = ((uint16_t)(d[1] & 0x0F) << 8) | d[0];
                gSpeed = raw * 0.01f * 3.6f;  // m/s → km/h
            }
            break;

        case CAN_ID_TEMP:
            if (len >= 1) {
                gTemp = (int)d[0] - 40;
            }
            break;

        default:
            // Sniffer: print onbekende frames op Serial voor debugging
            // Verwijder commentaar om alle frames te zien:
            // Serial.printf("CAN 0x%03X [%d]:", id, len);
            // for (int i = 0; i < len; i++) Serial.printf(" %02X", d[i]);
            // Serial.println();
            break;
    }
}

// =============================================================
//  Demo modus — gesimuleerde waarden
// =============================================================
#if SENSOR_MODE == MODE_DEMO
static void updateDemo() {
    static float phase = 0;
    phase += 0.05f;
    gVoltage = 52.0f + 2.0f * sinf(phase * 0.3f);
    gCurrent = -(8.0f + 4.0f * sinf(phase));   // negatief = ontladen
    gPower   = gVoltage * gCurrent;
    gSoc     = 75;
    gSpeed   = 30.0f + 10.0f * sinf(phase * 0.2f);
    gReady   = true;
    gCanOk   = true;
}
#endif

// =============================================================
//  Scherm update
// =============================================================
static void updateDisplay() {
    auto& lcd = M5.Display;
    lcd.fillScreen(TFT_BLACK);

    // Statusbalk
    lcd.setTextSize(1);
    lcd.setCursor(2, 2);
    lcd.setTextColor(gBleConnected ? TFT_GREEN : TFT_ORANGE);
    lcd.printf("BT:%s", gBleConnected ? "OK" : "--");
    lcd.setTextColor(gCanOk ? TFT_GREEN : TFT_RED);
    lcd.printf("  CAN:%s", gCanOk ? "OK" : "GEEN");
    lcd.setTextColor(TFT_WHITE);
    lcd.printf("  %s", gReady ? "AAN" : "UIT");

    // Spanning + Stroom
    lcd.setTextColor(TFT_CYAN);
    lcd.setTextSize(2);
    lcd.setCursor(2, 20);
    lcd.printf("%.1fV", gVoltage);
    lcd.setCursor(85, 20);
    // Stroom: negatief = ontladen (normaal rijden), positief = laden
    lcd.printf("%.1fA", fabsf(gCurrent));

    // Vermogen + SoC
    lcd.setTextColor(TFT_YELLOW);
    lcd.setCursor(2, 47);
    lcd.printf("%.0fW", fabsf(gPower));

    uint16_t socColor = gSoc > 40 ? TFT_GREEN : (gSoc > 15 ? TFT_YELLOW : TFT_RED);
    lcd.setTextColor(socColor);
    lcd.setCursor(85, 47);
    lcd.printf("%d%%", gSoc);

    // Snelheid + temperatuur
    lcd.setTextColor(TFT_WHITE);
    lcd.setTextSize(1);
    lcd.setCursor(2, 75);
    lcd.printf("%.0f km/h   %d C", gSpeed, gTemp);

    // Relais
    lcd.setCursor(2, 88);
    lcd.printf("R1:%s  R2:%s",
        gRelay1 ? "AAN" : "UIT",
        gRelay2 ? "AAN" : "UIT");
}

// =============================================================
//  BLE callbacks
// =============================================================
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*) override    { gBleConnected = true;  }
    void onDisconnect(NimBLEServer*) override {
        gBleConnected = false;
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
        bool val = doc["val"];
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
    pTxChar = svc->createCharacteristic(NUS_TX_CHAR, NIMBLE_PROPERTY::NOTIFY);

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
//  Stuur JSON naar Android app
// =============================================================
static void sendJson() {
    if (!gBleConnected || !pTxChar) return;
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"v\":%.2f,\"i\":%.2f,\"p\":%.1f,\"soc\":%d"
        ",\"spd\":%.1f,\"tmp\":%d,\"r1\":%s,\"r2\":%s}",
        gVoltage, gCurrent, gPower, gSoc,
        gSpeed, gTemp,
        gRelay1 ? "true" : "false",
        gRelay2 ? "true" : "false");
    pTxChar->setValue((uint8_t*)buf, strlen(buf));
    pTxChar->notify();
}

// =============================================================
//  Setup
// =============================================================
void setup() {
    Serial.begin(115200);

    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(3);
    M5.Display.setBrightness(80);
    M5.Display.setTextFont(1);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(5, 35);
    M5.Display.print("Twizy PowerBox");
    M5.Display.setCursor(5, 55);
    M5.Display.print("Opstarten...");

    pinMode(RELAY1_PIN, OUTPUT);
    pinMode(RELAY2_PIN, OUTPUT);
    digitalWrite(RELAY1_PIN, LOW);
    digitalWrite(RELAY2_PIN, LOW);

#if SENSOR_MODE == MODE_CAN
    M5.Display.setCursor(5, 70);
    if (canInit()) {
        M5.Display.setTextColor(TFT_GREEN);
        M5.Display.print("CAN: OK (500kbps)");
        Serial.println("CAN geinitialiseerd op 500kbps");
    } else {
        M5.Display.setTextColor(TFT_RED);
        M5.Display.print("CAN: FOUT!");
        Serial.println("CAN initialisatie mislukt - controleer bedrading");
    }
#else
    M5.Display.setCursor(5, 70);
    M5.Display.setTextColor(TFT_YELLOW);
    M5.Display.print("DEMO modus");
#endif

    setupBle();
    delay(1000);
}

// =============================================================
//  Loop
// =============================================================
void loop() {
    M5.update();

    // Button A — toggle relais 1
    if (M5.BtnA.wasPressed()) {
        gRelay1 = !gRelay1;
        digitalWrite(RELAY1_PIN, gRelay1 ? HIGH : LOW);
        Serial.printf("Relais 1: %s\n", gRelay1 ? "AAN" : "UIT");
    }
    // Button B — toggle relais 2
    if (M5.BtnB.wasPressed()) {
        gRelay2 = !gRelay2;
        digitalWrite(RELAY2_PIN, gRelay2 ? HIGH : LOW);
        Serial.printf("Relais 2: %s\n", gRelay2 ? "AAN" : "UIT");
    }

#if SENSOR_MODE == MODE_CAN
    // Lees beschikbare CAN frames (non-blocking)
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {
        if (!(msg.flags & TWAI_MSG_FLAG_EXTD)) {   // alleen standaard 11-bit
            processCanFrame(msg.identifier, msg.data, msg.data_length_code);
        }
    }
    // CAN time-out detectie (geen frames > 3s = contact uit of fout)
    if (gCanOk && (millis() - gLastCanMsg > 3000)) {
        gCanOk  = false;
        gReady  = false;
        gSpeed  = 0;
    }
#else
    updateDemo();
#endif

    // Stuur data elke 500ms
    if (millis() - gLastSend >= 500) {
        gLastSend = millis();
        updateDisplay();
        sendJson();
    }
}
