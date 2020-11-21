/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "sdkconfig.h"

#include "driver/uart.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_wifi.h"

#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#define DUCK_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define DUCK_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define DUCK_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY
#define PORT (3333)
#define DUCK_UART_PORT_NUM (UART_NUM_2)
#define DUCK_UART_TX_GPIO (17)
#define DUCK_UART_RX_GPIO (16)

// command(1 bytes) + length(2 bytes)
#define HEADER_SIZE (3)

// Commands
#define COMMAND_DOWNLOAD_SCRIPT 'b'
#define COMMAND_RUN_SCRIPT 'r'
#define COMMAND_STOP_SCRIPT 'k'
#define COMMAND_DELAY 'd'
#define COMMAND_DEFAULT_DELAY 'D'
#define COMMAND_REPEAT 'R'

#define TERMINATOR '\n'

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "Simple duck";

static int s_retry_num = 0;

#define MAX_SCRIPT_SIZE (65536)
static char script[MAX_SCRIPT_SIZE + 1];
static size_t script_size = 0;
static size_t default_delay = 5;
TaskHandle_t script_task_handle = NULL;
static int stop_signal = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < DUCK_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void setup_wifi(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = DUCK_WIFI_SSID,
            .password = DUCK_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to ap SSID:%s",
                 DUCK_WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s",
                 DUCK_WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

static int recv_all(int sock, char *buff, size_t size)
{
    size_t size_acc = 0;
    do {
        int recv_len = recv(sock, buff + size_acc, size - size_acc, 0);
        if (!recv_len) {
            return 0;
        } else if (recv_len < 0) {
            ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
            continue;
        }
        size_acc += recv_len;
    } while (size_acc < size);
    return size_acc;
}

static int send_all(int sock, const char *buff, size_t size)
{
    size_t size_acc = 0;
    do {
        int send_len = send(sock, buff + size_acc, size - size_acc, 0);
        if (!send_len) {
            return 0;
        } else if (send_len < 0) {
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            continue;
        }
        size_acc += send_len;
    } while (size_acc < size);
    return size_acc;
}

static void execute_script(void)
{
    if (!script_size)
        return;
    char *ptr = script;
    unsigned int delay = 0;
    unsigned int repeat = 0;
    stop_signal = 0;
    while (ptr < script + script_size) {
        char *end_ptr = memchr(ptr, TERMINATOR, script + script_size - ptr);
        if (!*end_ptr)
            return;
        if (!repeat)
            repeat = 1;
        do {
            if (stop_signal) {
                ESP_LOGD(TAG, "Script was stopped");
                stop_signal = 0;
                break;
            }
            if (*ptr == COMMAND_DELAY || *ptr == COMMAND_DEFAULT_DELAY) {
                *end_ptr = 0;
                ESP_LOGD(TAG, "Delay command: %s", ptr);
                *end_ptr = TERMINATOR;
                int parsed = sscanf(ptr + 1, "%u", &delay);
                if (parsed != 1) {
                    ESP_LOGE(TAG, "failed to parse delay command");
                }
                if (*ptr == COMMAND_DEFAULT_DELAY) {
                    default_delay = delay;
                } else {
                    vTaskDelay(delay / portTICK_PERIOD_MS);
                }
            } else if (*ptr == COMMAND_REPEAT) {
                *end_ptr = 0;
                ESP_LOGD(TAG, "Repeat command: %s", ptr);
                *end_ptr = TERMINATOR;
                int parsed = sscanf(ptr + 1, "%u", &repeat);
                if (parsed != 1) {
                    ESP_LOGE(TAG, "failed to parse repeat command");
                }
                break;
            } else {
                *end_ptr = 0;
                ESP_LOGD(TAG, "Keyboard command: %s", ptr);
                *end_ptr = TERMINATOR;
                uart_write_bytes(DUCK_UART_PORT_NUM, ptr, end_ptr - ptr + 1);
                vTaskDelay(default_delay / portTICK_PERIOD_MS);
            }
        } while (--repeat);
        ptr = end_ptr + 1;
    }
}

static void wake_script_task(void)
{
    xTaskNotify(script_task_handle, 1, eSetBits);
}

static void script_task(void *params)
{
    while (1) {
        if(xTaskNotifyWait(pdFALSE, ULONG_MAX, NULL, portMAX_DELAY) == pdTRUE) {
            ESP_LOGD(TAG, "Script started");
            execute_script();
            ESP_LOGD(TAG, "Script finished");
        }
    }
}

static void tcp_server_task(void *params)
{
    struct sockaddr_in server_addr;
    char addr_str[64];
    int rc;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        esp_restart();
    }
    rc = bind(listen_sock, (struct sockaddr *) &server_addr, sizeof(server_addr));
    if (rc) {
        ESP_LOGE(TAG, "Unable to bind: errno %d", errno);
        esp_restart();
    }
    rc = listen(listen_sock, 1);
    if (rc) {
        ESP_LOGE(TAG, "Unable to listen: errno %d", errno);
        esp_restart();
    }
    while (1) {
        struct sockaddr_in client_addr;
        uint client_addr_len = sizeof(client_addr);
        int sock = accept(listen_sock, (struct sockaddr *) &client_addr, &client_addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            continue;
        }
        inet_ntoa_r(((struct sockaddr_in *)&client_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGD(TAG, "Socket accepted ip address: %s", addr_str);
        int recv_len = 0;
        int send_len = 0;
        char buff[HEADER_SIZE];
        const char *ok_msg = "OK\n";
        while (1) {
            recv_len = recv_all(sock, buff, HEADER_SIZE);
            if (!recv_len) {
                ESP_LOGD(TAG, "recv: EOF");
                break;
            }
            char command = buff[0];
            size_t data_size = buff[1] + buff[2]*256;

            if (command == COMMAND_DOWNLOAD_SCRIPT) {
                script_size = data_size;
                script[script_size] = 0;
                recv_len = recv_all(sock, script, script_size);
                if (!recv_len) {
                    ESP_LOGD(TAG, "recv: EOF");
                    break;
                }
                ESP_LOGD(TAG, "got %u bytes: %s", script_size, script);
                send_len = send_all(sock, ok_msg, strlen(ok_msg));
                if (!send_len) {
                    ESP_LOGD(TAG, "send: EOF");
                    break;
                }
            } else if (command == COMMAND_RUN_SCRIPT) {
                wake_script_task();
                send_len = send_all(sock, ok_msg, strlen(ok_msg));
                if (!send_len) {
                    ESP_LOGD(TAG, "send: EOF");
                    break;
                }
            } else if (command == COMMAND_STOP_SCRIPT) {
                stop_signal = 1;
                send_len = send_all(sock, ok_msg, strlen(ok_msg));
                if (!send_len) {
                    ESP_LOGD(TAG, "send: EOF");
                    break;
                }
            }
        }
        close(sock);
    }
    close(listen_sock);
}

static void setup_uart(void)
{
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    ESP_ERROR_CHECK(uart_driver_install(DUCK_UART_PORT_NUM, 1024 * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(DUCK_UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(DUCK_UART_PORT_NUM, DUCK_UART_TX_GPIO, DUCK_UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void setup_script_task(void)
{
    xTaskCreate(script_task, "script_task", 4096, NULL, 5, &script_task_handle);
}

void setup_server(void)
{
    xTaskCreate(tcp_server_task, "tcp_server", 8192, NULL, 5, NULL);
}

void app_main(void)
{
    // Initialize NVS (needed for wifi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    setup_wifi();
    setup_uart();
    setup_script_task();
    setup_server();
}
