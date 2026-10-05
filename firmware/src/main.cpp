// Прошивка T-Embed: голосовой агент с лицом.
//
// Плата — клиент сервера агента (как client/terminal.py), протокол:
//   плата -> сервер:  hello, decision, cancel, audio_start, <байты звука>, audio_end
//   сервер -> плата:  status, confirm_request, result, error, transcript,
//                     speech_start, <байты озвучки>, speech_end
//
// Экраны и управление:
//   главный:   лицо агента. Удержание — говорить. Крутить — история
//   слушаю:    пока держишь кнопку. Отпустил — отправить
//   думаю:     клик — отменить задачу
//   карточка:  крутить — Да/Нет, клик — решение. Удержание игнорируется:
//              голосом не подтверждаем (BRAINSTORM.md)
//   ответ:     крутить — прокрутка, клик — замолчать и на главный
//   меню:      крутить с главного. «< назад», «Проект ▸», последние вопросы проекта
//              (с сервера, переживают сон). Клик по вопросу — открыть ответ
//   проекты:   «< назад», «+ новый проект», список. Клик — переключиться,
//              удержание на проекте — «Удалить?» Да/Нет (в корзину, «общее» нельзя)
//   боковая кнопка: уснуть сейчас
//
// Питание: без дела 30 с — экран тускнеет, 1 мин — гаснет (связь остаётся),
// 5 мин — глубокий сон (только от батареи). Будит любая кнопка.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <RotaryEncoder.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <vector>

#include "battery.h"
#include "button.h"
#include "face.h"
#include "mic.h"
#include "pins.h"
#include "power.h"
#include "secrets.h"
#include "speaker.h"
#include "textview.h"
#include "theme.h"

using namespace theme;

// ---------- железо ----------

TFT_eSPI tft;
U8g2_for_TFT_eSPI u8f;
Face face(tft);
RotaryEncoder encoder(ENCODER_INA, ENCODER_INB, RotaryEncoder::LatchMode::TWO03);
Button button(ENCODER_KEY);
WebSocketsClient ws;
Mic mic;
Speaker speaker;
Battery battery;
Power power;
Button userKey(BOARD_USER_KEY);
bool micOk = false, speakerOk = false, faceOk = false;

// ---------- состояние ----------

enum class Mode { Home, Listen, Think, Confirm, Answer, History, Projects, Delete };
Mode mode = Mode::Home;

bool wifiUp = false, wsUp = false;
bool speechActive = false;       // между speech_start и speech_end
BatteryInfo bat;

bool choiceYes = false;          // по умолчанию «Нет»: безопасный выбор
String confirmId, confirmTool, confirmArgs;
uint32_t recordStartedAt = 0;
const uint32_t MAX_RECORD_MS = 30000;   // как MAX_SECONDS на сервере
String question;                 // что услышал сервер (transcript)
String thinking = "";            // что агент делает сейчас, по-русски
bool answerIsError = false;

// История и проекты хранит сервер (sandbox/projects), плата только показывает
struct Entry { String q, a; bool err; };
std::vector<Entry> history;      // новые сверху
bool historyLoaded = false;
int historyIndex = 0;            // 0 — «назад», 1 — «Проект», дальше вопросы
const int HISTORY_FIRST = 2;

struct ProjectItem { String name; int turns; };
std::vector<ProjectItem> projects;
bool projectsLoaded = false;
int projectIndex = 0;            // 0 — «назад», 1 — «+ новый», дальше проекты
const int PROJECTS_FIRST = 2;
String projectName;              // текущий проект, приходит в сообщении project
String deleteName;               // какой проект спрашиваем удалить

// Питание
const uint32_t DIM_MS = 30 * 1000;
const uint32_t OFF_MS = 60 * 1000;
const uint32_t SLEEP_MS = 5 * 60 * 1000;
const uint8_t DIM_LEVEL = 40;    // из 255
uint32_t lastActivity = 0;
bool screenOff = false;
String currentAnswer;            // чтобы перерисовать ответ, когда экран проснётся

