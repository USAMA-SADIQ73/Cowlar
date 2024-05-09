#include <stdio.h>
#include "esp32_mqtt_mesh.h"
/* Mesh Internal Communication Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <inttypes.h>
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"

#include "nvs_flash.h"
#include "mesh_netif.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "sdkconfig.h"
/*******************************************************
 *                Macros
 *******************************************************/

// commands for internal mesh communication:
// <CMD> <PAYLOAD>, where CMD is one character, payload is variable dep. on command
// CMD_KEYPRESSED: payload is always 6 bytes identifying address of node sending keypress event
#define CMD_ROUTE_TABLE 0x55
// CMD_KEYPRESSED: payload is a multiple of 6 listing addresses in a routing table
#define CMD_Data 0x56
/*******************************************************
 *                Constants
 *******************************************************/
static const char *MESH_TAG = "mesh_main";
static const uint8_t MESH_ID[6] = { 0x77, 0x77, 0x77, 0x77, 0x77, 0x76};

#define RX_SIZE          (1500)
#define TX_SIZE          (1460)

/*******************************************************
 *                Variable Definitions
 *******************************************************/
static bool is_running = true;
static mesh_addr_t mesh_parent_addr;
static int mesh_layer = -1;
static esp_ip4_addr_t s_current_ip;
static mesh_addr_t s_route_table[CONFIG_MESH_ROUTE_TABLE_SIZE];
static int s_route_table_size = 0;
static SemaphoreHandle_t s_route_table_lock = NULL;
static uint8_t s_mesh_tx_payload[CONFIG_MESH_ROUTE_TABLE_SIZE*6+1];

int node_route_table_size = 1;
/*******************************************************
 *                Function Declarations
 *******************************************************/
// interaction with public mqtt broker
void mqtt_app_start(void);


/*******************************************************
 *                Function Definitions
 *******************************************************/

void static recv_cb(mesh_addr_t *from, mesh_data_t *data)
{
    if (data->data[0] == CMD_ROUTE_TABLE) {
        int size =  data->size - 1;
        if (s_route_table_lock == NULL || size%6 != 0) {
            ESP_LOGE(MESH_TAG, "Error in receiving raw mesh data: Unexpected size");
            return;
        }
        xSemaphoreTake(s_route_table_lock, portMAX_DELAY);
        s_route_table_size = size / 6;
        for (int i=0; i < s_route_table_size; ++i) {
            ESP_LOGI(MESH_TAG, "Received Routing table [%d] "
                    MACSTR, i, MAC2STR(data->data + 6*i + 1));
        }
        memcpy(&s_route_table, data->data + 1, size);
        xSemaphoreGive(s_route_table_lock);
    } 
    else if (data->data[0] == CMD_Data) {

        char addr_str[18]; // Buffer to hold the string representation of the address
        sprintf(addr_str, MACSTR, MAC2STR(from->addr)); // Convert the address to a string
        printf("Received data: %.*s from node: %s\n", data->size - 1, ((char*)data->data) + 1, addr_str); // Print the data and the address
    }
    else {
        
        ESP_LOGE(MESH_TAG, "Error in receiving raw mesh data: Unknown command");
    }
}


mesh_addr_t* get_routing_table() {
    static mesh_addr_t route_table[CONFIG_MESH_ROUTE_TABLE_SIZE + 1]; // +1 for the parent node's address
   // Get the parent node's address
    esp_mesh_get_parent_bssid(&route_table[0]); // Store the parent node's address at the top index
    // Get the routing table
    esp_mesh_get_routing_table(&route_table[1], CONFIG_MESH_ROUTE_TABLE_SIZE * 6, &node_route_table_size); // Start storing at index 1

    return route_table;
}

int NodeRoutingTableSize(){
    return node_route_table_size;
}

esp_err_t send_data_to_node(mesh_addr_t *to, const char *data_str) {
    mesh_data_t data;
    data.size = strlen(data_str) + 1 + sizeof(CMD_Data); // Size is length of string plus null terminator plus size of CMD_Data
    data.proto = MESH_PROTO_BIN;
    data.tos = MESH_TOS_P2P;
    s_mesh_tx_payload[0] = CMD_Data; // Include CMD_Data in the payload
    memcpy(s_mesh_tx_payload + 1, data_str, strlen(data_str)); // Copy data string to payload
    data.data = s_mesh_tx_payload;
    esp_err_t err = esp_mesh_send(to, &data, MESH_DATA_P2P, NULL, 0);
    return err;
}

esp_err_t get_current_node_address(uint8_t* mac) {
    // Get the MAC address of the station interface
    return esp_wifi_get_mac(WIFI_IF_STA, mac);
}

