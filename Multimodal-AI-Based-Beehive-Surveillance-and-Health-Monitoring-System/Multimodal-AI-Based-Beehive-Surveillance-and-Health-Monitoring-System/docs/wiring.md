# Wiring

Pin numbers below are taken from `firmware/bhms_sensor_node/bhms_sensor_node.ino`.

## ESP32 sensor node

| Sensor | Sensor pin | ESP32 GPIO |
|---|---|---|
| NAU7802 (load cell ADC, I2C) | SDA | 21 |
| NAU7802 | SCL | 22 |
| DHT22 inside hive | DATA | 26 |
| DHT22 outside air | DATA | 27 |
| INMP441 microphone | WS | 25 |
| INMP441 | SD | 32 |
| INMP441 | SCK | 33 |
| Maintenance (re-tare) button | one side to GND | 14 (internal pull-up, wakes from deep sleep when LOW) |

Power notes:

- INMP441 must run from **3.3 V**.
- Keep load-cell signal wires short and away from power wires.
- Wi-Fi is switched off during the weight reading to avoid supply noise on the ADC.

## Load cell wiring

Typical colour code (verify with a multimeter, some cells differ): red = E+, black = E-, white = S+, green = S-.
When two cells share one amplifier, join the excitation wires and cross-connect the signal wires; if the readings
cancel instead of adding, swap the signal pair of one cell.

## ESP32-CAM

Uses the standard AI-Thinker pin map from the Arduino CameraWebServer example (`board_config.h`).
Power it from a stable 5 V supply; brown-outs are the most common cause of reboots.
