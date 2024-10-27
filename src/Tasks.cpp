#include "Tasks.h"
#include "LEDController.h"

TaskHandle_t FastLEDTaskHandle;

void FastLEDTask(void *pvParameters) {
    while (true) {
        executePattern();
        vTaskDelay(5 / portTICK_PERIOD_MS);
    }
}

void startFastLEDTask() {
    xTaskCreatePinnedToCore(
        FastLEDTask,
        "FastLEDTask",
        10000,
        NULL,
        4,
        &FastLEDTaskHandle,
        0
    );
}