void print_node_info() {
    // Get and print the layer of the node
    int layer = esp_mesh_get_layer();
    printf("Node layer: %d\n", layer);

    // Check if the node is a root node
    bool is_root = esp_mesh_is_root();
    printf("Is root: %s\n", is_root ? "Yes" : "No");

    // Get and print the parent node's address
    mesh_addr_t parent_addr;
    esp_mesh_get_parent_bssid(&parent_addr);
    printf("Parent node: " MACSTR "\n", MAC2STR(parent_addr.addr));

    // Get and print the IP address of the node
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        printf("Node IP: " IPSTR "\n", IP2STR(&ip_info.ip));
    }

    // Get and print the routing table
    mesh_addr_t route_table[CONFIG_MESH_ROUTE_TABLE_SIZE];
    int route_table_size = 0;
    esp_mesh_get_routing_table(route_table, CONFIG_MESH_ROUTE_TABLE_SIZE * 6, &route_table_size);
    printf("Connected child nodes: %d\n", route_table_size-1);

    if(is_root){printf("Root Node address %d: " MACSTR "\n",0, MAC2STR(route_table[0].addr));}
    else{printf("Current Node address %d: " MACSTR "\n", 0, MAC2STR(route_table[0].addr));}

    for (int i = 1; i < route_table_size; i++) {
        printf("Child node address %d: " MACSTR "\n", i, MAC2STR(route_table[i].addr));
    }
}

esp_err_t send_routing_table_to_nodes() {
    mesh_data_t data;
    esp_err_t err;
    mesh_addr_t route_table[CONFIG_MESH_ROUTE_TABLE_SIZE];
    int route_table_size = 0;

    // Get the routing table
    esp_mesh_get_routing_table(route_table, CONFIG_MESH_ROUTE_TABLE_SIZE * 6, &route_table_size);

    // Prepare the data to be sent
    data.size = route_table_size * 6 + 1;
    data.proto = MESH_PROTO_BIN;
    data.tos = MESH_TOS_P2P;
    uint8_t payload[route_table_size * 6 + 1];
    payload[0] = CMD_ROUTE_TABLE;
    memcpy(payload + 1, route_table, route_table_size * 6);
    data.data = payload;

    // Send the routing table to all nodes in the network
    for (int i = 1; i < route_table_size; i++) {
        err = esp_mesh_send(&route_table[i], &data, MESH_DATA_P2P, NULL, 0);
        if (err != ESP_OK) {
            printf("Failed to send routing table to node %d: %d\n", i, err);
            return err;
        }
    }

    return ESP_OK;
}

