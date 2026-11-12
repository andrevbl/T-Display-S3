#include <Arduino.h>
#include <LovyanGFX.h>
#include <WiFi.h>
#include <esp_sleep.h>

namespace {

constexpr char AP_SSID[] = "T-Display-S3-Message";
constexpr char AP_PASSWORD[] = "message123";
constexpr int POWER_ENABLE_PIN = 15;
constexpr int SLEEP_BUTTON_PIN = 14;
constexpr uint16_t TCP_PORT = 23;
constexpr size_t MESSAGE_CAPACITY = 256;
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 5000;
constexpr unsigned long HEARTBEAT_TIMEOUT_MS = 5000;
constexpr unsigned long BUTTON_DEBOUNCE_MS = 40;
constexpr char HEARTBEAT_PING[] = "\x01PING";
constexpr char HEARTBEAT_PONG[] = "\x01PONG";
constexpr int BATTERY_VOLTAGE_PIN = 4;
constexpr unsigned long BATTERY_UPDATE_INTERVAL_MS = 30000;
constexpr int SCREEN_WIDTH = 320;
constexpr int SCREEN_HEIGHT = 170;

class MessageDisplay : public lgfx::LGFX_Device {
public:
    MessageDisplay()
    {
        auto busConfig = _bus.config();
        busConfig.pin_wr = 8;
        busConfig.pin_rd = 9;
        busConfig.pin_rs = 7;
        busConfig.pin_d0 = 39;
        busConfig.pin_d1 = 40;
        busConfig.pin_d2 = 41;
        busConfig.pin_d3 = 42;
        busConfig.pin_d4 = 45;
        busConfig.pin_d5 = 46;
        busConfig.pin_d6 = 47;
        busConfig.pin_d7 = 48;
        _bus.config(busConfig);
        _panel.setBus(&_bus);

        auto panelConfig = _panel.config();
        panelConfig.pin_cs = 6;
        panelConfig.pin_rst = 5;
        panelConfig.pin_busy = -1;
        panelConfig.offset_rotation = 1;
        panelConfig.offset_x = 35;
        panelConfig.readable = false;
        panelConfig.invert = true;
        panelConfig.rgb_order = false;
        panelConfig.dlen_16bit = false;
        panelConfig.bus_shared = false;
        panelConfig.panel_width = 170;
        panelConfig.panel_height = 320;
        _panel.config(panelConfig);
        setPanel(&_panel);

        auto lightConfig = _light.config();
        lightConfig.pin_bl = 38;
        lightConfig.invert = false;
        lightConfig.freq = 22000;
        lightConfig.pwm_channel = 7;
        _light.config(lightConfig);
        _panel.setLight(&_light);
    }

private:
    lgfx::Bus_Parallel8 _bus;
    lgfx::Panel_ST7789 _panel;
    lgfx::Light_PWM _light;
};

MessageDisplay display;
WiFiServer server(TCP_PORT);
WiFiClient client;
char messageBuffer[MESSAGE_CAPACITY];
size_t messageLength = 0;
bool droppingLongMessage = false;
String latestMessage = "Aguardando mensagem...";
bool lastClientConnected = false;
unsigned long lastHeartbeatAt = 0;
bool awaitingHeartbeatResponse = false;
int batteryPercent = -1;
unsigned long lastBatteryUpdateAt = 0;
bool batterySampled = false;
int sleepButtonRawState = HIGH;
int sleepButtonStableState = HIGH;
unsigned long sleepButtonStateChangedAt = 0;

void enterDeepSleep()
{
    Serial.println("Entrando em deep sleep...");
    while (digitalRead(SLEEP_BUTTON_PIN) == LOW) {
        delay(10);
    }

    if (esp_sleep_enable_ext0_wakeup(
            static_cast<gpio_num_t>(SLEEP_BUTTON_PIN), 0) != ESP_OK) {
        Serial.println("Falha ao configurar o botao para acordar.");
        return;
    }

    client.stop();
    server.end();
    WiFi.mode(WIFI_OFF);
    display.sleep();
    delay(120);
    digitalWrite(POWER_ENABLE_PIN, LOW);
    Serial.flush();
    esp_deep_sleep_start();
}

void checkSleepButton()
{
    const unsigned long now = millis();
    const int rawState = digitalRead(SLEEP_BUTTON_PIN);
    if (rawState != sleepButtonRawState) {
        sleepButtonRawState = rawState;
        sleepButtonStateChangedAt = now;
    }

    if (rawState != sleepButtonStableState &&
        now - sleepButtonStateChangedAt >= BUTTON_DEBOUNCE_MS) {
        sleepButtonStableState = rawState;
        if (sleepButtonStableState == LOW) {
            enterDeepSleep();
        }
    }
}

int estimateBatteryPercent(uint32_t voltageMillivolts)
{
    constexpr uint16_t voltageTable[][2] = {
        {3300, 0}, {3610, 5}, {3690, 10}, {3710, 15}, {3730, 20},
        {3750, 25}, {3770, 30}, {3790, 35}, {3800, 40}, {3820, 45},
        {3840, 50}, {3850, 55}, {3870, 60}, {3910, 65}, {3950, 70},
        {3980, 75}, {4020, 80}, {4080, 85}, {4110, 90}, {4150, 95},
        {4200, 100}
    };

    if (voltageMillivolts <= voltageTable[0][0]) {
        return 0;
    }

    for (size_t i = 1; i < sizeof(voltageTable) / sizeof(voltageTable[0]); ++i) {
        if (voltageMillivolts <= voltageTable[i][0]) {
            const uint32_t lowerVoltage = voltageTable[i - 1][0];
            const uint32_t upperVoltage = voltageTable[i][0];
            const uint32_t lowerPercent = voltageTable[i - 1][1];
            const uint32_t upperPercent = voltageTable[i][1];
            return lowerPercent +
                   (voltageMillivolts - lowerVoltage) * (upperPercent - lowerPercent) /
                       (upperVoltage - lowerVoltage);
        }
    }

    return 100;
}

bool updateBatteryStatus()
{
    const unsigned long now = millis();
    if (batterySampled && now - lastBatteryUpdateAt < BATTERY_UPDATE_INTERVAL_MS) {
        return false;
    }

    uint32_t voltageMillivolts = 0;
    for (int i = 0; i < 8; ++i) {
        voltageMillivolts += analogReadMilliVolts(BATTERY_VOLTAGE_PIN) * 2;
        delay(2);
    }
    voltageMillivolts /= 8;

    const int previousPercent = batteryPercent;
    batteryPercent = voltageMillivolts <= 4300
                         ? estimateBatteryPercent(voltageMillivolts)
                         : -1;
    lastBatteryUpdateAt = millis();
    batterySampled = true;
    return batteryPercent != previousPercent;
}

void drawBatteryStatus()
{
    constexpr int BATTERY_AREA_LEFT = 230;
    constexpr int BATTERY_AREA_WIDTH = 82;
    constexpr int BATTERY_AREA_HEIGHT = 24;
    constexpr uint16_t BACKGROUND = 0x0841;
    constexpr uint16_t TEXT = 0xFFFF;

    display.fillRect(BATTERY_AREA_LEFT, 0, BATTERY_AREA_WIDTH,
                     BATTERY_AREA_HEIGHT, BACKGROUND);
    display.setFont(&fonts::Font2);
    display.setTextColor(TEXT, BACKGROUND);
    const String status = batteryPercent < 0
                              ? "--%"
                              : String(batteryPercent) + "%";
    display.drawString(status,
                       SCREEN_WIDTH - 8 - display.textWidth(status), 5);
}

void drawMessage(const String &message)
{
    constexpr uint16_t BACKGROUND = 0x0841;
    constexpr uint16_t ACCENT = 0x07FF;
    constexpr uint16_t TEXT = 0xFFFF;
    constexpr int TEXT_LEFT = 8;
    constexpr int TEXT_LEFT_MSG = 4;
    constexpr int TEXT_RIGHT = 8;
    constexpr int TEXT_RIGHT_MSG = 4;
    constexpr int FIRST_LINE_Y = 80;
    constexpr int LINE_GAP = 2;
    constexpr int MAX_LINES = 6;
    const int lineHeight = display.fontHeight() + LINE_GAP;

    display.fillScreen(BACKGROUND);
    display.setFont(&fonts::Font2);
    display.setTextColor(ACCENT, BACKGROUND);
    display.drawString("MessageReceiver", TEXT_LEFT, 5);
    drawBatteryStatus();
    display.setTextColor(TEXT, BACKGROUND);
    display.drawString(WiFi.softAPIP().toString() + " - " + (client && client.connected() ? "Conectado!" : "Aguardando cliente..."), TEXT_LEFT, 25);
    display.setTextColor(client && client.connected() ? ACCENT : TEXT, BACKGROUND);
    display.drawFastHLine(0, 42, SCREEN_WIDTH, 0x4208);

    display.setTextColor(TEXT, BACKGROUND);
    String line;
    int lineNumber = 0;
    int wordStart = 0;

    display.setFont(isDigitsOnly(message) ? &fonts::Font8 : &fonts::Font4);

    while (wordStart < message.length() && lineNumber < MAX_LINES) {
        while (wordStart < message.length() &&
               (message[wordStart] == ' ' || message[wordStart] == '\r' ||
                message[wordStart] == '\n')) {
            ++wordStart;
        }
        if (wordStart >= message.length()) {
            break;
        }

        int wordEnd = wordStart;
        while (wordEnd < message.length() && message[wordEnd] != ' ' &&
               message[wordEnd] != '\r' && message[wordEnd] != '\n') {
            ++wordEnd;
        }

        String word = message.substring(wordStart, wordEnd);
        String candidate = line.length() == 0 ? word : line + " " + word;
        if (line.length() != 0 &&
            display.textWidth(candidate) > SCREEN_WIDTH - TEXT_LEFT_MSG - TEXT_RIGHT_MSG) {
            display.drawString(line, TEXT_LEFT_MSG, FIRST_LINE_Y + lineNumber * lineHeight);
            ++lineNumber;
            line = word;
        } else {
            line = candidate;
        }
        wordStart = wordEnd;
    }

    if (lineNumber < MAX_LINES && line.length() != 0) {
        display.drawString(line, TEXT_LEFT_MSG, FIRST_LINE_Y + lineNumber * lineHeight);
    }
}

bool isDigitsOnly(const String &text)
{
    bool hasDigit = false;

    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];

