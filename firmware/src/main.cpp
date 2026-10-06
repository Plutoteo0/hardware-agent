// Прошивка T-Embed: голосовой агент с лицом.
//
// Плата — клиент сервера агента (как client/terminal.py), протокол:
//   плата -> сервер:  hello, decision, cancel, audio_start, <байты звука>, audio_end
//   сервер -> плата:  status, confirm_request, result, error, transcript,
//                     speech_start, <байты озвучки>, speech_end
//
// Экраны и управление:
//   главный:   лицо агента и текущий проект. Удержание — говорить; без связи с сервером
//              голос пишется в заметку на SD-карту (notes.h)
//   слушаю:    пока держишь кнопку. Отпустил — отправить
//   думаю:     клик — отменить задачу
//   карточка:  крутить — Да/Нет, клик — решение. Удержание игнорируется:
//              голосом не подтверждаем (BRAINSTORM.md)
//   ответ:     крутить — прокрутка, клик — замолчать и назад
//   карточки (меню): боковая кнопка — открыть/закрыть, крутить — следующая, клик — открыть.
//              Проекты («+ новый», клик — переключиться, удержание — «Удалить?», для «общего» —
//              «Очистить?»), История (клик — ответ), Заметки (клик — прослушать, удержание —
//              отправить агенту / удалить), Настройки (громкость, озвучка — в NVS),
//              Почта и Wi-Fi (заглушки)
//   списки (стек экранов, ui.h): первый пункт «< назад», курсор открывается на нём
//   боковая кнопка: клик — карточки, удержание 4 с (с отсчётом) — уснуть
//
// Питание: без дела 30 с — экран тускнеет, 1 мин — гаснет (связь остаётся),
// 5 мин — глубокий сон (только от батареи). Будит любая кнопка.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <algorithm>
#include <functional>
#include <vector>

#include "battery.h"
#include "button.h"
#include "encoder.h"
#include "face.h"
#include "mic.h"
#include "netprobe.h"
#include "notes.h"
#include "pins.h"
#include "power.h"
#include "secrets.h"
#include "settings.h"
#include "speaker.h"
#include "textview.h"
#include "theme.h"
#include "ui.h"
#include "wifistore.h"

using namespace theme;

// ---------- железо ----------

TFT_eSPI tft;
U8g2_for_TFT_eSPI u8f;
Face face(tft);
Encoder encoder(ENCODER_INA, ENCODER_INB);
Button button(ENCODER_KEY);
WebSocketsClient ws;
ServerProbe probe;               // жив ли сервер — проверяет без блокировки (netprobe.h)
Mic mic;
Speaker speaker;
Battery battery;
Power power;
Button userKey(BOARD_USER_KEY);
bool micOk = false, speakerOk = false, faceOk = false;

// ---------- состояние ----------

// Nav — открыт экран из стека nav (меню, списки). Остальное — состояния агента
enum class Mode { Home, Listen, Think, Confirm, Answer, Nav };
Mode mode = Mode::Home;
ScreenStack nav;
Settings settings;
WifiStore wifiStore;             // сохранённые сети (wifistore.h)
Notes notes;                     // голосовые заметки на SD (notes.h)
bool recordingNote = false;      // идёт запись в заметку, а не на сервер (сервер недоступен)
String playingNote;              // какая заметка играет ("" — никакая)
size_t playPos = 0;
bool sendingNote = false;        // заметка уходит на сервер кадрами
size_t sendPos = 0;
String noteToDelete;             // отправленная заметка: удалим, когда сервер её распознает
// При старте подключаемся к последней сохранённой сети. Не вышло к этому времени —
// запасная из secrets.h. 0 — запасная не нужна
uint32_t bootFallbackAt = 0;

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

struct ProjectItem { String name; int turns; };
std::vector<ProjectItem> projects;
bool projectsLoaded = false;
String projectName;              // текущий проект, приходит в сообщении project
const char* DEFAULT_PROJECT = "общее";   // его нельзя удалить, только очистить историю
bool awaitingSwitch = false;     // попросили сменить проект: ответ project вернёт на главный

// Боковая кнопка: клик — карточки (меню), удержание OFF_HOLD_MS — уснуть.
// Отпустил раньше — отмена: случайное нажатие не выключит плату
const uint32_t OFF_HOLD_MS = 4000;
bool offArmed = false;           // додержали: уснём, когда кнопку отпустят
bool offShown = false;           // на экране полоска отсчёта

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

