// Прошивка T-Embed: Wi-Fi + WebSocket к серверу агента + голосовой ввод.
//
// Плата — второй клиент рядом с client/terminal.py, протокол тот же:
//   плата -> сервер:  hello, task, decision, cancel, audio_start, <байты звука>, audio_end
//   сервер -> плата:  status, confirm_request, result, error, transcript,
//                     speech_start, <байты озвучки>, speech_end
//
// Управление:
//   меню:      крутить — выбор задачи, клик — отправить
//   работа:    клик — отменить задачу (cancel)
//   карточка:  крутить — YES/NO, клик — решение (decision)
//   ответ:     крутить — прокрутка, клик — замолчать и назад в меню
//   удержание: запись голоса, звук идёт на сервер потоком, пока держишь.
//              В карточке игнорируется: голосом не подтверждаем (BRAINSTORM.md).
#include <Arduino.h>
#include <ArduinoJson.h>
#include <RotaryEncoder.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

#include "button.h"
#include "mic.h"
#include "pins.h"
#include "secrets.h"
#include "speaker.h"
#include "textview.h"

Mic mic;
bool micOk = false;
Speaker speaker;
bool speakerOk = false;
bool speechActive = false;   // между speech_start и speech_end: сервер ещё присылает озвучку
const uint32_t MAX_RECORD_MS = 30000;   // как MAX_SECONDS на сервере

TFT_eSPI tft;
U8g2_for_TFT_eSPI u8f;
TextView body(tft, u8f);         // основной текст: ответ модели, статус работы
TextView argsView(tft, u8f);     // аргументы тула в карточке подтверждения
RotaryEncoder encoder(ENCODER_INA, ENCODER_INB, RotaryEncoder::LatchMode::TWO03);
Button button(ENCODER_KEY);
WebSocketsClient ws;

// Голоса пока нет, поэтому задачи выбираются из списка
const char* PRESETS[] = {
    "Какие файлы есть в песочнице?",
    "Прочитай hello.txt и скажи, что в нём",
    "Запиши в файл tembed.txt текст: привет с T-Embed",
    "Какая последняя версия httpx на PyPI?",
};
const int PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

enum class Mode { Menu, Working, Confirm, Recording, Answer };
Mode mode = Mode::Menu;
int menuIndex = 0;
bool choiceYes = false;          // по умолчанию NO: безопасный выбор
String confirmId;                // id из confirm_request, вернём его в decision
uint32_t recordStartedAt = 0;
String statusLine = "запуск";
bool wifiUp = false, wsUp = false;

const int W = 320, H = 170;      // после setRotation(3) экран горизонтальный
const int BODY_Y = 34, STATUS_H = 22;

// ---------- экран ----------

void useText(uint16_t color) {
    u8f.setFont(u8g2_font_9x15_t_cyrillic);
    u8f.setFontMode(1);
    u8f.setForegroundColor(color);
}

void drawHeader(const char* title, uint16_t color) {
    tft.fillRect(0, 0, W, 28, color);
    tft.setTextColor(TFT_BLACK, color);
    tft.drawString(title, 8, 6, 2);
    // Справа индикатор связи: W = Wi-Fi, S = сервер
    tft.drawString(wifiUp ? "W" : "w", W - 40, 6, 2);
    tft.drawString(wsUp ? "S" : "s", W - 22, 6, 2);
}

void drawStatus() {
    tft.fillRect(0, H - STATUS_H, W, STATUS_H, TFT_DARKGREY);
    useText(TFT_WHITE);
    u8f.drawUTF8(8, H - 6, fitUtf8(u8f, statusLine, W - 16).c_str());
}

void drawMenuItems() {
    const int itemH = 24;
    tft.fillRect(0, BODY_Y, W, H - STATUS_H - BODY_Y, TFT_BLACK);
    for (int i = 0; i < PRESET_COUNT; i++) {
        int y = BODY_Y + 2 + i * itemH;
        bool sel = i == menuIndex;
        if (sel) tft.fillRoundRect(4, y, W - 8, itemH - 2, 4, TFT_NAVY);
        useText(sel ? TFT_YELLOW : TFT_LIGHTGREY);
        u8f.drawUTF8(12, y + 16, fitUtf8(u8f, PRESETS[i], W - 28).c_str());
    }
}

void drawMenu() {
    tft.fillScreen(TFT_BLACK);
    drawHeader("AGENT  click: send  hold: voice", TFT_CYAN);
    drawMenuItems();
    drawStatus();
}

