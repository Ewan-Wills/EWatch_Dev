#include "AXP2101.h"

/* =====================================================
   Core Low-Level I2C
   ===================================================== */

bool AXP2101::begin(TwoWire &wire, uint8_t address)
{
    _wire = &wire;
    _address = address;
    return devicePresent();
}

bool AXP2101::devicePresent()
{
    _wire->beginTransmission(_address);
    return (_wire->endTransmission() == 0);
}

uint8_t AXP2101::read8(uint8_t reg)
{
    _wire->beginTransmission(_address);
    _wire->write(reg);
    if (_wire->endTransmission(true) != 0)
        return 0;
    delayMicroseconds(50);          // give slave time to prepare
    if (_wire->requestFrom(_address, (uint8_t)1) != 1)
        return 0;
    return _wire->read();
}

void AXP2101::readBytes(uint8_t reg, uint8_t *buf, uint8_t len)
{
    _wire->beginTransmission(_address);
    _wire->write(reg);
    _wire->endTransmission(false);
    _wire->requestFrom(_address, len);

    for (uint8_t i = 0; i < len; i++)
        buf[i] = _wire->read();
}

void AXP2101::write8(uint8_t reg, uint8_t value)
{
    _wire->beginTransmission(_address);
    _wire->write(reg);
    _wire->write(value);
    _wire->endTransmission();
}

void AXP2101::writeBytes(uint8_t reg, const uint8_t *data, uint8_t len)
{
    _wire->beginTransmission(_address);
    _wire->write(reg);
    for (uint8_t i = 0; i < len; i++)
        _wire->write(data[i]);
    _wire->endTransmission();
}

/* =====================================================
   Bit Helpers
   ===================================================== */

void AXP2101::setBit(uint8_t reg, uint8_t bit)
{
    write8(reg, read8(reg) | (1 << bit));
}

void AXP2101::clearBit(uint8_t reg, uint8_t bit)
{
    write8(reg, read8(reg) & ~(1 << bit));
}

void AXP2101::writeBit(uint8_t reg, uint8_t bit, bool value)
{
    if (value) setBit(reg, bit);
    else clearBit(reg, bit);
}

void AXP2101::writeField(uint8_t reg, uint8_t mask, uint8_t shift, uint8_t value)
{
    uint8_t r = read8(reg);
    r &= ~mask;
    r |= (value << shift) & mask;
    write8(reg, r);
}


/* =====================================================
   ADC Helpers
   ===================================================== */

// AXP2101 uses full 16-bit ADC registers (NOT 14-bit format)
uint16_t AXP2101::readADC14(uint8_t highReg, uint8_t lowReg)
{
    uint16_t high = read8(highReg);
    uint16_t low  = read8(lowReg);
    return (high << 8) | low;
}

/* =====================================================
   STATUS
   ===================================================== */

bool AXP2101::isVBUSPresent()
{
    return read8(0x00) & 0x20;
}

bool AXP2101::isBatteryPresent()
{
    return read8(0x00) & 0x08;
}

bool AXP2101::isCharging()
{
    uint8_t r01 = read8(0x01);
    return((r01 & 0x07) >= 1 && (r01 & 0x07) <= 3);
}

bool AXP2101::isPowerGood()
{
    return read8(0x00) & 0x01;
}

/* =====================================================
   SYSTEM CONTROL
   ===================================================== */

void AXP2101::enableSleep(bool enable)
{
    writeBit(0x18, 1, enable);

}

void AXP2101::powerOff()
{
    setBit(0x10, 7);
}

void AXP2101::reboot()
{
    setBit(0x10, 6);
}

/* =====================================================
   CHARGER CONTROL
   ===================================================== */

