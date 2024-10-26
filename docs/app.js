document.addEventListener('DOMContentLoaded', function() {
    const connectButton = document.getElementById('connect-button');
    const powerButton = document.getElementById('power-button');
    const tiles = document.querySelectorAll('.tile');
    const brightnessSlider = document.getElementById('brightness');
    const frequencySlider = document.getElementById('frequency');
    const colorPicker = document.getElementById('color-picker');

    // BLE Device Specs
    var deviceName = 'LED_Jacket';
    var serviceUUID = '19b10000-e8f2-537e-4f6c-d104768a1214';
    var mode_characteristicUUID = '19b10001-e8f2-537e-4f6c-d104768a1214';
    var brightness_characteristicUUID = '19b10002-e8f2-537e-4f6c-d104768a1214';
    var frequency_characteristicUUID = '19b10003-e8f2-537e-4f6c-d104768a1214';
    var color_characteristicUUID = '19b10004-e8f2-537e-4f6c-d104768a1214';
    var power_characteristicUUID = '19b10005-e8f2-537e-4f6c-d104768a1214';

    var bleServer;
    var bleService;
    var bleModeCharacteristic;
    var bleBrightnessCharacteristic;
    var bleFrequencyCharacteristic;
    var bleColorCharacteristic;
    var blePowerCharacteristic;



    let jacketState = {
        power: false,
        mode: null,
        brightness: brightnessSlider.value,
        frequency: frequencySlider.value,
        color: colorPicker.value
    };

    // Connect Button
    connectButton.addEventListener('click', function() {
        if (isWebBluetoothEnabled()) {
            connectButton.textContent = 'Disconnect';
            connectToDevice()
            .then(characteristics => {
                console.log('Connected to characteristics:', characteristics);
            })
            .catch(error => {
                console.error('Error connecting to device:', error);
            });
        }
    });

    

    // Toggle power state
    powerButton.addEventListener('click', function() {
        jacketState.power = !jacketState.power;
        powerButton.textContent = jacketState.power ? 'Power On' : 'Power Off';
        powerButton.classList.toggle('on', jacketState.power);
        powerButton.classList.toggle('off', !jacketState.power);
        console.log('Power:', jacketState.power);
        writeCharacteristicValue(blePowerCharacteristic, jacketState.power ? 1 : 0);
    });

    // Select mode from tiles
    tiles.forEach(function(tile) {
        tile.addEventListener('click', function() {
            tiles.forEach(t => t.classList.remove('selected'));
            tile.classList.add('selected');
            jacketState.mode = tile.dataset.mode;
            console.log('Selected mode:', jacketState.mode);
        });
    });

    // Adjust brightness
    brightnessSlider.addEventListener('input', function() {
        jacketState.brightness = brightnessSlider.value;
        console.log('Brightness:', jacketState.brightness);
    });

    // Adjust frequency
    frequencySlider.addEventListener('input', function() {
        jacketState.frequency = frequencySlider.value;
        console.log('Frequency:', jacketState.frequency);
    });

    // Pick color
    colorPicker.addEventListener('input', function() {
        jacketState.color = colorPicker.value;
        console.log('Color:', jacketState.color);
    });


    // Handle characteristic notifications
    function handleCharacteristicValueChanged(event) {
        const value = event.target.value;
        console.log('Received characteristic value:', value);
    }

    // write characteristic value
    function writeCharacteristicValue(characteristic, value) {
        if (bleServer && bleService && characteristic) {
            data = new Uint8Array([value]);
            return characteristic.writeValue(data)
            .then(() => {
                console.log('Wrote characteristic value:', value);
            })
            .catch(error => {
                console.error('Error writing characteristic value:', error);
            });
        }
        
    }

    // Function to check if Web Bluetooth is enabled
    function isWebBluetoothEnabled() {
        if (!navigator.bluetooth) {
            console.error('Web Bluetooth API is not available in this browser!');
            return false;
        }
        console.log('Web Bluetooth API is available in this browser!');
        return true;
    }

    // Function to handle disconnection
    function onDisconnected(event) {
        console.log('Disconnected:', event);
    }

    // Function to connect to the BLE device
    function connectToDevice() {
        return navigator.bluetooth.requestDevice({
            filters: [{ name: deviceName }],
            optionalServices: [serviceUUID]
        })
        .then(device => {
            console.log('Connected to device:', device);
            device.addEventListener('gattserverdisconnected', onDisconnected);
            return device.gatt.connect();
        })
        .then(server => {
            console.log('Connected to server:', server);
            bleServer = server;
            return server.getPrimaryService(serviceUUID);
        })
        .then(service => {
            console.log('Connected to service:', service);
            bleService = service;
            return Promise.all([
                service.getCharacteristic(mode_characteristicUUID),
                service.getCharacteristic(brightness_characteristicUUID),
                service.getCharacteristic(frequency_characteristicUUID),
                service.getCharacteristic(color_characteristicUUID),
                service.getCharacteristic(power_characteristicUUID)
            ]);
        })
        .then(characteristics => {
            console.log('Connected to characteristics:', characteristics);
            bleModeCharacteristic = characteristics[0];
            bleBrightnessCharacteristic = characteristics[1];
            bleFrequencyCharacteristic = characteristics[2];
            bleColorCharacteristic = characteristics[3];
            blePowerCharacteristic = characteristics[4];

            // Listen for characteristic value changes
            bleModeCharacteristic.addEventListener('characteristicvaluechanged', handleCharacteristicValueChanged);
            bleBrightnessCharacteristic.addEventListener('characteristicvaluechanged', handleCharacteristicValueChanged);
            bleFrequencyCharacteristic.addEventListener('characteristicvaluechanged', handleCharacteristicValueChanged);
            bleColorCharacteristic.addEventListener('characteristicvaluechanged', handleCharacteristicValueChanged);
            blePowerCharacteristic.addEventListener('characteristicvaluechanged', handleCharacteristicValueChanged);

            // Enable notifications
            bleModeCharacteristic.startNotifications();
            bleBrightnessCharacteristic.startNotifications();
            bleFrequencyCharacteristic.startNotifications();
            bleColorCharacteristic.startNotifications();
            blePowerCharacteristic.startNotifications();

            console.log('Notifications enabled');
        
            return {
                mode: characteristics[0],
                brightness: characteristics[1],
                frequency: characteristics[2],
                color: characteristics[3],
                power: characteristics[4]
            };
        });
    }
});