// ---------- раскладка экрана ----------

const int W = 320, H = 170;      // после setRotation(3) экран горизонтальный
const int BAR_H = 22;            // верхняя панель
const int FACE_X = 6, FACE_Y = 34;
const int COL_X = 145, COL_W = W - COL_X - 6;   // правая колонка рядом с лицом

TextView body(tft, u8f);         // ответ на весь экран
TextView side(tft, u8f);         // мелкий текст в правой колонке
TextView argsView(tft, u8f);     // аргументы тула в карточке

const uint8_t* FONT_BIG = u8g2_font_10x20_t_cyrillic;
const uint8_t* FONT = u8g2_font_9x15_t_cyrillic;

void text(int x, int y, const String& s, uint16_t color, const uint8_t* font = FONT) {
    u8f.setFont(font);
    u8f.setFontMode(1);
    u8f.setForegroundColor(color);
    u8f.drawUTF8(x, y, s.c_str());
}

// Текст, обрезанный по ширине. Шрифт выбираем ДО измерения: ширина зависит от шрифта,
// а без выбранного шрифта библиотека обращается к пустому указателю и плата падает.
void textFit(int x, int y, const String& s, int maxW, uint16_t color, const uint8_t* font = FONT) {
    u8f.setFont(font);
    text(x, y, fitUtf8(u8f, s, maxW), color, font);
}

// ---------- верхняя панель: Wi-Fi, сервер, батарея ----------

void drawWifi(int x, int y) {
    int bars = 0;
    if (wifiUp) {
        int rssi = WiFi.RSSI();            // сила сигнала, дБм: ближе к 0 — лучше
        bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
    }
    for (int i = 0; i < 4; i++) {
        int h = 3 + i * 3;
        tft.fillRect(x + i * 5, y + 12 - h, 3, h, i < bars ? TEXT : LINE);
    }
}

void drawBattery(int x, int y) {
    if (!bat.ok) {
        tft.setTextColor(DIM, PANEL);
        tft.drawString("--%", x - 28, y + 1, 2);
        tft.drawRoundRect(x, y + 2, 24, 12, 2, DIM);
        return;
    }
    uint16_t c = bat.percent > 40 ? BAT_OK : bat.percent > 15 ? BAT_MID : BAT_LOW;
    char pct[6];
    snprintf(pct, sizeof(pct), "%d%%", bat.percent);
    tft.setTextColor(TEXT, PANEL);
    tft.drawRightString(pct, x - 4, y + 1, 2);
    tft.drawRoundRect(x, y + 2, 24, 12, 2, TEXT);
    tft.fillRect(x + 24, y + 5, 2, 6, TEXT);                    // «носик» батарейки
    int fill = 20 * bat.percent / 100;
    tft.fillRect(x + 2, y + 4, fill, 8, c);
    if (bat.charging || bat.full || bat.usb) {
        // Молния поверх батарейки: жёлтая — заряжается, зелёная — от USB, но уже полная
        uint16_t bolt = bat.charging ? THINK : BAT_OK;
        tft.fillTriangle(x + 13, y + 3, x + 8, y + 9, x + 12, y + 9, bolt);
        tft.fillTriangle(x + 11, y + 13, x + 16, y + 7, x + 12, y + 7, bolt);
    }
}

void drawTopBar() {
    tft.fillRect(0, 0, W, BAR_H, PANEL);
    tft.drawFastHLine(0, BAR_H, W, LINE);
    // Точка связи с сервером и текущий проект
    tft.fillCircle(10, 11, 4, wsUp ? SPEAK : (wifiUp ? THINK : ERR));
    String title = projectName.length() ? projectName : String("агент");
    textFit(20, 16, title, W - 140, DIM);
    drawWifi(W - 112, 4);
    drawBattery(W - 32, 3);
}

// ---------- экраны ----------