void AXP2101::enableCharging(bool enable)
{
    if (enable) {
        // Force bit 7 set: read current value, set bit 7, write back
        uint8_t current = read8(0x18);
        uint8_t toWrite = current | 0x80;  // Set bit 7
        
        Serial.println("[enableCharging] Forcing bit 7...");
        Serial.print("  Read 0x18: 0x"); Serial.println(current, HEX);
        Serial.print("  Writing 0x18: 0x"); Serial.println(toWrite, HEX);
        
        write8(0x18, toWrite);
        
        delay(100);  // Brief delay for PMIC to process
        
        uint8_t verify = read8(0x18);
        Serial.print("  Verify 0x18: 0x"); Serial.println(verify, HEX);
        
        if ((verify & 0x80) == 0) {
            Serial.println("[enableCharging] WARNING: Bit 7 not set! Retrying...");
            write8(0x18, 0xA0);  // Force bit 7 + bit 5 = 0xA0
            delay(10);
            verify = read8(0x18);
            Serial.print("  Retry verify: 0x"); Serial.println(verify, HEX);
        }
    } else {
        writeBit(0x18, 7, false);
    }
}

void AXP2101::setChargeCurrent(uint16_t mA)
{
    uint8_t val;
    if (mA < 200){
        val = mA/25;   
    }else{
        val = ((mA  - 200)/100 + 8);   
    }

    write8(0x62, val);
    // Serial.println("[setChargeCurrent] Set charge current to " + String(mA) + "mA (reg value: 0x" + String(val, HEX) + ")");
    
}

void AXP2101::setChargeVoltage(float volts)
{
    const float minV = 4.0f;
    const float step = 0.01f; // 10mV steps
    const uint8_t maxSteps = 0x3F; // 6-bit field
    const float maxV = minV + step * maxSteps;

    if (volts < minV) volts = minV;
    if (volts > maxV) volts = maxV;

    uint8_t val = (uint8_t)((volts - minV) / step + 0.5f);
    writeField(0x64, 0x3F, 0, val);
}

void AXP2101::setInputVoltageLimit(float volts)
{
    uint8_t val = round((volts - 3.88f)/0.08f);
    Serial.println("[setInputVoltageLimit] Set input voltage limit to " + String(volts) + "V (reg value: 0x" + String(val, HEX) + ")");
    writeField(0x15, 0x3F, 0, val);
}

void AXP2101::setInputCurrentLimit(uint16_t mA)
{
    uint8_t val = mA / 50;   // 50mA steps
    writeField(0x16, 0x3F, 0, val);
    
}

// =====================================================
// DCDC CONTROL
// =====================================================
// Enable register (example location — adjust if needed)
#define AXP_DCDC_ENABLE_REG   0x80
#define AXP_DCDC1_VOLT_REG    0x81
#define AXP_DCDC2_VOLT_REG    0x82
#define AXP_DCDC3_VOLT_REG    0x83
#define AXP_DCDC4_VOLT_REG  0x84


void AXP2101::enableDCDC1(bool enable)
{
    writeBit(AXP_DCDC_ENABLE_REG, 0, enable);
}

void AXP2101::enableDCDC2(bool enable)
{
    writeBit(AXP_DCDC_ENABLE_REG, 1, enable);
}

void AXP2101::enableDCDC3(bool enable)
{
    writeBit(AXP_DCDC_ENABLE_REG, 2, enable);
}
void AXP2101::enableDCDC4(bool enable)
{
    writeBit(0x80, 3, enable);
}
static uint8_t dcdcVoltageToReg(float volts)
{
    if (volts < 0.5f) volts = 0.5f;
    if (volts > 1.54f) volts = 1.54f;

    return (uint8_t)((volts - 0.5f) / 0.01f);  // 10mV step
}

void AXP2101::setDCDC1Voltage(float volts)
{
    write8(AXP_DCDC1_VOLT_REG, dcdcVoltageToReg(volts));
}

void AXP2101::setDCDC2Voltage(float volts)
{
    write8(AXP_DCDC2_VOLT_REG, dcdcVoltageToReg(volts));
}

