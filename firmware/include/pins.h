// Пины T-Embed CC1101. Источник: examples/utilities.h в репозитории
// Xinyuan-LilyGO/T-Embed-CC1101. Экран настраивается в platformio.ini.
#pragma once

#define BOARD_PWR_EN 15   // питание периферии (экран, усилитель): HIGH = включено

// Энкодер
#define ENCODER_INA 4
#define ENCODER_INB 5
#define ENCODER_KEY 0     // кнопка энкодера, нажата = LOW
#define BOARD_USER_KEY 6  // боковая кнопка, нажата = LOW

// Устройства на общей с экраном шине SPI. Их CS держим HIGH,
// чтобы они не отвечали, когда мы говорим с экраном.
#define BOARD_SD_CS   13
#define BOARD_LORA_CS 12  // CC1101
#define BOARD_NRF24_CS 44

// I2C: индикатор заряда BQ27220 (0x55) и зарядник BQ25896 (0x6B)
#define BOARD_I2C_SDA 8
#define BOARD_I2C_SCL 18

// Звук: PDM-микрофон и динамик
#define BOARD_MIC_DATA 42
#define BOARD_MIC_CLK  39
#define BOARD_VOICE_BCLK  46
#define BOARD_VOICE_LRCLK 40
#define BOARD_VOICE_DIN   7