String friendlyTool(const String& tool) {
    if (tool == "write_file") return "Записать файл";
    if (tool == "read_file") return "Прочитать файл";
    if (tool == "list_tree") return "Посмотреть файлы";
    if (tool == "web_search") return "Поиск в интернете";
    if (tool == "fetch_url") return "Открыть страницу";
    if (tool == "open_url") return "Открыть ссылку на ПК";
    if (tool == "open_app") return "Запустить программу";
    return tool;
}

// Статусы сервера на английском — переводим в то, что агент «делает»
String friendlyStatus(const String& s) {
    if (s.startsWith("running ")) {
        String tool = s.substring(8);
        if (tool == "list_tree") return "смотрю файлы";
        if (tool == "read_file") return "читаю файл";
        if (tool == "write_file") return "записываю файл";
        if (tool == "web_search") return "ищу в интернете";
        if (tool == "fetch_url") return "открываю страницу";
        if (tool == "remember") return "запоминаю";
        if (tool == "read_output") return "дочитываю";
        if (tool == "weather") return "смотрю погоду";
        if (tool == "open_search") return "открываю поиск на ПК";
        if (tool == "youtube") return "ищу на YouTube";
        if (tool == "open_url") return "открываю ссылку";
        if (tool == "open_app") return "запускаю программу";
        if (tool == "media") return "управляю музыкой";
        return tool;
    }
    if (s == "recognizing speech") return "разбираю, что ты сказал";
    if (s.startsWith("confirmation timed out")) return "не дождался ответа, отменил";
    if (s == "cancelled") return "отменено";
    return s;
}

void clearBody() { tft.fillRect(0, BAR_H + 1, W, H - BAR_H - 1, BG); }

void drawSide(const String& title, uint16_t color, const String& sub) {
    tft.fillRect(COL_X, BAR_H + 1, W - COL_X, H - BAR_H - 1, BG);
    text(COL_X, 52, title, color, FONT_BIG);
    side.setText(sub);
    side.draw();
}

void drawHome() {
    clearBody();
    if (!wifiUp) {
        face.setMood(Mood::Offline);
        drawSide("Сплю", OFF, "жду Wi-Fi...");
    } else if (!wsUp) {
        face.setMood(Mood::Offline);
        drawSide("Нет связи", OFF, "жду сервер агента...");
    } else {
        face.setMood(Mood::Idle);
        String hint = "держи кнопку и говори";
        hint += "\n\nкрути: история и проекты";
        drawSide("Готов", IDLE, hint);
    }
}

void drawListen() {
    clearBody();
    face.setMood(Mood::Listen);
    drawSide("Слушаю", LISTEN, "отпусти, когда договоришь");
}

// Полоска громкости и таймер под подписью «Слушаю»
void drawListenLevel(int peak, uint32_t ms) {
    const int x = COL_X, y = 120, w = COL_W, h = 8;
    int filled = min(w, (int)((long)w * peak / 12000));   // 12000 — уже громкая речь вблизи
    tft.fillRoundRect(x, y, w, h, 4, LINE);
    if (filled > 6) tft.fillRoundRect(x, y, filled, h, 4, peak > 30000 ? ERR : LISTEN);
    char t[16];
    snprintf(t, sizeof(t), "%lu / %lu c", ms / 1000, MAX_RECORD_MS / 1000);
    tft.fillRect(x, y + 14, w, 18, BG);
    text(x, y + 28, t, DIM);
}

void drawThink() {
    clearBody();
    face.setMood(Mood::Think);
    String sub = thinking.length() ? thinking : "секунду...";
    if (question.length()) sub = "«" + question + "»\n\n" + sub;
    sub += "\n\nклик: отменить";
    drawSide("Думаю", THINK, sub);
}

