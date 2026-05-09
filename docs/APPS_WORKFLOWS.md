# External Apps Workflow

External apps are loaded from the SD card through the `Apps` menu.

## SD Card Layout

Put app files here:

```text
SD:/apps/
```

The app browser looks for files ending in:

```text
.m1app
```

## Browse And Launch

1. Copy one or more `.m1app` files to `SD:/apps/`.
2. Reinsert the SD card and boot the M1.
3. Open `Apps > Browse Apps`.
4. Select an app with `OK`.
5. Press `BACK` to return when the app exits.

Expected result: the loader reads the app file, resolves supported SDK symbols, starts the app, and returns to the M1 UI when it exits.

## SDK Info Screen

Open `Apps > M1-SDK Info` for the on-device reminder:

```text
Apps: SD:/apps
Ext: .m1app
SDK: M1-SDK
```

The SDK firmware repo:

<https://github.com/bedge117/m1-sdk>

## App Development Notes

- Keep early apps small and simple while the loader is still new.
