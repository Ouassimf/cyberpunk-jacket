#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <FastLED.h>
#include <Config.h>
#include <BLEManager.h>
#include "LEDController.h"
#include "Tasks.h"


bool debug = false;

const int ledPin = 2; // Use the appropriate GPIO pin for your setup


void setup()
{
  Serial.begin(14400);
  pinMode(ledPin, OUTPUT);

  setupLEDController();
  setupBLE();
  startFastLEDTask();
}

void set_led()
{
  if (power == 1)
  {
    digitalWrite(ledPin, HIGH);
  }
  else
  {
    digitalWrite(ledPin, LOW);
  }
}

void loop()
{
  BLEManagerLoop();
  set_led();
}