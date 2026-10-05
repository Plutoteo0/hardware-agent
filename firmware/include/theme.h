// Цвета интерфейса. RGB565: экран хранит цвет в 16 битах (5 красный, 6 зелёный, 5 синий).
#pragma once
#include <stdint.h>

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

namespace theme {
constexpr uint16_t BG      = rgb(10, 14, 22);     // почти чёрный с синевой
constexpr uint16_t PANEL   = rgb(22, 28, 40);     // верхняя панель, карточки
constexpr uint16_t LINE    = rgb(45, 55, 75);     // разделители, рамки
constexpr uint16_t TEXT    = rgb(225, 230, 240);
constexpr uint16_t DIM     = rgb(120, 130, 150);  // подсказки

constexpr uint16_t IDLE    = rgb(60, 220, 230);   // бирюзовый: готов
constexpr uint16_t LISTEN  = rgb(255, 80, 100);   // красный: слушаю
constexpr uint16_t THINK   = rgb(255, 200, 60);   // жёлтый: думаю
constexpr uint16_t SPEAK   = rgb(90, 230, 120);   // зелёный: говорю
constexpr uint16_t ASK     = rgb(255, 150, 50);   // оранжевый: спрашиваю разрешение
constexpr uint16_t ERR     = rgb(255, 70, 70);
constexpr uint16_t OFF     = rgb(90, 95, 110);    // серый: нет связи

constexpr uint16_t BAT_OK  = rgb(90, 230, 120);
constexpr uint16_t BAT_MID = rgb(255, 200, 60);
constexpr uint16_t BAT_LOW = rgb(255, 70, 70);
}  // namespace theme
