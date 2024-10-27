#include "BLEManager.h"

BLEServer *pServer = NULL;
BLECharacteristic *pModeCharacteristic = NULL;
BLECharacteristic *pBrightnessCharacteristic = NULL;
BLECharacteristic *pFrequencyCharacteristic = NULL;
BLECharacteristic *pColorRCharacteristic = NULL;
BLECharacteristic *pColorGCharacteristic = NULL;
BLECharacteristic *pColorBCharacteristic = NULL;
BLECharacteristic *pPowerCharacteristic = NULL;

bool deviceConnected = false;
bool oldDeviceConnected = false;
unsigned long previousMillis = 0;
unsigned long notify_interval = 2000;

// initial values for mode, brightness, frequency, color, power
uint8_t mode = 6;
uint8_t brightness = 128;
uint8_t frequency = 60;
uint8_t color_r = 255;
uint8_t color_g = 255;
uint8_t color_b = 255;
uint8_t power = 0;




class MyServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *pServer)
  {
    deviceConnected = true;
  };

  void onDisconnect(BLEServer *pServer)
  {
    deviceConnected = false;
  }
};



class ModeCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pModeCharacteristic)
  {
    std::string value = pModeCharacteristic->getValue();

    if (value.length() > 0)
    {
      //Serial.print("Mode Characteristic event, written: ");
      //Serial.println(value.c_str());
      mode = atoi(value.c_str());
    }
  }
};

class BrightnessCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pBrightnessCharacteristic)
  {
    std::string value = pBrightnessCharacteristic->getValue();

    if (value.length() > 0)
    {
      //Serial.print("Brightness Characteristic event, written: ");
      //Serial.println(value.c_str());
      brightness = atoi(value.c_str());
    }
  }
};

class FrequencyCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pFrequencyCharacteristic)
  {
    std::string value = pFrequencyCharacteristic->getValue();

    if (value.length() > 0)
    {
      //Serial.print("Frequency Characteristic event, written: ");
      //Serial.println(value.c_str());
      frequency = atoi(value.c_str());
    }
  }
};

class ColorRCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pColorRCharacteristic)
  {
    std::string value = pColorRCharacteristic->getValue();

    if (value.length() > 0)
    {
      //Serial.print("Color R Characteristic event, written: ");
      //Serial.println(value.c_str());
      color_r = atoi(value.c_str());
    }
  }
};

class ColorGCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pColorGCharacteristic)
  {
    std::string value = pColorGCharacteristic->getValue();

    if (value.length() > 0)
    {
      //Serial.print("Color G Characteristic event, written: ");
      //Serial.println(value.c_str());
      color_g = atoi(value.c_str());
    }
  }
};

class ColorBCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pColorBCharacteristic)
  {
    std::string value = pColorBCharacteristic->getValue();

    if (value.length() > 0)
    {
      //Serial.print("Color B Characteristic event, written: ");
      //Serial.println(value.c_str());
      color_b = atoi(value.c_str());
    }
  }
};


class PowerCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pPowerCharacteristic)
  {
    std::string value = pPowerCharacteristic->getValue();

    if (value.length() > 0)
    {
      //Serial.print("Power Characteristic event, written: ");
      //Serial.println(value.c_str());
      power = atoi(value.c_str());
    }
  }
};

void setupBLE()
{
    // Create the BLE Device
    BLEDevice::init("LED_Jacket");

    // Create the BLE Server
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    // Create the BLE Service
    BLEService *pService = pServer->createService(SERVICE_UUID);

    // Create a BLE Characteristic

    pModeCharacteristic = pService->createCharacteristic(
        MODE_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY |
            BLECharacteristic::PROPERTY_INDICATE);

    pBrightnessCharacteristic = pService->createCharacteristic(
        BRIGHTNESS_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY |
            BLECharacteristic::PROPERTY_INDICATE);

    pFrequencyCharacteristic = pService->createCharacteristic(
        FREQUENCY_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY |
            BLECharacteristic::PROPERTY_INDICATE);

    pPowerCharacteristic = pService->createCharacteristic(
        POWER_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY |
            BLECharacteristic::PROPERTY_INDICATE);

    pColorRCharacteristic = pService->createCharacteristic(
        COLOR_R_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY |
            BLECharacteristic::PROPERTY_INDICATE);

    pColorGCharacteristic = pService->createCharacteristic(
        COLOR_G_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY |
            BLECharacteristic::PROPERTY_INDICATE);

    pColorBCharacteristic = pService->createCharacteristic(
        COLOR_B_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY |
            BLECharacteristic::PROPERTY_INDICATE);


    // Register the callback for the ON button characteristic
    pModeCharacteristic->setCallbacks(new ModeCharacteristicCallbacks());
    pBrightnessCharacteristic->setCallbacks(new BrightnessCharacteristicCallbacks());
    pFrequencyCharacteristic->setCallbacks(new FrequencyCharacteristicCallbacks());
    pPowerCharacteristic->setCallbacks(new PowerCharacteristicCallbacks());
    pColorRCharacteristic->setCallbacks(new ColorRCharacteristicCallbacks());
    pColorGCharacteristic->setCallbacks(new ColorGCharacteristicCallbacks());
    pColorBCharacteristic->setCallbacks(new ColorBCharacteristicCallbacks());

    // Start the service
    pService->start();

    // Start advertising
    pServer->getAdvertising()->start();

    // Log
    Serial.println("BLE Service Started");
}

void BLEManagerLoop()
{
    if (deviceConnected)
    {
        notify_all_characteristics();
    }
    // disconnecting
    if (!deviceConnected && oldDeviceConnected)
    {
        Serial.println("Device disconnected.");
        delay(500);                  // give the bluetooth stack the chance to get things ready
        pServer->startAdvertising(); // restart advertising
        Serial.println("Start advertising");
        oldDeviceConnected = deviceConnected;
    }
    // connecting
    if (deviceConnected && !oldDeviceConnected)
    {
        // do stuff here on connecting
        oldDeviceConnected = deviceConnected;
        Serial.println("Device Connected");
    }
}

void set_all_values()
{
    pModeCharacteristic->setValue((uint8_t *)&mode, 1);
    pBrightnessCharacteristic->setValue((uint8_t *)&brightness, 1);
    pFrequencyCharacteristic->setValue((uint8_t *)&frequency, 1);
    pPowerCharacteristic->setValue((uint8_t *)&power, 1);
    pColorRCharacteristic->setValue((uint8_t *)&color_r, 1);
    pColorGCharacteristic->setValue((uint8_t *)&color_g, 1);
    pColorBCharacteristic->setValue((uint8_t *)&color_b, 1);

}

void notify_all_characteristics()
{
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= notify_interval)
    {
        set_all_values();
        previousMillis = currentMillis;
        pModeCharacteristic->notify();
        pBrightnessCharacteristic->notify();
        pFrequencyCharacteristic->notify();
        pPowerCharacteristic->notify();
        pColorRCharacteristic->notify();
        pColorGCharacteristic->notify();
        pColorBCharacteristic->notify();
    }
}


void print_all_values()
{
    Serial.print("Mode: ");
    Serial.println(mode);
    Serial.print("Brightness: ");
    Serial.println(brightness);
    Serial.print("Frequency: ");
    Serial.println(frequency);
    Serial.print("Color: ");
    Serial.print(color_r);
    Serial.print(", ");
    Serial.print(color_g);
    Serial.print(", ");
    Serial.println(color_b);
    Serial.print("Power: ");
    Serial.println(power);
}

