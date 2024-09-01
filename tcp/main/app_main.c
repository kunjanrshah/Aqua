/* MQTT (over TCP) Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include <stdlib.h>
#include "freertos/event_groups.h"
#include "esp_wpa2.h"
#include "esp_smartconfig.h"

#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "esp_spiffs.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "freertos/queue.h"

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
static const int CONNECTED_BIT = BIT0;
static const int ESPTOUCH_DONE_BIT = BIT1;

static void smartconfig_example_task(void *parm);
static void mqtt_app_start(void);

static const char *TAG = "MQTT_EXAMPLE";

static int s_retry_num = 0;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1


#define CONFIG_EXAMPLE_UART_PORT_NUM 2
#define CONFIG_EXAMPLE_UART_BAUD_RATE  115200//9600
#define CONFIG_EXAMPLE_UART_RXD 16
#define CONFIG_EXAMPLE_UART_TXD 17
#define CONFIG_EXAMPLE_TASK_STACK_SIZE 4096



#define ECHO_TEST_TXD (CONFIG_EXAMPLE_UART_TXD)
#define ECHO_TEST_RXD (CONFIG_EXAMPLE_UART_RXD)
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)

#define ECHO_UART_PORT_NUM      (CONFIG_EXAMPLE_UART_PORT_NUM)
//#define ECHO_UART_BAUD_RATE     (CONFIG_EXAMPLE_UART_BAUD_RATE)
#define ECHO_TASK_STACK_SIZE    (CONFIG_EXAMPLE_TASK_STACK_SIZE)

//static const char *TAG = "UART TEST";

#define BUF_SIZE (1024)

uint8_t *data_new = NULL;

static void echo_task(void *arg)
{
    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    int intr_alloc_flags = 0;

#if CONFIG_UART_ISR_IN_IRAM
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

    ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, ECHO_TEST_TXD, ECHO_TEST_RXD, ECHO_TEST_RTS, ECHO_TEST_CTS));
    // Configure a temporary buffer for the incoming data
 
    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);
    ESP_LOGI(TAG, "echo_task");
    while (1) {
        // Read data from the UART
        int len = uart_read_bytes(ECHO_UART_PORT_NUM, data, (BUF_SIZE - 1), 20 / portTICK_RATE_MS);
        // Write data back to the UART
        
        data_new=data;
        uart_write_bytes(ECHO_UART_PORT_NUM, (const char *) data_new, len);
        
        if (len) {
            data[len] = '\0';
            ESP_LOGI(TAG, "Recv str: %s", (char *) data);
        }
    }
}

static void reset_password(void *pvParameters)
{
     gpio_set_direction(GPIO_NUM_23, GPIO_MODE_INPUT);
     gpio_set_pull_mode(GPIO_NUM_23, GPIO_PULLUP_ONLY);

    
    while (1)
    {
        int reset_key_press = gpio_get_level(GPIO_NUM_23);
        
        if (reset_key_press == 0) // 1 is pressed
        {
            printf("Reset key pressed\n");
            struct stat st;
            if (stat("/spiffs/aqua_ssid.txt", &st) == 0)
            {
                // Delete it if it exists
                unlink("/spiffs/aqua_ssid.txt");
                unlink("/spiffs/aqua_password.txt");
                // FILE *f = fopen("/spiffs/aqua_ssid.txt", "r");
                // FILE *f1 = fopen("/spiffs/aqua_password.txt", "r");

            }
            esp_restart();
        }
        else
        {

           // printf("Reset key not pressed\n");
        }
        //vTaskDelay(5000 / portTICK_PERIOD_MS);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    vTaskDelete(NULL);
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {

        if (s_retry_num < 10)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");

                gpio_set_level(GPIO_NUM_2, 0);  //RED LED OFF

    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;

        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "wifi_init finished.");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        gpio_set_level(GPIO_NUM_2, 1);  //RED LED ON

        mqtt_app_start();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac), event->aid);
        //  xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*)AF_INET, 5, NULL);
    }
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac), event->aid);
    }
}

static void event_handler_smartconfig(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        xTaskCreate(smartconfig_example_task, "smartconfig_example_task", 4096, NULL, 3, NULL);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE)
    {
        ESP_LOGI(TAG, "Scan done");
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL)
    {
        ESP_LOGI(TAG, "Found channel");
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD)
    {
        ESP_LOGI(TAG, "Got SSID and password");

        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
        wifi_config_t wifi_config;
        uint8_t ssid[33] = {0};
        uint8_t password[65] = {0};

        bzero(&wifi_config, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
        wifi_config.sta.bssid_set = evt->bssid_set;
        if (wifi_config.sta.bssid_set == true)
        {
            memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
        }

        memcpy(ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(password, evt->password, sizeof(evt->password));
        ESP_LOGI(TAG, "SSID:%s", ssid);
        ESP_LOGI(TAG, "PASSWORD:%s", password);

        ESP_LOGI(TAG, "Opening file");
        FILE *f = fopen("/spiffs/aqua_ssid.txt", "w");
        if (f == NULL)
        {
            ESP_LOGE(TAG, "Failed to open file for writing");
            return;
        }
        else
        {
            ESP_LOGI(TAG, "SSID in aqua_ssid file written");
        }
        fprintf(f, (char *)ssid);
        fclose(f);

        ESP_LOGI(TAG, "Opening file");
        FILE *f1 = fopen("/spiffs/aqua_password.txt", "w");
        if (f1 == NULL)
        {
            ESP_LOGI(TAG, "Failed to open file for writing");
            return;
        }
        else
        {
            ESP_LOGI(TAG, "Password in aqua_password file written");
        }
        fprintf(f1, (char *)password);
        fclose(f1);

        ESP_ERROR_CHECK(esp_wifi_disconnect());
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        esp_wifi_connect();
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        mqtt_app_start();
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE)
    {
        xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_DONE_BIT);
    }
}

static void smartconfig_example_task(void *parm)
{
    EventBits_t uxBits;
    ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH));
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));
    while (1)
    {
        uxBits = xEventGroupWaitBits(s_wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT, true, false, portMAX_DELAY);
        if (uxBits & CONNECTED_BIT)
        {
            ESP_LOGI(TAG, "WiFi Connected to ap");
        }
        if (uxBits & ESPTOUCH_DONE_BIT)
        {
            ESP_LOGI(TAG, "smartconfig over");
            esp_smartconfig_stop();
            vTaskDelete(NULL);
        }
    }
}

static void esp_mqtt_publish_task(void *parm)
{
    int msg_id;
    esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)parm;
    while (1)
    {
        msg_id = esp_mqtt_client_publish(client, "kunjan/superb1", (char *)data_new, 7, 1, 1);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
         gpio_set_level(GPIO_NUM_5, 0); //GREEN led on
        vTaskDelay(5000 / portTICK_RATE_MS);
         gpio_set_level(GPIO_NUM_5, 1); // green led off
    }
    vTaskDelete(NULL);
}

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        gpio_set_level(GPIO_NUM_2, 1);  //RED LED ON
        xTaskCreate(&esp_mqtt_publish_task, "esp_mqtt_publish_task", 4096, (void *)client, 3, NULL);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        gpio_set_level(GPIO_NUM_2, 0);  //RED LED OFF
        
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        
        gpio_set_level(GPIO_NUM_2, 0);  //RED LED OFF
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtt://broker.hivemq.com:1883", // mqtt://192.168.1.101:1883
        .username = "kunjanrshah@gmail.com",
        .password = "kunjan@123"};
#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.uri, "FROM_STDIN") == 0)
    {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128)
        {
            int c = fgetc(stdin);
            if (c == '\n')
            {
                line[count] = '\0';
                break;
            }
            else if (c > 0 && c < 127)
            {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.uri = line;
        printf("Broker url: %s\n", line);
    }
    else
    {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

static void initialise_wifi(char *ssid1, char *password1, uint8_t apmode)
{
    ESP_ERROR_CHECK(esp_netif_init());
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_LOGI(TAG, "[APP] KUNJAN INIT..");
    if (apmode == 0)
    {
        ESP_LOGI(TAG, "[APP] SMART CONFIG..");
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler_smartconfig, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler_smartconfig, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler_smartconfig, NULL));

        wifi_config_t wifi_config1;
        char *ssid_ap = "SUPERB01";
        char *password_ap = "12345678";
        bzero(&wifi_config1, sizeof(wifi_config_t));
        memcpy(wifi_config1.ap.ssid, ssid_ap, strlen(ssid_ap));
        memcpy(wifi_config1.ap.password, password_ap, strlen(password_ap));
        

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config1));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    else
    {
        ESP_LOGI(TAG, "[APP] WIFI INIT..");
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

        wifi_config_t wifi_config;
        bzero(&wifi_config, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, ssid1, strlen(ssid1));
        memcpy(wifi_config.sta.password, password1, strlen(password1));


        wifi_config_t wifi_config1;
        char *ssid_ap = "SUPERB01";
        char *password_ap = "12345678";
        bzero(&wifi_config1, sizeof(wifi_config_t));
        memcpy(wifi_config1.ap.ssid, ssid_ap, strlen(ssid_ap));
        memcpy(wifi_config1.ap.password, password_ap, strlen(password_ap));


        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config1));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
}

void app_main(void)
{
    //gpio_set_direction(GPIO_NUM_23, GPIO_MODE_INPUT); // key input
    

    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
    gpio_set_direction(GPIO_NUM_5, GPIO_MODE_OUTPUT);
   
    gpio_set_level(GPIO_NUM_2, 0); // off
    gpio_set_level(GPIO_NUM_5, 0); // off
    

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    //init_uart();

    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true};

    // Use settings defined above to initialize and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    // Open renamed file for reading
    ESP_LOGI(TAG, "Reading file");
    FILE *f = fopen("/spiffs/aqua_ssid.txt", "r");
    FILE *f1 = fopen("/spiffs/aqua_password.txt", "r");

    char line[64];
    char line1[64];
    if (f == NULL || f1 == NULL)
    {
        ESP_LOGI(TAG, "Failed to open file for reading");
        initialise_wifi(line, line1, 0);
    }
    else
    {

        fgets(line, sizeof(line), f);
        fclose(f);
        // strip newline
        char *pos = strchr(line, '\n');
        if (pos)
        {
            *pos = '\0';
        }
        ESP_LOGI(TAG, "Read SSID from file: '%s'", line);

        fgets(line1, sizeof(line), f1);
        fclose(f1);
        // strip newline
        char *pos1 = strchr(line1, '\n');
        if (pos1)
        {
            *pos1 = '\0';
        }
        ESP_LOGI(TAG, "Read Password from file: '%s'", line1);

        initialise_wifi(line, line1, 1);
    }

    // xTaskCreate(&rx_task, "rx_task", 1024 * 2, NULL, 10, NULL);
    xTaskCreate(echo_task, "uart_echo_task", ECHO_TASK_STACK_SIZE, NULL, 10, NULL);
    xTaskCreate(reset_password, "reset_password", 4096, NULL, 5, NULL);

   
}