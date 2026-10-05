// Цифровой PDM-микрофон T-Embed через I2S.
//
// Настройки из примера LilyGO examples/record_test: 16 кГц, 16 бит, моно.
// Это ровно формат, который ждёт Whisper на сервере, перекодировать не надо.
//
// I2S пишет звук в кольцевые DMA-буферы сам, без участия процессора.
// read() только забирает то, что уже накопилось, и не ждёт (таймаут 0),
// поэтому экран и WebSocket не подвисают, пока идёт запись.
#pragma once
#include <Arduino.h>
#include <driver/i2s.h>

#include "pins.h"

class Mic {
public:
    static constexpr uint32_t SAMPLE_RATE = 16000;
    static constexpr i2s_port_t PORT = I2S_NUM_0;

    bool begin() {
        i2s_config_t cfg = {};
        cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
        cfg.sample_rate = SAMPLE_RATE;
        cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
        cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
        cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
        cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL2;
        cfg.dma_buf_count = 8;
        cfg.dma_buf_len = 512;          // 8 * 512 сэмплов = ~256 мс запаса, если loop задержится

        i2s_pin_config_t pins = {};
        pins.mck_io_num = I2S_PIN_NO_CHANGE;
        pins.bck_io_num = I2S_PIN_NO_CHANGE;
        pins.ws_io_num = BOARD_MIC_CLK;   // в режиме PDM такт идёт по линии WS
        pins.data_out_num = I2S_PIN_NO_CHANGE;
        pins.data_in_num = BOARD_MIC_DATA;

        if (i2s_driver_install(PORT, &cfg, 0, nullptr) != ESP_OK) return false;
        if (i2s_set_pin(PORT, &pins) != ESP_OK) return false;
        return i2s_set_clk(PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO) == ESP_OK;
    }

    // Выбросить старый звук из буферов, чтобы запись началась с момента нажатия
    void flush() {
        int16_t junk[256];
        size_t got = 0;
        do {
            i2s_read(PORT, junk, sizeof(junk), &got, 0);
        } while (got > 0);
    }

    // Забрать накопленные сэмплы, не ожидая. Возвращает число сэмплов.
    size_t read(int16_t* buf, size_t maxSamples) {
        size_t bytes = 0;
        i2s_read(PORT, buf, maxSamples * sizeof(int16_t), &bytes, 0);
        return bytes / sizeof(int16_t);
    }
};