void drawChoice() {
    const int y = 128, bw = 78, bh = 30;
    const int xYes = COL_X, xNo = COL_X + COL_W - bw;
    uint16_t yesBg = choiceYes ? SPEAK : BG, noBg = choiceYes ? BG : ERR;
    tft.fillRoundRect(xYes, y, bw, bh, 15, yesBg);
    tft.drawRoundRect(xYes, y, bw, bh, 15, SPEAK);
    tft.fillRoundRect(xNo, y, bw, bh, 15, noBg);
    tft.drawRoundRect(xNo, y, bw, bh, 15, ERR);
    text(xYes + 24, y + 21, "Да", choiceYes ? BG : SPEAK, FONT_BIG);
    text(xNo + 19, y + 21, "Нет", choiceYes ? ERR : BG, FONT_BIG);
}

void drawConfirm() {
    clearBody();
    face.setMood(Mood::Ask);
    tft.fillRect(COL_X, BAR_H + 1, W - COL_X, H - BAR_H - 1, BG);
    text(COL_X, 48, "Разрешить?", ASK, FONT_BIG);
    text(COL_X, 68, friendlyTool(confirmTool), TEXT);
    argsView.setText(confirmArgs);    // реальный вызов тула, а не пересказ модели
    argsView.draw();
    drawChoice();
}

void drawAnswerHeader() {
    tft.fillRect(0, BAR_H + 1, W, 22, BG);
    text(8, BAR_H + 17, answerIsError ? "Ошибка" : "Ответ", answerIsError ? ERR : SPEAK, FONT_BIG);
    text(W - 128, BAR_H + 16, "клик: назад", DIM);
}

// «Эквалайзер» рядом с заголовком, пока агент говорит
void drawSpeakingBars() {
    const int x = 82, y = BAR_H + 4, h = 16;
    tft.fillRect(x, y, 40, h, BG);
    if (!speaker.playing()) return;
    uint32_t t = millis();
    for (int i = 0; i < 5; i++) {
        int bh = 3 + (int)((h - 3) * fabsf(sinf(t / (70.0f + i * 23) + i)));
        tft.fillRoundRect(x + i * 7, y + h - bh, 4, bh, 2, SPEAK);
    }
}

void drawAnswer(const String& s) {
    currentAnswer = s;
    clearBody();
    drawAnswerHeader();
    body.setText(s);
    body.draw();
}

// Общий список: заголовок и пункты. label(i, color, sel) отдаёт текст пункта i и может сменить цвет
template <typename LabelFn>
void drawList(const char* title, int count, int selected, LabelFn label) {
    clearBody();
    text(8, BAR_H + 17, title, IDLE, FONT_BIG);
    const int itemH = 22, top = BAR_H + 26;
    const int visible = (H - top) / itemH;
    // Окно списка: выбранный пункт всегда виден
    int first = max(0, selected - visible + 1);
    for (int row = 0; row < visible; row++) {
        int i = first + row;
        if (i >= count) break;
        int y = top + row * itemH;
        bool sel = i == selected;
        if (sel) tft.fillRoundRect(4, y, W - 8, itemH - 2, 6, PANEL);
        uint16_t color = sel ? TEXT : DIM;
        String s = label(i, color, sel);
        textFit(14, y + 15, s, W - 30, color);
    }
}

int historyCount() { return HISTORY_FIRST + (historyLoaded ? max(1, (int)history.size()) : 1); }

void drawHistory() {
    drawList("История", historyCount(), historyIndex, [](int i, uint16_t& color, bool sel) -> String {
        if (i == 0) return "< назад";
        if (i == 1) {
            color = sel ? IDLE : rgb(40, 140, 150);
            return "Проект: " + projectName + "  >";
        }
        if (!historyLoaded) return "загружаю...";
        if (history.empty()) return "(пока пусто)";
        const Entry& e = history[i - HISTORY_FIRST];
        if (e.err) color = sel ? ERR : rgb(150, 70, 70);
        return e.q.length() ? e.q : "(без вопроса)";
    });
}

int projectCount() { return PROJECTS_FIRST + (projectsLoaded ? (int)projects.size() : 1); }