void drawWorking(const String& text) {
    tft.fillScreen(TFT_BLACK);
    drawHeader("WORKING  click: cancel", TFT_YELLOW);
    body.setText(text);
    body.draw();
    drawStatus();
}

void drawChoice() {
    int y = 100, bw = 120, bh = 36;
    uint16_t yesBg = choiceYes ? TFT_GREEN : TFT_BLACK;
    uint16_t noBg = choiceYes ? TFT_BLACK : TFT_RED;
    tft.fillRoundRect(20, y, bw, bh, 6, yesBg);
    tft.drawRoundRect(20, y, bw, bh, 6, TFT_GREEN);
    tft.fillRoundRect(W - 20 - bw, y, bw, bh, 6, noBg);
    tft.drawRoundRect(W - 20 - bw, y, bw, bh, 6, TFT_RED);
    tft.setTextColor(choiceYes ? TFT_BLACK : TFT_GREEN, yesBg);
    tft.drawCentreString("YES", 20 + bw / 2, y + 6, 4);
    tft.setTextColor(choiceYes ? TFT_RED : TFT_BLACK, noBg);
    tft.drawCentreString("NO", W - 20 - bw / 2, y + 6, 4);
}

void drawConfirm(const String& tool, const String& argsText, int timeoutS) {
    tft.fillScreen(TFT_BLACK);
    String title = "CONFIRM  " + tool + "  " + String(timeoutS) + "s";
    drawHeader(title.c_str(), TFT_ORANGE);
    argsView.setText(argsText);   // реальный вызов тула, а не пересказ модели
    argsView.draw();
    drawChoice();
}

void drawRecording() {
    tft.fillScreen(TFT_BLACK);
    drawHeader("RECORDING   release: send", TFT_RED);
    tft.fillCircle(30, 70, 12, TFT_RED);
    useText(TFT_WHITE);
    u8f.drawUTF8(52, 76, "говори...");
}