        if (c >= '0' && c <= '9') {
            hasDigit = true;
        } else if (c != ' ') {
            return false;
        }
    }

    return hasDigit;
}

void showStatus(const char *status)
{
    Serial.println(status);
    display.fillScreen(TFT_BLACK);
    display.setFont(&fonts::Font4);
    display.setTextColor(0xFFFF, TFT_BLACK);
    display.drawString("MessageReceiver", 8, 8);
    display.drawString(status, 8, 38);
}

void finishMessage()
{
    messageBuffer[messageLength] = '\0';
    latestMessage = messageBuffer;
    Serial.println(latestMessage);
    messageLength = 0;
    drawMessage(latestMessage);
}

void resetPendingMessage()
{
    messageLength = 0;
    droppingLongMessage = false;
}

void receiveAvailableBytes()
{
    while (client && client.connected() && client.available()) {
        const char value = static_cast<char>(client.read());
        if (value == '\r') {
            continue;
        }
        if (value == '\n') {
            if (droppingLongMessage) {
                Serial.println("Mensagem rejeitada: limite máximo de tamanho excedido.");
                droppingLongMessage = false;
                messageLength = 0;
                client.println("ERRO: mensagem muito longa.");
            } else {
                if (messageLength != 0) {
                    messageBuffer[messageLength] = '\0';
                    if (strcmp(messageBuffer, HEARTBEAT_PONG) == 0) {
                        awaitingHeartbeatResponse = false;
                        lastHeartbeatAt = millis();
                        messageLength = 0;
                    } else {
                        finishMessage();
                        client.println("OK");
                    }
                } else {
                    client.println("OK");
                }
            }
            continue;
        }
        if (droppingLongMessage) {
            continue;
        }
        if (messageLength < MESSAGE_CAPACITY - 1) {
            messageBuffer[messageLength++] = value;
        } else {
            droppingLongMessage = true;
            messageLength = 0;
        }
    }
}