void AXP2101::setDCDC3Voltage(float volts)
{
    write8(AXP_DCDC3_VOLT_REG, dcdcVoltageToReg(volts));
}
void AXP2101::setDCDC4Voltage(float volts)
{
    if (volts < 1.0f) volts = 1.0f;
    if (volts > 3.4f) volts = 3.4f;

    uint8_t reg = (volts - 1.0f) / 0.05f;  // 50mV steps
    write8(AXP_DCDC4_VOLT_REG, reg);
}
// =====================================================
// LDO CONTROL
// =====================================================
#define AXP_LDO_ENABLE_REG   0x90
#define AXP_LDO1_VOLT_REG    0x91
#define AXP_LDO2_VOLT_REG    0x92
#define AXP_LDO3_VOLT_REG    0x93
#define AXP_LDO4_VOLT_REG    0x94

void AXP2101::enableLDO1(bool enable)
{
    writeBit(AXP_LDO_ENABLE_REG, 0, enable);
}

void AXP2101::enableLDO2(bool enable)
{
    writeBit(AXP_LDO_ENABLE_REG, 1, enable);
}

void AXP2101::enableLDO3(bool enable)
{
    writeBit(AXP_LDO_ENABLE_REG, 2, enable);
}

void AXP2101::enableLDO4(bool enable)
{
    writeBit(AXP_LDO_ENABLE_REG, 3, enable);
}

static uint8_t ldoVoltageToReg(float volts)
{
    if (volts < 0.5f) volts = 0.5f;
    if (volts > 3.5f) volts = 3.5f;

    return (uint8_t)((volts - 0.5f) / 0.1f);  // 100mV step
}

void AXP2101::setLDO1Voltage(float volts)
{
    write8(AXP_LDO1_VOLT_REG, ldoVoltageToReg(volts));
}

void AXP2101::setLDO2Voltage(float volts)
{
    write8(AXP_LDO2_VOLT_REG, ldoVoltageToReg(volts));
}

void AXP2101::setLDO3Voltage(float volts)
{
    write8(AXP_LDO3_VOLT_REG, ldoVoltageToReg(volts));
}

void AXP2101::setLDO4Voltage(float volts)
{
    write8(AXP_LDO4_VOLT_REG, ldoVoltageToReg(volts));
}

/* =====================================================
   ADC Voltage Reads (Correct Region)
   ===================================================== */

#define ADC_LSB_mV  1.0f   // Fine-tune if needed

float AXP2101::getBatteryVoltage()
{
    uint16_t vbat_raw = ((read8(0x34) & 0x3F) << 8) | read8(0x35);
    return (vbat_raw / 1000.0f);
}

float AXP2101::getSystemVoltage()
{
    uint16_t vsys_raw = ((read8(0x3A) & 0x3F) << 8) | read8(0x3B);
    return (vsys_raw / 1000.0f);
}

float AXP2101::getVBUSVoltage()
{
    uint16_t vbus_raw = ((read8(0x38) & 0x3F) << 8) | read8(0x39);
    return (vbus_raw / 1000.0f);
}

//TODO: scale this properly based on battery curve for better accuracy
uint8_t AXP2101::getBatteryPercent()
{
    float v = getBatteryVoltage();

    if (v >= 4.2f) return 100;
    if (v <= 3.0f) return 0;

    return (uint8_t)((v - 3.0f) * 100.0f / 1.2f);
}

/* =====================================================
   INTERRUPTS (basic passthrough)
   ===================================================== */

void AXP2101::enableInterrupt(uint8_t reg, uint8_t mask)
{
    write8(reg, read8(reg) | mask);
}

void AXP2101::disableInterrupt(uint8_t reg, uint8_t mask)
{
    write8(reg, read8(reg) & ~mask);
}

uint8_t AXP2101::getInterruptStatus(uint8_t reg)
{
    return read8(reg);
}

void AXP2101::clearInterrupt(uint8_t reg, uint8_t mask)
{
    write8(reg, mask);
}

/* =====================================================
   INIT
   ===================================================== */


