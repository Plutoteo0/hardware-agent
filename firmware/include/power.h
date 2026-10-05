// Питание: яркость подсветки и глубокий сон.
//
// Подсветка — светодиоды за экраном, это главный потребитель. Яркость меняем ШИМ:
// светодиод быстро мигает, и доля времени «включено» задаёт яркость на глаз.
//
// Глубокий сон (deep sleep): процессор выключен, работает только крошечная
// RTC-часть, которая следит за кнопками. Просыпание = перезагрузка с setup(),
// поэтому Wi-Fi и сервер подключаются заново (~3 с).
// Как у заводской прошивки LilyGO (examples/factory: enterSystemSleepNow).
#pragma once
#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "pins.h"

class Power {
public:
    static constexpr int BL_CHANNEL = 7;   // канал ШИМ, свободный от других библиотек

    void begin() {
        ledcSetup(BL_CHANNEL, 5000, 8);     // 5 кГц: мигание глазом не видно, 8 бит: яркость 0..255
        ledcAttachPin(TFT_BL, BL_CHANNEL);
        setBrightness(255);
    }

    void setBrightness(uint8_t v) {
        if (v == brightness_) return;
        brightness_ = v;
        ledcWrite(BL_CHANNEL, v);
    }
    uint8_t brightness() const { return brightness_; }

    static bool wokeFromSleep() {
        return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;
    }

    // Уснуть до нажатия кнопки. Вызывать, когда кнопки отпущены:
    // будит уровень LOW, и зажатая кнопка разбудила бы сразу.
    [[noreturn]] void deepSleep() {
        setBrightness(0);
        digitalWrite(BOARD_PWR_EN, LOW);        // питание экрана, усилителя и радио — выключить
        uint64_t mask = 0;
        for (int pin : {BOARD_USER_KEY, ENCODER_KEY}) {
            rtc_gpio_pullup_en((gpio_num_t)pin);   // во сне обычная подтяжка не работает, нужна RTC
            rtc_gpio_pulldown_dis((gpio_num_t)pin);
            mask |= 1ULL << pin;
        }
        esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
        Serial.println("[power] deep sleep");
        Serial.flush();
        delay(20);
        esp_deep_sleep_start();
    }

private:
    uint8_t brightness_ = 0;
};
