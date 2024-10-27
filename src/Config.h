#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <FastLED.h>

#define NUM_LEDS 144
#define DATA_PIN_1 14
#define DATA_PIN_2 12
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB

// BLE UUIDs
#define SERVICE_UUID "19b10000-e8f2-537e-4f6c-d104768a1214"
#define MODE_CHARACTERISTIC_UUID "19b10001-e8f2-537e-4f6c-d104768a1214"
#define BRIGHTNESS_CHARACTERISTIC_UUID "19b10002-e8f2-537e-4f6c-d104768a1214"
#define FREQUENCY_CHARACTERISTIC_UUID "19b10003-e8f2-537e-4f6c-d104768a1214"
#define POWER_CHARACTERISTIC_UUID "19b10005-e8f2-537e-4f6c-d104768a1214"
#define COLOR_R_CHARACTERISTIC_UUID "19b10006-e8f2-537e-4f6c-d104768a1214"
#define COLOR_G_CHARACTERISTIC_UUID "19b10007-e8f2-537e-4f6c-d104768a1214"
#define COLOR_B_CHARACTERISTIC_UUID "19b10008-e8f2-537e-4f6c-d104768a1214"

extern uint8_t mode;
extern uint8_t brightness;
extern uint8_t frequency;
extern u_int8_t color_r;
extern u_int8_t color_g;
extern u_int8_t color_b;
extern uint8_t power;

extern bool debug;
extern bool deviceConnected;
extern bool oldDeviceConnected;
extern unsigned long notify_interval;

#endif // CONFIG_H
