// Батарея: заряд от BQ27220 (fuel gauge), состояние зарядки от BQ25896 (зарядник).
//
// Обе микросхемы на шине I2C (SDA 8, SCL 18). Регистры — из библиотеки LilyGO
// lib/BQ27220/bq27220_def.h и даташита BQ25896.
//
// Fuel gauge сам считает заряд, учитывая ток и напряжение: это точнее,
// чем угадывать процент по одному напряжению.
#pragma once
#include <Arduino.h>
#include <Wire.h>

struct BatteryInfo {
    bool ok = false;        // микросхема ответила
    int percent = 0;        // 0..100
    int millivolts = 0;
    bool usb = false;       // подключено питание по USB
    bool charging = false;  // идёт зарядка
    bool full = false;      // зарядка завершена
};

class Battery {
public:
    static constexpr uint8_t GAUGE = 0x55;     // BQ27220
    static constexpr uint8_t CHARGER = 0x6B;   // BQ25896

    void begin(int sda = 8, int scl = 18) { Wire.begin(sda, scl); }

    BatteryInfo read() {
        BatteryInfo b;
        int soc = readWord(GAUGE, 0x2C);       // StateOfCharge, %
        int mv = readWord(GAUGE, 0x08);        // Voltage, мВ
        if (soc < 0 || mv < 0) return b;
        b.ok = true;
        b.percent = constrain(soc, 0, 100);
        b.millivolts = mv;

        // BQ25896, регистр 0x0B: биты 7..5 — источник питания (0 = нет),
        // биты 4..3 — зарядка: 0 нет, 1 предзаряд, 2 быстрая, 3 завершена
        int st = readByte(CHARGER, 0x0B);
        if (st >= 0) {
            int vbus = (st >> 5) & 0x07;
            int chrg = (st >> 3) & 0x03;
            b.usb = vbus != 0;
            b.charging = chrg == 1 || chrg == 2;
            b.full = chrg == 3;
        }
        return b;
    }

private:
    // Регистры fuel gauge 16-битные, младший байт первым. -1 = нет ответа
    int readWord(uint8_t addr, uint8_t reg) {
        Wire.beginTransmission(addr);
        Wire.write(reg);
        if (Wire.endTransmission(false) != 0) return -1;
        if (Wire.requestFrom(addr, (uint8_t)2) != 2) return -1;
        int lo = Wire.read(), hi = Wire.read();
        return lo | (hi << 8);
    }

    int readByte(uint8_t addr, uint8_t reg) {
        Wire.beginTransmission(addr);
        Wire.write(reg);
        if (Wire.endTransmission(false) != 0) return -1;
        if (Wire.requestFrom(addr, (uint8_t)1) != 1) return -1;
        return Wire.read();
    }
};
