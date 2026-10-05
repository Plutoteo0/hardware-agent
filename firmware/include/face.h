// Лицо агента: два глаза-«пилюли» и рот. Настроение = цвет + движение.
//
// Рисуем в спрайт — картинку в памяти — и выводим её на экран целиком.
// Если рисовать прямо на экране (стереть, нарисовать заново), глаз видно
// пустой момент между кадрами — это мерцание. Спрайт его убирает.
//
// Все движения считаются от millis(): кадр не зависит от предыдущего,
// поэтому анимация не «разъезжается», даже если loop() иногда тормозит.
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

#include "theme.h"

enum class Mood { Idle, Listen, Think, Speak, Ask, Error, Offline };

class Face {
public:
    static constexpr int W = 130, H = 110;

    explicit Face(TFT_eSPI& tft) : tft_(tft), spr_(&tft) {}

    bool begin() {
        spr_.setColorDepth(16);
        return spr_.createSprite(W, H) != nullptr;
    }

    void setMood(Mood m) {
        if (m != mood_) {
            mood_ = m;
            moodSince_ = millis();
        }
    }
    Mood mood() const { return mood_; }

    void setLevel(float level) { level_ = constrain(level, 0.0f, 1.0f); }  // громкость голоса 0..1

    // Нарисовать кадр в точке (x, y). Вызывать ~25 раз в секунду
    void draw(int x, int y) {
        uint32_t t = millis();
        spr_.fillSprite(theme::BG);

        uint16_t color = theme::IDLE;
        float lookX = 0, lookY = 0;        // куда смотрят глаза, в пикселях
        float eyeH = 1.0f;                 // высота глаз: 1 — открыты, ~0 — закрыты
        float squintR = 1.0f;              // прищур правого глаза
        bool mouth = false;
        float mouthOpen = 0;

        switch (mood_) {
        case Mood::Idle:
            color = theme::IDLE;
            // Иногда смотрит по сторонам: плавно, с паузами
            lookX = 7 * sinf(t / 1900.0f) * (sinf(t / 5300.0f) > 0.3f ? 1 : 0.2f);
            lookY = 2 * sinf(t / 2700.0f);
            eyeH = blink(t);
            break;
        case Mood::Listen:
            color = theme::LISTEN;
            eyeH = 1.0f + 0.35f * level_;   // «подпрыгивают» от громкости
            lookY = -3 * level_;
            break;
        case Mood::Think:
            color = theme::THINK;
            lookX = 9 * sinf(t / 450.0f);   // бегают туда-сюда
            lookY = -8;                     // смотрят вверх: «вспоминает»
            eyeH = 0.75f;
            break;
        case Mood::Speak:
            color = theme::SPEAK;
            eyeH = blink(t);
            mouth = true;
            // Рот шевелится: сумма двух синусов похожа на речь, а не на метроном
            mouthOpen = 0.25f + 0.75f * fabsf(sinf(t / 90.0f) * 0.6f + sinf(t / 37.0f) * 0.4f);
            break;
        case Mood::Ask:
            color = theme::ASK;
            squintR = 0.45f;                // прищур: «точно можно?»
            lookY = -2;
            break;
        case Mood::Error:
            color = theme::ERR;
            eyeH = 0.35f;
            break;
        case Mood::Offline:
            color = theme::OFF;
            eyeH = 0.12f;                   // спит
            lookY = 6;
            break;
        }

        // Плавное появление нового настроения: первые 150 мс глаза «моргают»
        uint32_t since = t - moodSince_;
        if (since < 150) eyeH *= 0.3f + 0.7f * since / 150.0f;

        const int cx = W / 2, cy = mouth ? 42 : 50;
        const int eyeW = 26, eyeMaxH = 44, gap = 22;
        drawEye(cx - gap - eyeW / 2 + (int)lookX, cy + (int)lookY, eyeW, (int)(eyeMaxH * eyeH), color);
        drawEye(cx + gap + eyeW / 2 + (int)lookX, cy + (int)lookY, eyeW, (int)(eyeMaxH * eyeH * squintR), color);

        if (mouth) {
            int mw = 34, mh = 4 + (int)(14 * mouthOpen);
            spr_.fillRoundRect(cx - mw / 2, 88 - mh / 2, mw, mh, min(mh / 2, 7), color);
        }
        if (mood_ == Mood::Think) drawSpinner(t, color);

        spr_.pushSprite(x, y);
    }

private:
    TFT_eSPI& tft_;
    TFT_eSprite spr_;
    Mood mood_ = Mood::Offline;
    uint32_t moodSince_ = 0;
    float level_ = 0;

    // Моргание: раз в ~3.5 с, быстро (120 мс)
    static float blink(uint32_t t) {
        uint32_t phase = t % 3500;
        if (phase > 120) return 1.0f;
        return fabsf(phase - 60.0f) / 60.0f;   // 1 -> 0 -> 1
    }

    void drawEye(int cx, int cy, int w, int h, uint16_t color) {
        h = max(h, 4);
        int r = min(w, h) / 2;
        spr_.fillRoundRect(cx - w / 2, cy - h / 2, w, h, r, color);
    }

    // Три точки бегут по кругу над головой, как спиннер «думаю»
    void drawSpinner(uint32_t t, uint16_t color) {
        const int sx = W - 18, sy = 14, R = 8;
        for (int i = 0; i < 3; i++) {
            float a = t / 180.0f + i * 0.9f;
            int r = 3 - i;  // хвост из точек поменьше
            spr_.fillCircle(sx + (int)(R * cosf(a)), sy + (int)(R * sinf(a)), max(r, 1), color);
        }
    }
};
