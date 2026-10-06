// Проверка «сервер агента отвечает?» в отдельной задаче.
//
// Зачем: библиотека WebSocket подключается прямо внутри ws.loop(), и если сервер
// не отвечает (выключен ПК, закрыт порт), попытка блокирует всю плату до 5 секунд
// (замер: ws=5001 мс раз в ~8 с). Экран и энкодер в это время мертвы.
//
// Поэтому пробуем подключиться здесь, в своей задаче на втором ядре: пусть ждёт
// сколько угодно, интерфейс не замечает. Пока сервер не ответил, main.cpp не зовёт
// ws.loop() вообще. Ответил — настоящее подключение проходит за миллисекунды.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <atomic>

class ServerProbe {
public:
    void begin(const char* host, uint16_t port) {
        host_ = host;
        port_ = port;
        // Ядро 1, как задача динамика; приоритет ниже: проверка не срочная
        xTaskCreatePinnedToCore(taskEntry, "probe", 4096, this, 1, nullptr, 1);
    }

    bool reachable() const { return reachable_.load(); }

    // Сколько мс назад сервер ответил на проверку
    uint32_t sinceOk() const { return millis() - okAt_.load(); }

    // Связь пропала или подключиться не вышло — снова проверять, прежде чем звать ws.loop()
    void lost() { reachable_.store(false); }

private:
    static constexpr uint32_t PERIOD_MS = 3000;    // как часто проверять, пока сервера нет
    static constexpr int32_t TIMEOUT_MS = 2000;

    static void taskEntry(void* self) { ((ServerProbe*)self)->run(); }

    void run() {
        for (;;) {
            if (!reachable_.load() && WiFi.status() == WL_CONNECTED) {
                WiFiClient c;
                if (c.connect(host_, port_, TIMEOUT_MS)) {
                    okAt_.store(millis());
                    reachable_.store(true);
                }
                c.stop();
            }
            vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
        }
    }

    const char* host_ = "";
    uint16_t port_ = 0;
    std::atomic<bool> reachable_{false};
    std::atomic<uint32_t> okAt_{0};
};
