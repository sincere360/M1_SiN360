# Sub-GHz And RFID Features

This document tracks the current Sub-GHz and 125 kHz RFID feature set in SiN360 firmware.

Use these tools only with devices, tags, and RF environments you own or have permission to test.

## Sub-GHz

The Sub-GHz app uses the onboard Si446x radio for receive, transmit, signal inspection, and simple protocol tools.

### Radio Coverage

- Preset bands for common Sub-GHz work
- Exact custom frequency entry from `300.000` to `928.000` MHz
- OOK and FSK radio modes where supported by the selected tool
- Adjustable TX power
- Region-check toggle for lab use

Custom frequency entry no longer snaps to the nearest preset. The firmware computes and applies the Si446x frequency-control values for the exact frequency, then uses the nearest practical front-end path for the selected range.

### Signal Tools

- RSSI Meter with live signal level and peak tracking
- Frequency Scanner for sweeping configured ranges
- Spectrum Analyzer for visual RF activity
- Record Ready view with live frequency and radio state

### File And Transmit Tools

- Load and transmit supported Sub-GHz files from SD card
- Playlist Player for sequential `.sub` playback
- Repeat count and progress display for playlist playback
- Basic Flipper-style path handling for playlist entries

### Protocol Support

- Princeton decode/transmit support
- Security+ 2.0 decode support
- Security+ 1.0 ternary decode
- CAME / Prastel / Airforce style fixed-code decode
- Nice FLO fixed-code decode
- Linear fixed-code decode
- Holtek decode
- Hormann decode
- Doitrand decode
- Dooya decode
- CAME Atomo decode
- CAME Twee decode
- Ansonic decode
- BETT decode
- Clemsa decode
- Gate TX decode
- Marantec decode
- Mastercode decode
- SMC5326 decode
- Holtek HT12x decode
- Nero Radio decode
- Nice Flor S / Nice One decode
- Alutech AT-4N decode
- Kinggates Stylo 4K decode
- Honeywell WDB decode
- MegaCode decode
- Chamberlain 7/8/9-bit decode
- Linear Delta3 decode
- Feron decode
- GangQi decode
- Hay21 decode
- Marantec24 decode
- Roger decode
- Hollarm decode
- KeeLoq raw frame decode
- Star Line raw frame decode
- FAAC SLH raw frame decode
- Phoenix V2 raw frame decode
- Magellan raw frame decode
- Legrand decode
- Dickert MAHS decode
- Kia raw frame decode
- Scher-Khan raw frame decode
- iDo raw frame decode
- Nero Sketch decode
- Intertechno V3 decode
- Add Manually for Princeton-style keys
- Brute-force helpers for Princeton, CAME, Nice FLO, Linear, and Holtek style fixed-code ranges
- Capture display shows parsed serial, button, counter, event, fixed-code, hopping-code, and manufacturer detail where the protocol exposes it

## 125 kHz RFID

The RFID app supports read, save, write, clone, and emulate flows for a larger LF RFID protocol set.

### Current Protocol Coverage

- EM4100
- EM4100 32-bit variant
- EM4100 16-bit variant
- H10301
- HIDProx
- HIDExt
- IOProxXSF
- AWID
- FDX-A
- FDX-B
- Pyramid
- Indala26
- Idteck
- Keri
- Nexwatch
- Jablotron
- Viking
- Paradox
- PAC/Stanley
- Noralsy
- GProxII
- RadioKey/Securakey
- Gallagher
- Electra

Protocols have read/write/emulate support where the local protocol structure and tag type support it. T5577 writing is available for compatible LF formats.

Gallagher and Electra currently save, write, and emulate raw frames. They work as protocol ports, but they still need friendlier decoded field views.

### RFID Tools

- Read card
- Save card
- Emulate saved card
- Clone compatible cards to T5577
- Blank T5577 writer
- H10301 facility-code brute-force writer
- H10301 RFID fuzzer

## Field Testing Notes

- Test RFID write/emulate with expendable T5577 tags first.
- PSK/direct protocols may need more antenna and timing validation.
- For Sub-GHz custom frequencies, verify output with a spectrum analyzer, SDR, or VNA-assisted setup before relying on a new frequency range.

## Sub-GHz Workflows

### RSSI And Frequency Check

1. Open `Sub-GHz > RSSI Meter`.
2. Trigger a known test transmitter nearby.
3. Watch the live RSSI and peak value.
4. Open `Sub-GHz > Frequency Scanner`.
5. Sweep the expected band.

Expected result: the RSSI meter reacts to nearby transmitters and the scanner finds activity near the expected frequency.

### Custom Frequency

1. Open `Sub-GHz > Radio Settings > Frequency`.
2. Enter an exact frequency between `300.000` and `928.000` MHz.
3. Return to RX or record mode.
4. Verify the active frequency on-device and with external RF test gear when available.

Expected result: the chosen frequency is applied directly instead of snapping to a preset.

### Playlist Player

1. Put `.sub` files on the SD card.
2. Put a playlist `.txt` under the Sub-GHz playlist folder.
3. Add one `.sub` path per line.
4. Open the playlist player and select the file.
5. Set the repeat count and run the playlist.

Expected result: each listed `.sub` file is transmitted in order, with progress shown on the display.

## RFID Workflows

### Read And Save

1. Open `125 kHz RFID > Read`.
2. Hold a known compatible card near the LF antenna.
3. Save the card after a successful read.

Expected result: the protocol and decoded card data are shown, then saved to SD card.

### Clone To T5577

1. Read or load a compatible LF card.
2. Use a blank expendable T5577 test tag.
3. Start the clone/write flow.
4. Read the T5577 back after writing.

Expected result: the cloned tag reads back as the selected protocol when the protocol and T5577 modulation support the write.

### Emulate Saved Card

1. Load a saved RFID card file.
2. Start emulate.
3. Test against a lab reader.
4. Press `BACK` to stop.

Expected result: the M1 emits the saved LF format while emulate is running.
