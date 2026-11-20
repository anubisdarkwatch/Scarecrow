# CYD HUD — build variants

The HUD is one sketch (`cyd-hud.ino`) built for different Cheap Yellow Display
boards via `-D` flags. The panel is configured through TFT_eSPI build flags; the
sketch only differs per board in backlight pins and how touch is sensed
(resistive PENIRQ vs GT911 capacitive). Touch gesture is identical everywhere:
quick **tap** cycles the view, **hold** performs the view's action.

Partition: `default`, 4 MB flash. Library path: your Arduino `libraries`
(TFT_eSPI, ArduinoJson).

## 2.8" / 2.4" — ESP32-2432S028R / 2432S024 (ILI9341, resistive)

```
-DUSER_SETUP_LOADED=1 -DILI9341_DRIVER=1 -DTFT_WIDTH=240 -DTFT_HEIGHT=320 \
-DTFT_MISO=12 -DTFT_MOSI=13 -DTFT_SCLK=14 -DTFT_CS=15 -DTFT_DC=2 -DTFT_RST=-1 \
-DTFT_BL=27 -DTFT_BACKLIGHT_ON=HIGH -DTFT_RGB_ORDER=TFT_BGR \
-DLOAD_GLCD=1 -DLOAD_FONT2=1 -DLOAD_FONT4=1 -DLOAD_FONT6=1 \
-DSPI_FREQUENCY=55000000 -DBOARD_CYD28
```

## 3.2" — ESP32-2432S032C(-I) (ST7789 inverted, GT911 capacitive)

```
-DUSER_SETUP_LOADED=1 -DST7789_DRIVER=1 -DTFT_WIDTH=240 -DTFT_HEIGHT=320 \
-DTFT_MISO=12 -DTFT_MOSI=13 -DTFT_SCLK=14 -DTFT_CS=15 -DTFT_DC=2 -DTFT_RST=-1 \
-DTFT_BL=27 -DTFT_BACKLIGHT_ON=HIGH -DTFT_INVERSION_ON=1 \
-DLOAD_GLCD=1 -DLOAD_FONT2=1 -DLOAD_FONT4=1 -DLOAD_FONT6=1 \
-DSPI_FREQUENCY=55000000 -DBOARD_CYD32
```

GT911 capacitive touch is on I2C **SDA 33 / SCL 32**; reset uses INT 21 / RST 25;
address auto-detects 0x5D or 0x14. Backlight is GPIO27 (GPIO21 is the touch INT
on this board, so it is *not* driven as backlight in the CYD32 build).

### Colors look wrong on the 3.2"?
ST7789 CYD panels vary. If colors are a negative, drop `-DTFT_INVERSION_ON=1`.
If red/blue are swapped, add `-DTFT_RGB_ORDER=TFT_BGR`.

Build example:
```
arduino-cli compile --fqbn "esp32:esp32:esp32:PartitionScheme=default,FlashSize=4M" \
  --build-property "compiler.cpp.extra_flags=<flags above>" cyd-hud.ino
```
