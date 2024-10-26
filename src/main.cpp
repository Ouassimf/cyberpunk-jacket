#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

BLEServer *pServer = NULL;
BLECharacteristic *pModeCharacteristic = NULL;
BLECharacteristic *pBrightnessCharacteristic = NULL;
BLECharacteristic *pFrequencyCharacteristic = NULL;
BLECharacteristic *pColorCharacteristic = NULL;
BLECharacteristic *pPowerCharacteristic = NULL;

bool deviceConnected = false;
bool oldDeviceConnected = false;

// initial values for mode, brightness, frequency, color, power
uint8_t mode = 3;
uint8_t brightness = 128;
uint8_t frequency = 60;
uint8_t color = 2;
uint8_t power = 0;

unsigned long previousMillis = 0;
unsigned long notify_interval = 1000; // interval at which to notify characteristics (milliseconds)

const int ledPin = 2; // Use the appropriate GPIO pin for your setup

// See the following for generating UUIDs:
// https://www.uuidgenerator.net/
#define SERVICE_UUID "19b10000-e8f2-537e-4f6c-d104768a1214"
#define MODE_CHARACTERISTIC_UUID "19b10001-e8f2-537e-4f6c-d104768a1214"
#define BRIGHTNESS_CHARACTERISTIC_UUID "19b10002-e8f2-537e-4f6c-d104768a1214"
#define FREQUENCY_CHARACTERISTIC_UUID "19b10003-e8f2-537e-4f6c-d104768a1214"
#define COLOR_CHARACTERISTIC_UUID "19b10004-e8f2-537e-4f6c-d104768a1214"
#define POWER_CHARACTERISTIC_UUID "19b10005-e8f2-537e-4f6c-d104768a1214"

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
      Serial.print("Mode Characteristic event, written: ");
      // Print the value
      Serial.println(value.c_str());
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
      Serial.print("Brightness Characteristic event, written: ");
      // Print the value
      Serial.println(value.c_str());
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
      Serial.print("Frequency Characteristic event, written: ");
      // Print the value
      Serial.println(value.c_str());
    }
  }
};

class ColorCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pColorCharacteristic)
  {
    std::string value = pColorCharacteristic->getValue();

    if (value.length() > 0)
    {
      Serial.print("Color Characteristic event, written: ");
      // Print the value
      Serial.println(value.c_str());
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
      Serial.print("Power Characteristic event, written: ");
      // Print the value
      Serial.println(String(value.c_str()));
      // Set the power value
      power = atoi(value.c_str());
      
    }
  }
};

void setup()
{
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);

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

  pColorCharacteristic = pService->createCharacteristic(
      COLOR_CHARACTERISTIC_UUID,
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

  // Register the callback for the ON button characteristic
  pModeCharacteristic->setCallbacks(new ModeCharacteristicCallbacks());
  pBrightnessCharacteristic->setCallbacks(new BrightnessCharacteristicCallbacks());
  pFrequencyCharacteristic->setCallbacks(new FrequencyCharacteristicCallbacks());
  pColorCharacteristic->setCallbacks(new ColorCharacteristicCallbacks());
  pPowerCharacteristic->setCallbacks(new PowerCharacteristicCallbacks());

  // https://www.bluetooth.com/specifications/gatt/viewer?attributeXmlFile=org.bluetooth.descriptor.gatt.client_characteristic_configuration.xml
  // Create a BLE Descriptor
  pModeCharacteristic->addDescriptor(new BLE2902());
  pBrightnessCharacteristic->addDescriptor(new BLE2902());
  pFrequencyCharacteristic->addDescriptor(new BLE2902());
  pColorCharacteristic->addDescriptor(new BLE2902());
  pPowerCharacteristic->addDescriptor(new BLE2902());

  // Start the service
  pService->start();

  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x0); // set value to 0x00 to not advertise this parameter
  BLEDevice::startAdvertising();
  Serial.println("Waiting a client connection to notify...");
}

void set_all_values()
{
  pModeCharacteristic->setValue(String(mode).c_str());
  pBrightnessCharacteristic->setValue(String(brightness).c_str());
  pFrequencyCharacteristic->setValue(String(frequency).c_str());
  pColorCharacteristic->setValue(String(color).c_str());
  pPowerCharacteristic->setValue(String(power).c_str());
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
  Serial.println(color);
  Serial.print("Power: ");
  Serial.println(power);
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
    pColorCharacteristic->notify();
    pPowerCharacteristic->notify();
    print_all_values();
  }
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
  // notify changed value
  if (deviceConnected)
  {
    // set_all_values();
    notify_all_characteristics();
    set_led();

    // Serial.println("Notified all characteristics");
    // Print all values
    // Serial.print("Mode: ");
    // Serial.println(mode);
    // Serial.print("Brightness: ");
    // Serial.println(brightness);
    // Serial.print("Frequency: ");
    // Serial.println(frequency);
    // Serial.print("Color: ");
    // Serial.println(color);
    // Serial.print("Power: ");
    // Serial.println(power);
    // delay(3000); // bluetooth stack will go into congestion, if too many packets are sent, in 6 hours test i was able to go as low as 3ms
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