void drawProjects() {
    drawList("Проекты", projectCount(), projectIndex, [](int i, uint16_t& color, bool sel) -> String {
        if (i == 0) return "< назад";
        if (i == 1) {
            color = sel ? SPEAK : rgb(50, 140, 80);
            return "+ новый проект";
        }
        if (!projectsLoaded) return "загружаю...";
        const ProjectItem& p = projects[i - PROJECTS_FIRST];
        bool current = p.name == projectName;
        if (current) color = sel ? IDLE : rgb(40, 140, 150);
        return String(current ? "* " : "  ") + p.name + "  (" + String(p.turns) + ")";
    });
    text(W - 150, BAR_H + 16, "держи: удалить", DIM);
}

void drawDelete();   // ниже, рядом с отправкой на сервер

void drawScreen() {
    switch (mode) {
    case Mode::Home: drawHome(); break;
    case Mode::Listen: drawListen(); break;
    case Mode::Think: drawThink(); break;
    case Mode::Confirm: drawConfirm(); break;
    case Mode::Answer: break;   // ответ рисуется вместе с текстом в drawAnswer
    case Mode::History: drawHistory(); break;
    case Mode::Projects: drawProjects(); break;
    case Mode::Delete: drawDelete(); break;
    }
}

bool faceVisible() {
    return mode == Mode::Home || mode == Mode::Listen || mode == Mode::Think || mode == Mode::Confirm ||
           mode == Mode::Delete;
}

void go(Mode m) {
    mode = m;
    drawScreen();
}

void log(const String& s) { Serial.println("[ui] " + s); }

// ---------- отправка на сервер ----------

void sendJson(JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    ws.sendTXT(out);
    Serial.println("[ws ->] " + out);
}

void sendType(const char* type) {
    JsonDocument doc;
    doc["type"] = type;
    sendJson(doc);
}

void drawDelete() {
    clearBody();
    face.setMood(Mood::Ask);
    tft.fillRect(COL_X, BAR_H + 1, W - COL_X, H - BAR_H - 1, BG);
    text(COL_X, 48, "Удалить?", ERR, FONT_BIG);
    textFit(COL_X, 68, deleteName, COL_W, TEXT);
    text(COL_X, 88, "файлы и историю", DIM);
    text(COL_X, 104, "в корзину", DIM);
    drawChoice();
}

void openHistory() {
    historyLoaded = false;
    history.clear();
    historyIndex = HISTORY_FIRST;    // сразу на последний вопрос
    sendType("history_list");
}

void openProjects() {
    projectsLoaded = false;
    projects.clear();
    projectIndex = PROJECTS_FIRST;
    sendType("project_list");
}

void sendDecision(bool approve) {
    JsonDocument doc;
    doc["type"] = "decision";
    doc["id"] = confirmId;
    doc["approve"] = approve;
    sendJson(doc);
}

// ---------- голос и речь ----------

// Замолчать. Если сервер ещё присылает озвучку, его задача не закончена:
// отменяем её, иначе следующая задача получит "task already running".
void stopSpeech() {
    if (speechActive) {
        sendType("cancel");
        speechActive = false;
    }
    speaker.stop();
}

void startListening() {
    if (!micOk) { log("mic failed"); return; }
    if (!wsUp) { log("no server"); return; }
    stopSpeech();                // не записывать собственный голос агента
    mic.flush();                 // старый звук из буферов не нужен
    sendType("audio_start");
    recordStartedAt = millis();
    question = "";
    thinking = "";
    go(Mode::Listen);
    drawListenLevel(0, 0);
}

void finishListening() {
    sendType("audio_end");
    thinking = "разбираю, что ты сказал";
    face.setLevel(0);
    go(Mode::Think);
}

// Пока идёт запись: забрать звук с микрофона и сразу отправить
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
    if (millis() - lastDraw > 100) {
        face.setLevel(peak / 12000.0f);
        drawListenLevel(peak, ms);
        lastDraw = millis();
        peak = 0;
    }
    if (ms >= MAX_RECORD_MS) finishListening();   // кнопку держат слишком долго
}

