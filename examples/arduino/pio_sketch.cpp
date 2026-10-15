// Builds one example sketch from this folder under PlatformIO, which only
// converts .ino files inside src/. platformio.ini picks the sketch with
// -D ROCLINK_SKETCH="name/name.ino". The Arduino IDE ignores this file.
#include <Arduino.h>
#include ROCLINK_SKETCH
