document.addEventListener('DOMContentLoaded', function () {
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
    var power_characteristicUUID = '19b10005-e8f2-537e-4f6c-d104768a1214';
    var color_r_characteristicUUID = '19b10006-e8f2-537e-4f6c-d104768a1214';
    var color_g_characteristicUUID = '19b10007-e8f2-537e-4f6c-d104768a1214';
    var color_b_characteristicUUID = '19b10008-e8f2-537e-4f6c-d104768a1214';

    var bleServer;
    var bleService;
    var bleModeCharacteristic;
    var bleBrightnessCharacteristic;
    var bleFrequencyCharacteristic;
    var bleColorRCharacteristic;
    var bleColorGCharacteristic;
    var bleColorBCharacteristic;
    var blePowerCharacteristic;


    let jacketState = {
        power: false,
        mode: null,
        brightness: brightnessSlider.value,
        frequency: frequencySlider.value,
        color: colorPicker.value
    };

    const debug = true;

    // Connect Button
    connectButton.addEventListener('click', function () {
        // remove all 
        connectButton.classList.remove('connected');
        connectButton.classList.remove('disconnected');

        if (isWebBluetoothEnabled()) {
            connectButton.textContent = 'Disconnect';
            connectToDevice()
                .then(characteristics => {
                    console.log('Connected to characteristics:', characteristics);
                    // set connected on button
                    connectButton.classList.remove('disconnected');
                    connectButton.classList.add('connected');
                })
                .catch(error => {
                    console.error('Error connecting to device:', error);
                    // set connected on button
                    connectButton.classList.remove('connected');
                    connectButton.classList.add('disconnected');
                    connectButton.textContent = 'Connect';
                });
        }
    });



    // Toggle power state
    powerButton.addEventListener('click', function () {
        jacketState.power = !jacketState.power;
        powerButton.textContent = jacketState.power ? 'Power On' : 'Power Off';
        powerButton.classList.toggle('on', jacketState.power);
        powerButton.classList.toggle('off', !jacketState.power);
        console.log('Power:', jacketState.power);
        writeCharacteristicValue(blePowerCharacteristic, jacketState.power ? 1 : 0);
    });

    // Select mode from tiles
    tiles.forEach(function (tile) {
        tile.addEventListener('click', function () {
            tiles.forEach(t => t.classList.remove('selected'));
            tile.classList.add('selected');
            jacketState.mode = tile.dataset.mode;
            console.log('Selected mode:', jacketState.mode);
            // update value
            writeCharacteristicValue(bleModeCharacteristic, jacketState.mode);
        });
    });

    // Adjust brightness
    brightnessSlider.addEventListener('input', function () {
        jacketState.brightness = brightnessSlider.value;
        console.log('Brightness:', jacketState.brightness);
        // update value
        writeCharacteristicValue(bleBrightnessCharacteristic, jacketState.brightness);
    });

    // Adjust frequency
    frequencySlider.addEventListener('input', function () {
        jacketState.frequency = frequencySlider.value;
        console.log('Frequency:', jacketState.frequency);
        // update value
        writeCharacteristicValue(bleFrequencyCharacteristic, jacketState.frequency);
    });

    // Pick color
    colorPicker.addEventListener('input', function () {
        jacketState.color = colorPicker.value;
        console.log('Color:', jacketState.color);
        // update value, keep only the last 6 characters
        var color_r = parseInt(jacketState.color.substring(1, 3), 16);
        var color_g = parseInt(jacketState.color.substring(3, 5), 16);
        var color_b = parseInt(jacketState.color.substring(5, 7), 16);
        console.log('Color R:', color_r);
        console.log('Color G:', color_g);
        console.log('Color B:', color_b);
        writeCharacteristicValue(bleColorRCharacteristic, color_r);
        writeCharacteristicValue(bleColorGCharacteristic, color_g);
        writeCharacteristicValue(bleColorBCharacteristic, color_b);
    });


    // Handle characteristic notifications
    function handleCharacteristicValueChanged(event) {
        const characteristic = event.currentTarget;
        const value = event.target.value;
        const textDecoder = new TextDecoder();
        const decodedValue = textDecoder.decode(value);

        if (debug) {
            console.log('Received data for characteristic:', characteristic.uuid);
            console.log('Received data:', event.target.value);
            console.log('Received characteristic value:', decodedValue);
        }


        // update jacket state
        switch (characteristic.uuid) {
            case mode_characteristicUUID:
                jacketState.mode = decodedValue;
                break;
            case brightness_characteristicUUID:
                jacketState.brightness = decodedValue;
                break;
            case frequency_characteristicUUID:
                jacketState.frequency = decodedValue;
                break;
            // case color_characteristicUUID:
            //     // format color value
            //     jacketState.color = '#' + decodedValue;

            //     break;
            case power_characteristicUUID:
                jacketState.power = decodedValue;
                powerButton.textContent = jacketState.power ? 'Power On' : 'Power Off';
                powerButton.classList.toggle('on', jacketState.power);
                powerButton.classList.toggle('off', !jacketState.power);
                break;
        }

        // update UI
        tiles.forEach(t => t.classList.remove('selected'));
        tiles.forEach(t => {
            if (t.dataset.mode === jacketState.mode) {
                t.classList.add('selected');
            }
        });
        brightnessSlider.value = jacketState.brightness;
        frequencySlider.value = jacketState.frequency;
        colorPicker.value = jacketState.color;
        powerButton.textContent = jacketState.power ? 'Power On' : 'Power Off';
        powerButton.classList.toggle('on', jacketState.power);
        powerButton.classList.toggle('off', !jacketState.power);

    }

    // write characteristic value
    function writeCharacteristicValue(characteristic, value) {
        if (bleServer && bleService && characteristic) {
            //data = new Uint8Array([value]);
            const textEncoder = new TextEncoder();
            const data = textEncoder.encode(value.toString());
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
        // set connected on button
        connectButton.classList.remove('connected');
        connectButton.classList.add('disconnected');
        connectButton.textContent = 'Connect';
        
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
                    service.getCharacteristic(power_characteristicUUID),
                    service.getCharacteristic(color_r_characteristicUUID),
                    service.getCharacteristic(color_g_characteristicUUID),
                    service.getCharacteristic(color_b_characteristicUUID),

                ]);
            })
            .then(characteristics => {
                console.log('Connected to characteristics:', characteristics);
                bleModeCharacteristic = characteristics[0];
                bleBrightnessCharacteristic = characteristics[1];
                bleFrequencyCharacteristic = characteristics[2];
                blePowerCharacteristic = characteristics[3];
                bleColorRCharacteristic = characteristics[4];
                bleColorGCharacteristic = characteristics[5];
                bleColorBCharacteristic = characteristics[6];


                // Listen for characteristic value changes
                bleModeCharacteristic.addEventListener('characteristicvaluechanged', handleCharacteristicValueChanged);
                bleBrightnessCharacteristic.addEventListener('characteristicvaluechanged', handleCharacteristicValueChanged);
                bleFrequencyCharacteristic.addEventListener('characteristicvaluechanged', handleCharacteristicValueChanged);
                bleColorRCharacteristic.addEventListener('characteristicvaluechanged', handleCharacteristicValueChanged);
                bleColorGCharacteristic.addEventListener('characteristicvaluechanged', handleCharacteristicValueChanged);
                bleColorBCharacteristic.addEventListener('characteristicvaluechanged', handleCharacteristicValueChanged);
                blePowerCharacteristic.addEventListener('characteristicvaluechanged', handleCharacteristicValueChanged);

                // Enable notifications
                bleModeCharacteristic.startNotifications();
                bleBrightnessCharacteristic.startNotifications();
                bleFrequencyCharacteristic.startNotifications();
                bleColorRCharacteristic.startNotifications();
                bleColorGCharacteristic.startNotifications();
                bleColorBCharacteristic.startNotifications();
                blePowerCharacteristic.startNotifications();

                console.log('Notifications enabled');

                return {
                    mode: characteristics[0],
                    brightness: characteristics[1],
                    frequency: characteristics[2],
                    power: characteristics[3],
                    color_r: characteristics[4],
                    color_g: characteristics[5],
                    color_b: characteristics[6]
                };
            });
    }
});