// ---------- сообщения от сервера ----------

bool touch();   // объявлена ниже, в разделе «питание»

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
    touch();                         // новости от сервера — показать
    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) {
        log("bad JSON from server");
        return;
    }
    String type = doc["type"] | "";
    String msg = doc["text"] | "";

    if (type == "status") {
        log("status: " + msg);
        thinking = friendlyStatus(msg);
        // В карточке status значит, что подтверждение закрыто (например, таймаут)
        if (mode == Mode::Confirm) confirmId = "";
        if (mode == Mode::Confirm || mode == Mode::Think) go(Mode::Think);
    } else if (type == "transcript") {
        question = msg;              // показываем, что услышал: если криво — сразу видно
        thinking = "думаю...";
        if (mode == Mode::Think) drawThink();
    } else if (type == "confirm_request") {
        confirmId = doc["id"] | "";
        confirmTool = doc["tool"] | "?";
        confirmArgs = argsToText(doc["args"].as<JsonObject>());
        choiceYes = false;
        go(Mode::Confirm);
    } else if (type == "project") {
        projectName = doc["name"] | "";
        drawTopBar();
        // Переключились из списка проектов — на главный
        if (mode == Mode::Projects || mode == Mode::Home) go(Mode::Home);
    } else if (type == "projects") {
        projects.clear();
        for (JsonObject it : doc["items"].as<JsonArray>()) {
            projects.push_back(ProjectItem{it["name"] | "", it["turns"] | 0});
        }
        projectsLoaded = true;
        projectIndex = constrain(projectIndex, 0, projectCount() - 1);
        if (mode == Mode::Projects) drawProjects();
    } else if (type == "history") {
        history.clear();
        for (JsonObject it : doc["items"].as<JsonArray>()) {
            history.push_back(Entry{it["q"] | "", it["a"] | "", it["error"] | false});
        }
        historyLoaded = true;
        historyIndex = constrain(historyIndex, 0, historyCount() - 1);
        if (mode == Mode::History) drawHistory();
    } else if (type == "speech_start") {
        speaker.start(doc["sample_rate"] | 22050);
        speechActive = true;
    } else if (type == "speech_end") {
        speechActive = false;        // буфер доиграет сам
    } else if (type == "result" || type == "error") {
        bool isError = type == "error";
        // error без активной задачи (например, "task already running") — только в лог
        if (isError && mode != Mode::Think && mode != Mode::Confirm) {
            log("error: " + msg);
            return;
        }
        answerIsError = isError;
        mode = Mode::Answer;
        drawAnswer(msg);
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
        drawTopBar();
        if (mode == Mode::Home) drawHome();
        break;
    }
    case WStype_DISCONNECTED:
        if (wsUp) {
            // Сервер при обрыве отменяет задачу, поэтому и мы возвращаемся на главный.
            // Висящее подтверждение не «досылаем»: после переподключения оно недействительно.
            wsUp = false;
            confirmId = "";
            speechActive = false;
            speaker.stop();
            drawTopBar();
            go(Mode::Home);
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

// ---------- питание ----------

// Что-то произошло: продлить «бодрствование». Если экран погас — включить и перерисовать.
// Возвращает true, если экран был выключен: тогда это касание только будит.
bool touch() {
    lastActivity = millis();
    power.setBrightness(255);
    if (!screenOff) return false;
    screenOff = false;
    drawTopBar();
    if (mode == Mode::Answer) drawAnswer(currentAnswer);
    else drawScreen();
    return true;
}

bool busy() {
    return mode == Mode::Listen || mode == Mode::Think || mode == Mode::Confirm ||
           speechActive || speaker.playing();
}

void goToSleep() {
    log("going to deep sleep");
    stopSpeech();
    face.setMood(Mood::Offline);
    clearBody();
    face.draw(FACE_X, FACE_Y);
    drawSide("Сплю", OFF, "нажми любую кнопку, чтобы разбудить");
    delay(1200);
    ws.disconnect();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    power.deepSleep();
}

void updatePower() {
    if (busy()) {
        lastActivity = millis();     // агент работает — не спим
        return;
    }
    uint32_t idle = millis() - lastActivity;
    // Глубокий сон только от батареи: от USB экономить нечего, а просыпаться дольше
    if (idle > SLEEP_MS && bat.ok && !bat.usb) {
        goToSleep();
    } else if (idle > OFF_MS) {
        if (!screenOff) {
            power.setBrightness(0);
            screenOff = true;
            log("screen off");
        }
    } else if (idle > DIM_MS) {
        power.setBrightness(DIM_LEVEL);
    }
}

// ---------- энкодер и кнопка ----------

void onRotate(int dir) {
    switch (mode) {
    case Mode::Home:
        if (wsUp) {
            openHistory();
            go(Mode::History);
        }
        break;
    case Mode::History:
        historyIndex = constrain(historyIndex + dir, 0, historyCount() - 1);
        drawHistory();
        break;
    case Mode::Projects:
        projectIndex = constrain(projectIndex + dir, 0, projectCount() - 1);
        drawProjects();
        break;
    case Mode::Confirm:
    case Mode::Delete:
        choiceYes = !choiceYes;      // два варианта: любой поворот переключает
        drawChoice();
        break;
    case Mode::Answer:
        if (body.scroll(dir)) body.draw();
        break;
    default:
        break;
    }
}

void onButton(ButtonEvent ev) {
    // Удержание — начать говорить. Кроме карточек, записи, работы и списка проектов
    // (там удержание = удалить проект)
    bool holdIsVoice = mode != Mode::Confirm && mode != Mode::Listen && mode != Mode::Think &&
                       mode != Mode::Projects && mode != Mode::Delete;
    if (ev == ButtonEvent::HoldStart && holdIsVoice) {
        startListening();
        return;
    }
    switch (mode) {
    case Mode::Listen:
        if (ev == ButtonEvent::HoldEnd) finishListening();
        break;
    case Mode::Think:
        if (ev == ButtonEvent::Click) {
            sendType("cancel");
            thinking = "отменяю...";
            drawThink();
        }
        break;
    case Mode::Confirm:
        if (ev == ButtonEvent::Click) {
            sendDecision(choiceYes);
            confirmId = "";
            thinking = choiceYes ? "разрешено, выполняю" : "отклонено";
            go(Mode::Think);
        }
        break;
    case Mode::Answer:
        if (ev == ButtonEvent::Click) {
            stopSpeech();
            go(Mode::Home);
        }
        break;
    case Mode::History:
        if (ev != ButtonEvent::Click) break;
        if (historyIndex == 0) {
            go(Mode::Home);
        } else if (historyIndex == 1) {
            openProjects();
            go(Mode::Projects);
        } else if (historyLoaded && !history.empty()) {
            const Entry& e = history[historyIndex - HISTORY_FIRST];
            answerIsError = e.err;
            mode = Mode::Answer;
            drawAnswer(e.a);
        }
        break;
    case Mode::Projects:
        if (ev == ButtonEvent::HoldStart) {
            if (projectsLoaded && projectIndex >= PROJECTS_FIRST) {
                deleteName = projects[projectIndex - PROJECTS_FIRST].name;
                choiceYes = false;          // по умолчанию «Нет»
                go(Mode::Delete);
            }
            break;
        }
        if (ev != ButtonEvent::Click) break;
        if (projectIndex == 0) {
            openHistory();
            go(Mode::History);
        } else if (projectIndex == 1) {
            sendType("project_new");      // имя придумает модель после первого вопроса
        } else if (projectsLoaded) {
            const String& name = projects[projectIndex - PROJECTS_FIRST].name;
            if (name == projectName) {
                go(Mode::Home);
            } else {
                JsonDocument doc;
                doc["type"] = "project_switch";
                doc["name"] = name;
                sendJson(doc);                // ответ project переключит экран на главный
            }
        }
        break;
    case Mode::Delete:
        if (ev != ButtonEvent::Click) break;
        if (choiceYes) {
            JsonDocument doc;
            doc["type"] = "project_delete";
            doc["name"] = deleteName;
            sendJson(doc);                    // в ответ сервер пришлёт новый список проектов
            projectsLoaded = false;
            projects.clear();
            projectIndex = PROJECTS_FIRST;
        }
        go(Mode::Projects);
        break;
    default:
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
    tft.fillScreen(BG);
    power.begin();                   // подсветка через ШИМ, после tft.begin
    u8f.begin(tft);
    body.setArea(8, BAR_H + 26, W - 12, H - BAR_H - 30);
    body.setColors(TEXT, BG);
    side.setArea(COL_X, 62, COL_W, H - 62 - 4);
    side.setColors(DIM, BG);
    argsView.setArea(COL_X, 76, COL_W, 48);
    argsView.setColors(DIM, BG);

    faceOk = face.begin();
    button.begin();
    userKey.begin();
    micOk = mic.begin();
    speakerOk = speaker.begin();
    battery.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    bat = battery.read();
    Serial.printf("face: %s, mic: %s, speaker: %s, battery: %s %d%% %dmV usb=%d charging=%d\n",
                  faceOk ? "ok" : "FAILED", micOk ? "ok" : "FAILED", speakerOk ? "ok" : "FAILED",
                  bat.ok ? "ok" : "FAILED", bat.percent, bat.millivolts, bat.usb, bat.charging);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    drawTopBar();
    go(Mode::Home);

    // Токен в заголовке, а не в адресе: адрес сервер пишет в лог.
    ws.begin(AGENT_HOST, AGENT_PORT, "/ws");
    ws.setExtraHeaders("Authorization: Bearer " AGENT_TOKEN);
    ws.onEvent(onWsEvent);
    ws.setReconnectInterval(3000);
    ws.enableHeartbeat(15000, 3000, 2);   // заметить обрыв, даже если сервер молчит

    lastActivity = millis();
    Serial.printf("hardware-agent firmware: face UI (%s)\n", Power::wokeFromSleep() ? "woke from sleep" : "power on");
}

void loop() {
    ws.loop();
    if (mode == Mode::Listen) pumpAudio();

    // Wi-Fi
    bool up = WiFi.status() == WL_CONNECTED;
    if (up != wifiUp) {
        wifiUp = up;
        log(up ? "wifi " + WiFi.localIP().toString() : "wifi lost");
        drawTopBar();
        if (mode == Mode::Home) drawHome();
    }

    // Батарея и сила Wi-Fi: раз в 10 с хватает
    static uint32_t lastBar = 0;
    if (millis() - lastBar > 10000) {
        lastBar = millis();
        bat = battery.read();
        drawTopBar();
    }

    updatePower();

    // Анимация ~25 кадров в секунду. Экран погас — не рисуем, экономим процессор
    static uint32_t lastFrame = 0;
    if (!screenOff && millis() - lastFrame > 40) {
        lastFrame = millis();
        if (faceOk && faceVisible()) face.draw(FACE_X, FACE_Y);
        if (mode == Mode::Answer) drawSpeakingBars();
    }

    encoder.tick();
    static long lastPos = 0;
    long pos = encoder.getPosition();
    if (pos != lastPos) {
        if (!touch()) onRotate(pos > lastPos ? 1 : -1);   // при выключенном экране — только разбудить
        lastPos = pos;
    }

    ButtonEvent ev = button.poll();
    if (ev != ButtonEvent::None) {
        bool wasOff = touch();
        // Клик по погасшему экрану только будит. Удержание сразу работает: можно говорить
        if (!(wasOff && ev == ButtonEvent::Click)) onButton(ev);
    }

    // Боковая кнопка: клик — уснуть сейчас (кнопка уже отпущена, иначе сразу проснёмся)
    if (userKey.poll() == ButtonEvent::Click) goToSleep();

    delay(1);
}
