// Настройки платы в NVS (энергонезависимая память ESP32).
//
// NVS — хранилище «ключ -> значение» во flash: переживает выключение, глубокий сон
// и перепрошивку. Flash выдерживает ограниченное число перезаписей, поэтому save()
// зовём, когда значение выбрано (клик), а не на каждый щелчок энкодера.
#pragma once
#include <Preferences.h>

struct Settings {
    static constexpr uint8_t VOLUME_MAX = 10;
    static constexpr uint8_t VOLUME_DEFAULT = 7;   // 7 * 0.2 = 1.4, как было до настроек

    uint8_t volume = VOLUME_DEFAULT;   // 0..10
    bool speech = true;                // озвучка ответов

    // Усиление для speaker.setVolume: 0..2.0
    float gain() const { return volume * 0.2f; }

    void load() {
        Preferences p;
        p.begin("agent", false);       // false = чтение и запись: при первом запуске создаст «папку»
        volume = min(p.getUChar("vol", VOLUME_DEFAULT), VOLUME_MAX);
        speech = p.getBool("speech", true);
        p.end();
    }

    void save() {
        Preferences p;
        p.begin("agent", false);
        p.putUChar("vol", volume);
        p.putBool("speech", speech);
        p.end();
    }
};
