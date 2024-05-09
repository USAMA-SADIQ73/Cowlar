#include "esp_mesh.h"


void mqtt_app_publish(char* topic, char *publish_string);

mesh_addr_t* get_routing_table();

esp_err_t send_data_to_node(mesh_addr_t *to, const char *data_str);
esp_err_t get_current_node_address(uint8_t* mac);

esp_err_t send_routing_table_to_nodes();
int NodeRoutingTableSize();
void print_node_info();
void initilize_mest_mqtt(void);