// Текст по центру относительно cx, обрезанный по ширине maxW
void textCenter(int cx, int y, const String& s, int maxW, uint16_t color, const uint8_t* font = FONT) {
    u8f.setFont(font);   // шрифт до измерения, см. textFit
    String fit = fitUtf8(u8f, s, maxW);
    text(cx - u8f.getUTF8Width(fit.c_str()) / 2, y, fit, color, font);
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
    if (tool == "remember") return "Запомнить";
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

// Главный: состояние, текущий проект крупно (чтобы не искать его в меню) и подсказки
void drawHome() {
    clearBody();
    const char* hint;
    if (!wifiUp) {
        face.setMood(Mood::Offline);
        drawSide("Нет Wi-Fi", OFF, "");
        hint = notes.ok() ? "держи: заметка" : "жду Wi-Fi...";
    } else if (!wsUp) {
        face.setMood(Mood::Offline);
        drawSide("Нет сервера", OFF, "");
        hint = notes.ok() ? "держи: заметка" : "жду сервер агента...";
    } else {
        face.setMood(Mood::Idle);
        drawSide("Готов", IDLE, "");
        hint = "держи: говорить";
    }
    text(COL_X, 80, "проект", DIM);
    textFit(COL_X, 102, projectName.length() ? projectName : String("..."), COL_W, TEXT, FONT_BIG);
    text(COL_X, 140, hint, DIM);
    text(COL_X, 160, "боковая: меню", DIM);
}

void drawListen() {
    clearBody();
    face.setMood(Mood::Listen);
    // Без связи запись идёт в заметку на карту
    drawSide(recordingNote ? "Заметка" : "Слушаю", LISTEN,
             recordingNote ? "сервер недоступен: запишу на карту" : "отпусти, когда договоришь");
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

// Общий список: заголовок и пункты. label(i, color, sel) отдаёт текст пункта i и может сменить цвет.
// prev >= 0 — курсор сдвинулся с пункта prev. Если окно списка осталось на месте, перерисовываем
// только две строки: весь экран рисуется десятки мс и мигает на каждом щелчке.
// Возвращает true, если экран перерисован целиком (тогда поверх можно дорисовать своё)
template <typename LabelFn>
bool drawList(const char* title, int count, int selected, LabelFn label, int prev = -1) {
    const int itemH = 22, top = BAR_H + 26;
    const int visible = (H - top) / itemH;
    // Окно списка: выбранный пункт всегда виден
    auto firstFor = [&](int sel) { return max(0, sel - visible + 1); };
    int first = firstFor(selected);

    auto drawRow = [&](int i) {
        int y = top + (i - first) * itemH;
        bool sel = i == selected;
        tft.fillRect(4, y, W - 8, itemH - 2, BG);
        if (sel) tft.fillRoundRect(4, y, W - 8, itemH - 2, 6, PANEL);
        uint16_t color = sel ? TEXT : DIM;
        String s = label(i, color, sel);
        textFit(14, y + 15, s, W - 30, color);
    };

    if (prev >= 0 && prev < count && firstFor(prev) == first) {
        drawRow(prev);
        drawRow(selected);
        return false;
    }
    clearBody();
    text(8, BAR_H + 17, title, IDLE, FONT_BIG);
    for (int row = 0; row < visible && first + row < count; row++) drawRow(first + row);
    return true;
}

void drawScreen() {
    switch (mode) {
    case Mode::Home: drawHome(); break;
    case Mode::Listen: drawListen(); break;
    case Mode::Think: drawThink(); break;
    case Mode::Confirm: drawConfirm(); break;
    case Mode::Answer: break;   // ответ рисуется вместе с текстом в drawAnswer
    case Mode::Nav: nav.top()->draw(); break;
    }
}

bool faceVisible() {
    if (mode == Mode::Nav) return nav.top()->showsFace();
    return mode == Mode::Home || mode == Mode::Listen || mode == Mode::Think || mode == Mode::Confirm;
}

// Главный экран = стек пуст. Поэтому переход на главный всегда закрывает все меню
void go(Mode m) {
    if (m == Mode::Home) nav.clear();
    mode = m;
    drawScreen();
}

// Открыть экран поверх текущего
void openScreen(Screen* s) {
    nav.push(s);
    s->onOpen();
    mode = Mode::Nav;
    s->draw();
}

// На шаг назад. Стек опустел — на главный
void back() {
    nav.pop();
    if (nav.empty()) {
        go(Mode::Home);
    } else {
        mode = Mode::Nav;
        nav.top()->draw();
    }
}

bool showing(Screen* s) { return mode == Mode::Nav && nav.top() == s; }

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

void sendHello() {
    // Сообщаем серверу, умеем ли играть звук: без speech=true озвучку не пришлют.
    // Озвучка выключена в настройках — сервер даже не запускает синтез
    JsonDocument hello;
    hello["type"] = "hello";
    hello["speech"] = speakerOk && settings.speech;
    sendJson(hello);
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

void stopPlayback();   // ниже, в разделе заметок

void startListening() {
    if (!micOk) { log("mic failed"); return; }
    // Сервер недоступен — пишем заметку на карту, отправить можно будет потом из «Заметок»
    recordingNote = !wsUp;
    if (recordingNote && !notes.ok()) { log("no server and no SD card"); return; }
    stopSpeech();                // не записывать собственный голос агента
    stopPlayback();
    mic.flush();                 // старый звук из буферов не нужен
    if (recordingNote) notes.startRecording();
    else sendType("audio_start");
    recordStartedAt = millis();
    question = "";
    thinking = "";
    nav.clear();                 // заговорили из меню — после ответа вернёмся на главный
    go(Mode::Listen);
    drawListenLevel(0, 0);
}

void finishListening() {
    face.setLevel(0);
    if (recordingNote) {
        recordingNote = false;
        float sec = notes.recordedSeconds();
        String msg;
        answerIsError = false;
        if (sec < 0.5f) {
            msg = "Слишком коротко, заметку не сохранил.";
        } else if (notes.saveRecording()) {
            msg = "Заметка сохранена (" + String((int)(sec + 0.5f)) + " с).\n\n"
                  "Прослушать или отправить агенту: боковая кнопка, «Заметки».";
        } else {
            msg = "Не смог записать заметку на SD-карту.";
            answerIsError = true;
        }
        mode = Mode::Answer;
        drawAnswer(msg);
        return;
    }
    sendType("audio_end");
    thinking = "разбираю, что ты сказал";
    go(Mode::Think);
}

// Пока идёт запись: забрать звук с микрофона и сразу отправить
void pumpAudio() {
    static int16_t buf[1024];             // 1024 сэмпла = 64 мс звука, 2 КБ на кадр
    static uint32_t lastDraw = 0;
    static int peak = 0;

    size_t n = mic.read(buf, 1024);
    if (n) {
        if (recordingNote) notes.add(buf, n);
        else ws.sendBIN((uint8_t*)buf, n * sizeof(int16_t));
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

// Короткий сигнал, чтобы слышать громкость, пока крутишь. Агент говорит — не перебиваем
void beep() {
    if (!speakerOk || speechActive) return;
    const int RATE = 16000, N = RATE / 10;   // 100 мс
    static int16_t tone[N];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < N; i++) {
            // Плавные края (по 5 мс): резкое начало и конец звука слышны как щелчок
            float edge = min(1.0f, min(i, N - i) / (RATE * 0.005f));
            tone[i] = (int16_t)(0.5f * 32767 * edge * sinf(2 * PI * 660 * i / RATE));
        }
        ready = true;
    }
    speaker.start(RATE);
    speaker.push((const uint8_t*)tone, sizeof(tone));
}

// ---------- экраны навигации (стек nav, см. ui.h) ----------

const char* BACK_LABEL = "< назад";

// Курсор в списке по кругу: из «назад» в обратную сторону — на последний пункт.
// Так меню листается в ту же сторону, в которую его открыли, и курсор не упирается
int wrap(int i, int count) { return (i % count + count) % count; }

// Пункт меню. value — для настроек: текущее значение после подписи
struct MenuItem {
    // Конструктор нужен для записи {"подпись", действие}: в C++11 структура
    // со значением по умолчанию у поля из фигурных скобок не собирается
    MenuItem(const char* label, std::function<void()> onClick, std::function<String()> value = nullptr)
        : label(label), onClick(std::move(onClick)), value(std::move(value)) {}
    String label;
    std::function<void()> onClick;
    std::function<String()> value;
};

// Меню из списка пунктов. «< назад» — всегда первый, курсор открывается на нём:
// случайный клик просто закроет меню
class MenuScreen : public Screen {
public:
    MenuScreen(const char* title, std::vector<MenuItem> items) : title_(title), items_(std::move(items)) {}

    void onOpen() override { index_ = 0; }

    void draw() override { drawItems(-1); }

    void onRotate(int dir) override {
        int prev = index_;
        index_ = wrap(index_ + dir, count());
        drawItems(prev);
    }

    void onClick() override {
        if (index_ == 0) back();
        else items_[index_ - 1].onClick();
    }

private:
    void drawItems(int prev) {
        drawList(title_, count(), index_, [this](int i, uint16_t&, bool) -> String {
            if (i == 0) return BACK_LABEL;
            const MenuItem& it = items_[i - 1];
            return it.value ? it.label + ": " + it.value() : it.label;
        }, prev);
    }

    int count() const { return 1 + (int)items_.size(); }
    const char* title_;
    std::vector<MenuItem> items_;
    int index_ = 0;
};

// Заглушка для пунктов «на потом»
class InfoScreen : public Screen {
public:
    InfoScreen(const char* title, const char* text) : title_(title), text_(text) {}
    void draw() override {
        clearBody();
        text(8, BAR_H + 17, title_, IDLE, FONT_BIG);
        text(8, BAR_H + 52, text_, TEXT);
        text(8, H - 10, "клик: назад", DIM);
    }
    void onClick() override { back(); }

private:
    const char* title_;
    const char* text_;
};

// Громкость озвучки: крутишь — меняется сразу и пикает, клик — сохранить в NVS
class VolumeScreen : public Screen {
public:
    void draw() override {
        clearBody();
        text(8, BAR_H + 17, "Громкость", IDLE, FONT_BIG);
        text(8, H - 10, "клик: сохранить", DIM);
        drawLevel();
    }

    void onRotate(int dir) override {
        int v = constrain((int)settings.volume + dir, 0, (int)Settings::VOLUME_MAX);
        if (v == settings.volume) return;
        settings.volume = v;
        speaker.setVolume(settings.gain());
        drawLevel();
        beep();
    }

    void onClick() override {
        settings.save();
        back();
    }

private:
    void drawLevel() {
        const int x = 20, y = 80, w = W - 40, h = 14;
        tft.fillRect(0, y, W, 50, BG);
        tft.fillRoundRect(x, y, w, h, 7, LINE);
        int filled = w * settings.volume / Settings::VOLUME_MAX;
        if (filled > 8) tft.fillRoundRect(x, y, filled, h, 7, SPEAK);
        text(x, y + 40, String(settings.volume) + " из " + String(Settings::VOLUME_MAX), TEXT, FONT_BIG);
    }
};

// Вопрос «Удалить?» / «Очистить историю?» поверх списка проектов
class DeleteScreen : public Screen {
public:
    void ask(const String& name) { name_ = name; }
    void onOpen() override { choiceYes = false; }   // по умолчанию «Нет»
    bool showsFace() const override { return true; }
    bool onHold() override { return true; }         // удержание здесь не значит «говорить»

    void draw() override {
        clearBody();
        face.setMood(Mood::Ask);
        // «Общее» удалить нельзя, но историю можно начать заново (файлы и заметки остаются)
        bool clear = name_ == DEFAULT_PROJECT;
        text(COL_X, 48, clear ? "Очистить?" : "Удалить?", ERR, FONT_BIG);
        textFit(COL_X, 68, name_, COL_W, TEXT);
        text(COL_X, 88, clear ? "историю вопросов" : "файлы и историю", DIM);
        text(COL_X, 104, clear ? "файлы останутся" : "в корзину", DIM);
        drawChoice();
    }

    void onRotate(int) override {
        choiceYes = !choiceYes;   // два варианта: любой поворот переключает
        drawChoice();
    }

    void onClick() override;      // ниже: нужен экран проектов

private:
    String name_;
};

DeleteScreen deleteScreen;

// История текущего проекта (хранит сервер). Клик по вопросу — ответ
class HistoryScreen : public Screen {
public:
    void onOpen() override {
        index_ = 0;
        historyLoaded = false;
        history.clear();
        sendType("history_list");
    }

    void draw() override { drawItems(-1); }

    // Пришёл ответ сервера
    void refresh() {
        index_ = constrain(index_, 0, count() - 1);
        draw();
    }

    void onRotate(int dir) override {
        int prev = index_;
        index_ = wrap(index_ + dir, count());
        drawItems(prev);
    }

    void onClick() override {
        if (index_ == 0) {
            back();
        } else if (historyLoaded && !history.empty()) {
            const Entry& e = history[index_ - 1];
            answerIsError = e.err;
            mode = Mode::Answer;   // стек остаётся: клик в ответе вернёт сюда
            drawAnswer(e.a);
        }
    }

private:
    void drawItems(int prev) {
        drawList("История", count(), index_, [](int i, uint16_t& color, bool sel) -> String {
            if (i == 0) return BACK_LABEL;
            if (!historyLoaded) return wsUp ? "загружаю..." : "сервер недоступен";
            if (history.empty()) return "(пока пусто)";
            const Entry& e = history[i - 1];
            if (e.err) color = sel ? ERR : rgb(150, 70, 70);
            return e.q.length() ? e.q : "(без вопроса)";
        }, prev);
    }

    int count() const { return 1 + (historyLoaded ? max(1, (int)history.size()) : 1); }
    int index_ = 0;
};

// Проекты: «+ новый», клик — переключиться, удержание — удалить (или очистить «общее»)
class ProjectsScreen : public Screen {
public:
    static constexpr int FIRST = 2;   // 0 — «назад», 1 — «+ новый», дальше проекты

    void onOpen() override {
        index_ = 0;
        reload();
    }

    void reload() {
        projectsLoaded = false;
        projects.clear();
        sendType("project_list");
    }

    void draw() override {
        drawItems(-1);
        drawHint();
    }

    void refresh() {
        index_ = constrain(index_, 0, count() - 1);
        draw();
    }

    void onRotate(int dir) override {
        int prev = index_;
        index_ = wrap(index_ + dir, count());
        if (drawItems(prev)) drawHint();   // окно сдвинулось, экран перерисован целиком
    }

    void onClick() override {
        if (index_ == 0) {
            back();
        } else if (index_ == 1) {
            awaitingSwitch = true;
            sendType("project_new");      // имя придумает модель после первого вопроса
        } else if (projectsLoaded) {
            const String& name = projects[index_ - FIRST].name;
            if (name == projectName) {
                go(Mode::Home);
            } else {
                awaitingSwitch = true;    // ответ project переключит экран на главный
                JsonDocument doc;
                doc["type"] = "project_switch";
                doc["name"] = name;
                sendJson(doc);
            }
        }
    }

    bool onHold() override {
        if (projectsLoaded && index_ >= FIRST) {
            deleteScreen.ask(projects[index_ - FIRST].name);
            openScreen(&deleteScreen);
        }
        return true;   // на списке проектов удержание = удалить, не «говорить»
    }

private:
    bool drawItems(int prev) {
        return drawList("Проекты", count(), index_, [](int i, uint16_t& color, bool sel) -> String {
            if (i == 0) return BACK_LABEL;
            if (i == 1) {
                color = sel ? SPEAK : rgb(50, 140, 80);
                return "+ новый проект";
            }
            if (!projectsLoaded) return wsUp ? "загружаю..." : "сервер недоступен";
            const ProjectItem& p = projects[i - FIRST];
            bool current = p.name == projectName;
            if (current) color = sel ? IDLE : rgb(40, 140, 150);
            return String(current ? "* " : "  ") + p.name + "  (" + String(p.turns) + ")";
        }, prev);
    }

    void drawHint() { text(W - 150, BAR_H + 16, "держи: удалить", DIM); }

    int count() const { return FIRST + (projectsLoaded ? (int)projects.size() : 1); }
    int index_ = 0;
};

HistoryScreen historyScreen;
ProjectsScreen projectsScreen;

void DeleteScreen::onClick() {
    if (choiceYes) {
        JsonDocument doc;
        doc["type"] = name_ == DEFAULT_PROJECT ? "project_clear" : "project_delete";
        doc["name"] = name_;
        sendJson(doc);
        // Свежий список: при успехе сервер пришлёт его сам, а при ошибке (например, агент
        // занят) — нет, и экран завис бы на «загружаю...»
        projectsScreen.reload();
    }
    back();
}

VolumeScreen volumeScreen;
InfoScreen mailScreen("Почта", "скоро будет");

// ---------- Wi-Fi: подключение и ввод пароля ----------

// Подключение к сети: «подключаюсь...», потом результат. Получилось — сеть сохраняется
// в NVS. Не вышло — возвращаемся к прежней сети, клик — назад к паролю (введённое цело)
class ConnectScreen : public Screen {
public:
    static constexpr uint32_t TIMEOUT_MS = 15000;

    void start(const String& ssid, const String& pass) {
        ssid_ = ssid;
        pass_ = pass;
    }

    void onOpen() override {
        // Запоминаем прежнюю сеть: если новая не подключится, вернёмся на неё
        prevSsid_ = WiFi.SSID();
        prevPass_ = WiFi.psk();
        bootFallbackAt = 0;              // подключаемся сами — запасная сеть при старте не нужна
        state_ = State::Connecting;
        startedAt_ = millis();
        lastStatus_ = WL_NO_SHIELD;
        log("wifi: connecting to " + ssid_);
        WiFi.begin(ssid_.c_str(), pass_.c_str());
    }

    void draw() override {
        clearBody();
        text(8, BAR_H + 17, "Wi-Fi", IDLE, FONT_BIG);
        textFit(8, 72, ssid_, W - 16, TEXT, FONT_BIG);
        if (state_ == State::Connecting) {
            drawProgress();
        } else if (state_ == State::Ok) {
            text(8, 104, "подключено, сеть сохранена", SPEAK);
            text(8, H - 6, "клик: готово", DIM);
        } else {
            text(8, 104, "не подключилось.", ERR);
            text(8, 124, "неверный пароль или сеть далеко", DIM);
            text(8, H - 6, "клик: назад к паролю", DIM);
        }
    }

    void tick() override {
        if (state_ != State::Connecting) return;
        wl_status_t st = WiFi.status();
        uint32_t ms = millis() - startedAt_;
        // Этапы в лог: видно, на каком шаге ждём и сколько
        if (st != lastStatus_) {
            log("wifi: status " + String((int)st) + " at " + String(ms) + " ms");
            lastStatus_ = st;
            drawProgress();
        } else if (ms / 1000 != shownSec_) {
            drawProgress();              // секундомер
        }
        bool joined = joinedNow();
        if (st == WL_CONNECTED && WiFi.SSID() == ssid_) {
            state_ = State::Ok;
            wifiStore.remember(ssid_, pass_);
            log("wifi: connected, saved");
            draw();
        } else if (ms > (joined ? TIMEOUT_MS + DHCP_EXTRA_MS : TIMEOUT_MS)) {
            state_ = State::Failed;
            log("wifi: failed, back to " + prevSsid_);
            if (prevSsid_.length()) WiFi.begin(prevSsid_.c_str(), prevPass_.c_str());
            else WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            draw();
        }
    }

    void onClick() override {
        if (state_ == State::Ok) go(Mode::Home);
        else if (state_ == State::Failed) back();
        // Пока подключаемся — клик ничего не делает: результат через несколько секунд
    }

    bool onHold() override { return true; }

private:
    // Уже соединились с роутером (видна сила сигнала), но ещё ждём IP-адрес. Ядро Arduino
    // в это время отдаёт WL_IDLE_STATUS, а WL_CONNECTED — только когда адрес получен
    static constexpr uint32_t DHCP_EXTRA_MS = 10000;   // раздаче с телефона нужно время на адрес

    bool joinedNow() const { return WiFi.status() == WL_IDLE_STATUS && WiFi.SSID() == ssid_; }

    void drawProgress() {
        uint32_t sec = (millis() - startedAt_) / 1000;
        shownSec_ = sec;
        tft.fillRect(0, 88, W, 24, BG);
        String stage = joinedNow() ? "получаю адрес..." : "подключаюсь к сети...";
        text(8, 104, stage + "  " + String(sec) + " c", DIM);
    }

    enum class State { Connecting, Ok, Failed };
    State state_ = State::Connecting;
    String ssid_, pass_, prevSsid_, prevPass_;
    uint32_t startedAt_ = 0, shownSec_ = 0;
    wl_status_t lastStatus_ = WL_NO_SHIELD;
};

ConnectScreen connectScreen;

// Ввод пароля энкодером. Внизу — лента символов: крутишь — выбираешь, клик — ввести.
// В начале ленты служебные: OK (подключиться), «стереть» и переключение набора
// (abc / ABC / 123). Удержание энкодера — тоже OK, чтобы не крутить к началу ленты
class PasswordScreen : public Screen {
public:
    void start(const String& ssid) { ssid_ = ssid; }

    void onOpen() override {
        pass_ = "";
        setCharset(0);
    }

    void draw() override {
        clearBody();
        textFit(8, BAR_H + 17, "Пароль: " + ssid_, W - 16, IDLE, FONT_BIG);
        text(8, H - 6, "клик: ввести   держи: подключить", DIM);
        drawField();
        drawRibbon();
    }

    void onRotate(int dir) override {
        index_ = wrap(index_ + dir, (int)keys_.size());
        drawRibbon();
    }

    void onClick() override {
        const Key& k = keys_[index_];
        switch (k.kind) {
        case Kind::Char:
            if (pass_.length() < 63) pass_ += k.ch;   // 63 — предел длины пароля Wi-Fi
            drawField();
            break;
        case Kind::Del:
            if (pass_.length()) pass_.remove(pass_.length() - 1);
            drawField();
            break;
        case Kind::Set:
            setCharset(k.ch);
            drawRibbon();
            break;
        case Kind::Ok:
            connect();
            break;
        }
    }

    bool onHold() override {
        connect();
        return true;                     // здесь удержание — «подключить», не «говорить»
    }

private:
    enum class Kind { Char, Del, Set, Ok };
    struct Key { String label; Kind kind; char ch; };

    // Наборы символов: только английская раскладка, пароли Wi-Fi обычно такие
    static constexpr const char* SETS[] = {
        "abcdefghijklmnopqrstuvwxyz",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        "0123456789!@#$%^&*()-_=+.,:;?/\\|'\"<>[]{}~` ",
    };
    static constexpr const char* SET_NAMES[] = {"abc", "ABC", "123"};
    static constexpr int SERVICE = 4;    // OK, стереть и два переключателя набора

    void setCharset(int set) {
        keys_.clear();
        keys_.push_back(Key{"OK", Kind::Ok, 0});
        keys_.push_back(Key{"стереть", Kind::Del, 0});
        for (int s = 0; s < 3; s++) {
            if (s != set) keys_.push_back(Key{SET_NAMES[s], Kind::Set, (char)s});
        }
        for (const char* c = SETS[set]; *c; c++) {
            keys_.push_back(Key{*c == ' ' ? String("пробел") : String(*c), Kind::Char, *c});
        }
        index_ = SERVICE;                // курсор — на первый символ набора
    }

    void connect() {
        connectScreen.start(ssid_, pass_);
        openScreen(&connectScreen);
    }

    // Поле пароля: видны последние символы, если не влезает. Пароль показываем открыто:
    // вводить вслепую энкодером слишком легко ошибиться
    void drawField() {
        const int x = 8, y = BAR_H + 30, w = W - 16, h = 32;
        tft.fillRoundRect(x, y, w, h, 8, PANEL);
        u8f.setFont(FONT_BIG);
        String shown = pass_ + "_";
        while (shown.length() > 1 && u8f.getUTF8Width(shown.c_str()) > w - 16) shown.remove(0, 1);
        text(x + 8, y + 23, shown, TEXT, FONT_BIG);
    }

    // Лента: выбранный символ в рамке по центру, соседи по бокам
    void drawRibbon() {
        const int y = 100, cx = W / 2, step = 44, boxW = 72, boxH = 34;
        tft.fillRect(0, y - 4, W, boxH + 8, BG);
        int n = keys_.size();
        for (int k = -3; k <= 3; k++) {
            const Key& key = keys_[wrap(index_ + k, n)];
            // Служебные кнопки длинные — у соседей показываем коротко
            String label = key.label;
            if (k != 0 && key.kind == Kind::Del) label = "<-";
            int x = cx + k * step + (k > 0 ? boxW / 2 - step / 2 : k < 0 ? -(boxW / 2 - step / 2) : 0);
            if (k == 0) {
                tft.fillRoundRect(cx - boxW / 2, y, boxW, boxH, 8, PANEL);
                tft.drawRoundRect(cx - boxW / 2, y, boxW, boxH, 8, IDLE);
                textCenter(cx, y + 24, label, boxW - 6, key.kind == Kind::Char ? TEXT : IDLE,
                           key.label.length() > 3 ? FONT : FONT_BIG);
            } else {
                textCenter(x, y + 23, label, step - 4, DIM);
            }
        }
    }

    String ssid_, pass_;
    std::vector<Key> keys_;
    int index_ = 0;
};

constexpr const char* PasswordScreen::SETS[];
constexpr const char* PasswordScreen::SET_NAMES[];

PasswordScreen passwordScreen;

// Сила сигнала палочками, как значок Wi-Fi: rssi ближе к 0 — лучше
String signalBars(int rssi) {
    int bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
    String s;
    for (int i = 0; i < 4; i++) s += i < bars ? "|" : ".";
    return s;
}

// Сети рядом. Поиск в фоне (scanNetworks(true)): обычный поиск морозит плату на 2-3 с.
// Пока ищем — «ищу сети...», tick() забирает результат, когда он готов
class NetworksScreen : public Screen {
public:
    static constexpr int FIRST = 2;   // 0 — «назад», 1 — «обновить», дальше сети
    static constexpr uint32_t SCAN_MAX_MS = 15000;

    struct Net { String ssid; int rssi; bool secure; };

    void onOpen() override {
        index_ = 0;
        scan();
    }

    void draw() override { drawItems(-1); }

    void onRotate(int dir) override {
        int prev = index_;
        index_ = wrap(index_ + dir, count());
        drawItems(prev);
    }

    void onClick() override {
        if (index_ == 0) back();
        else if (index_ == 1 && !scanning_) scan();
        else if (index_ >= FIRST && !scanning_ && !nets_.empty()) openNet(nets_[index_ - FIRST]);
    }

    void tick() override {
        if (!scanning_) return;
        int n = WiFi.scanComplete();          // >= 0 — готово, -1 — ищет, -2 — «не удался»
        if (n == WIFI_SCAN_RUNNING) return;
        // Ядро Arduino считает поиск неудачным через 6 с, но пока плата подключена к сети,
        // поиск идёт дольше (между каналами она возвращается на свой). Сам поиск при этом
        // продолжается, и результат приходит позже — ждём его до SCAN_MAX_MS
        if (n == WIFI_SCAN_FAILED && millis() - scanAt_ < SCAN_MAX_MS) return;
        scanning_ = false;
        nets_.clear();
        for (int i = 0; i < n; i++) {
            String ssid = WiFi.SSID(i);
            if (!ssid.length()) continue;     // скрытые сети без имени не показываем
            // Одна сеть может прийти от нескольких роутеров: оставляем самый сильный сигнал
            bool merged = false;
            for (Net& known : nets_) {
                if (known.ssid == ssid) {
                    known.rssi = max(known.rssi, (int)WiFi.RSSI(i));
                    merged = true;
                }
            }
            if (!merged) nets_.push_back(Net{ssid, (int)WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
        }
        WiFi.scanDelete();                    // результаты лежат в памяти Wi-Fi — освобождаем
        std::sort(nets_.begin(), nets_.end(), [](const Net& a, const Net& b) { return a.rssi > b.rssi; });
        failed_ = n < 0;
        index_ = constrain(index_, 0, count() - 1);
        draw();
    }

private:
    // Сеть без пароля или уже сохранённая — сразу подключаемся, иначе — ввод пароля
    void openNet(const Net& n) {
        if (wifiUp && n.ssid == WiFi.SSID()) return;   // уже на ней
        const WifiNet* saved = wifiStore.find(n.ssid);
        if (!n.secure || saved) {
            connectScreen.start(n.ssid, saved ? saved->pass : String(""));
            openScreen(&connectScreen);
        } else {
            passwordScreen.start(n.ssid);
            openScreen(&passwordScreen);
        }
    }

    void scan() {
        nets_.clear();
        failed_ = false;
        // true — в фоне: функция сразу возвращается, результат заберёт tick()
        scanAt_ = millis();
        // Не запустился — скорее всего, ещё идёт прошлый поиск (ушли с экрана и вернулись).
        // Тогда просто ждём его результат в tick()
        WiFi.scanNetworks(true);
        scanning_ = true;
        draw();
    }

    void drawItems(int prev) {
        drawList("Сети рядом", count(), index_, [this](int i, uint16_t& color, bool sel) -> String {
            if (i == 0) return BACK_LABEL;
            if (i == 1) {
                color = sel ? SPEAK : rgb(50, 140, 80);
                return scanning_ ? "ищу сети..." : "обновить";
            }
            if (scanning_) return "";
            if (failed_) return "поиск не удался";
            if (nets_.empty()) return "(сетей не видно)";
            const Net& n = nets_[i - FIRST];
            bool current = wifiUp && n.ssid == WiFi.SSID();
            if (current) color = sel ? IDLE : rgb(40, 140, 150);
            // * — текущая сеть; сети без пароля помечаем: к ним подключаемся без ввода
            return String(current ? "* " : "  ") + signalBars(n.rssi) + "  " + n.ssid + (n.secure ? "" : "  (открытая)");
        }, prev);
    }

    int count() const { return FIRST + (scanning_ ? 0 : max(1, (int)nets_.size())); }

    std::vector<Net> nets_;
    uint32_t scanAt_ = 0;
    bool scanning_ = false, failed_ = false;
    int index_ = 0;
};

NetworksScreen networksScreen;

// «Забыть?» поверх списка сохранённых. По умолчанию «Нет»
class ForgetScreen : public Screen {
public:
    void ask(const String& ssid) { ssid_ = ssid; }
    void onOpen() override { choiceYes = false; }
    bool showsFace() const override { return true; }
    bool onHold() override { return true; }

    void draw() override {
        clearBody();
        face.setMood(Mood::Ask);
        text(COL_X, 48, "Забыть?", ERR, FONT_BIG);
        textFit(COL_X, 68, ssid_, COL_W, TEXT);
        text(COL_X, 88, "пароль сотрётся", DIM);
        text(COL_X, 104, "из памяти платы", DIM);
        drawChoice();
    }

    void onRotate(int) override {
        choiceYes = !choiceYes;
        drawChoice();
    }

    void onClick() override {
        // Текущая сеть не отключается: забытая просто не выберется при следующем включении
        if (choiceYes) wifiStore.forget(ssid_);
        back();
    }

private:
    String ssid_;
};

ForgetScreen forgetScreen;

// Сохранённые сети (NVS). Клик — подключиться, удержание — «Забыть?».
// Сеть из secrets.h сюда не входит: она в прошивке, запасная
class SavedWifiScreen : public Screen {
public:
    void onOpen() override { index_ = 0; }

    void draw() override {
        index_ = constrain(index_, 0, count() - 1);   // после «Забыть» пунктов меньше
        drawItems(-1);
        text(W - 150, BAR_H + 16, "держи: забыть", DIM);
    }

    void onRotate(int dir) override {
        int prev = index_;
        index_ = wrap(index_ + dir, count());
        if (drawItems(prev)) text(W - 150, BAR_H + 16, "держи: забыть", DIM);
    }

    void onClick() override {
        if (index_ == 0) {
            back();
            return;
        }
        const auto& nets = wifiStore.nets();
        if (nets.empty()) return;
        const WifiNet& n = nets[index_ - 1];
        if (wifiUp && n.ssid == WiFi.SSID()) return;   // уже на ней
        connectScreen.start(n.ssid, n.pass);
        openScreen(&connectScreen);
    }

    bool onHold() override {
        const auto& nets = wifiStore.nets();
        if (index_ >= 1 && !nets.empty()) {
            forgetScreen.ask(nets[index_ - 1].ssid);
            openScreen(&forgetScreen);
        }
        return true;   // здесь удержание — «забыть», не «говорить»
    }

private:
    bool drawItems(int prev) {
        return drawList("Сохранённые", count(), index_, [](int i, uint16_t& color, bool sel) -> String {
            if (i == 0) return BACK_LABEL;
            const auto& nets = wifiStore.nets();
            if (nets.empty()) return "(пока нет)";
            const WifiNet& n = nets[i - 1];
            bool current = wifiUp && n.ssid == WiFi.SSID();
            if (current) color = sel ? IDLE : rgb(40, 140, 150);
            return String(current ? "* " : "  ") + n.ssid;
        }, prev);
    }

    int count() const { return 1 + max(1, (int)wifiStore.nets().size()); }
    int index_ = 0;
};

SavedWifiScreen savedWifiScreen;

MenuScreen wifiMenu("Wi-Fi", {
    {"Сети рядом", [] { openScreen(&networksScreen); }},
    {"Сохранённые", [] { openScreen(&savedWifiScreen); }},
});

// Настройки платы. Новая настройка — пункт здесь (значение хранить в settings.h)
MenuScreen settingsMenu("Настройки", {
    {"Громкость", [] { openScreen(&volumeScreen); },
     [] { return String(settings.volume) + " из " + String(Settings::VOLUME_MAX); }},
    {"Озвучка",
     [] {
         settings.speech = !settings.speech;
         settings.save();
         if (!settings.speech) stopSpeech();
         if (wsUp) sendHello();       // сервер узнаёт сразу, без переподключения
         nav.top()->draw();
     },
     [] { return String(settings.speech ? "вкл" : "выкл"); }},
});

// ---------- голосовые заметки (notes.h) ----------

// Прослушать: файл целиком в память, дальше pumpPlayback() кормит динамик кусками
bool startPlayback(const String& name) {
    if (!speakerOk || speechActive || !notes.load(name)) return false;
    speaker.start(Notes::RATE);
    playingNote = name;
    playPos = 0;
    return true;
}

void stopPlayback() {
    if (!playingNote.length()) return;
    playingNote = "";
    speaker.stop();
}

// В каждом loop(): докладываем звук в буфер динамика, сколько влезет
void pumpPlayback() {
    if (!playingNote.length()) return;
    if (playPos < notes.length()) {
        size_t chunk = min((size_t)4096, notes.length() - playPos);
        playPos += speaker.push(notes.data() + playPos, chunk);
    } else if (!speaker.playing()) {
        playingNote = "";                 // доиграла
    }
}

// Отправить агенту: тот же путь, что живой голос (audio_start, кадры, audio_end).
// Файл удалим, когда сервер пришлёт transcript: значит, звук дошёл и распознан
void sendNote(const String& name) {
    stopPlayback();
    if (!wsUp || !notes.load(name)) return;
    sendType("audio_start");
    sendingNote = true;
    sendPos = 0;
    noteToDelete = name;
    question = "";
    thinking = "отправляю заметку";
    nav.clear();
    go(Mode::Think);
}

// Кадры по 2 КБ, как с микрофона, и не больше 16 за проход loop(): экран не замирает
void pumpNoteSend() {
    if (!sendingNote) return;
    const size_t FRAME = 2048;
    for (int i = 0; i < 16 && sendPos < notes.length(); i++) {
        size_t n = min(FRAME, notes.length() - sendPos);
        ws.sendBIN((uint8_t*)notes.data() + sendPos, n);
        sendPos += n;
    }
    if (sendPos >= notes.length()) {
        sendingNote = false;
        sendType("audio_end");
        thinking = "разбираю заметку";
        if (mode == Mode::Think) drawThink();
    }
}

// Отправка сорвалась (обрыв, отмена, ошибка распознавания): файл остаётся на карте
void keepNote() {
    sendingNote = false;
    noteToDelete = "";
}

// Действия с заметкой (удержание в списке): «< назад», «Отправить», «Удалить»
class NoteActionsScreen : public Screen {
public:
    void ask(const NoteInfo& n) { note_ = n; }
    void onOpen() override { index_ = 0; }   // курсор на «назад»

    void draw() override { drawItems(-1); }

    void onRotate(int dir) override {
        int prev = index_;
        index_ = wrap(index_ + dir, 3);
        drawItems(prev);
    }

    void onClick() override {
        if (index_ == 0) {
            back();
        } else if (index_ == 1) {
            if (wsUp) sendNote(note_.name);  // без связи пункт серый и не работает
        } else {
            stopPlayback();
            notes.remove(note_.name);
            back();
        }
    }

    bool onHold() override { return true; }

private:
    void drawItems(int prev) {
        String title = "Заметка " + Notes::label(note_);
        drawList(title.c_str(), 3, index_, [this](int i, uint16_t& color, bool sel) -> String {
            if (i == 0) return BACK_LABEL;
            if (i == 1) {
                if (!wsUp) {
                    color = LINE;
                    return "Отправить (сервер недоступен)";
                }
                return "Отправить агенту";
            }
            color = sel ? ERR : rgb(150, 70, 70);
            return "Удалить";
        }, prev);
    }

    NoteInfo note_;
    int index_ = 0;
};

NoteActionsScreen noteActionsScreen;

// Список заметок: клик — прослушать (ещё клик — стоп), удержание — действия
class NotesScreen : public Screen {
public:
    void onOpen() override {
        index_ = 0;
        if (!notes.ok()) notes.begin(tft.getSPIinstance(), BOARD_SD_CS);   // карту вставили позже
    }

    void draw() override {
        items_ = notes.ok() ? notes.list() : std::vector<NoteInfo>();   // после «Удалить» список свежий
        index_ = constrain(index_, 0, count() - 1);
        drawItems(-1);
        drawHint();
    }

    void onRotate(int dir) override {
        int prev = index_;
        index_ = wrap(index_ + dir, count());
        if (drawItems(prev)) drawHint();
    }

    void onClick() override {
        if (index_ == 0) {
            stopPlayback();
            back();
            return;
        }
        if (items_.empty()) return;
        const String& name = items_[index_ - 1].name;
        bool wasThis = playingNote == name;
        stopPlayback();
        if (!wasThis) startPlayback(name);
        drawItems(-1);
        drawHint();
    }

    bool onHold() override {
        if (index_ >= 1 && !items_.empty()) {
            noteActionsScreen.ask(items_[index_ - 1]);
            openScreen(&noteActionsScreen);
        }
        return true;   // здесь удержание — действия с заметкой, не «говорить»
    }

    // Заметка доиграла — убрать значок «играет»
    void tick() override {
        if (shownPlaying_ != playingNote) {
            drawItems(-1);
            drawHint();
        }
    }

private:
    bool drawItems(int prev) {
        shownPlaying_ = playingNote;
        return drawList("Заметки", count(), index_, [this](int i, uint16_t& color, bool sel) -> String {
            if (i == 0) return BACK_LABEL;
            if (!notes.ok()) return "нет SD-карты";
            if (items_.empty()) return "(пока нет)";
            const NoteInfo& n = items_[i - 1];
            bool playing = playingNote == n.name;
            if (playing) color = sel ? SPEAK : rgb(50, 140, 80);
            return String(playing ? "> " : "  ") + Notes::label(n) + "   " + String(n.seconds) + " с";
        }, prev);
    }

    void drawHint() { text(W - 150, BAR_H + 16, "держи: действия", DIM); }

    int count() const { return 1 + max(1, (int)items_.size()); }

    std::vector<NoteInfo> items_;
    String shownPlaying_;
    int index_ = 0;
};

NotesScreen notesScreen;

// Карточка меню: заголовок, строка-подсказка (текущее значение) и что открыть
struct Card {
    Card(const char* title, std::function<String()> sub, std::function<void()> open)
        : title(title), sub(std::move(sub)), open(std::move(open)) {}
    const char* title;
    std::function<String()> sub;
    std::function<void()> open;
};

// Меню карточками: одна карточка на экране, крутишь — следующая (по кругу), клик — открыть.
// Открывается и закрывается боковой кнопкой. Карточек может быть сколько угодно:
// внизу точки показывают, где ты
class CardsScreen : public Screen {
public:
    explicit CardsScreen(std::vector<Card> cards) : cards_(std::move(cards)) {}

    // index_ не сбрасываем: меню откроется на той карточке, где его закрыли
    void draw() override {
        clearBody();
        drawCard();
    }

    void onRotate(int dir) override {
        index_ = wrap(index_ + dir, (int)cards_.size());
        drawCard();
    }

    void onClick() override { cards_[index_].open(); }

private:
    void drawCard() {
        const int x = 36, y = BAR_H + 12, w = W - 2 * x, h = 96, cx = W / 2;
        tft.fillRect(0, y - 2, W, H - y + 2, BG);
        tft.fillRoundRect(x, y, w, h, 12, PANEL);
        tft.drawRoundRect(x, y, w, h, 12, IDLE);
        const Card& c = cards_[index_];
        textCenter(cx, y + 42, c.title, w - 20, TEXT, FONT_BIG);
        textCenter(cx, y + 72, c.sub(), w - 20, DIM);
        // Стрелки по бокам: туда можно крутить
        int my = y + h / 2;
        tft.fillTriangle(x - 10, my - 8, x - 10, my + 8, x - 20, my, DIM);
        tft.fillTriangle(x + w + 10, my - 8, x + w + 10, my + 8, x + w + 20, my, DIM);
        // Точки: сколько карточек и какая сейчас
        const int n = cards_.size(), gap = 12, dy = y + h + 18;
        int dx = cx - (n - 1) * gap / 2;
        for (int i = 0; i < n; i++) {
            if (i == index_) tft.fillCircle(dx + i * gap, dy, 4, IDLE);
            else tft.fillCircle(dx + i * gap, dy, 2, DIM);
        }
        text(8, H - 6, "клик: открыть", DIM);
        text(W - 130, H - 6, "боковая: выход", DIM);
    }

    std::vector<Card> cards_;
    int index_ = 0;
};

// Новая карточка — строка здесь
CardsScreen cardsScreen({
    {"Проекты", [] { return projectName; }, [] { openScreen(&projectsScreen); }},
    {"История", [] { return String("последние вопросы"); }, [] { openScreen(&historyScreen); }},
    {"Настройки",
     [] { return "громкость " + String(settings.volume) + ", озвучка " + (settings.speech ? "вкл" : "выкл"); },
     [] { openScreen(&settingsMenu); }},
    {"Заметки",
     [] { return notes.ok() ? String(notes.count()) + " шт." : String("нет SD-карты"); },
     [] { openScreen(&notesScreen); }},
    {"Почта", [] { return String("скоро"); }, [] { openScreen(&mailScreen); }},
    {"Wi-Fi",
     [] { return wifiUp ? WiFi.SSID() + "  " + signalBars(WiFi.RSSI()) : String("нет сети"); },
     [] { openScreen(&wifiMenu); }},
});

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
        // Отправленную заметку сервер распознал — с карты её можно убирать
        if (noteToDelete.length()) {
            notes.remove(noteToDelete);
            noteToDelete = "";
        }
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
        if (awaitingSwitch) {
            // Переключились (или создали новый) из списка проектов — на главный
            awaitingSwitch = false;
            if (mode == Mode::Nav) go(Mode::Home);
        } else if (mode == Mode::Home) {
            drawHome();
        }
    } else if (type == "projects") {
        projects.clear();
        for (JsonObject it : doc["items"].as<JsonArray>()) {
            projects.push_back(ProjectItem{it["name"] | "", it["turns"] | 0});
        }
        projectsLoaded = true;
        if (showing(&projectsScreen)) projectsScreen.refresh();
    } else if (type == "history") {
        history.clear();
        for (JsonObject it : doc["items"].as<JsonArray>()) {
            history.push_back(Entry{it["q"] | "", it["a"] | "", it["error"] | false});
        }
        historyLoaded = true;
        if (showing(&historyScreen)) historyScreen.refresh();
    } else if (type == "speech_start") {
        speaker.start(doc["sample_rate"] | 22050);
        speechActive = true;
    } else if (type == "speech_end") {
        speechActive = false;        // буфер доиграет сам
    } else if (type == "result" || type == "error") {
        bool isError = type == "error";
        if (isError) keepNote();     // «не расслышал» и т.п.: заметка остаётся на карте
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
        sendHello();
        drawTopBar();
        if (mode == Mode::Home) drawHome();
        break;
    }
    case WStype_DISCONNECTED:
        probe.lost();                // прежде чем подключаться снова — проверить, что сервер жив
        if (wsUp) {
            // Сервер при обрыве отменяет задачу, поэтому и мы возвращаемся на главный.
            // Висящее подтверждение не «досылаем»: после переподключения оно недействительно.
            wsUp = false;
            confirmId = "";
            awaitingSwitch = false;
            keepNote();
            recordingNote = false;
            speechActive = false;
            speaker.stop();
            drawTopBar();
            // В меню остаёмся: звук настраивается и без связи, а списки покажут «сервер недоступен»
            if (mode == Mode::Nav) nav.top()->draw();
            else go(Mode::Home);
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
    case Mode::Nav:
        nav.top()->onRotate(dir);
        break;
    case Mode::Confirm:
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
    // Удержание — начать говорить. Кроме карточки, записи и работы. В меню экран может
    // забрать удержание себе (в проектах это «удалить»)
    if (ev == ButtonEvent::HoldStart) {
        if (mode == Mode::Nav && nav.top()->onHold()) return;
        if (mode == Mode::Home || mode == Mode::Answer || mode == Mode::Nav) {
            startListening();
            return;
        }
    }
    switch (mode) {
    case Mode::Listen:
        if (ev == ButtonEvent::HoldEnd) finishListening();
        break;
    case Mode::Think:
        if (ev == ButtonEvent::Click) {
            keepNote();               // отменили посреди отправки заметки — она остаётся
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
            // Ответ открыт из истории — назад в историю, ответ агента — на главный
            if (nav.empty()) {
                go(Mode::Home);
            } else {
                mode = Mode::Nav;
                nav.top()->draw();
            }
        }
        break;
    case Mode::Nav:
        if (ev == ButtonEvent::Click) nav.top()->onClick();
        break;
    default:
        break;
    }
}

// ---------- боковая кнопка ----------

// Перерисовать то, что сейчас на экране (после полоски отсчёта)
void redraw() {
    if (mode == Mode::Answer) drawAnswer(currentAnswer);
    else drawScreen();
}

// Клик: карточки открыть или закрыть. Пока агент слушает, думает или ждёт «Да/Нет» — не мешаем
void onSideClick() {
    if (mode == Mode::Nav) {
        go(Mode::Home);
    } else if (mode == Mode::Home || mode == Mode::Answer) {
        stopSpeech();
        nav.clear();
        openScreen(&cardsScreen);
    }
}

// Полоска внизу: сколько ещё держать до сна
void drawOffBar(uint32_t held) {
    const int y = H - 28;
    tft.fillRect(0, y, W, H - y, PANEL);
    if (held >= OFF_HOLD_MS) {
        text(8, y + 18, "отпусти — усну", ERR);
        return;
    }
    int left = (OFF_HOLD_MS - held + 999) / 1000;   // секунды, с округлением вверх
    text(8, y + 18, "сон: держи ещё " + String(left) + " c", TEXT);
    const int bx = 190, bw = W - bx - 8;
    tft.fillRoundRect(bx, y + 10, bw, 8, 4, LINE);
    tft.fillRoundRect(bx, y + 10, bw * held / OFF_HOLD_MS, 8, 4, ERR);
}

void handleSideKey(ButtonEvent ev) {
    if (ev == ButtonEvent::Click) {
        if (!touch()) onSideClick();   // погасший экран клик только будит
        return;
    }
    if (ev == ButtonEvent::HoldEnd) {
        // Сон только после отпускания: зажатая кнопка разбудила бы плату сразу
        if (offArmed) goToSleep();
        offArmed = offShown = false;
        redraw();                      // отпустили раньше — отмена, убираем полоску
        return;
    }
    uint32_t held = userKey.heldMs();
    if (held < Button::HOLD_MS) return;
    touch();
    static uint32_t lastDraw = 0;
    if (!offShown || millis() - lastDraw > 100) {
        offShown = true;
        lastDraw = millis();
        drawOffBar(held);
    }
    if (held >= OFF_HOLD_MS) offArmed = true;
}

// ---------- setup / loop ----------

void setup() {
    Serial.begin(115200);
    // Печать в лог никогда не ждёт. По умолчанию при полном буфере USB ждёт до 100 мс,
    // а если плата подключена к ПК без открытого монитора порта, буфер не освобождается:
    // каждая строка лога тормозила экран и энкодер. Теперь лишние строки просто теряются
    Serial.setTxTimeoutMs(0);

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
    // SD-карта на той же шине SPI, что экран: только после tft.begin()
    bool sdOk = notes.begin(tft.getSPIinstance(), BOARD_SD_CS);
    encoder.begin();                 // аппаратный счётчик щелчков (encoder.h)
    button.begin();
    userKey.begin();
    micOk = mic.begin();
    speakerOk = speaker.begin();
    settings.load();                 // громкость и озвучка из NVS
    speaker.setVolume(settings.gain());
    battery.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    bat = battery.read();
    Serial.printf("face: %s, mic: %s, speaker: %s, sd: %s (%d notes), battery: %s %d%% %dmV usb=%d charging=%d\n",
                  faceOk ? "ok" : "FAILED", micOk ? "ok" : "FAILED", speakerOk ? "ok" : "FAILED",
                  sdOk ? "ok" : "none", notes.count(),
                  bat.ok ? "ok" : "FAILED", bat.percent, bat.millivolts, bat.usb, bat.charging);

    // Часы по интернету (NTP), время Польши: для дат в именах заметок. Сверяются сами,
    // когда появится Wi-Fi; в глубоком сне идут дальше, при выключении сбрасываются
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org");

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    wifiStore.load();
    const WifiNet* last = wifiStore.last();
    if (last && last->ssid != WIFI_SSID) {
        WiFi.begin(last->ssid.c_str(), last->pass.c_str());
        bootFallbackAt = millis() + 15000;
    } else {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }

    drawTopBar();
    go(Mode::Home);

    // Токен в заголовке, а не в адресе: адрес сервер пишет в лог.
    ws.begin(AGENT_HOST, AGENT_PORT, "/ws");
    ws.setExtraHeaders("Authorization: Bearer " AGENT_TOKEN);
    ws.onEvent(onWsEvent);
    ws.setReconnectInterval(3000);
    ws.enableHeartbeat(15000, 3000, 2);   // заметить обрыв, даже если сервер молчит
    probe.begin(AGENT_HOST, AGENT_PORT);

    lastActivity = millis();
    Serial.printf("hardware-agent firmware: face UI (%s)\n", Power::wokeFromSleep() ? "woke from sleep" : "power on");
}

void loop() {
    // Подключение к серверу блокирует цикл, пока сервер не ответит (до 5 с). Поэтому, пока
    // связи нет, ws.loop() зовём, только когда фоновая проверка увидела живой сервер
    if (wsUp || probe.reachable()) ws.loop();
    // Сервер ответил на проверку, но подключиться не вышло — проверять заново
    if (!wsUp && probe.reachable() && probe.sinceOk() > 10000) probe.lost();
    if (mode == Mode::Listen) pumpAudio();
    pumpPlayback();
    pumpNoteSend();

    // Wi-Fi
    bool up = WiFi.status() == WL_CONNECTED;
    if (up != wifiUp) {
        wifiUp = up;
        log(up ? "wifi " + WiFi.localIP().toString() : "wifi lost");
        drawTopBar();
        if (mode == Mode::Home) drawHome();
    }

    // Сохранённая сеть при старте не подключилась — запасная из прошивки
    if (bootFallbackAt && (int32_t)(millis() - bootFallbackAt) > 0) {
        bootFallbackAt = 0;
        if (!wifiUp) {
            log("wifi: saved network failed, using secrets.h");
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
    }

    // Батарея и сила Wi-Fi: раз в 10 с хватает
    static uint32_t lastBar = 0;
    if (millis() - lastBar > 10000) {
        lastBar = millis();
        bat = battery.read();
        drawTopBar();
    }

    updatePower();
    if (mode == Mode::Nav) nav.top()->tick();   // фоновая работа экрана (поиск сетей)

    // Анимация ~25 кадров в секунду. Экран погас — не рисуем, экономим процессор
    static uint32_t lastFrame = 0;
    if (!screenOff && millis() - lastFrame > 40) {
        lastFrame = millis();
        if (faceOk && faceVisible()) face.draw(FACE_X, FACE_Y);
        if (mode == Mode::Answer) drawSpeakingBars();
    }

    // Щелчки считает железо (encoder.h), здесь забираем накопленные: пока рисовался
    // экран, их могло набраться несколько. По часовой = +1 (вниз по списку, громче)
    int steps = encoder.readSteps();
    if (steps != 0) {
        // Кнопка энкодера нажата — поворот случайный: при нажатии ручку легко чуть провернуть,
        // и курсор уехал бы на соседний пункт перед самым кликом
        bool knobDown = digitalRead(ENCODER_KEY) == LOW;
        // При выключенном экране поворот только будит
        if (!knobDown && !touch()) onRotate(steps);
    }

    ButtonEvent ev = button.poll();
    if (ev != ButtonEvent::None) {
        bool wasOff = touch();
        // Клик по погасшему экрану только будит. Удержание сразу работает: можно говорить
        if (!(wasOff && ev == ButtonEvent::Click)) onButton(ev);
    }

    handleSideKey(userKey.poll());

    delay(1);
}
