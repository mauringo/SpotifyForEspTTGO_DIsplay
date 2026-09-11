# ESPttgoTdisplay

Spotify now-playing display for an ESP32 TTGO T-Display, built with Arduino and PlatformIO.

This project is based on [mauringo/SpotifyEsp32_fixedAuthReboot](https://github.com/mauringo/SpotifyEsp32_fixedAuthReboot).

## Setup

1. Copy `include/secrets.h.example` to `include/secrets.h` and enter your Wi-Fi and Spotify application credentials. The local credentials file is ignored by Git.
2. Connect your TTGO T-Display.
3. Build with `pio run`, upload with `pio run --target upload`, and open the serial monitor with `pio device monitor`.
4. Follow the Spotify authorization instructions shown by the firmware.

The active firmware is in `src/main.cpp`; display pins and library dependencies are configured in `platformio.ini`.
