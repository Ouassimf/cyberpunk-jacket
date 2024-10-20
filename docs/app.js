document.addEventListener('DOMContentLoaded', function() {
    const powerButton = document.getElementById('power-button');
    const tiles = document.querySelectorAll('.tile');
    const brightnessSlider = document.getElementById('brightness');
    const frequencySlider = document.getElementById('frequency');
    const colorPicker = document.getElementById('color-picker');

    let jacketState = {
        power: false,
        mode: null,
        brightness: brightnessSlider.value,
        frequency: frequencySlider.value,
        color: colorPicker.value
    };

    // Toggle power state
    powerButton.addEventListener('click', function() {
        jacketState.power = !jacketState.power;
        powerButton.textContent = jacketState.power ? 'Power On' : 'Power Off';
        powerButton.classList.toggle('on', jacketState.power);
        powerButton.classList.toggle('off', !jacketState.power);
        console.log('Power:', jacketState.power);
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
});