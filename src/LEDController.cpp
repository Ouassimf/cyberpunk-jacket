#include "LEDController.h"

CRGB leds_R[NUM_LEDS];
CRGB leds_L[NUM_LEDS];

int matrixCount = 10;

void setupLEDController()
{
    FastLED.addLeds<LED_TYPE, DATA_PIN_1, COLOR_ORDER>(leds_L, NUM_LEDS);
    FastLED.addLeds<LED_TYPE, DATA_PIN_2, COLOR_ORDER>(leds_R, NUM_LEDS);
    FastLED.setBrightness(brightness);
}

void beat()
{
    static uint16_t currentLed = 0;
    static unsigned long nextUpdate = 0;

    if (millis() > nextUpdate)
    {
        leds_L[currentLed].r = color_r;
        leds_L[currentLed].g = color_g;
        leds_L[currentLed].b = color_b;

        leds_R[currentLed].r = color_r;
        leds_R[currentLed].g = color_g;
        leds_R[currentLed].b = color_b;
        FastLED.show();
        fadeToBlackBy(leds_L, NUM_LEDS, 20);
        fadeToBlackBy(leds_R, NUM_LEDS, 20);
        nextUpdate = millis() + frequency;
        currentLed = (currentLed + 1) % NUM_LEDS;
    }
}

void rainbow()
{
    static uint16_t currentLed = 0;
    static unsigned long nextUpdate = 0;

    // Check if time elapsed is greater than delayTime
    if (millis() > nextUpdate)
    {
        currentLed = currentLed > NUM_LEDS ? 0 : currentLed + 1;
        // Do something
        fill_rainbow(leds_L, NUM_LEDS, int(currentLed * 255 / NUM_LEDS), 255 / NUM_LEDS);
        fill_rainbow(leds_R, NUM_LEDS, int(currentLed * 255 / NUM_LEDS), 255 / NUM_LEDS);
        FastLED.show();
        // Update nextUpdate
        nextUpdate = millis() + frequency;
    }
}

void randomSparks()
{

    CRGB green = CHSV(HUE_GREEN, 255, 255);
    CRGB white = CRGB::White;
    CRGB green1 = CRGB(0, 255, 38);
    CRGB green2 = CRGB(131, 247, 156);
    CRGB green3 = CRGB(0, 82, 29);
    CRGB green4 = CRGB(6, 147, 123);
    static unsigned long nextUpdate = 0;
    static TBlendType currentBlending = LINEARBLEND;
    static CRGBPalette16 currentPalette = CRGBPalette16(
        green, green1, green2, green3,
        green4, white, white, green,
        green, green1, green2, green3,
        green4, white, white, green

    );
    // set brightness
    FastLED.setBrightness(brightness);
    if (millis() > nextUpdate)
    {
        // Generate N random numbers between 0 and NUM_LEDS
        for (int i = 0; i < matrixCount; i++)
        {
            int randomLed = random(0, NUM_LEDS);
            // generate a random brightness
            int brightness = random(0, 255);
            int randomColor = random(0, 255);
            // assign the random brightness to the color green
            leds_L[randomLed] = ColorFromPalette(currentPalette, randomColor, brightness, currentBlending);
            leds_R[randomLed] = ColorFromPalette(currentPalette, randomColor, brightness, currentBlending);
        }
        fadeToBlackBy(leds_L, NUM_LEDS, 64);
        fadeToBlackBy(leds_R, NUM_LEDS, 64);
        FastLED.show();
        nextUpdate = millis() + frequency;
    }
}

void breathe()
{
    static unsigned long nextUpdate = 0;
    static uint8_t brightness = 0;
    static int8_t delta = 5;

    // set color

    if (millis() > nextUpdate)
    {
        brightness += delta;
        if (brightness == 0 || brightness == 255)
        {
            delta = -delta;
        }
        fill_solid(leds_L, NUM_LEDS, CRGB(color_r, color_g, color_b));
        fill_solid(leds_R, NUM_LEDS, CRGB(color_r, color_g, color_b));

        FastLED.setBrightness(brightness);
        FastLED.show();
        nextUpdate = millis() + frequency;
    }
}

