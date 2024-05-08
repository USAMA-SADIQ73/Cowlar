
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp32_mqtt_mesh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"





void app_main(void)
{    
    initilize_mest_mqtt();
    printf("main ended\n");
    while(1)
    {
        printf("main loop\n");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    
}
