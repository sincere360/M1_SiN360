# BadUSB And BadBLE Workflows

Use these workflows only on computers, phones, and test accounts you own or have permission to test.

## Script Files

1. Put scripts on the SD card with a `.txt`, `.ducky`, or `.duck` extension.
2. Use the file browser from the BadUSB or BadBLE screen to select the script.
3. Review the confirmation screen.
4. Press `OK` to run the script.
5. Press `BACK` to cancel or stop.

The right arrow does not run scripts. Use `OK` from the confirmation screen.

## Supported Script Commands

The first script runner supports the common keyboard basics:

- `STRING text`
- `STRINGLN text`
- `DELAY ms`
- `DEFAULT_DELAY ms`
- Named keys such as `ENTER`, `TAB`, `ESC`, `SPACE`, and arrow keys
- Modifier combos such as `CTRL`, `ALT`, `SHIFT`, and `GUI` with another key

More DuckyScript commands, preview, pause, and stop controls are planned for later passes.

## BadUSB Test

1. Connect the M1 to the target computer over USB.
2. Open `BadUSB`.
3. Select a script from the SD card.
4. Put the cursor in a safe test field, such as an empty text editor.
5. Press `OK` on the M1 confirmation screen.
6. Press `BACK` to stop or return.

On a host computer, the M1 should enumerate as a generic composite device:

```text
Manufacturer: M-USB
Product: Composite Device
Interface: Storage Serial HID
```


## BadBLE Test

1. Flash compatible ESP32-C6 firmware.
2. Open `Bluetooth > Attacks > BadBLE`.
3. Select a script from the SD card.
4. Pair or connect the target host to the advertised keyboard.
5. Press `OK` on the M1 confirmation screen after the host is connected.
6. Press `BACK` to stop.

BadBLE advertises with a generic `Keyboard-XX` name and rotates the BLE address when a run starts. The suffix is only there to make repeated tests easier to identify.

Expected result: once the host accepts the BLE keyboard connection, the selected script types into the focused window.


## Troubleshooting

- If nothing happens, make sure you pressed `OK` on the confirmation screen.
- If text goes to the wrong place, click into the target window before running.
- If BadBLE shows `BLE HID missing`, flash the matching ESP32-C6 firmware.
- If BadBLE shows an ESP32 response error, stop other WiFi/BLE modes and retry.
- If a host remembered an old BLE keyboard entry, remove it from the host Bluetooth list and start BadBLE again so it advertises a fresh address.
- If characters are wrong, check the host keyboard layout. The current HID runner sends standard keyboard usage codes.
