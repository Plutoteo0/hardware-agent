// Окно с длинным текстом (ответ модели): перенос по словам и прокрутка энкодером.
//
// Встроенные шрифты TFT_eSPI не знают кириллицу, поэтому текст рисуем через
// U8g2_for_TFT_eSPI: у него есть шрифты *_t_cyrillic и вывод UTF-8.
//
// UTF-8: русская буква занимает 2 байта. Резать строку посреди буквы нельзя,
// иначе на экране появится мусор. Байты вида 10xxxxxx — это продолжение
// буквы, перед ними резать запрещено.
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <vector>

// Обрезает строку по ширине в пикселях, не разрывая UTF-8 букву. Добавляет "..." если обрезала.
inline String fitUtf8(U8g2_for_TFT_eSPI& u8f, const String& s, int maxW) {
    if (u8f.getUTF8Width(s.c_str()) <= maxW) return s;
    int dots = u8f.getUTF8Width("...");
    int best = 0;
    for (int b = 1; b <= (int)s.length(); b++) {
        if (b < (int)s.length() && ((uint8_t)s[b] & 0xC0) == 0x80) continue;
        if (u8f.getUTF8Width(s.substring(0, b).c_str()) + dots > maxW) break;
        best = b;
    }
    return s.substring(0, best) + "...";
}

class TextView {
public:
    TextView(TFT_eSPI& tft, U8g2_for_TFT_eSPI& u8f) : tft_(tft), u8f_(u8f) {}

    // Область экрана, где живёт текст
    void setArea(int x, int y, int w, int h) { x_ = x; y_ = y; w_ = w; h_ = h; }
    void setColors(uint16_t fg, uint16_t bg) { fg_ = fg; bg_ = bg; }

    void setText(const String& text) {
        lines_.clear();
        top_ = 0;
        u8f_.setFont(FONT);
        int start = 0;
        // Сначала режем по \n, потом каждый абзац переносим по ширине
        while (start <= (int)text.length()) {
            int nl = text.indexOf('\n', start);
            if (nl < 0) nl = text.length();
            wrapParagraph(text.substring(start, nl));
            start = nl + 1;
        }
    }

    // dir > 0 — вниз, dir < 0 — вверх. Возвращает true, если позиция изменилась.
    bool scroll(int dir) {
        int maxTop = max(0, (int)lines_.size() - visibleLines());
        int next = constrain(top_ + dir, 0, maxTop);
        if (next == top_) return false;
        top_ = next;
        return true;
    }

    void draw() {
        tft_.fillRect(x_, y_, w_, h_, bg_);
        u8f_.setFont(FONT);
        u8f_.setFontMode(1);
        u8f_.setForegroundColor(fg_);
        int n = visibleLines();
        for (int i = 0; i < n && top_ + i < (int)lines_.size(); i++) {
            // drawUTF8 рисует от базовой линии, поэтому + ascent
            u8f_.drawUTF8(x_, y_ + i * LINE_H + u8f_.getFontAscent(), lines_[top_ + i].c_str());
        }
        drawScrollbar();
    }

private:
    static constexpr const uint8_t* FONT = u8g2_font_9x15_t_cyrillic;
    static constexpr int LINE_H = 17;
    static constexpr int SCROLLBAR_W = 4;

    TFT_eSPI& tft_;
    U8g2_for_TFT_eSPI& u8f_;
    std::vector<String> lines_;
    int top_ = 0;
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    uint16_t fg_ = TFT_WHITE, bg_ = TFT_BLACK;

    int textWidth() const { return w_ - SCROLLBAR_W - 4; }
    int visibleLines() const { return max(1, h_ / LINE_H); }

    int width(const String& s) { return u8f_.getUTF8Width(s.c_str()); }

    void wrapParagraph(const String& para) {
        String line;
        int i = 0;
        while (i < (int)para.length()) {
            int sp = para.indexOf(' ', i);
            if (sp < 0) sp = para.length();
            String word = para.substring(i, sp);
            i = sp + 1;

            String candidate = line.length() ? line + " " + word : word;
            if (width(candidate) <= textWidth()) {
                line = candidate;
                continue;
            }
            if (line.length()) lines_.push_back(line);
            // Слово длиннее строки (например, URL) — режем по буквам
            while (width(word) > textWidth()) {
                int cut = fitBytes(word);
                lines_.push_back(word.substring(0, cut));
                word = word.substring(cut);
            }
            line = word;
        }
        lines_.push_back(line);   // пустой абзац тоже даёт строку
    }

    // Сколько байт строки влезает в ширину, не разрывая UTF-8 букву
    int fitBytes(const String& s) {
        int best = 1;
        for (int b = 1; b <= (int)s.length(); b++) {
            if (b < (int)s.length() && ((uint8_t)s[b] & 0xC0) == 0x80) continue;
            if (width(s.substring(0, b)) > textWidth()) break;
            best = b;
        }
        return best;
    }

    void drawScrollbar() {
        int total = lines_.size(), n = visibleLines();
        if (total <= n) return;
        int bx = x_ + w_ - SCROLLBAR_W;
        int barH = max(10, h_ * n / total);
        int barY = y_ + (h_ - barH) * top_ / (total - n);
        tft_.fillRect(bx, y_, SCROLLBAR_W, h_, bg_ == TFT_BLACK ? TFT_DARKGREY : (uint16_t)(bg_ + 0x2104));
        tft_.fillRect(bx, barY, SCROLLBAR_W, barH, TFT_LIGHTGREY);
    }
};