void AXP2101::init(float chargeVoltage, uint16_t chargeCurrent)
{   
    
    // writeBit(0x17, 3, true); // reset fuel guage, whatever that means

    write8(0x30, 0x1F); // ADC channel enable control. 00111111 
    
    writeBit(0x50, 4, true); //TS pin ctrl. TS pin to not effect charger. I think it might be high by default.    

    
    // setInputVoltageLimit(5.08f);
    // write8(0x15, 0x0F); // should be setting input voltage limit to 5.08v but makes it not work
    write8(0x15, 0x06);  // VINDPM = 4.36V // 

    write8(0x16, 0x04); //sets input current limit to 0100, 100mA

    writeBit(0x18, 3, false); // reset fuel guage, whatever that means
    writeBit(0x18, 1, true); //cell battery charge enable

    write8(0x22, 0x00); //PWROFF_EN, disable 2 and 1 - DIE over temp and PWRON> OFFLEVEL as PWEROFF Source enable 
    // write8(0x23, 0x00); //PWROFF of DCDC OVP / UVP control. Disable all.
    
    //reg25 "check PWROK pin enable after all dcdc /ldo output valid" could be something there

    //no fucking clue what JEITA is but try disabling it?
    write8(0x58, 0x00);

    write8(0x62, 0x09); //constant current charrge. set to 300mA

    writeBit(0x63,4, 0); //charging termitation current. Disable - probs dangerous idk

    write8(0x64, 0x03); // set charge voltage limit to 4.2v - default
    

        //dcdc1 to 3.4v
    write8(0x82, 0x13);
    //1.4v -> 3.7v in 100mV. => 3.3-1.4 = 1.9v range / 0.1v step = 19 steps, so 0x13
    //dcdc5 
    write8(0x86, 0x13);


    //DCDC on/off and DVM control - enable force DCDC to work in CCM (whatever CCM is??)
    //DVM voltage ramp leave as 15.65uS/step
    //disable all except DCDC1 and DCDC5
    // 0101 0001
    write8(0x80, 0x51); 

    // pwm and pfm control 0
    //DCDC UVP debounce time to max 240uS from 60uS. seems like a good thing
    write8(0x81, 0x03);


    //make sure all LDOs are off
    write8(0x90, 0x00);
    write8(0x91, 0x00);    


}