void esp_mesh_mqtt_task(void *arg)
{

    is_running = true;
    esp_err_t err;
    mqtt_app_start();
    int count = 0;
    int rcount = 0;
    uint8_t current_node_mac[6]; // Buffer to hold the MAC address
    char addr_str[18]; // Buffer to hold the string representation of the address
    char count_str[50]; // Buffer to hold count string
    while (is_running) {
        
        if (esp_mesh_is_root()) {
            print_node_info();
            err = send_routing_table_to_nodes();
            if (err != ESP_OK) {
                printf("Failed to send routing table: %d\n", err);
            }
            sprintf(count_str, "Root count %d", rcount++); // Convert count to string
                
                mesh_addr_t *route_table = get_routing_table();
               //  for (int i = 0; i < node_route_table_size + 1; i++) {
                       // printf("Root Routing table [%d] " MACSTR "\n", i, MAC2STR(route_table[i].addr));
                        
                        err = send_data_to_node(&route_table[2], count_str);
                        if (err != ESP_OK) {
                            printf("Failed to send data: %d\n", err);
                        }
                        else {
                            printf("Data Send to" MACSTR "\n", MAC2STR(route_table[2].addr));
                        }
                  //  }
                

                // Publish count data to MQTT
                
                get_current_node_address(current_node_mac); // Get the MAC address of the node

                
                sprintf(addr_str, MACSTR, MAC2STR(current_node_mac)); // Convert the address to a string
                sprintf(count_str, "count %d from node: %s", rcount, addr_str); // Include the address in the count string
                // Publish count data to MQTT
                mqtt_app_publish("Root_data", count_str);

                
                // mesh_addr_t *route_table = get_routing_table();
                // for (int i = 0; i < node_route_table_size + 1; i++) {
                //         printf("Root Routing table [%d] " MACSTR "\n", i, MAC2STR(route_table[i].addr));
                //     }
        }
        else {
                print_node_info();
                

                sprintf(count_str, "Node count %d", count++); // Convert count to string

                err = send_data_to_node(&mesh_parent_addr, count_str);
                if (err != ESP_OK) {
                    printf("Failed to send data: %d\n", err);
                }
                
                uint8_t current_node_mac[6]; // Buffer to hold the MAC address
                get_current_node_address(current_node_mac); // Get the MAC address of the node

                
                sprintf(addr_str, MACSTR, MAC2STR(current_node_mac)); // Convert the address to a string
                sprintf(count_str, "count %d from node: %s", count, addr_str); // Include the address in the count string
                // Publish count data to MQTT
                mqtt_app_publish("Node_data4", count_str);
            }
        vTaskDelay(2 * 1000 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}




esp_err_t esp_mesh_comm_mqtt_task_start(void)
{
    static bool is_comm_mqtt_task_started = false;

    s_route_table_lock = xSemaphoreCreateMutex();

    if (!is_comm_mqtt_task_started) {
        xTaskCreate(esp_mesh_mqtt_task, "mqtt task", 3072, NULL, 5, NULL);

        is_comm_mqtt_task_started = true;
    }
    printf("Task started\n");
    return ESP_OK;
}

void mesh_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    mesh_addr_t id = {0,};
    static uint8_t last_layer = 0;

    switch (event_id) {
    case MESH_EVENT_STARTED: {
        esp_mesh_get_id(&id);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_MESH_STARTED>ID:"MACSTR"", MAC2STR(id.addr));
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_STOPPED: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOPPED>");
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *child_connected = (mesh_event_child_connected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_CONNECTED>aid:%d, "MACSTR"",
                 child_connected->aid,
                 MAC2STR(child_connected->mac));
    }
    break;
    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *child_disconnected = (mesh_event_child_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_DISCONNECTED>aid:%d, "MACSTR"",
                 child_disconnected->aid,
                 MAC2STR(child_disconnected->mac));
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_ADD: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_ADD>add %d, new:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new);
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_REMOVE: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_REMOVE>remove %d, new:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new);
    }
    break;
    case MESH_EVENT_NO_PARENT_FOUND: {
        mesh_event_no_parent_found_t *no_parent = (mesh_event_no_parent_found_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NO_PARENT_FOUND>scan times:%d",
                 no_parent->scan_times);
    }
    /* TODO handler for the failure */
    break;
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
        esp_mesh_get_id(&id);
        mesh_layer = connected->self_layer;
        memcpy(&mesh_parent_addr.addr, connected->connected.bssid, 6);
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_CONNECTED>layer:%d-->%d, parent:"MACSTR"%s, ID:"MACSTR"",
                 last_layer, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "", MAC2STR(id.addr));
        last_layer = mesh_layer;
        mesh_netifs_start(esp_mesh_is_root());
    }
    break;
    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_DISCONNECTED>reason:%d",
                 disconnected->reason);
        mesh_layer = esp_mesh_get_layer();
        mesh_netifs_stop();
    }
    break;
    case MESH_EVENT_LAYER_CHANGE: {
        mesh_event_layer_change_t *layer_change = (mesh_event_layer_change_t *)event_data;
        mesh_layer = layer_change->new_layer;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_LAYER_CHANGE>layer:%d-->%d%s",
                 last_layer, mesh_layer,
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "");
        last_layer = mesh_layer;
    }
    break;
    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_ADDRESS>root address:"MACSTR"",
                 MAC2STR(root_addr->addr));
    }
    break;
    case MESH_EVENT_VOTE_STARTED: {
        mesh_event_vote_started_t *vote_started = (mesh_event_vote_started_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_VOTE_STARTED>attempts:%d, reason:%d, rc_addr:"MACSTR"",
                 vote_started->attempts,
                 vote_started->reason,
                 MAC2STR(vote_started->rc_addr.addr));
    }
    break;
    case MESH_EVENT_VOTE_STOPPED: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_VOTE_STOPPED>");
        break;
    }
    case MESH_EVENT_ROOT_SWITCH_REQ: {
        mesh_event_root_switch_req_t *switch_req = (mesh_event_root_switch_req_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_SWITCH_REQ>reason:%d, rc_addr:"MACSTR"",
                 switch_req->reason,
                 MAC2STR( switch_req->rc_addr.addr));
    }
    break;
    case MESH_EVENT_ROOT_SWITCH_ACK: {
        /* new root */
        mesh_layer = esp_mesh_get_layer();
        esp_mesh_get_parent_bssid(&mesh_parent_addr);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_SWITCH_ACK>layer:%d, parent:"MACSTR"", mesh_layer, MAC2STR(mesh_parent_addr.addr));
    }
    break;
    case MESH_EVENT_TODS_STATE: {
        mesh_event_toDS_state_t *toDs_state = (mesh_event_toDS_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_TODS_REACHABLE>state:%d", *toDs_state);
    }
    break;
    case MESH_EVENT_ROOT_FIXED: {
        mesh_event_root_fixed_t *root_fixed = (mesh_event_root_fixed_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_FIXED>%s",
                 root_fixed->is_fixed ? "fixed" : "not fixed");
    }
    break;
    case MESH_EVENT_ROOT_ASKED_YIELD: {
        mesh_event_root_conflict_t *root_conflict = (mesh_event_root_conflict_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_ASKED_YIELD>"MACSTR", rssi:%d, capacity:%d",
                 MAC2STR(root_conflict->addr),
                 root_conflict->rssi,
                 root_conflict->capacity);
    }
    break;
    case MESH_EVENT_CHANNEL_SWITCH: {
        mesh_event_channel_switch_t *channel_switch = (mesh_event_channel_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHANNEL_SWITCH>new channel:%d", channel_switch->channel);
    }
    break;
    case MESH_EVENT_SCAN_DONE: {
        mesh_event_scan_done_t *scan_done = (mesh_event_scan_done_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_SCAN_DONE>number:%d",
                 scan_done->number);
    }
    break;
    case MESH_EVENT_NETWORK_STATE: {
        mesh_event_network_state_t *network_state = (mesh_event_network_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NETWORK_STATE>is_rootless:%d",
                 network_state->is_rootless);
    }
    break;
    case MESH_EVENT_STOP_RECONNECTION: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOP_RECONNECTION>");
    }
    break;
    case MESH_EVENT_FIND_NETWORK: {
        mesh_event_find_network_t *find_network = (mesh_event_find_network_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_FIND_NETWORK>new channel:%d, router BSSID:"MACSTR"",
                 find_network->channel, MAC2STR(find_network->router_bssid));
    }
    break;
    case MESH_EVENT_ROUTER_SWITCH: {
        mesh_event_router_switch_t *router_switch = (mesh_event_router_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROUTER_SWITCH>new router:%s, channel:%d, "MACSTR"",
                 router_switch->ssid, router_switch->channel, MAC2STR(router_switch->bssid));
    }
    break;
    default:
        ESP_LOGI(MESH_TAG, "unknown id:%" PRId32 "", event_id);
        break;
    }
}

