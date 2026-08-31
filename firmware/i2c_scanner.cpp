#include <Arduino.h>
#include <Wire.h>

void setup() {
  Wire.begin(21, 22);
  Wire.setClock(400000);
  Serial.begin(115200);
  
  Serial.println("Scanning I2C bus...");
  
  for (byte addr = 0x68; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("Found I2C device at: 0x%02X\n", addr);
    }
  }
}

void loop() {}
