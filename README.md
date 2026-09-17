# ESP32-2way-comunicator
A two-way, non-blocking text communicator built on the ESP32 microcontroller. The project uses the nRF24L01+ radio module for wireless transmission, a color TFT display for a split user interface, and a miniature I2C M5Stack CardKB for text input.

## ✨ Main Features
* **True 2-way chat:** Sending and receiving messages happens without freezing (non-blocking) thanks to an efficient main loop.
* **Shared SPI bus:** Both the display and the radio module use a common bus (VSPI), saving valuable GPIO pins on the ESP32.
* **Split UI:** The screen is divided into a chat history (top, capacity of 30 messages) and an active typing field with a blinking cursor (bottom).
* **Configured via `platformio.ini`:** The display driver (`TFT_eSPI`) is fully configured using `build_flags`. The project is ready to compile out-of-the-box without modifying any library files manually.

## 🛠️ Hardware Requirements
* 2x **ESP32-WROOM-32** (30-pin DevKit)
* 2x **nRF24L01+ PA+LNA** (Radio module)
* 2x **1.8" TFT ST7735S 128x160** (SPI display)
* 2x **M5Stack CardKB** (I2C keyboard)

## 🔌 Pinout / Wiring

| Component | Component Pin | ESP32 Pin | Bus / Note |
| :--- | :--- | :--- | :--- |
| **CardKB** | SDA | GPIO 21 | I2C |
| **CardKB** | SCL | GPIO 22 | I2C |
| **Shared** | SCK / SCLK | GPIO 18 | VSPI Clock |
| **Shared** | MOSI | GPIO 23 | VSPI Data Out |
| **nRF24** | MISO | GPIO 19 | VSPI Data In |
| **nRF24** | CS / CSN | GPIO 15 | SPI Chip Select (Radio) |
| **nRF24** | CE | GPIO 4 | Radio Enable |
| **TFT** | CS | GPIO 5 | SPI Chip Select (Display) |
| **TFT** | DC / A0 | GPIO 16 | Data / Command |
| **TFT** | RST / RES | GPIO 17 | Reset |
