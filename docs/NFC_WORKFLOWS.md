# NFC Workflows

Use these workflows with tags and devices you own or have permission to test.

## Basic Tag Read

1. Open `NFC > Read`.
2. Hold the tag near the M1 antenna.
3. Wait for the technology and UID summary.
4. Use the available actions to save, inspect, emulate, or write when supported.

Expected result: common Type 2 tags such as NTAG213/215/216 show as Ultralight/NTAG-style tags.

## Tag Info

1. Read a tag first.
2. Open `NFC > Tools > Tag Info`.
3. Review manufacturer, SAK, technology, and type information.

Expected result: the info view gives a cleaner summary than the raw read screen.

## Type 2 Page Dump

1. Read an NTAG or other Type 2 tag.
2. Open `NFC > Tools > T2T Page Dump`.
3. Scroll through memory pages.

Expected result: page data is shown in hex and ASCII-friendly form where possible.

## Write URL To NTAG

1. Use a writable Type 2 tag, such as a blank NTAG215 sticker.
2. Open `NFC > Tools > Write URL`.
3. Enter either a full URL or a bare domain.
4. Hold the tag on the antenna until the write completes.
5. Test the tag with a phone.

Examples that should work:

```text
https://example.com
http://example.com
example.com
```

Bare domains are written as HTTPS URLs. The writer uses NDEF URI records so phones should detect the tag as a normal web link.

## If A Phone Does Not Open The URL

1. Read the tag again with the M1.
2. Confirm the URL appears in the NDEF or ASCII dump.
3. Rewrite the tag while keeping it steady on the antenna.
4. Try a different phone NFC scan position.
5. If the tag was previously locked, protected, or used for another app, test with a fresh blank NTAG.

Seeing the URL in a raw memory dump is not always enough. Phones expect a valid NDEF TLV and terminator, which the current Write URL flow now writes for Type 2 tags.

## Amiibo / NTAG215 Workflow

1. Use a blank writable NTAG215 tag.
2. Open the saved Amiibo/NFC file from the file browser.
3. Use `Write Tag` for the full tag contents.
4. Keep the tag steady until the write completes.
5. Read the tag back with the M1 before testing it elsewhere.

`Write UID` is not the normal Amiibo write path. It is for compatible magic UID-changeable cards and will reject normal NTAG stickers.

## Field Test

1. Open `NFC > Tools > Field Test`.
2. Hold a field detector card, visualizer ring, or test coil near the M1 antenna.
3. Check that the RF field is present and stable.
4. Press `BACK` to stop.

This is useful for antenna and range-extender tuning because it keeps the NFC field active while showing live field-related status.

## Notes

- Amiibo-style data requires the right tag type and a complete compatible dump.
- Do not use `Write UID` unless the tag is a known UID-changeable magic card.
