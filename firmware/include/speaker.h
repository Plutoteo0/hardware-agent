// Динамик T-Embed через I2S (второй канал, микрофон на первом).
//
// Звук приходит по WebSocket кусками и неравномерно: сеть то быстрее, то медленнее.
// Поэтому между сетью и динамиком стоит кольцевой буфер:
//   loop() (производитель)  -> push() кладёт байты в буфер
//   своя задача FreeRTOS     -> берёт из буфера и пишет в I2S
// i2s_write блокирует, пока динамик не проиграет звук. Если делать это в loop(),
// экран и WebSocket замирали бы на время речи. Отдельная задача решает это.
//
// Буфер в PSRAM (внешняя память 8 МБ): 512 КБ = ~12 с звука при 22050 Гц.
// Сервер присылает не больше чем на 2 с вперёд, так что запас большой.
#pragma once
#include <Arduino.h>
#include <atomic>
#include <driver/i2s.h>

#include "pins.h"

class Speaker {
public:
    static constexpr i2s_port_t PORT = I2S_NUM_1;
    static constexpr size_t BUF_SIZE = 512 * 1024;

    bool begin() {
        buf_ = (uint8_t*)ps_malloc(BUF_SIZE);
        if (!buf_) return false;

        i2s_config_t cfg = {};
        cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
        cfg.sample_rate = 22050;
        cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
        cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
        cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
        cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
        cfg.dma_buf_count = 8;
        cfg.dma_buf_len = 256;
        cfg.tx_desc_auto_clear = true;   // буфер пуст — играет тишину, а не повтор старого звука

        i2s_pin_config_t pins = {};
        pins.mck_io_num = I2S_PIN_NO_CHANGE;
        pins.bck_io_num = BOARD_VOICE_BCLK;
        pins.ws_io_num = BOARD_VOICE_LRCLK;
        pins.data_out_num = BOARD_VOICE_DIN;
        pins.data_in_num = I2S_PIN_NO_CHANGE;

        if (i2s_driver_install(PORT, &cfg, 0, nullptr) != ESP_OK) return false;
        if (i2s_set_pin(PORT, &pins) != ESP_OK) return false;
        i2s_zero_dma_buffer(PORT);

        // Ядро 1: loop() и Wi-Fi на ядре 0 (ARDUINO_RUNNING_CORE=0 в описании платы)
        xTaskCreatePinnedToCore(taskEntry, "speaker", 4096, this, 5, nullptr, 1);
        return true;
    }

    void start(uint32_t sampleRate) {
        stop();
        // Ждём, пока задача динамика очистит буфер. Иначе она может очистить его
        // позже и выбросить начало новой речи, которое уже успело прийти.
        // Один i2s_write длится до ~100 мс (8 DMA-буферов по 256 сэмплов).
        uint32_t t0 = millis();
        while (clearRequested_.load() && millis() - t0 < 300) delay(1);
        i2s_set_clk(PORT, sampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
    }

    // Положить звук в буфер. Возвращает, сколько байт влезло.
    size_t push(const uint8_t* data, size_t len) {
        size_t head = head_.load(), tail = tail_.load();
        size_t free = BUF_SIZE - 1 - (head - tail + BUF_SIZE) % BUF_SIZE;
        len = min(len, free) & ~(size_t)1;   // только целые сэмплы
        for (size_t i = 0; i < len; i++) buf_[(head + i) % BUF_SIZE] = data[i];
        head_.store((head + len) % BUF_SIZE);
        return len;
    }

    // Оборвать речь: выбросить всё, что не проиграно
    void stop() {
        clearRequested_.store(true);
    }

    bool playing() const { return head_.load() != tail_.load(); }

    void setVolume(float v) { volume_ = constrain(v, 0.0f, 1.0f); }

private:
    uint8_t* buf_ = nullptr;
    std::atomic<size_t> head_{0}, tail_{0};   // head пишет loop, tail — задача динамика
    std::atomic<bool> clearRequested_{false};
    float volume_ = 0.6f;

    static void taskEntry(void* self) { ((Speaker*)self)->run(); }

    void run() {
        int16_t chunk[256];
        for (;;) {
            if (clearRequested_.exchange(false)) {
                tail_.store(head_.load());       // очистку делает только читатель: так нет гонки за tail
                i2s_zero_dma_buffer(PORT);
            }
            size_t head = head_.load(), tail = tail_.load();
            size_t avail = (head - tail + BUF_SIZE) % BUF_SIZE;
            if (avail < 2) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            size_t n = min(avail, sizeof(chunk)) & ~(size_t)1;
            uint8_t* out = (uint8_t*)chunk;
            for (size_t i = 0; i < n; i++) out[i] = buf_[(tail + i) % BUF_SIZE];
            tail_.store((tail + n) % BUF_SIZE);

            // Громкость программно: умножаем каждый сэмпл
            for (size_t i = 0; i < n / 2; i++) chunk[i] = (int16_t)(chunk[i] * volume_);

            size_t written = 0;
            i2s_write(PORT, chunk, n, &written, portMAX_DELAY);
        }
    }
};