void ip_event_handler(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI(MESH_TAG, "<IP_EVENT_STA_GOT_IP>IP:" IPSTR, IP2STR(&event->ip_info.ip));
    s_current_ip.addr = event->ip_info.ip.addr;
#if !CONFIG_MESH_USE_GLOBAL_DNS_IP
    esp_netif_t *netif = event->esp_netif;
    esp_netif_dns_info_t dns;
    ESP_ERROR_CHECK(esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));
    mesh_netif_start_root_ap(esp_mesh_is_root(), dns.ip.u_addr.ip4.addr);
#endif
    esp_mesh_comm_mqtt_task_start();
}


void initilize_mest_mqtt(void)
{
   ESP_ERROR_CHECK(nvs_flash_init());
    /*  tcpip initialization */
    ESP_ERROR_CHECK(esp_netif_init());
    /*  event initialization */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /*  crete network interfaces for mesh (only station instance saved for further manipulation, soft AP instance ignored */
    ESP_ERROR_CHECK(mesh_netifs_init(recv_cb));

    /*  wifi initialization */
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    /*  mesh initialization */
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(1));
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(10));
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    /* mesh ID */
    memcpy((uint8_t *) &cfg.mesh_id, MESH_ID, 6);
    /* router */
    cfg.channel = CONFIG_MESH_CHANNEL;
    cfg.router.ssid_len = strlen(CONFIG_MESH_ROUTER_SSID);
    memcpy((uint8_t *) &cfg.router.ssid, CONFIG_MESH_ROUTER_SSID, cfg.router.ssid_len);
    memcpy((uint8_t *) &cfg.router.password, CONFIG_MESH_ROUTER_PASSWD,
           strlen(CONFIG_MESH_ROUTER_PASSWD));
    /* mesh softAP */
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(CONFIG_MESH_AP_AUTHMODE));
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    cfg.mesh_ap.nonmesh_max_connection = CONFIG_MESH_NON_MESH_AP_CONNECTIONS;
    memcpy((uint8_t *) &cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWD,
           strlen(CONFIG_MESH_AP_PASSWD));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    /* mesh start */
    ESP_ERROR_CHECK(esp_mesh_start());
    ESP_LOGI(MESH_TAG, "mesh starts successfully, heap:%" PRId32 ", %s\n",  esp_get_free_heap_size(),
             esp_mesh_is_root_fixed() ? "root fixed" : "root not fixed");
    printf("main ended\n");
}

