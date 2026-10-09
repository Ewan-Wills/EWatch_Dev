# EWatch Firmware Pinout

This file is the authoritative firmware pin reference for the current EWatch design.
The pin definitions in `src/drivers/pins.h` are the source of truth for the firmware.

## Power

- `PIN_LDO_LATCH` = GPIO17
  - drives the LDO enable latch
  - `latchPower()` must be the first call in `setup()`
- `PIN_BTN` = GPIO18
  - SW2 power / wake button

## Shared I2C bus

- `PIN_I2C_SDA` = GPIO8
- `PIN_I2C_SCL` = GPIO9
- `I2C_FREQ_HZ` = 400000

## Display (ST7789 over FSPI)

- `PIN_DISP_SCK` = GPIO10
- `PIN_DISP_MOSI` = GPIO11
- `PIN_DISP_MISO` = not used
- `PIN_DISP_DC` = GPIO6
- `PIN_DISP_CS` = GPIO7
- `PIN_DISP_RST` = GPIO12
- `PIN_DISP_BL` = GPIO21
  - drives the backlight transistor

## Touch

- `PIN_TOUCH_RST` = GPIO13
- `PIN_TOUCH_INT` = GPIO14

## Haptic

- `PIN_MOTOR_EN` = GPIO38
- `PIN_MOTOR_PWM` = GPIO37

## Battery monitor

- `PIN_BAT_MON_EN` = GPIO33
  - enables the voltage divider
- `PIN_BAT_MON_ADC` = GPIO5
  - ADC1_CH4 on the S3
  - note: this is a reroute from the schematic's GPIO34 because the S3 does not support ADC on GPIO34

## Peripheral interrupts

- `PIN_RTC_INT` = GPIO2
- `PIN_MMA_INT1` = GPIO3
  - also a JTAG strap pin; the IMU defaults inactive at boot
- `PIN_MMA_INT2` = GPIO4

## I2C addresses

- `I2C_ADDR_RV3028` = `0x52`
- `I2C_ADDR_MMA8451_A` = `0x1C`
- `I2C_ADDR_MMA8451_B` = `0x1D`
- `I2C_ADDR_TOUCH` = `0x15`

## Notes

- This pin map was verified against the current board netlist and the firmware source.
- If the hardware changes, update `src/drivers/pins.h` and this document together.
