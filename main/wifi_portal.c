#include "wifi_portal.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"

#ifndef MIN
#define MIN(a,b) (((a)<(b))?(a):(b))
#endif

static const char *TAG = "wifi_portal";

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT      = BIT1;
static int s_retry_num = 0;
static bool s_internet_ok = false;

/* HTML e Handlers do Servidor Web (Movidos do main.c) */
static const char* index_html_prefix = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{font-family:sans-serif;padding:20px}input,select{width:100%;padding:10px;margin:10px 0}input[type=submit]{background:#007bff;color:#fff;border:none;cursor:pointer}</style></head><body><h1>Configuração Wi-Fi</h1><form action='/save' method='POST'>Rede:<br><select name='ssid'>";
static const char* index_html_middle = "</select><br>Senha:<br><input type='password' name='password'><br><input type='submit' value='Conectar e Salvar'></form></body></html>";

static esp_err_t root_get_handler(httpd_req_t *req) {
    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK) {
        if (strcmp(host, "192.168.4.1") != 0 && strcmp(host, "esp.config") != 0) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
    }
    esp_wifi_scan_start(NULL, true);
    uint16_t number = 10;
    wifi_ap_record_t ap_info[10];
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 10) ap_count = 10;
    number = ap_count;
    if (number > 0) esp_wifi_scan_get_ap_records(&number, ap_info);
    httpd_resp_send_chunk(req, index_html_prefix, strlen(index_html_prefix));
    if (number == 0) {
        httpd_resp_send_chunk(req, "<option value=''>Nenhuma rede encontrada</option>", -1);
    } else {
        for (int i = 0; i < number; i++) {
            char buf[128];
            snprintf(buf, sizeof(buf), "<option value='%s'>%s (%d dBm)</option>", (char*)ap_info[i].ssid, (char*)ap_info[i].ssid, ap_info[i].rssi);
            httpd_resp_send_chunk(req, buf, strlen(buf));
        }
    }
    httpd_resp_send_chunk(req, index_html_middle, strlen(index_html_middle));
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf)));
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    char raw_ssid[64]={0}, raw_pass[128]={0}, ssid[33]={0}, pass[64]={0};
    if (httpd_query_key_value(buf, "ssid", raw_ssid, sizeof(raw_ssid)) == ESP_OK &&
        httpd_query_key_value(buf, "password", raw_pass, sizeof(raw_pass)) == ESP_OK) {
        char *d = ssid; const char *s = raw_ssid;
        while (*s && (d-ssid)<32) { if(*s=='+')*d=' '; else if(*s=='%'&&s[1]&&s[2]){unsigned int v;sscanf(s+1,"%02x",&v);*d=(char)v;s+=2;}else *d=*s; s++;d++; } *d='\0';
        d = pass; s = raw_pass;
        while (*s && (d-pass)<63) { if(*s=='+')*d=' '; else if(*s=='%'&&s[1]&&s[2]){unsigned int v;sscanf(s+1,"%02x",&v);*d=(char)v;s+=2;}else *d=*s; s++;d++; } *d='\0';
        nvs_handle_t h;
        if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "ssid", ssid); nvs_set_str(h, "password", pass);
            nvs_commit(h); nvs_close(h);
            httpd_resp_sendstr(req, "Credenciais salvas. Reiniciando...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
    return ESP_OK;
}

static esp_err_t handler_404(httpd_req_t *req, httpd_err_code_t error) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void dns_server_task(void *pvParameters) {
    uint8_t buf[512];
    struct sockaddr_in addr;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&source_addr, &socklen);
        if (len > 12) {
            buf[2] = 0x81; buf[3] = 0x80;
            uint16_t qd = (buf[4] << 8) | buf[5];
            if (qd == 0) continue;
            buf[6] = buf[4]; buf[7] = buf[5];
            buf[8]=0; buf[9]=0; buf[10]=0; buf[11]=0;
            int pos = 12, res_pos = len;
            for (int i=0; i<qd; i++) {
                int ns = pos; while (pos<len && buf[pos]!=0) pos++; pos+=5;
                if (res_pos+16 < sizeof(buf)) {
                    buf[res_pos++] = 0xC0; buf[res_pos++] = (uint8_t)ns;
                    buf[res_pos++] = 0x00; buf[res_pos++] = 0x01; buf[res_pos++] = 0x00; buf[res_pos++] = 0x01;
                    buf[res_pos++] = 0; buf[res_pos++] = 0; buf[res_pos++] = 0; buf[res_pos++] = 0;
                    buf[res_pos++] = 0; buf[res_pos++] = 4;
                    buf[res_pos++] = 192; buf[res_pos++] = 168; buf[res_pos++] = 4; buf[res_pos++] = 1;
                }
            }
            sendto(sock, buf, res_pos, 0, (struct sockaddr *)&source_addr, socklen);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void internet_test_task(void *pvParameters) {
    while (1) {
        if (wifi_portal_is_connected()) {
            esp_http_client_config_t config = { .url = "http://www.google.com", .timeout_ms = 3000 };
            esp_http_client_handle_t client = esp_http_client_init(&config);
            s_internet_ok = (esp_http_client_perform(client) == ESP_OK);
            esp_http_client_cleanup(client);
        } else {
            s_internet_ok = false;
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_retry_num < 3) { esp_wifi_connect(); s_retry_num++; }
        else xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_portal_init(void) {
    // Inicializa NVS internamente para simplificar o main
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t sta_cfg = {0};
    bool has_creds = false;
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) {
        size_t s = 32; if(nvs_get_str(h, "ssid", (char*)sta_cfg.sta.ssid, &s)==ESP_OK) {
            s = 64; nvs_get_str(h, "password", (char*)sta_cfg.sta.password, &s);
            has_creds = true;
        }
        nvs_close(h);
    }

    wifi_config_t ap_cfg = { .ap = { .ssid=SSID_PORTAL, .channel=1, .authmode=WIFI_AUTH_OPEN, .max_connection=4 } };
    
    if (has_creds) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    } else {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();

    // Start Services
    httpd_handle_t server = NULL;
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&server, &http_cfg) == ESP_OK) {
        httpd_uri_t uri_get = { .uri="/", .method=HTTP_GET, .handler=root_get_handler };
        httpd_uri_t uri_save = { .uri="/save", .method=HTTP_POST, .handler=save_post_handler };
        httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &uri_save);
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handler_404);
    }
    xTaskCreate(dns_server_task, "dns", 4096, NULL, 5, NULL);
    xTaskCreate(internet_test_task, "inet_test", 4096, NULL, 4, NULL);
}

bool wifi_portal_is_connected(void) {
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_portal_has_internet(void) {
    return s_internet_ok;
}

void wifi_portal_reset_credentials(void) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "password");
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Credenciais apagadas. Reiniciando em modo Portal...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}
