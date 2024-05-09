
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp32_mqtt_mesh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"





void mqtt_mesh_node_tasks(void *pvParameters)
{
    char nodedata[150]; // Increased size to accommodate MAC address
    uint8_t mac[6];
    bool nroot = false;

    while(1)
    {   
        nroot = esp_mesh_is_root();
        if(!nroot)
        {
            get_current_node_address(mac);
            sprintf(nodedata, "From Node %02x:%02x:%02x:%02x:%02x:%02x Hello to Mqtt broker %ld ", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], esp_random() % 100);
            mqtt_app_publish("/Nodes/",nodedata);
            printf("Node Mqtt Data send\n");
            vTaskDelay(1500 / portTICK_PERIOD_MS);
        }
    }
}

void mqtt_mesh_Root_tasks(void *pvParameters)
{
    char rootdata[150]; // Increased size to accommodate MAC address
    bool root = false;
    uint8_t rmac[6];

    while(1)
    {   
        root = esp_mesh_is_root();
        if(root)
        {
            get_current_node_address(rmac);
            printf("i am root %d\n",root);
            sprintf(rootdata, "from Root %02x:%02x:%02x:%02x:%02x:%02x Hello to Mqtt broker %ld ",  rmac[0], rmac[1], rmac[2], rmac[3], rmac[4], rmac[5], esp_random() % 100);
            printf("Root Mqtt Data send\n");
            mqtt_app_publish("/Root/",rootdata);
        }
        vTaskDelay(1500 / portTICK_PERIOD_MS);
    }
}


void app_main(void)
{    
    initilize_mest_mqtt();

    
    xTaskCreate(mqtt_mesh_Root_tasks, "mqtt_mesh_Root_tasks", 4096, NULL, 5, NULL);
    xTaskCreate(mqtt_mesh_node_tasks, "mqtt_mesh_node_tasks", 4096, NULL, 5, NULL);
    
    
    

    
}
