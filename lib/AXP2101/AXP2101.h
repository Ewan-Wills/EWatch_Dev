#pragma once
#include <Arduino.h>
#include <Wire.h>

#ifdef PMIC_REG
#define AXP2101_I2C_ADDR PMIC_REG
#endif

#ifndef AXP2101_I2C_ADDR
#define AXP2101_I2C_ADDR 0x34
#endif

// =====================================================
// AXP2101 Register Map (Key Registers)
// =====================================================
// STATUS
#define AXP_STATUS_REG       0x00  // bit5=VBUS, bit3=BAT present, bit2=charging, bit1=power good
// SYSTEM CONTROL
#define AXP_SYS_CTRL_REG     0x10  // bit7=poweroff, bit6=reboot
#define AXP_CHG_CTRL_REG     0x18  // bit1=charge enable, bit0=sleep
// CHARGER SETTINGS
#define AXP_VIN_LIMIT_REG    0x15  // Input voltage limit: 100mV/step
#define AXP_IIN_LIMIT_REG    0x16  // Input current limit: 100mA/step (bits[5:0])  <-- FIXED: was 50mA
#define AXP_CHG_CURR_REG     0x19  // Charge current: 25mA/step  (bits[3:0])       <-- FIXED: was 64mA
#define AXP_CHG_VOLT_REG     0x1A  // Charge voltage: 10mV/step from 4.0V (bits[5:0])
#define AXP_FAULT_REG        0x1B  // Fault status — write 0xFF to clear
// ADC CONTROL
#define AXP_ADC_CTRL_REG     0x30  // Write 0xFF to enable all ADCs
// TS PIN CONTROL
#define AXP_TS_CTRL_REG      0x50  // bit4=1 disables TS influence on charger
// ADC READS
#define AXP_VBAT_H_REG       0x34
#define AXP_VSYS_H_REG       0x36
#define AXP_VBUS_H_REG       0x38
// DIE TEMP
#define AXP_TDIE_H_REG       0x3C
// DCDC ENABLE / VOLTAGE
#define AXP_DCDC_EN_REG      0x80
#define AXP_DCDC1_VOLT_REG   0x81
#define AXP_DCDC2_VOLT_REG   0x82
#define AXP_DCDC3_VOLT_REG   0x83
#define AXP_DCDC4_VOLT_REG   0x84
// LDO ENABLE / VOLTAGE
#define AXP_LDO_EN_REG       0x90
#define AXP_LDO1_VOLT_REG    0x91
#define AXP_LDO2_VOLT_REG    0x92
#define AXP_LDO3_VOLT_REG    0x93
#define AXP_LDO4_VOLT_REG    0x94

class AXP2101
{
public:
    // ---- Init ----
    bool begin(TwoWire &wire = Wire, uint8_t address = AXP2101_I2C_ADDR);
    bool devicePresent();

    // ---- Core Register Access (all use _wire / _address — no globals) ----
    uint8_t  read8(uint8_t reg);
    void     readBytes(uint8_t reg, uint8_t *buf, uint8_t len);
    void     write8(uint8_t reg, uint8_t value);
    void     writeBytes(uint8_t reg, const uint8_t *data, uint8_t len);

    // ---- Bit / Field Helpers ----
    void    setBit(uint8_t reg, uint8_t bit);
    void    clearBit(uint8_t reg, uint8_t bit);
    void    writeBit(uint8_t reg, uint8_t bit, bool value);
    void    writeField(uint8_t reg, uint8_t mask, uint8_t shift, uint8_t value);
    uint16_t readADC14(uint8_t highReg, uint8_t lowReg);

    // ---- Status ----
    bool isVBUSPresent();
    bool isBatteryPresent();
    bool isCharging();
    bool isPowerGood();
    float getDieTemperature();

    // ---- System Control ----
    void enableSleep(bool enable);
    void powerOff();
    void reboot();

    // ---- Charging — PRIMARY INTERFACE ----
    // Call setupCharging() once in setup(). Handles everything.
    // Pass sdaPin/sclPin to enable automatic I2C recovery on VBUS plug-in.
    void setupCharging(float chargeVolts = 4.2f,
                       uint16_t chargeCurrent_mA = 384,
                       float inputVolts = 5.5f,
                       uint16_t inputCurrent_mA = 1000,
                       int sdaPin = -1,
                       int sclPin = -1);

    // I2C bus recovery — call when Wire requestFrom() errors appear.
    // Bit-bangs 9 SCL pulses to release any stuck slave, then reinits Wire.
    bool recoverBus(int sdaPin, int sclPin);

    void enableCharging(bool enable);              // toggle only, call setupCharging() first
    void setChargeCurrent(uint16_t mA);            // 25mA steps, max 375mA
    void setChargeVoltage(float volts);            // 4.0–4.63V, 10mV steps
    void setInputVoltageLimit(float volts);        // 100mV steps
    void setInputCurrentLimit(uint16_t mA);        // 100mA steps

    // ---- DCDC ----
    void enableDCDC1(bool enable);
    void enableDCDC2(bool enable);
    void enableDCDC3(bool enable);
    void enableDCDC4(bool enable);
    void setDCDC1Voltage(float volts);
    void setDCDC2Voltage(float volts);
    void setDCDC3Voltage(float volts);
    void setDCDC4Voltage(float volts);

    // ---- LDO ----
    void enableLDO1(bool enable);
    void enableLDO2(bool enable);
    void enableLDO3(bool enable);
    void enableLDO4(bool enable);
    void setLDO1Voltage(float volts);
    void setLDO2Voltage(float volts);
    void setLDO3Voltage(float volts);
    void setLDO4Voltage(float volts);

    // ---- ADC Reads ----
    float   getBatteryVoltage();
    float   getVBUSVoltage();
    float   getSystemVoltage();
    uint8_t getBatteryPercent();

    // ---- Interrupts ----
    void    enableInterrupt(uint8_t reg, uint8_t mask);
    void    disableInterrupt(uint8_t reg, uint8_t mask);
    uint8_t getInterruptStatus(uint8_t reg);
    void    clearInterrupt(uint8_t reg, uint8_t mask);

    // ---- Legacy aliases (kept for compatibility, delegate to above) ----
    void    init(float chargeVoltage = 4.2f, uint16_t chargeCurrent = 300);

    void dumpRegisters();


private:
    TwoWire *_wire    = &Wire;
    uint8_t  _address = AXP2101_I2C_ADDR;
    int      _sdaPin  = -1;
    int      _sclPin  = -1;

    static uint8_t dcdcVoltageToReg(float volts);
    static uint8_t ldoVoltageToReg(float volts);
};