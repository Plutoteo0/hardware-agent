// Голосовые заметки на SD-карте: запись без связи, потом прослушать, отправить или удалить.
//
// Карта сидит на одной шине SPI с экраном, поэтому подключаем её к той же шине,
// что у TFT_eSPI (tft.getSPIinstance()). Обращения идут по очереди из loop(),
// транзакции SPI не дают экрану и карте мешать друг другу.
//
// Звук держим в PSRAM (8 МБ): 30 с = ~1 МБ. Писать на карту прямо во время записи
// нельзя: карта иногда задумывается на сотни мс, а у микрофона запас ~256 мс —
// звук бы терялся. Поэтому: запись в память, на карту одним куском после отпускания.
// Прослушивание и отправка тоже идут из памяти: файл читается целиком.
//
// Формат — WAV, 16 кГц, 16 бит, моно: тот же, что ждёт сервер, и открывается на ПК.
#pragma once
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <time.h>
#include <algorithm>
#include <vector>

struct NoteInfo {
    String name;          // имя файла в /notes
    uint32_t seconds;
};

class Notes {
public:
    static constexpr uint32_t RATE = 16000;
    static constexpr size_t MAX_BYTES = 30 * RATE * 2;   // 30 с, как MAX_RECORD_MS
    static constexpr size_t HEADER = 44;                  // заголовок WAV

    bool begin(SPIClass& spi, int cs) {
        if (!buf_) buf_ = (uint8_t*)ps_malloc(MAX_BYTES);
        ok_ = buf_ && SD.begin(cs, spi, 20000000);        // 20 МГц хватает с запасом
        if (ok_ && !SD.exists(DIR)) SD.mkdir(DIR);
        if (ok_) list();                                  // посчитать заметки для карточки
        return ok_;
    }

    bool ok() const { return ok_; }
    int count() const { return count_; }

    // ---- запись (в память) ----

    void startRecording() { len_ = 0; }

    void add(const int16_t* samples, size_t n) {
        size_t bytes = min(n * sizeof(int16_t), MAX_BYTES - len_);
        memcpy(buf_ + len_, samples, bytes);
        len_ += bytes;
    }

    float recordedSeconds() const { return len_ / float(RATE * 2); }

    // Записанное — в файл на карту. Имя по времени, если оно известно (NTP)
    bool saveRecording() {
        String name = newName();
        File f = SD.open(path(name), FILE_WRITE);
        if (!f) return false;
        writeHeader(f, len_);
        bool ok = f.write(buf_, len_) == len_;
        f.close();
        if (!ok) SD.remove(path(name));
        else count_++;
        return ok;
    }

    // ---- список, чтение, удаление ----

    // Новые сверху
    std::vector<NoteInfo> list() {
        std::vector<NoteInfo> out;
        File dir = SD.open(DIR);
        if (!dir) return out;
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            String name = f.name();                       // в ядре 2.x — без пути
            if (!f.isDirectory() && name.endsWith(".wav") && f.size() >= HEADER) {
                out.push_back(NoteInfo{name, (uint32_t)((f.size() - HEADER) / (RATE * 2))});
            }
            f.close();
        }
        dir.close();
        std::sort(out.begin(), out.end(), [](const NoteInfo& a, const NoteInfo& b) { return a.name > b.name; });
        count_ = out.size();
        return out;
    }

    // Файл целиком в память: дальше data()/length() для прослушивания и отправки
    bool load(const String& name) {
        File f = SD.open(path(name));
        if (!f || f.size() < HEADER) return false;
        f.seek(HEADER);
        len_ = f.read(buf_, min((size_t)f.size() - HEADER, MAX_BYTES));
        f.close();
        return true;
    }

    const uint8_t* data() const { return buf_; }
    size_t length() const { return len_; }

    bool remove(const String& name) {
        bool ok = SD.remove(path(name));
        if (ok && count_ > 0) count_--;
        return ok;
    }

    // Подпись: «06.10 18:30», если при записи было известно время, иначе «без даты»
    static String label(const NoteInfo& n) {
        if (!n.name.startsWith("n")) return "без даты";
        time_t t = (time_t)n.name.substring(1).toInt();
        struct tm tm;
        localtime_r(&t, &tm);
        char s[16];
        strftime(s, sizeof(s), "%d.%m %H:%M", &tm);
        return s;
    }

private:
    static constexpr const char* DIR = "/notes";

    static String path(const String& name) { return String(DIR) + "/" + name; }

    // n<секунды с 1970>.wav — время известно (Wi-Fi был, NTP сверил часы);
    // x<случайное>.wav — нет. Часы идут и в глубоком сне, но сбрасываются при выключении
    static String newName() {
        time_t now = time(nullptr);
        if (now > 1700000000) return "n" + String((uint32_t)now) + ".wav";
        return "x" + String(esp_random(), HEX) + ".wav";
    }

    static void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
    static void put16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }

    static void writeHeader(File& f, uint32_t dataBytes) {
        uint8_t h[HEADER];
        memcpy(h, "RIFF", 4);
        put32(h + 4, 36 + dataBytes);
        memcpy(h + 8, "WAVEfmt ", 8);
        put32(h + 16, 16);              // размер блока fmt
        put16(h + 20, 1);               // PCM
        put16(h + 22, 1);               // моно
        put32(h + 24, RATE);
        put32(h + 28, RATE * 2);        // байт в секунду
        put16(h + 32, 2);               // байт на сэмпл
        put16(h + 34, 16);              // бит на сэмпл
        memcpy(h + 36, "data", 4);
        put32(h + 40, dataBytes);
        f.write(h, HEADER);
    }

    uint8_t* buf_ = nullptr;
    size_t len_ = 0;
    bool ok_ = false;
    int count_ = 0;
};
