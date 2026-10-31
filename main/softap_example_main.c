/* This example code is in the Public Domain (or CC0 licensed, at your option). */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "wifi_uart_config.h"

#define WIFI_READY_BIT BIT0
#define UART_STOP_BIT  BIT1
#define UART_DONE_BIT  BIT2
#define BUFFER_SIZE   1024
#define UART_BUFFER_SIZE 4096

_Static_assert(sizeof(TRANSP_WIFI_SSID) > 1 && sizeof(TRANSP_WIFI_SSID) <= 33,
               "SSID must contain 1..32 bytes");
_Static_assert(sizeof(TRANSP_WIFI_PASSWORD) == 1 ||
               (sizeof(TRANSP_WIFI_PASSWORD) >= 9 && sizeof(TRANSP_WIFI_PASSWORD) <= 64),
               "Password must be empty (open network) or contain 8..63 bytes");
_Static_assert(TRANSP_TCP_PORT > 0 && TRANSP_TCP_PORT <= 65535, "Invalid TCP port");
_Static_assert(TRANSP_UART_PORT >= UART_NUM_0 && TRANSP_UART_PORT < UART_NUM_MAX,
               "Invalid UART port");
_Static_assert(TRANSP_UART_TX_PIN != TRANSP_UART_RX_PIN, "TX and RX must use different pins");
_Static_assert(TRANSP_UART_TX_PIN >= 0 && TRANSP_UART_RX_PIN >= 0, "Specify both UART pins");
_Static_assert(TRANSP_UART_BAUD_RATE > 0, "Invalid UART baud rate");
#if CONFIG_ESP_CONSOLE_UART
_Static_assert(TRANSP_UART_PORT != CONFIG_ESP_CONSOLE_UART_NUM,
               "Use a different UART from the log console");
#endif

static const char *TAG = "wifi_uart";
static EventGroupHandle_t bridge_events;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
#if TRANSP_WIFI_MODE == TRANSP_WIFI_MODE_STA
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(bridge_events, WIFI_READY_BIT);
        xEventGroupSetBits(bridge_events, UART_STOP_BIT);
        ESP_LOGW(TAG, "WiFi disconnected; reconnecting");
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "STA ready: connect TCP client to " IPSTR ":%d",
                 IP2STR(&event->ip_info.ip), TRANSP_TCP_PORT);
        xEventGroupSetBits(bridge_events, WIFI_READY_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(bridge_events, WIFI_READY_BIT);
        xEventGroupSetBits(bridge_events, UART_STOP_BIT);
    }
#else
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        xEventGroupSetBits(bridge_events, WIFI_READY_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        xEventGroupClearBits(bridge_events, WIFI_READY_BIT);
        xEventGroupSetBits(bridge_events, UART_STOP_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "WiFi station joined");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "WiFi station left");
    }
#endif
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if TRANSP_WIFI_MODE == TRANSP_WIFI_MODE_AP
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
#else
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
#endif
    ESP_ERROR_CHECK(netif ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL));
#if TRANSP_WIFI_MODE == TRANSP_WIFI_MODE_AP
    wifi_config_t config = {
        .ap = {
            .ssid = TRANSP_WIFI_SSID,
            .ssid_len = sizeof(TRANSP_WIFI_SSID) - 1,
            .password = TRANSP_WIFI_PASSWORD,
            .channel = TRANSP_WIFI_CHANNEL,
            .max_connection = TRANSP_MAX_STA_CONN,
#ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
            .authmode = WIFI_AUTH_WPA3_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
#else
            .authmode = WIFI_AUTH_WPA2_PSK,
#endif
            .pmf_cfg.required = true,
#ifdef CONFIG_ESP_WIFI_BSS_MAX_IDLE_SUPPORT
            .bss_max_idle_cfg = {
                .period = WIFI_AP_DEFAULT_MAX_IDLE_PERIOD,
                .protected_keep_alive = 1,
            },
#endif
#if CONFIG_ESP_GTK_REKEYING_ENABLE
            .gtk_rekey_interval = CONFIG_ESP_GTK_REKEY_INTERVAL,
#endif
        },
    };
    if (sizeof(TRANSP_WIFI_PASSWORD) == 1) {
        config.ap.authmode = WIFI_AUTH_OPEN;
        config.ap.pmf_cfg.required = false;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
#else
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL));
    wifi_config_t config = {
        .sta = {
            .ssid = TRANSP_WIFI_SSID,
            .password = TRANSP_WIFI_PASSWORD,
            .threshold.authmode = sizeof(TRANSP_WIFI_PASSWORD) == 1 ?
                                  WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
#endif
    ESP_ERROR_CHECK(esp_wifi_start());
#if TRANSP_WIFI_MODE == TRANSP_WIFI_MODE_AP
    esp_netif_ip_info_t ip;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &ip));
    ESP_LOGI(TAG, "AP SSID: %s; connect TCP client to " IPSTR ":%d",
             TRANSP_WIFI_SSID, IP2STR(&ip.ip), TRANSP_TCP_PORT);
#endif
}

