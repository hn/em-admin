# em-admin

Read and write [Engelmann](https://www.engelmann.de/),
[Lorenz](https://www.lorenz-meters.de/), [Brummerhoop](https://www.brummerhoop.com/)
watermeter radio parameters and read various info via infrared M-Bus interface.

This project is in no way affiliated with the above-mentioned vendors.

Tested with water meters from the WaterStar M “M-ETH” “DWZ” series, other models may work.
Use at your own risk.

## Hardware

To connect to the infrared interface of the water meter
(located on the far left side of the front, clear IR-LED at the top, dark IR-sensor at the bottom),
you need an “opto head”. These can be bought ready-made for about 30€,
curiously you can also find offers with a price of 300€.

As a cheap alternative, an UART adapter with 3mm IR-LED and sensor (2€ in total) is sufficient,
e.g. adapt https://github.com/openv/openv/wiki/ESPHome-Optolink#hardware

## Usage

```
$ ./em-admin
Usage: ./em-admin <serial port> [get_params|set_params|set_time|set_aes|set_keyday|read_months|read_info]

$ ./em-admin /dev/ttyUSB0 get_params
Setting serial port to 2400 baud 8N1
Sending wakeup bytes
UART>025> 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55
[...]
UART>025> 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55
Setting serial port to 2400 baud 8E1
Getting device parameters
MBUS_C: 0x53
MBUS_ADR: 254
MBUS_CI: 0x51
UART>014> 68 08 08 68 53 fe 51 0f 04 00 00 60 15 16
UART<045< 68 27 27 68 08 00 72 ee ff c0 20 fa 12 02 07 03 00 00 00 07 03 12 a4 01 ff
 0f ff ff ff 7f 7f ff ff ff 21 30 e8 03 0a 00 00 00 00 6c 16
MBUS_C: 0x08
MBUS_ADR: 0
MBUS_CI: 0x72
MBUS_SECADR: 0x20c0ffee
MBUS_MANUFACTURER: 0x12FA (DWZ)
MBUS_VERSION: 2
MBUS_MEDIUM: 0x07
MBUS_ACCESSCOUNT: 3
MBUS_STATE: 0x00
MBUS_SIGNATURE: 0x0000
EM_FLAGS: 0x07
EM_OMSMODE: 3
EM_FRAMETYPE: 18
EM_INTERVAL: 420 s
EM_MONTHS: 0b111111111111 (Dec - Jan)
EM_WEEKOMS: 0b1111111111111111111111111111111 (31 - 1)
EM_DAYOWS: 0b1111111 (Sun - Mon)
EM_HOURS: 0b111111111111111111111111 (23 - 00)
EM_ONDAY: 2024-01-01 (inactive)
EM_ONVOL: 1000 l (inactive)
EM_OPYEARS: 10
Operation completed successfully

$ ./em-admin /dev/ttyUSB0 read_months
EM_METER_READING_2025-04-30: 901
EM_METER_READING_2025-03-31: 783
EM_METER_READING_2025-02-28: 495
EM_METER_READING_2025-01-31: 302
EM_METER_READING_2024-12-31: 239
EM_METER_READING_2024-11-30: 199
[...]

$ ./em-admin /dev/ttyUSB0 read_highres
EM_HIGHRES_READING: 791234 ml
```

## Documentation

First of all, use `get_params` and carefully backup the current `EM_*` settings.
Then change the `SET_EM_*` defines according to your needs,
recompile and use `set_params`.

:raised_hand: If you set the readout interval too low and also do not limit the hours and days,
the battery will discharge before the end of the water meter's service life.
Frequent reading via infrared likewise drains the battery.
You can use the [Lorenz Web Configuration Tool](https://konfigurator.lorenz-meters.de/) to
calculate expected battery lifetime ([JS source code](https://konfigurator.lorenz-meters.de/battery.js)).

M-Bus timing and protocol parsing has been loosely implemented
according to the specification and is “works for me” ware.

## Supplementary

To visualize the readings of the watermeter in Home Assistant, you can use this
[ESPHome configuration](https://github.com/hn/esphome-configs/blob/master/watermeter-waterstarm/watermeter-waterstarm.yaml).
Requires a [CC1101 receiver](https://www.ti.com/product/de-de/CC1101) wired to an ESP32
(e.g. like [this](https://github.com/hn/esphome-configs/tree/master/watermeter-waterstarm/cc1101-esp32-c3.jpg)).
