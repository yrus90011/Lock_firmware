![homekey-logo-white](https://github.com/user-attachments/assets/fc93a70a-ef1e-4390-9067-6fafb255e5ac)

# HomeKey-ESP32 [![Discord](https://badgen.net/discord/members/VWpZ5YyUcm?icon=discord)](https://discord.com/invite/VWpZ5YyUcm) [![CI](https://github.com/rednblkx/HomeKey-ESP32/actions/workflows/esp32.yml/badge.svg?branch=main)](https://github.com/rednblkx/HomeKey-ESP32/actions/workflows/esp32.yml)

### HomeKey functionality for the rest of us.

## Overview

This project aims to provide the Apple HomeKey functionality with just an ESP32 and PN532 NFC Module. Sole purpose of the project is to provide the HomeKey functionality and other NFC functionalities such as MIfare Authentication or others are out of scope.

- It integrates with HomeAssistant's Tags which makes it easier to create automations based on a person(issuer) or device(endpoint).
- The internal state is published and controlled via MQTT through user-defined topics
- Any NFC Target that's not identified as homekey will skip the flow and publish the UID, ATQA and SAK on the same MQTT topic as HomeKey with the `"homekey"` field set to `false` 
- Code is not ready for battery-powered applications
- Designed for a board with an ESP32 chip and 4MB Flash size

Goal of the project is to make it possible to add the homekey functionality to locks that don't support it or to anything for that matter :)

For more advanced functionality, you might also be interested in [HAP-ESPHome](https://github.com/rednblkx/HAP-ESPHome) which attempts to integrate HomeKit (and HomeKey) into ESPHome for ultimate automations.

## Usage

Visit the [wiki](https://github.com/rednblkx/HomeKey-ESP32/wiki) for documentation on the project

## Local Firmware Notes

This fork targets an ESP32-based lock controller with HomeKey NFC, an MPR121 capacitive keypad, ESP-NOW lock signalling, MQTT/backend websocket support, OTA, and a small status UI.

### Main Modules

| Module | Location | Purpose |
| --- | --- | --- |
| Application entry point | `main/main.cpp` | Initializes NVS, HomeSpan/HomeKit, PN532 NFC, MPR121 keypad, ESP-NOW, backend websocket, OTA hooks, monitoring, and runtime tasks. |
| Hardware defaults | `main/include/config.h` | Default MQTT topics, HomeKit metadata, PN532 pins, optional action pins, and Web UI defaults. |
| PN532 NFC | `components/PN532` | SPI PN532 driver used for HomeKey and ISO14443A card detection. |
| MPR121 keypad | `components/mpr121` | Bare Conductive MPR121 library adapted as an ESP-IDF component. Uses the ESP-IDF I2C master driver. |
| HomeKit/HomeKey | `components/HomeSpan`, `components/HK-HomeKit-Lib` | HomeKit accessory framework and HomeKey cryptographic/authentication flow. |
| Websocket backend | `main/ws_client.c`, `main/api_client.c` | Backend login, websocket command handling, OTA command dispatch, remote diagnostics, and config commands. |
| OTA client | `main/ota_client.c` | Downloads firmware by backend firmware ID, verifies SHA-256, writes OTA partition, and reboots. |
| Event log | `main/evtlog.c` | Counts selected runtime events such as NFC, websocket, and MPR121 failures in NVS-backed state. |
| Monitor | `main/monitor.c` | Periodic task/runtime diagnostics used during debugging. |
| Web assets | `data/` | Files packed into the `spiffs`/LittleFS partition during build. |

### Important Runtime Functions and Tasks

| Function/task | Location | Notes |
| --- | --- | --- |
| `setup()` | `main/main.cpp` | Main boot path. Loads NVS config, creates the PN532 driver, initializes GPIO/UI, HomeSpan, OTA validation, ESP-NOW, NFC, MPR121, backend, and websocket keepalive tasks. |
| `loop()` | `main/main.cpp` | Calls `homeSpan.poll()` unless ESP-NOW is temporarily using Wi-Fi resources. |
| `create_nfc_driver()` / `destroy_nfc_driver()` | `main/main.cpp` | Owns PN532 SPI object creation/removal. Logs active PN532 pins at boot. |
| `nfc_thread_entry()` | `main/main.cpp` | Checks PN532 firmware, configures SAM/RF field, polls ISO14443A targets, runs HomeKey flow, and publishes/queues unlock events. |
| `nfc_retry()` | `main/main.cpp` | Reconnect path when PN532 startup or polling fails. |
| `mpr121_task_entry()` | `main/main.cpp` | Logs MPR121 I2C config, initializes MPR121, applies touch settings, and polls the keypad. |
| `touch_keypad()` | `main/main.cpp` | Converts MPR121 electrode touches into PIN/menu actions and ESP-NOW commands. |
| `espnow_task_entry()` | `main/main.cpp` | Initializes ESP-NOW and sends queued lock commands such as `UNLOCK` and `RINGBELL`. |
| `backend_task` / `ws_keepalive` | `main/main.cpp`, `main/ws_client.c` | Maintains backend connectivity and sends keepalive/status messages. |
| `ota_client_start_by_id()` | `main/ota_client.c` | Starts OTA for a firmware ID received from the backend. |
| `evtlog_push()` | `main/evtlog.c` | Records failure/event counters from NFC, MPR121, websocket, and related code paths. |

### Backend Websocket Commands

`main/ws_client.c` accepts text JSON websocket frames and forwards commands to `on_ws_event()` in `main/main.cpp`.

Supported inbound command envelopes:

```json
{"type":"event","payload":{"command":"ping"}}
```

```json
{"type":"command","payload":{"command":"ping"}}
```

```json
{"type":"signal","payload":{"type":"ping"}}
```

`type: "live"` is a keepalive acknowledgement. It resets the websocket miss counter and does not require a payload command.

Most command handlers use a simple string prefix check. Send the canonical forms below to avoid accidental matches.

| Command | Example payload command | Purpose / response |
| --- | --- | --- |
| `ping` | `ping` | Replies with `{"response":"pong"}`. |
| `ota_update` | `ota_update, fw=1` | Starts OTA using backend firmware ID `fw`. Replies with `ota_result` statuses. |
| `fw_list` | `fw_list` | Fetches available firmware from the backend and sends `fw_list_result`. |
| `version` | `version` | Sends an OK event with command `printf_test, version=2.5`. |
| `evtlog_get` | `evtlog_get` | Sends event-log counters such as boot count, websocket disconnects, NFC failures, and MPR121 failures. |
| `monitor_task` | `monitor_task` | Sends one monitor snapshot with heap and task diagnostics. |
| `monitor_start` | `monitor_start` or `monitor_start 30000` | Starts monitor collection. Optional timeout is milliseconds; default is `15000`. |
| `monitor_stop` | `monitor_stop` | Stops monitor collection. |
| `add_new_card` | `add_new_card` | Arms the next detected card to be stored. |
| `request_card_number` | `request_card_number` | Same behavior as `add_new_card`. |
| `uids_list` | `uids_list` | Sends stored card UIDs. |
| `delete_uid` | `delete_uid,uid=04AABBCC` | Deletes a stored UID. The `uid=` value ends at comma or newline. |
| `deactivate` | `deactivate,uid=04AABBCC` | Deactivates a stored UID. |
| `activate` | `activate,uid=04AABBCC` | Reactivates a stored UID. |
| `get_mac` | `get_mac` | Sends the ESP32 Wi-Fi STA MAC address. |
| `get_receiver_mac` | `get_receiver_mac` | Sends the configured ESP-NOW receiver MAC. |
| `set_receiver_mac` | `set_receiver_mac AA:BB:CC:DD:EE:FF` | Saves a new ESP-NOW receiver MAC. Separators after the command may be space, tab, comma, or `=`. |
| `get_wifi_channel` | `get_wifi_channel` | Sends the current Wi-Fi primary channel. |
| `get_wifi_rssi` | `get_wifi_rssi` | Sends current Wi-Fi RSSI. |
| `wifi_disconnect` | `wifi_disconnect` | Disconnects the STA interface from the AP. |
| `send_via_esp` | `send_via_esp UNLOCK` | Queues a text payload for ESP-NOW. |
| `espnow_blast` | `espnow_blast UNLOCK` | Starts an async ESP-NOW blast task with the provided payload. |
| `reboot` | `reboot` | Sends OK, waits briefly, then restarts the ESP32. |
| `start_config_ap` | `start_config_ap` | Runs the HomeSpan `A` command to start the config AP. |
| `reset_hk_pair` | `reset_hk_pair` | Deletes reader data and runs the HomeSpan `H` command. |
| `reset_wifi_cred` | `reset_wifi_cred` | Runs the HomeSpan `X` command to clear Wi-Fi credentials. |
| `get_misc` | `get_misc` | Sends the current misc config object. |
| `save_misc` | `save_misc,{"deviceName":"Front Door"}` | Applies and persists a partial misc config object, then restarts. |
| `clear_misc` | `clear_misc` | Erases saved misc config from NVS and resets in-memory misc config to defaults. |
| `get_hkinfo` | `get_hkinfo` | Sends serialized HomeKey reader identifiers and issuer endpoint IDs. |

`save_misc` accepts only keys already present in the misc config. Known keys include `deviceName`, `otaPasswd`, `hk_key_color`, `setupCode`, `lockAlwaysUnlock`, `lockAlwaysLock`, `controlPin`, `hsStatusPin`, `gpioActionPin`, `gpioActionLockState`, `gpioActionUnlockState`, `gpioActionMomentaryEnabled`, `gpioActionMomentaryTimeout`, `webAuthEnabled`, `webUsername`, `webPassword`, `nfcGpioPins`, `btrLowStatusThreshold`, `proxBatEnabled`, `hkDumbSwitchMode`, `hkAltActionInitPin`, `hkAltActionInitLedPin`, `hkAltActionInitTimeout`, `hkAltActionPin`, `hkAltActionTimeout`, `hkAltActionGpioState`, and `hkGpioControlledState`.

Useful command examples:

```json
{"type":"event","payload":{"command":"ota_update, fw=12"}}
```

```json
{"type":"command","payload":{"command":"save_misc,{\"webAuthEnabled\":true,\"webUsername\":\"admin\"}"}}
```

```json
{"type":"event","payload":{"command":"set_receiver_mac=AA:BB:CC:DD:EE:FF"}}
```

```json
{"type":"event","payload":{"command":"send_via_esp UNLOCK"}}
```

`ws_client.c` also routes websocket log-upload acknowledgements directly to `main/ws_log_upload.c`, not to `on_ws_event()`. Those inbound message types are `log_upload_ready`, `log_upload_done`, and `log_upload_error`; device-originated upload messages are `log_upload_begin`, `log_upload_chunk`, and `log_upload_end`.

### Pin Configuration

Current ESP32 hardware pin map:

| Signal | GPIO | Source | Notes |
| --- | ---: | --- | --- |
| PN532 `SS` / chip select | 5 | `NFC_PN532_SS` in `main/include/config.h` | PN532 uses SPI. |
| PN532 `SCK` | 18 | `NFC_PN532_SCK` in `main/include/config.h` | Do not share with MPR121 I2C. |
| PN532 `MISO` | 19 | `NFC_PN532_MISO` in `main/include/config.h` | Do not share with MPR121 I2C. |
| PN532 `MOSI` | 23 | `NFC_PN532_MOSI` in `main/include/config.h` | SPI data out. |
| MPR121 `SCL` | 22 | `CONFIG_SCL_GPIO` in `sdkconfig.defaults` and `components/mpr121/Kconfig.projbuild` | I2C clock. |
| MPR121 `SDA` | 21 | `CONFIG_SDA_GPIO` in `sdkconfig.defaults` and `components/mpr121/Kconfig.projbuild` | I2C data. |
| MPR121 `IRQ` | 15 | `CONFIG_IRQ_GPIO` in `sdkconfig`/Kconfig default | Interrupt pin passed to `MPR121_begin()`. |
| MPR121 I2C address | `0x5A` | `CONFIG_I2C_ADDRESS` | Default address unless the breakout ADDR pin is strapped differently. |
| Buzzer | 33 | `PIN_BUZZER` in `main/main.cpp` | Output. |
| Red LED | 27 | `PIN_RED` in `main/main.cpp` | Output. |
| Green LED | 26 | `PIN_GREEN` in `main/main.cpp` | Output. |
| Keypad backlight | 25 | `PIN_BACKLIGHT` in `main/main.cpp` | Output. |
| HomeSpan config button | 255 disabled | `HS_PIN` in `main/include/config.h` | Set a real GPIO to enable. |
| HomeSpan status LED | 255 disabled | `HS_STATUS_LED` in `main/include/config.h` | Set a real GPIO to enable. |
| GPIO lock action | 255 disabled | `GPIO_ACTION_PIN` in `main/include/config.h` | Optional lock relay/action output. |

MPR121 was moved off GPIO18/GPIO19 because those pins are used by the PN532 SPI bus. If MPR121 is wired to GPIO18/GPIO19, the firmware can show I2C register failures such as `code: 0x103` and PN532 can become unreliable.

### MPR121 Keypad Map

The electrode-to-key map is defined by `touchPinMap` in `main/main.cpp`:

| Electrode | Key |
| ---: | ---: |
| 0 | 1 |
| 1 | 2 |
| 2 | 11 |
| 3 | 9 |
| 4 | 6 |
| 5 | 8 |
| 6 | 0 |
| 7 | 3 |
| 8 | 5 |
| 9 | 10 |
| 10 | 7 |
| 11 | 4 |

Special key handling:

| Key/electrode meaning | Value |
| --- | ---: |
| Cancel / long beep | Electrode 10 |
| Enter / change PIN | Electrode 11 |
| Default master PIN | `990011` |
| Add-card menu code | `123456` |
| Change-PIN menu code | `000` |
| Reset Wi-Fi menu code | `211` |
| Restart menu code | `888` |

### Build and Flash

Build with ESP-IDF 5.4.3:

```sh
idf.py build
```

Flash:

```sh
idf.py -p PORT flash monitor
```

On Windows, if the build fails at `.bin_timestamp` with "file is being used by another process", close any monitor/indexer holding files in `build/`, remove `build/.bin_timestamp`, and run `idf.py build` again.

### Runtime Checks

Expected useful boot logs:

```text
NFC_SETUP: PN532 SPI pins: SS=5 SCK=18 MISO=19 MOSI=23
NFC_SETUP: PN532 tuned: passive retries=0xFF poll timeout=1000 ms
MPR121: CONFIG_I2C_ADDRESS=0x5A
MPR121: CONFIG_SCL_GPIO=22
MPR121: CONFIG_SDA_GPIO=21
MPR121: CONFIG_IRQ_GPIO=15
```

If PN532 does not answer, first verify the module is in SPI mode and wired to GPIO5/18/19/23. If MPR121 shows repeated `i2c_getRegister` or `i2c_setRegister` failures, verify SDA/SCL are on GPIO21/GPIO22, the address is `0x5A`, and the breakout has working I2C pullups.

PN532 detection range is primarily limited by the antenna/module, tag/phone coil alignment, power stability, and nearby metal. The firmware keeps passive activation retries at `0xFF` and uses a 1000 ms passive poll timeout to improve marginal reads, but larger gains usually require moving the antenna away from metal, using a larger PN532 antenna, or adding a ferrite sheet behind the coil.

## Disclaimer

Use this at your own risk, i'm not a cryptographic expert, just a hobbyist. Keep in mind that the HomeKey was implemented through reverse-engineering as indicated above so it might be lacking stuff from Apple's specification to which us private individuals do not have access.

While functional as it is now, the project should still be considered as a **work in progress** so expect breaking changes.

## Contributing & Support

All contributions to the repository are welcomed, if you think you can bring an improvement into the project, feel free to fork the repository and submit your pull requests.

If you have a suggestion or are in need of assistance, you can open an issue. Additionally, you can join the Discord server at https://discord.com/invite/VWpZ5YyUcm

If you like the project, please consider giving it a star ⭐ to show the appreciation for it and for others to know this repository is worth something.

## Credits

- [@kormax](https://github.com/kormax) for reverse-engineering the Homekey [NFC Protocol](https://github.com/kormax/apple-home-key) and publishing a [PoC](https://github.com/kormax/apple-home-key-reader)
- [@kupa22](https://github.com/kupa22) for the [research](https://github.com/kupa22/apple-homekey) on the HAP side of things for Homekey
- [HomeSpan](https://github.com/HomeSpan/HomeSpan) which is being used as the framework implementing the HomeKit accessory