void AXP2101::dumpRegisters() {
    // Serial.println("\n========== AXP2101 FULL REGISTER DUMP ==========");
    // Serial.println("     00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F");
    // Serial.println("     -----------------------------------------------");

    // for (uint8_t row = 0; row < 0x10; row++) {
    //     Serial.print("0x");
    //     if (row < 0x10) Serial.print("0");
    //     Serial.print(row * 16, HEX);
    //     Serial.print(" |");

    //     for (uint8_t col = 0; col < 16; col++) {
    //         uint8_t val = read8(row * 16 + col);
    //         Serial.print(" ");
    //         if (val < 0x10) Serial.print("0");
    //         Serial.print(val, HEX);
    //     }
    //     Serial.println();
    // }

    // Serial.println("=================================================");

    // Human-readable summary of the key charging registers
    Serial.println("\n---------- KEY REGISTERS ----------");

    uint8_t r00 = read8(0x00);
    uint8_t r01 = read8(0x01);
    uint8_t r15 = read8(0x15);
    uint8_t r16 = read8(0x16);
    uint8_t r18 = read8(0x18);
    uint8_t r30 = read8(0x30);
    uint8_t r50 = read8(0x50);
    uint8_t r62 = read8(0x62);
    uint8_t r64 = read8(0x64);

    Serial.print("REG00 (PMU status1):      0x"); Serial.print(r00, HEX);
    Serial.print("  VBUS_good=");  Serial.print((r00 >> 5) & 1);
    Serial.print(" BAT_present="); Serial.print((r00 >> 3) & 1);
    Serial.print(" thermal_reg="); Serial.println((r00 >> 1) & 1);

    Serial.print("REG01 (PMU status2):      0x"); Serial.print(r01, HEX);
    Serial.print("  chg_status=");
    switch (r01 & 0x07) {
        case 0: Serial.println("tri_charge");           break;
        case 1: Serial.println("pre_charge");           break;
        case 2: Serial.println("CC (constant current)"); break;
        case 3: Serial.println("CV (constant voltage)"); break;
        case 4: Serial.println("charge DONE");          break;
        case 5: Serial.println("not charging");         break;
        default: Serial.println("reserved");            break;
    }

    Serial.print("REG15 (VINDPM):           0x"); Serial.print(r15, HEX);
    Serial.print("  = "); Serial.print(3.88f + (r15 & 0x0F) * 0.08f, 2); Serial.println("V");

    Serial.print("REG16 (input curr limit): 0x"); Serial.print(r16, HEX);
    Serial.print("  = ");
    switch (r16 & 0x07) {
        case 0: Serial.println("100mA");    break;
        case 1: Serial.println("500mA");    break;
        case 2: Serial.println("900mA");    break;
        case 3: Serial.println("1000mA");   break;
        case 4: Serial.println("1500mA");   break;
        case 5: Serial.println("2000mA");   break;
        default: Serial.println("reserved"); break;
    }

    Serial.print("REG18 (chg/gauge/wdog):   0x"); Serial.print(r18, HEX);
    Serial.print("  charge_en="); Serial.print((r18 >> 1) & 1);
    Serial.print(" gauge_en=");   Serial.print((r18 >> 3) & 1);
    Serial.print(" wdog_en=");    Serial.println(r18 & 1);

    Serial.print("REG30 (ADC enable):       0x"); Serial.print(r30, HEX);
    Serial.print("  bat=");  Serial.print(r30 & 1);
    Serial.print(" ts=");    Serial.print((r30 >> 1) & 1);
    Serial.print(" vbus=");  Serial.print((r30 >> 2) & 1);
    Serial.print(" vsys=");  Serial.println((r30 >> 3) & 1);

    Serial.print("REG50 (TS ctrl):          0x"); Serial.print(r50, HEX);
    Serial.print("  TS_affects_charger=");
    Serial.println(((r50 >> 4) & 1) ? "NO (good)" : "YES (may block charging)");

    Serial.print("REG62 (charge current):   0x"); Serial.print(r62, HEX);
    uint8_t icc = r62 & 0x1F;
    Serial.print("  = ");
    if (icc <= 8) Serial.print(icc * 25);
    else          Serial.print(200 + (icc - 8) * 100);
    Serial.println("mA");

    Serial.print("REG64 (charge voltage):   0x"); Serial.print(r64, HEX);
    Serial.print("  = ");
    switch (r64 & 0x07) {
        case 1: Serial.println("4.0V");  break;
        case 2: Serial.println("4.1V");  break;
        case 3: Serial.println("4.2V");  break;
        case 4: Serial.println("4.35V"); break;
        case 5: Serial.println("4.4V");  break;
        default: Serial.println("reserved"); break;
    }

    // ADC reads — 14-bit, mask top 2 bits of high byte per datasheet
    uint16_t vbat_raw = ((read8(0x34) & 0x3F) << 8) | read8(0x35);
    uint16_t vbus_raw = ((read8(0x38) & 0x3F) << 8) | read8(0x39);
    uint16_t vsys_raw = ((read8(0x3A) & 0x3F) << 8) | read8(0x3B);

    Serial.print("\nVBAT (0x34/35): raw="); Serial.print(vbat_raw);
    Serial.print("  "); Serial.print(vbat_raw / 1000.0f, 3); Serial.println("V");

    Serial.print("VBUS (0x38/39): raw="); Serial.print(vbus_raw);
    Serial.print("  "); Serial.print(vbus_raw / 1000.0f, 3); Serial.println("V");

    Serial.print("VSYS (0x3A/3B): raw="); Serial.print(vsys_raw);
    Serial.print("  "); Serial.print(vsys_raw / 1000.0f, 3); Serial.println("V");

    
    Serial.println("-----------------------------------\n");
}
