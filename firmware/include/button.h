// Кнопка энкодера: отличает короткий клик от удержания.
//
// Механическая кнопка "дребезжит": при нажатии контакт несколько миллисекунд
// прыгает между LOW и HIGH. Поэтому состояние считается новым, только если
// держится DEBOUNCE_MS подряд.
#pragma once
#include <Arduino.h>

enum class ButtonEvent { None, Click, HoldStart, HoldEnd };

class Button {
public:
    static constexpr uint32_t DEBOUNCE_MS = 25;
    static constexpr uint32_t HOLD_MS = 600;   // дольше этого — удержание, а не клик

    explicit Button(uint8_t pin) : pin_(pin) {}

    void begin() { pinMode(pin_, INPUT_PULLUP); }

    // Вызывать в каждом loop(). Возвращает не больше одного события за вызов.
    ButtonEvent poll() {
        uint32_t now = millis();
        bool raw = digitalRead(pin_) == LOW;   // LOW = нажата

        if (raw != lastRaw_) {                 // контакт изменился — ждём, пока успокоится
            lastRaw_ = raw;
            changedAt_ = now;
        }
        if (now - changedAt_ < DEBOUNCE_MS) return ButtonEvent::None;

        if (raw && !pressed_) {                // устойчиво нажали
            pressed_ = true;
            pressedAt_ = now;
            holding_ = false;
        } else if (!raw && pressed_) {         // устойчиво отпустили
            pressed_ = false;
            if (holding_) return ButtonEvent::HoldEnd;
            return ButtonEvent::Click;
        } else if (pressed_ && !holding_ && now - pressedAt_ >= HOLD_MS) {
            holding_ = true;                   // удержание начинается ещё до отпускания
            return ButtonEvent::HoldStart;
        }
        return ButtonEvent::None;
    }

    // Сколько держат прямо сейчас (0 — отпущена). Для отсчёта «держи ещё N с»
    uint32_t heldMs() const { return pressed_ ? millis() - pressedAt_ : 0; }

private:
    uint8_t pin_;
    bool lastRaw_ = false, pressed_ = false, holding_ = false;
    uint32_t changedAt_ = 0, pressedAt_ = 0;
};