void serviceHeartbeat()
{
    if (!client || !client.connected()) {
        return;
    }

    const unsigned long now = millis();
    if (awaitingHeartbeatResponse) {
        if (now - lastHeartbeatAt >= HEARTBEAT_TIMEOUT_MS) {
            Serial.println("Cliente sem resposta ao heartbeat; desconectando.");
            client.stop();
            resetPendingMessage();
            awaitingHeartbeatResponse = false;
            drawMessage(latestMessage);
        }
        return;
    }

    if (now - lastHeartbeatAt >= HEARTBEAT_INTERVAL_MS) {
        client.println(HEARTBEAT_PING);
        lastHeartbeatAt = now;
        awaitingHeartbeatResponse = true;
    }
}

} // fim do namespace

void setup()
{
    Serial.begin(115200);
    pinMode(POWER_ENABLE_PIN, OUTPUT);
    digitalWrite(POWER_ENABLE_PIN, HIGH);
    pinMode(SLEEP_BUTTON_PIN, INPUT_PULLUP);
    sleepButtonRawState = digitalRead(SLEEP_BUTTON_PIN);
    sleepButtonStableState = sleepButtonRawState;
    sleepButtonStateChangedAt = millis();
    analogReadResolution(12);
    analogSetPinAttenuation(BATTERY_VOLTAGE_PIN, ADC_11db);
    display.init();
    display.setRotation(2);
    display.setBrightness(255);
    showStatus("Iniciando o Wi-Fi...");

    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
        showStatus("Falha ao iniciar o ponto de acesso Wi-Fi");
        return;
    }

    server.begin();
    server.setNoDelay(true);

    Serial.println();
    Serial.println("MessageReceiver pronto.");
    Serial.print("Rede Wi-Fi: ");
    Serial.println(AP_SSID);
    Serial.print("Senha do Wi-Fi: ");
    Serial.println(AP_PASSWORD);
    Serial.print("Conecte-se a ");
    Serial.print(WiFi.softAPIP());
    Serial.print(" na porta TCP ");
    Serial.println(TCP_PORT);

    drawMessage(latestMessage);
}

void loop()
{
    checkSleepButton();

    if (updateBatteryStatus()) {
        drawBatteryStatus();
    }

    WiFiClient incoming = server.accept();
    if (incoming) {
        if (client) {
            client.stop();
        }
        resetPendingMessage();
        client = incoming;
        lastHeartbeatAt = millis();
        awaitingHeartbeatResponse = false;
        Serial.println("Programa de envio conectado.");
        drawMessage(latestMessage);
    }

    receiveAvailableBytes();
    serviceHeartbeat();

    if (client && !client.connected()) {
        client.stop();
        resetPendingMessage();
        awaitingHeartbeatResponse = false;
        Serial.println("Programa de envio desconectado.");
        drawMessage(latestMessage);
    }

    const bool connected = WiFi.softAPgetStationNum() > 0;
    if (connected != lastClientConnected) {
        lastClientConnected = connected;
        Serial.println(connected ? "Dispositivo conectado ao Wi-Fi." : "Nenhum dispositivo conectado ao Wi-Fi.");
        drawMessage(latestMessage);
    }

    delay(2);
}
