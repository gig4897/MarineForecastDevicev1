README.md
# Marine Forecast Display (ESP32)

A flicker-free ESP32-based device that displays:
- 2-day tide graphs from NOAA
- NOAA weather forecasts
- Open-Meteo marine forecasts (wave height and period)
- Rotating future forecast display
- Touch-controlled backlight brightness
- Wi-Fi Access Point (AP) configuration screen

Created for small TFT displays like 240×320 ILI9341 with touchscreen support.

---

## 🔧 Hardware

| Component                | Notes                                      |
|--------------------------|--------------------------------------------|
| ESP32 dev board          | Tested with ESP32-WROOM and WROVER         |
| 2.8" or 3.2" TFT display | ILI9341-compatible, 240×320 resolution     |
| XPT2046 touchscreen      | SPI-compatible touchscreen controller      |
| Optional: PSRAM          | Not required (uses memory-safe partial sprite) |

**Pins (can be modified in code):**

```cpp
#define TP_CS   33
#define TP_CLK  25
#define TP_MOSI 32
#define TP_MISO 39
#define TFT_BL  21  // backlight pin

🧰 Required Libraries
Install via Library Manager in Arduino IDE:

TFT_eSPI (by Bodmer)

XPT2046_Touchscreen

ArduinoJson

WiFi, WebServer, HTTPClient (included with ESP32 core)

🚀 Setup
Flash the code to your ESP32

On first boot, it will enter Wi-Fi AP setup mode

Connect to:
Marine-Forecast-Setup
Password: tideclock

Enter:

ZIP code

NOAA tide station ID

Timezone offset (e.g., -4 for EDT)

Your Wi-Fi SSID and password

Press Save & Reboot

🌊 Features
Tide graph with real-time red marker and last + next tide visualization

NOAA weather: current and future periods

Open-Meteo marine forecast: wave height & period per forecast period

Touch to adjust brightness (cycles through 0–100%)

Smooth, flicker-free forecast rendering using partial TFT_eSprite

Automatic screen refresh every 15 minutes

Rotates through future forecasts every 6 seconds

Press screen 5 seconds to factory reset

📦 Customization
In code:

cpp
Copy
Edit
const uint8_t BL_LEVELS[] = { 0, 64, 128, 192, 255 };  // brightness steps
screenSprite.createSprite(240, 160);                  // forecast-only sprite
#define MAX_FUTURE 6                                   // how many forecasts to store
To change default ZIP or station ID, modify:

cpp
Copy
Edit
cfg.zip     = "32359";
cfg.station = "8727520";
🧪 Tested Configurations
ESP32-WROOM + ILI9341 SPI display + XPT2046

240×320 screen

5-level backlight control

140px sprite for bottom forecast section

📄 License
MIT License.
Feel free to fork, improve, or adapt for your harbor, surf, or tide-clock projects 🌊

💬 Credits
Project developed by Kevin (OpenAI-assisted).
Wave and tide data via NOAA and Open-Meteo.