// Индикатор громкости и таймер. peak — самый громкий сэмпл за последний кусок (0..32767)
void drawLevel(int peak, uint32_t ms) {
    const int x = 20, y = 100, w = W - 40, h = 16;
    int filled = min(w, (int)((long)w * peak / 12000));   // 12000 — уже громкая речь вблизи
    uint16_t color = peak > 30000 ? TFT_RED : peak > 1500 ? TFT_GREEN : TFT_DARKGREEN;
    tft.fillRect(x, y, filled, h, color);
    tft.fillRect(x + filled, y, w - filled, h, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    char t[16];
    snprintf(t, sizeof(t), "%2lu / %lu s  ", ms / 1000, MAX_RECORD_MS / 1000);
    tft.drawString(t, x, y + 26, 2);
}

void drawAnswer(const String& text, bool isError) {
    tft.fillScreen(TFT_BLACK);
    drawHeader(isError ? "ERROR   click: back" : "ANSWER  rotate: scroll  click: stop/back",
               isError ? TFT_RED : TFT_GREEN);
    body.setText(text);
    body.draw();
    drawStatus();
}

void setStatus(const String& s) {
    statusLine = s;
    Serial.println("[status] " + s);
    if (mode != Mode::Recording && mode != Mode::Confirm) drawStatus();
}

void redrawHeader() {
    // Перерисовать текущий экран целиком проще, чем помнить заголовок каждого
    if (mode == Mode::Menu) drawMenu();
}

// ---------- отправка на сервер ----------

void sendJson(JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    ws.sendTXT(out);
    Serial.println("[ws ->] " + out);
}

void sendTask(const char* text) {
    JsonDocument doc;
    doc["type"] = "task";
    doc["text"] = text;
    sendJson(doc);
}

void sendDecision(bool approve) {
    JsonDocument doc;
    doc["type"] = "decision";
    doc["id"] = confirmId;
    doc["approve"] = approve;
    sendJson(doc);
}

void sendType(const char* type) {
    JsonDocument doc;
    doc["type"] = type;
    sendJson(doc);
}

void sendCancel() { sendType("cancel"); }

// ---------- сообщения от сервера ----------

String argsToText(JsonObject args) {
    String s;
    for (JsonPair kv : args) {
        if (s.length()) s += "\n";
        s += kv.key().c_str();
        s += ": ";
        if (kv.value().is<const char*>()) {
            s += kv.value().as<const char*>();
        } else {
            String v;
            serializeJson(kv.value(), v);
            s += v;
        }
    }
    return s.length() ? s : "(без аргументов)";
}

void onServerMessage(const char* payload, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) {
        setStatus("плохой JSON от сервера");
        return;
    }
    String type = doc["type"] | "";
    String text = doc["text"] | "";

    if (type == "status") {
        // В карточке status значит, что подтверждение закрыто (например, таймаут)
        if (mode == Mode::Confirm) {
            mode = Mode::Working;
            confirmId = "";
        }
        setStatus(text);
        if (mode == Mode::Working) drawWorking(text);
    } else if (type == "transcript") {
        // Показываем, что сервер услышал: если распознал криво, сразу видно
        setStatus("услышал");
        drawWorking("Ты: " + text + "\n\nдумаю...");
    } else if (type == "confirm_request") {
        confirmId = doc["id"] | "";
        choiceYes = false;
        mode = Mode::Confirm;
        drawConfirm(doc["tool"] | "?", argsToText(doc["args"].as<JsonObject>()), doc["timeout_s"] | 0);
    } else if (type == "speech_start") {
        speaker.start(doc["sample_rate"] | 22050);
        speechActive = true;
    } else if (type == "speech_end") {
        speechActive = false;      // буфер доиграет сам
    } else if (type == "result" || type == "error") {
        bool isError = type == "error";
        // error без активной задачи (например, "task already running") показываем в статусе
        if (isError && mode == Mode::Menu) {
            setStatus("ошибка: " + text);
            return;
        }
        mode = Mode::Answer;
        setStatus(isError ? "ошибка" : "готово");
        drawAnswer(text, isError);
    }
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
    case WStype_CONNECTED: {
        wsUp = true;
        // Сообщаем серверу, что умеем играть звук: без hello озвучку не пришлют
        JsonDocument hello;
        hello["type"] = "hello";
        hello["speech"] = speakerOk;
        sendJson(hello);
        setStatus("сервер подключён");
        redrawHeader();
        break;
    }
    case WStype_DISCONNECTED:
        if (wsUp) {
            // Сервер при обрыве отменяет задачу, поэтому и мы возвращаемся в меню.
            // Висящее подтверждение не "досылаем": после переподключения оно недействительно.
            wsUp = false;
            confirmId = "";
            speechActive = false;
            speaker.stop();
            mode = Mode::Menu;
            setStatus("связь с сервером потеряна");
            drawMenu();
        }
        break;
    case WStype_TEXT:
        onServerMessage((const char*)payload, length);
        break;
    case WStype_BIN:
        if (speechActive) speaker.push(payload, length);
        break;
    default:
        break;
    }
}

// ---------- энкодер и кнопка ----------

void onRotate(int dir) {
    switch (mode) {
    case Mode::Menu:
        menuIndex = (menuIndex + dir + PRESET_COUNT) % PRESET_COUNT;
        drawMenuItems();
        break;
    case Mode::Confirm:
        choiceYes = !choiceYes;   // два варианта: любой поворот переключает
        drawChoice();
        break;
    case Mode::Answer:
        if (body.scroll(dir)) body.draw();
        break;
    default:
        break;
    }
}

// Замолчать. Если сервер ещё присылает озвучку, его задача не закончена:
// отменяем её, иначе следующая задача получит "task already running".
void stopSpeech() {
    if (speechActive) {
        sendCancel();
        speechActive = false;
    }
    speaker.stop();
}

void startRecording() {
    if (!micOk) {
        setStatus("микрофон не запустился");
        return;
    }
    if (!wsUp) {
        setStatus("нет связи с сервером");
        return;
    }
    stopSpeech();                // не записывать собственный голос агента
    mic.flush();                 // старый звук из буферов не нужен
    sendType("audio_start");
    mode = Mode::Recording;
    recordStartedAt = millis();
    drawRecording();
    drawLevel(0, 0);
}

void finishRecording() {
    sendType("audio_end");
    float sec = (millis() - recordStartedAt) / 1000.0f;
    mode = Mode::Working;
    setStatus("записано " + String(sec, 1) + " с");
    drawWorking("распознаю речь...");
}

