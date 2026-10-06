// Энкодер через аппаратный счётчик импульсов PCNT (есть в ESP32-S3).
//
// Почему не программно: контакты энкодера дребезжат, и переходы иногда идут
// быстрее, чем программа успевает их прочитать (лог платы: прерывание видело
// «2», а на ноге уже «0»). Пропущенный переход — потерянный щелчок.
// PCNT считает фронты в железе и не пропускает ни одного.
//
// Как считаем. Контакты A и B сдвинуты на четверть шага (код Грея):
//   в одну сторону: 3 -> 1 -> 0 -> 2 -> 3   (состояние = A | B<<1)
// Щелчок (покой) — в состояниях 0 и 3, то есть за щелчок меняются и A, и B:
// 2 отсчёта. Канал 0 считает фронты A (направление — по уровню B), канал 1 —
// фронты B (направление — по уровню A).
//
// Щелчок засчитываем, только когда набралось ±2. В покое ручка стоит рядом с
// фронтом одного из контактов, и люфт даёт +1 -1 +1. Если считать каждый отсчёт,
// курсор дёргается туда-сюда. С порогом 2 люфт никогда не превращается в шаг.
#pragma once
#include <Arduino.h>
#include <driver/pcnt.h>

class Encoder {
public:
    Encoder(int pinA, int pinB) : pinA_(pinA), pinB_(pinB) {}

    void begin() {
        // Канал 0: фронты A. A растёт при B=1 — плюс, при B=0 — минус (и наоборот для спада)
        pcnt_config_t a = {};
        a.pulse_gpio_num = pinA_;
        a.ctrl_gpio_num = pinB_;
        a.unit = UNIT;
        a.channel = PCNT_CHANNEL_0;
        a.pos_mode = PCNT_COUNT_INC;
        a.neg_mode = PCNT_COUNT_DEC;
        a.hctrl_mode = PCNT_MODE_KEEP;       // B = 1: как есть
        a.lctrl_mode = PCNT_MODE_REVERSE;    // B = 0: наоборот
        a.counter_h_lim = LIMIT;             // счётчик 16-битный: на пределе сбрасывается в 0
        a.counter_l_lim = -LIMIT;
        pcnt_unit_config(&a);                // заодно включает подтяжку ног к питанию

        // Канал 1: фронты B. B растёт при A=0 — плюс, при A=1 — минус (и наоборот для спада)
        pcnt_config_t b = a;
        b.pulse_gpio_num = pinB_;
        b.ctrl_gpio_num = pinA_;
        b.channel = PCNT_CHANNEL_1;
        b.pos_mode = PCNT_COUNT_DEC;
        b.neg_mode = PCNT_COUNT_INC;
        pcnt_unit_config(&b);

        // Аппаратный фильтр: импульсы короче 1023 тактов (12.8 мкс) не считаются
        pcnt_set_filter_value(UNIT, 1023);
        pcnt_filter_enable(UNIT);

        pcnt_counter_pause(UNIT);
        pcnt_counter_clear(UNIT);
        pcnt_counter_resume(UNIT);
    }

    // Целые щелчки с прошлого вызова. По часовой — плюс
    int readSteps() {
        int16_t now = 0;
        pcnt_get_counter_value(UNIT, &now);
        int delta = now - last_;
        last_ = now;
        // Счётчик дошёл до предела и сбросился в 0: поправляем на LIMIT
        if (delta > LIMIT / 2) delta -= LIMIT;
        if (delta < -LIMIT / 2) delta += LIMIT;
        // По логу платы этот счёт растёт против часовой — разворачиваем
        acc_ -= delta;

        int steps = acc_ / COUNTS_PER_STEP;  // целые щелчки, остаток (люфт) ждёт
        acc_ -= steps * COUNTS_PER_STEP;
        return steps;
    }

private:
    static constexpr pcnt_unit_t UNIT = PCNT_UNIT_0;
    static constexpr int16_t LIMIT = 1000;   // кратно COUNTS_PER_STEP: сброс не сбивает щелчки
    static constexpr int COUNTS_PER_STEP = 2;
    int pinA_, pinB_;
    int16_t last_ = 0;
    int acc_ = 0;                            // отсчёты, ещё не ставшие щелчком
};