static void uart_init(void)
{
    const uart_config_t config = {
        .baud_rate = TRANSP_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(TRANSP_UART_PORT, UART_BUFFER_SIZE,
                                       UART_BUFFER_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(TRANSP_UART_PORT, &config));
    ESP_ERROR_CHECK(uart_set_pin(TRANSP_UART_PORT, TRANSP_UART_TX_PIN, TRANSP_UART_RX_PIN,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d: TX GPIO%d, RX GPIO%d, %d baud, 8N1",
             TRANSP_UART_PORT, TRANSP_UART_TX_PIN, TRANSP_UART_RX_PIN, TRANSP_UART_BAUD_RATE);
}

static void uart_to_tcp_task(void *arg)
{
    const int sock = (int)(intptr_t)arg;
    uint8_t buffer[BUFFER_SIZE];
    while (!(xEventGroupGetBits(bridge_events) & UART_STOP_BIT)) {
        int count = uart_read_bytes(TRANSP_UART_PORT, buffer, sizeof(buffer), pdMS_TO_TICKS(20));
        if (count < 0) {
            ESP_LOGE(TAG, "UART read failed");
            break;
        }
        int sent = 0;
        while (sent < count && !(xEventGroupGetBits(bridge_events) & UART_STOP_BIT)) {
            int n = send(sock, buffer + sent, count - sent, 0);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                ESP_LOGW(TAG, "TCP send failed or timed out: errno %d", errno);
                goto done;
            }
            sent += n;
        }
    }
done:
    shutdown(sock, SHUT_RDWR);
    xEventGroupSetBits(bridge_events, UART_DONE_BIT);
    vTaskDelete(NULL);
}

static bool configure_client(int sock)
{
    const struct timeval timeout = {.tv_sec = 1};
    const int enabled = 1, idle = 5, interval = 5, probes = 3;
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
           setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 &&
           setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) == 0 &&
           setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) == 0 &&
           setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) == 0 &&
           setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes)) == 0;
}

static void bridge_client(int sock)
{
    uint8_t buffer[BUFFER_SIZE];
    if (!configure_client(sock)) {
        ESP_LOGE(TAG, "Socket configuration failed: errno %d", errno);
        return;
    }
    /* Finish queued TX before discarding input from the previous session. */
    if (uart_wait_tx_done(TRANSP_UART_PORT, pdMS_TO_TICKS(1000)) != ESP_OK) {
        ESP_LOGW(TAG, "UART still transmitting; reject this client");
        return;
    }
    ESP_ERROR_CHECK(uart_flush_input(TRANSP_UART_PORT));
    xEventGroupClearBits(bridge_events, UART_STOP_BIT | UART_DONE_BIT);
    if (xTaskCreate(uart_to_tcp_task, "uart_to_tcp", 4096, (void *)(intptr_t)sock,
                    5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create UART forwarding task");
        return;
    }
    while ((xEventGroupGetBits(bridge_events) & (WIFI_READY_BIT | UART_STOP_BIT)) == WIFI_READY_BIT) {
        int count = recv(sock, buffer, sizeof(buffer), 0);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        if (uart_write_bytes(TRANSP_UART_PORT, buffer, count) != count) {
            ESP_LOGE(TAG, "UART write failed");
            break;
        }
    }
    xEventGroupSetBits(bridge_events, UART_STOP_BIT);
    shutdown(sock, SHUT_RDWR);
    /* The writer must stop before close() allows this descriptor to be reused. */
    xEventGroupWaitBits(bridge_events, UART_DONE_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

static void tcp_server_task(void *arg)
{
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(TRANSP_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    for (;;) {
        xEventGroupWaitBits(bridge_events, WIFI_READY_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listener < 0) {
            ESP_LOGE(TAG, "socket failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        const int reuse = 1;
        if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
            bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
            listen(listener, 1) != 0) {
            ESP_LOGE(TAG, "TCP listen failed: errno %d", errno);
            close(listener);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "TCP server listening on port %d (one active client)", TRANSP_TCP_PORT);
        while (xEventGroupGetBits(bridge_events) & WIFI_READY_BIT) {
            fd_set reads;
            FD_ZERO(&reads);
            FD_SET(listener, &reads);
            struct timeval timeout = {.tv_sec = 1};
            int ready = select(listener + 1, &reads, NULL, NULL, &timeout);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (ready == 0) {
                continue;
            }
            int sock = accept(listener, NULL, NULL);
            if (sock < 0) {
                ESP_LOGW(TAG, "accept failed: errno %d", errno);
                break;
            }
            ESP_LOGI(TAG, "TCP client connected");
            bridge_client(sock);
            close(sock);
            ESP_LOGI(TAG, "TCP client disconnected");
        }
        close(listener);
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    bridge_events = xEventGroupCreate();
    ESP_ERROR_CHECK(bridge_events ? ESP_OK : ESP_ERR_NO_MEM);
    uart_init();
    wifi_init();
    ESP_ERROR_CHECK(xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL) == pdPASS ?
                    ESP_OK : ESP_ERR_NO_MEM);
}
