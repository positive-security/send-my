# Send My Firmware for ESP32 (based on OpenHaystack)

This project contains a PoC firmware for Espressif ESP32 chips that turns them into an (upload only) serial modem using the Find My Offline Finding network.

Before flashing, please change the modem ID and the default message (for id 0).

After boot, the device will send out the hardcoded default message until new data is received via the serial interface.
Data is sent by encoding it according to the scheme described in https://positive.security/blog/send-my and broadcasting the corresponding public keys to nearby Apple devices.

## Disclaimer

Note that the firmware is just a proof-of-concept and does not implement encrytion or authentication in the protocol. 

## Requirements

To change and rebuild the firmware, you need Espressif's IoT Development Framework (ESP-IDF).
Installation instructions for the latest version of the ESP-IDF can be found in [its documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/).
The firmware is tested on version 4.2.

For deploying the firmware, you need Python 3 on your path, either as `python3` (preferred) or as `python`, and the `venv` module needs to be available.

## Build

With the ESP-IDF on your `$PATH`, you can use `idf.py` to build the application from within this directory:

```bash
idf.py build
```

This will create the following files:

- `build/bootloader/bootloader.bin` -- The second stage bootloader
- `build/partition_table/partition-table.bin` -- The partition table
- `build/openhaystack.bin` -- The application itself

These files are required for the next step: Deploy the firmware.

## Deploy the Firmware

Use the `flash_esp32.sh` script to deploy the firmware and a public key to an ESP32 device connected to your local machine:

```bash
./flash_esp32.sh -p /dev/yourSerialPort
```

> **Note:** You might need to reset your device after running the script before it starts sending advertisements.

For more options, see `./flash-esp32.h --help`.