void v_path()
{
    static unsigned long nextUpdate = 0;

    // set color

    if (millis() > nextUpdate)
    {

        // set first 40% of leds to black and rest to color
        for (int i = 0; i < NUM_LEDS; i++)
        {
            if (i < NUM_LEDS * 0.6)
            {
                leds_L[i] = CRGB(color_r, color_g, color_b);
                leds_R[i] = CRGB(color_r, color_g, color_b);
            }
            else
            {
                leds_L[i] = CRGB::Black;
                leds_R[i] = CRGB::Black;
            }
        }

        FastLED.setBrightness(brightness);
        FastLED.show();
        nextUpdate = millis() + frequency;
    }
}

void cylon()
{
    static unsigned long nextUpdate = 0;
    static int currentLed = 0;
    static int direction = 1;

    if (millis() > nextUpdate)
    {
        leds_L[currentLed] = CRGB(color_r, color_g, color_b);
        leds_R[currentLed] = CRGB(color_r, color_g, color_b);
        FastLED.show();
        leds_L[currentLed] = CRGB::Black;
        leds_R[currentLed] = CRGB::Black;
        currentLed += direction;
        if (currentLed == NUM_LEDS - 1 || currentLed == 0)
        {
            direction = -direction;
        }
        nextUpdate = millis() + frequency;
    }
}

void heartbeat()
{
    static unsigned long nextUpdate = 0;
    static uint8_t fadeAmount = 60;  // Controls fade speed
    int start1 = 33;       // 25% point of each strip
    int start2 = 121; // 75% point of each strip
    static int offset = 0;

    if (millis() > nextUpdate)
    {
        // Set brightness for the heartbeat pulse
        FastLED.setBrightness(brightness);

        // 25% starting point expansion
        if (start1 - offset >= 0)
        {
            leds_L[start1 - offset] = CRGB(color_r, color_g, color_b);
            leds_L[start1 + offset] = CRGB(color_r, color_g, color_b);
            leds_R[start1 - offset] = CRGB(color_r, color_g, color_b);
            leds_R[start1 + offset] = CRGB(color_r, color_g, color_b);
        }
        else
        {
            offset = 0;
        }

        // 75% starting point expansion
        if (start2 - offset >= start1 && start2 + offset < NUM_LEDS)
        {
            leds_L[start2 - offset] = CRGB(color_r, color_g, color_b);
            leds_L[start2 + offset] = CRGB(color_r, color_g, color_b);
            leds_R[start2 - offset] = CRGB(color_r, color_g, color_b);
            leds_R[start2 + offset] = CRGB(color_r, color_g, color_b);
        }
        else
        {
            //offset = 0;
        }

        offset++;

        // Fade out the LEDs to create the heartbeat's fading effect
        fadeToBlackBy(leds_L, NUM_LEDS, fadeAmount);
        fadeToBlackBy(leds_R, NUM_LEDS, fadeAmount);

        FastLED.show();
        // Set the time for the next heartbeat update
        nextUpdate = millis() + frequency;
    }
}

void off()
{
    static unsigned long nextUpdate = 0;
    if (millis() > nextUpdate)
    {
        fill_solid(leds_L, NUM_LEDS, CRGB::Black);
        fill_solid(leds_R, NUM_LEDS, CRGB::Black);
        FastLED.show();
        nextUpdate = millis() + frequency;
    }
}

void executePattern()
{
    // if power is off, turn off the leds
    if (power == 0)
    {
        off();
        return;
    }

    switch (mode)
    {
    case 1:
        beat();
        break;
    case 2:
        rainbow();
        break;
    case 3:
        randomSparks();
        break;
    case 4:
        breathe();
        break;
    case 5:
        heartbeat();
        break;
    case 6:
        cylon();
        break;
    default:
        cylon();
    }

    // give sometime to the core
    delay(2);
}