// Вызывается из loop, пока идёт запись: забрать звук и сразу отправить
void pumpAudio() {
    static int16_t buf[1024];             // 1024 сэмпла = 64 мс звука, 2 КБ на кадр
    static uint32_t lastDraw = 0;
    static int peak = 0;

    size_t n = mic.read(buf, 1024);
    if (n) {
        ws.sendBIN((uint8_t*)buf, n * sizeof(int16_t));
        for (size_t i = 0; i < n; i++) peak = max(peak, abs((int)buf[i]));
    }

    uint32_t ms = millis() - recordStartedAt;
    if (millis() - lastDraw > 100) {      // экран 10 раз в секунду хватает
        drawLevel(peak, ms);
        lastDraw = millis();
        peak = 0;
    }
    if (ms >= MAX_RECORD_MS) finishRecording();   // кнопку держат слишком долго
}

void onButton(ButtonEvent ev) {
    switch (mode) {
    case Mode::Menu:
        if (ev == ButtonEvent::Click) {
            if (!wsUp) {
                setStatus("нет связи с сервером");
                return;
            }
            sendTask(PRESETS[menuIndex]);
            mode = Mode::Working;
            setStatus("отправлено");
            drawWorking(PRESETS[menuIndex]);
        } else if (ev == ButtonEvent::HoldStart) {
            startRecording();
        }
        break;

    case Mode::Working:
        if (ev == ButtonEvent::Click) {
            sendCancel();
            setStatus("отмена...");
        }
        break;

    case Mode::Confirm:
        if (ev == ButtonEvent::Click) {
            sendDecision(choiceYes);
            confirmId = "";
            mode = Mode::Working;
            setStatus(choiceYes ? "разрешено" : "отклонено");
            drawWorking(choiceYes ? "разрешено, выполняю..." : "отклонено, жду ответ модели...");
        }
        break;

    case Mode::Recording:
        if (ev == ButtonEvent::HoldEnd) finishRecording();
        break;

    case Mode::Answer:
        if (ev == ButtonEvent::Click) {
            stopSpeech();
            mode = Mode::Menu;
            drawMenu();
        } else if (ev == ButtonEvent::HoldStart) {
            startRecording();
        }
        break;
    }
}

// ---------- setup / loop ----------

void setup() {
    Serial.begin(115200);

    pinMode(BOARD_PWR_EN, OUTPUT);       // без этого экран и звук без питания
    digitalWrite(BOARD_PWR_EN, HIGH);

    // Экран, SD, CC1101 и NRF24 сидят на одной шине SPI. Пока CS не подняты,
    // они могут отвечать одновременно с экраном и портить картинку.
    for (int cs : {BOARD_SD_CS, BOARD_LORA_CS, BOARD_NRF24_CS}) {
        pinMode(cs, OUTPUT);
        digitalWrite(cs, HIGH);
    }

    tft.begin();
    tft.setRotation(3);
    u8f.begin(tft);
    body.setArea(8, BODY_Y, W - 12, H - BODY_Y - STATUS_H - 4);
    argsView.setArea(8, BODY_Y, W - 12, 60);   // над кнопками YES/NO
    button.begin();
    micOk = mic.begin();
    Serial.println(micOk ? "mic: ok" : "mic: FAILED");
    speakerOk = speaker.begin();
    Serial.printf("speaker: %s (psram %u KB free)\n", speakerOk ? "ok" : "FAILED", ESP.getFreePsram() / 1024);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    statusLine = "подключаюсь к Wi-Fi...";
    drawMenu();

    // Токен в заголовке, а не в адресе: адрес сервер пишет в лог.
    // Без токена сервер не пустит плату из сети.
    ws.begin(AGENT_HOST, AGENT_PORT, "/ws");
    ws.setExtraHeaders("Authorization: Bearer " AGENT_TOKEN);
    ws.onEvent(onWsEvent);
    ws.setReconnectInterval(3000);
    ws.enableHeartbeat(15000, 3000, 2);   // заметить обрыв, даже если сервер молчит

    Serial.println("hardware-agent firmware: step 4 (voice in + speech out)");
}

void loop() {
    ws.loop();
    if (mode == Mode::Recording) pumpAudio();

    bool up = WiFi.status() == WL_CONNECTED;
    if (up != wifiUp) {
        wifiUp = up;
        setStatus(up ? "Wi-Fi: " + WiFi.localIP().toString() : "Wi-Fi потерян");
        redrawHeader();
    }

    encoder.tick();
    static long lastPos = 0;
    long pos = encoder.getPosition();
    if (pos != lastPos) {
        onRotate(pos > lastPos ? 1 : -1);
        lastPos = pos;
    }

    ButtonEvent ev = button.poll();
    if (ev != ButtonEvent::None) onButton(ev);

    delay(1);
}
