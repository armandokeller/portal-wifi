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
static bool s_ap_active = true;
static wifi_config_t s_ap_cfg = {0};
static esp_netif_t *s_ap_netif = NULL;
static const char *CAPTIVE_PORTAL_URL = "http://192.168.4.1/portal?full=1";
static const char *CAPTIVE_PORTAL_API_URL = "http://192.168.4.1/captive-portal/api";
static const char *NVS_NS = "storage";
static const char *NVS_KEY_SSID = "ssid";
static const char *NVS_KEY_PASS = "password";
static const char *NVS_KEY_RESET_MARKER = "reset_marker";

/* HTML e Handlers do Servidor Web (Movidos do main.c) */
static const char* index_html_prefix = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{font-family:sans-serif;padding:20px}input,select{width:100%;padding:10px;margin:10px 0}input[type=submit]{background:#007bff;color:#fff;border:none;cursor:pointer}</style></head><body><h1>Configuração Wi-Fi</h1><form action='/save' method='POST'>Rede:<br><select name='ssid'>";
static const char* index_html_middle = "</select><br>Ou digite o nome da rede:<br><input type='text' name='ssid_manual' placeholder='SSID manual (fallback)'><br>Senha:<br><input type='password' name='password'><br><input type='submit' value='Conectar e Salvar'></form></body></html>";

static bool host_is_portal(const char *host) {
    if (host == NULL) {
        return false;
    }
    return strncmp(host, "192.168.4.1", 11) == 0 || strncmp(host, "esp.config", 10) == 0;
}

static void log_http_request(httpd_req_t *req, const char *tag) {
    char host[96] = {0};
    char user_agent[160] = {0};

    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        strcpy(host, "-");
    }
    if (httpd_req_get_hdr_value_str(req, "User-Agent", user_agent, sizeof(user_agent)) != ESP_OK) {
        strcpy(user_agent, "-");
    }

    const char *os = "unknown";
    if (strstr(user_agent, "Android") != NULL || strstr(user_agent, "Dalvik") != NULL) {
        os = "android";
    } else if (strstr(user_agent, "Windows") != NULL || strstr(user_agent, "NCSI") != NULL) {
        os = "windows";
    } else if (strstr(user_agent, "CaptiveNetworkSupport") != NULL || strstr(user_agent, "iPhone") != NULL || strstr(user_agent, "iPad") != NULL) {
        os = "apple";
    }

    ESP_LOGI(TAG, "%s os=%s uri=%s host=%s ua=%s", tag, os, req->uri, host, user_agent);
}

static esp_err_t redirect_to_portal(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Link", "<http://192.168.4.1/captive-portal/api>; rel=\"captive\"");
    httpd_resp_set_hdr(req, "Location", CAPTIVE_PORTAL_URL);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void log_probe_request(httpd_req_t *req, const char *probe_tag) {
    char tag[48] = {0};
    snprintf(tag, sizeof(tag), "Probe[%s]", probe_tag);
    log_http_request(req, tag);
}

static esp_err_t captive_probe_handler(httpd_req_t *req, const char *probe_tag) {
    log_probe_request(req, probe_tag);
    return redirect_to_portal(req);
}

static esp_err_t probe_android_handler(httpd_req_t *req) {
    return captive_probe_handler(req, "android");
}

static esp_err_t probe_apple_handler(httpd_req_t *req) {
    return captive_probe_handler(req, "apple");
}

static esp_err_t probe_windows_handler(httpd_req_t *req) {
    return captive_probe_handler(req, "windows");
}

static esp_err_t probe_generic_handler(httpd_req_t *req) {
    return captive_probe_handler(req, "generic");
}

static esp_err_t probe_msftconnect_handler(httpd_req_t *req) {
    // Retorno diferente do esperado pelo NCSI para disparar fluxo captive no Windows.
    log_probe_request(req, "windows-connecttest");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ESP Captive Portal");
    return ESP_OK;
}

static esp_err_t probe_msftncsi_handler(httpd_req_t *req) {
    log_probe_request(req, "windows-ncsi");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ESP Captive Portal");
    return ESP_OK;
}

static esp_err_t capport_api_handler(httpd_req_t *req) {
    log_probe_request(req, "capport-api");
    httpd_resp_set_type(req, "application/captive+json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_sendstr(req,
        "{\"captive\":true,\"user-portal-url\":\"http://192.168.4.1/\",\"venue-info-url\":\"http://192.168.4.1/\"}");
    return ESP_OK;
}

static void configure_ap_dhcp_for_captive_portal(void) {
    if (s_ap_netif == NULL) {
        ESP_LOGW(TAG, "AP netif indisponivel para configurar DHCP captive portal");
        return;
    }

    esp_netif_ip_info_t ip_info = {0};
    esp_err_t ret = esp_netif_get_ip_info(s_ap_netif, &ip_info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao ler IP do AP netif: %s", esp_err_to_name(ret));
        return;
    }

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4 = ip_info.ip;
    ret = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao definir DNS main do AP: %s", esp_err_to_name(ret));
    }

    uint8_t offer_dns = 1;
    ret = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao habilitar oferta DHCP DNS: %s", esp_err_to_name(ret));
    }

    ret = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                 (void *)CAPTIVE_PORTAL_API_URL, strlen(CAPTIVE_PORTAL_API_URL) + 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao definir URI captive portal no DHCP: %s", esp_err_to_name(ret));
    }

    char dns_ip_str[20] = {0};
    esp_ip4addr_ntoa(&ip_info.ip, dns_ip_str, sizeof(dns_ip_str));
    ESP_LOGI(TAG, "DHCP captive configurado: DNS=%s URI=%s", dns_ip_str, CAPTIVE_PORTAL_API_URL);
}

static bool is_probe_user_agent(const char *user_agent) {
    if (user_agent == NULL) {
        return false;
    }
    return strstr(user_agent, "Dalvik") != NULL ||
           strstr(user_agent, "CaptiveNetworkSupport") != NULL ||
           strstr(user_agent, "NCSI") != NULL;
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    log_http_request(req, "HTTP[root]");

    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK) {
        if (!host_is_portal(host)) {
            return redirect_to_portal(req);
        }
    }
    char user_agent[160] = {0};
    bool is_probe_client = false;
    if (httpd_req_get_hdr_value_str(req, "User-Agent", user_agent, sizeof(user_agent)) == ESP_OK) {
        is_probe_client = is_probe_user_agent(user_agent);
    }

    if (is_probe_client) {
        return redirect_to_portal(req);
    }

    // Aguarda o scan terminar para preencher a lista de redes no formulario.
    esp_err_t scan_ret = esp_wifi_scan_start(NULL, true);
    if (scan_ret != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao iniciar scan Wi-Fi: %s", esp_err_to_name(scan_ret));
    }
    uint16_t number = 10;
    wifi_ap_record_t ap_info[10];
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 10) ap_count = 10;
    number = ap_count;
    printf("Buscando redes wifi\n");
    if (number > 0) esp_wifi_scan_get_ap_records(&number, ap_info);
    httpd_resp_send_chunk(req, index_html_prefix, strlen(index_html_prefix));
    if (number == 0) {
        printf("Nenhuma rede encontrada\n");
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

static esp_err_t portal_get_handler(httpd_req_t *req) {
    log_http_request(req, "HTTP[portal]");

    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK) {
        if (!host_is_portal(host)) {
            return redirect_to_portal(req);
        }
    }

    esp_err_t scan_ret = esp_wifi_scan_start(NULL, true);
    if (scan_ret != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao iniciar scan Wi-Fi no /portal: %s", esp_err_to_name(scan_ret));
    }

    uint16_t number = 10;
    wifi_ap_record_t ap_info[10];
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 10) ap_count = 10;
    number = ap_count;
    printf("Buscando redes wifi\n");
    if (number > 0) esp_wifi_scan_get_ap_records(&number, ap_info);

    httpd_resp_send_chunk(req, index_html_prefix, strlen(index_html_prefix));
    if (number == 0) {
        printf("Nenhuma rede encontrada\n");
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
    char raw_ssid[64]={0}, raw_ssid_manual[64]={0}, raw_pass[128]={0};
    char ssid[33]={0}, ssid_manual[33]={0}, pass[64]={0};
    if (httpd_query_key_value(buf, "password", raw_pass, sizeof(raw_pass)) == ESP_OK) {
        httpd_query_key_value(buf, "ssid", raw_ssid, sizeof(raw_ssid));
        httpd_query_key_value(buf, "ssid_manual", raw_ssid_manual, sizeof(raw_ssid_manual));
        char *d = ssid; const char *s = raw_ssid;
        while (*s && (d-ssid)<32) { if(*s=='+')*d=' '; else if(*s=='%'&&s[1]&&s[2]){unsigned int v;sscanf(s+1,"%02x",&v);*d=(char)v;s+=2;}else *d=*s; s++;d++; } *d='\0';
        d = ssid_manual; s = raw_ssid_manual;
        while (*s && (d-ssid_manual)<32) { if(*s=='+')*d=' '; else if(*s=='%'&&s[1]&&s[2]){unsigned int v;sscanf(s+1,"%02x",&v);*d=(char)v;s+=2;}else *d=*s; s++;d++; } *d='\0';
        d = pass; s = raw_pass;
        while (*s && (d-pass)<63) { if(*s=='+')*d=' '; else if(*s=='%'&&s[1]&&s[2]){unsigned int v;sscanf(s+1,"%02x",&v);*d=(char)v;s+=2;}else *d=*s; s++;d++; } *d='\0';

        const char *ssid_to_save = (strlen(ssid_manual) > 0) ? ssid_manual : ssid;
        if (strlen(ssid_to_save) == 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "SSID invalido. Selecione uma rede ou digite o nome manualmente.");
            return ESP_OK;
        }

        nvs_handle_t h;
        if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "ssid", ssid_to_save); nvs_set_str(h, "password", pass);
            nvs_commit(h); nvs_close(h);
            httpd_resp_sendstr(req, "Credenciais salvas. Reiniciando...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
    return ESP_OK;
}

static esp_err_t handler_404(httpd_req_t *req, httpd_err_code_t error) {
    (void)error;
    log_http_request(req, "HTTP[404]");
    return redirect_to_portal(req);
}

static int dns_extract_qname(const uint8_t *buf, int len, char *out, size_t out_len) {
    int pos = 12;
    int out_pos = 0;

    if (len <= 12 || out_len == 0) {
        return -1;
    }

    while (pos < len && buf[pos] != 0) {
        int label_len = buf[pos++];
        if (label_len <= 0 || pos + label_len > len) {
            return -1;
        }

        if (out_pos != 0 && out_pos < (int)out_len - 1) {
            out[out_pos++] = '.';
        }

        for (int i = 0; i < label_len && out_pos < (int)out_len - 1; i++) {
            out[out_pos++] = (char)buf[pos + i];
        }
        pos += label_len;
    }

    out[out_pos] = '\0';
    return out_pos;
}

static bool dns_is_ipv4_literal(const char *name) {
    esp_ip4_addr_t tmp = {0};
    return (name != NULL) && (esp_netif_str_to_ip4(name, &tmp) == ESP_OK);
}

static bool dns_should_hijack_name(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    // Nao sequestrar dominios usados para testes anti-hijack ou casos especiais.
    if (strchr(name, '*') != NULL) {
        return false;
    }
    if (strstr(name, ".onion") != NULL) {
        return false;
    }
    if (strstr(name, ".in-addr.arpa") != NULL || strstr(name, ".ip6.arpa") != NULL) {
        return false;
    }
    if (dns_is_ipv4_literal(name)) {
        return false;
    }

    return true;
}

static void dns_server_task(void *pvParameters) {
    uint8_t buf[512];
    struct sockaddr_in addr;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Falha ao criar socket DNS");
        vTaskDelete(NULL);
        return;
    }

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "Falha ao fazer bind do DNS na porta 53");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&source_addr, &socklen);
        if (len <= 12) {
            continue;
        }

        char qname[128] = {0};
        if (dns_extract_qname(buf, len, qname, sizeof(qname)) > 0) {
            ESP_LOGI(TAG, "DNS query from %s: %s", inet_ntoa(source_addr.sin_addr), qname);
        }

        int pos = 12;
        while (pos < len && buf[pos] != 0) {
            int label_len = (int)buf[pos];
            if (label_len <= 0 || pos + label_len >= len) {
                pos = -1;
                break;
            }
            pos += label_len + 1;
        }

        if (pos < 0 || pos + 5 > len) {
            continue;
        }

        uint16_t qtype = (uint16_t)((buf[pos + 1] << 8) | buf[pos + 2]);
        uint16_t qclass = (uint16_t)((buf[pos + 3] << 8) | buf[pos + 4]);
        int question_end = pos + 5;

        // Resposta DNS valida: mantem apenas cabecalho + primeira pergunta e opcionalmente 1 answer A.
        buf[2] = 0x81;
        buf[3] = 0x80;
        buf[4] = 0x00;
        buf[5] = 0x01;

        bool should_hijack = dns_should_hijack_name(qname);
        bool reply_with_a_record = should_hijack && (qclass == 0x0001) && (qtype == 0x0001 || qtype == 0x00FF);
        buf[6] = 0x00;
        buf[7] = reply_with_a_record ? 0x01 : 0x00;
        buf[8] = 0x00;
        buf[9] = 0x00;
        buf[10] = 0x00;
        buf[11] = 0x00;

        if (!should_hijack) {
            // NXDOMAIN para testes anti-hijack e consultas especiais.
            buf[3] = 0x83;
        }

        int res_pos = question_end;
        if (reply_with_a_record && (res_pos + 16 <= (int)sizeof(buf))) {
            buf[res_pos++] = 0xC0;
            buf[res_pos++] = 0x0C;
            buf[res_pos++] = 0x00;
            buf[res_pos++] = 0x01;
            buf[res_pos++] = 0x00;
            buf[res_pos++] = 0x01;
            buf[res_pos++] = 0x00;
            buf[res_pos++] = 0x00;
            buf[res_pos++] = 0x00;
            buf[res_pos++] = 0x1E;
            buf[res_pos++] = 0x00;
            buf[res_pos++] = 0x04;
            buf[res_pos++] = 192;
            buf[res_pos++] = 168;
            buf[res_pos++] = 4;
            buf[res_pos++] = 1;
        }

        sendto(sock, buf, res_pos, 0, (struct sockaddr *)&source_addr, socklen);
        ESP_LOGI(TAG, "DNS reply to %s: qtype=%u hijack=%s answer=%s", inet_ntoa(source_addr.sin_addr),
             (unsigned)qtype, should_hijack ? "yes" : "no",
             reply_with_a_record ? "A 192.168.4.1" : (should_hijack ? "NOERROR NODATA" : "NXDOMAIN"));

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

        if (!s_ap_active) {
            if (esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK) {
                esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
                s_ap_active = true;
                ESP_LOGI(TAG, "STA desconectada. AP de configuracao reativado.");
            } else {
                ESP_LOGW(TAG, "Falha ao reativar AP de configuracao.");
            }
        }

        if (s_retry_num < 3) { esp_wifi_connect(); s_retry_num++; }
        else xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (s_ap_active) {
            if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
                s_ap_active = false;
                ESP_LOGI(TAG, "STA conectada. AP de configuracao desativado.");
            } else {
                ESP_LOGW(TAG, "Falha ao desativar AP de configuracao.");
            }
        }
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
    s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    // Evita reuso automatico de configuracao STA persistida pelo driver Wi-Fi.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t sta_cfg = {0};
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) {
        size_t s = 32; if(nvs_get_str(h, "ssid", (char*)sta_cfg.sta.ssid, &s)==ESP_OK) {
            s = 64; nvs_get_str(h, "password", (char*)sta_cfg.sta.password, &s);
        }
        nvs_close(h);
    }

    s_ap_cfg = (wifi_config_t){ .ap = { .ssid=SSID_PORTAL, .channel=1, .authmode=WIFI_AUTH_OPEN, .max_connection=4 } };
    
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    // Sempre aplica config STA (vazia ou preenchida) para impedir auto-connect com credenciais antigas.
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
    configure_ap_dhcp_for_captive_portal();
    s_ap_active = true;
    esp_wifi_start();

    // Start Services
    httpd_handle_t server = NULL;
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 24;
    if (httpd_start(&server, &http_cfg) == ESP_OK) {
        httpd_uri_t uri_get = { .uri="/", .method=HTTP_GET, .handler=root_get_handler };
        httpd_uri_t uri_portal = { .uri="/portal", .method=HTTP_GET, .handler=portal_get_handler };
        httpd_uri_t uri_save = { .uri="/save", .method=HTTP_POST, .handler=save_post_handler };
        httpd_uri_t uri_generate_204 = { .uri="/generate_204", .method=HTTP_GET, .handler=probe_android_handler };
        httpd_uri_t uri_gen_204 = { .uri="/gen_204", .method=HTTP_GET, .handler=probe_android_handler };
        httpd_uri_t uri_generate204 = { .uri="/generate204", .method=HTTP_GET, .handler=probe_android_handler };
        httpd_uri_t uri_hotspot_detect = { .uri="/hotspot-detect.html", .method=HTTP_GET, .handler=probe_apple_handler };
        httpd_uri_t uri_ncsi = { .uri="/ncsi.txt", .method=HTTP_GET, .handler=probe_msftncsi_handler };
        httpd_uri_t uri_connecttest = { .uri="/connecttest.txt", .method=HTTP_GET, .handler=probe_msftconnect_handler };
        httpd_uri_t uri_redirect = { .uri="/redirect", .method=HTTP_GET, .handler=probe_windows_handler };
        httpd_uri_t uri_fwlink = { .uri="/fwlink", .method=HTTP_GET, .handler=probe_windows_handler };
        httpd_uri_t uri_canonical = { .uri="/canonical.html", .method=HTTP_GET, .handler=probe_generic_handler };
        httpd_uri_t uri_library_test = { .uri="/library/test/success.html", .method=HTTP_GET, .handler=probe_android_handler };
        httpd_uri_t uri_success_txt = { .uri="/success.txt", .method=HTTP_GET, .handler=probe_android_handler };
        httpd_uri_t uri_capport_api = { .uri="/captive-portal/api", .method=HTTP_GET, .handler=capport_api_handler };
        httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &uri_portal);
        httpd_register_uri_handler(server, &uri_save);
        httpd_register_uri_handler(server, &uri_generate_204);
        httpd_register_uri_handler(server, &uri_gen_204);
        httpd_register_uri_handler(server, &uri_generate204);
        httpd_register_uri_handler(server, &uri_hotspot_detect);
        httpd_register_uri_handler(server, &uri_ncsi);
        httpd_register_uri_handler(server, &uri_connecttest);
        httpd_register_uri_handler(server, &uri_redirect);
        httpd_register_uri_handler(server, &uri_fwlink);
        httpd_register_uri_handler(server, &uri_canonical);
        httpd_register_uri_handler(server, &uri_library_test);
        httpd_register_uri_handler(server, &uri_success_txt);
        httpd_register_uri_handler(server, &uri_capport_api);
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
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS inconsistente, apagando particao...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar NVS para reset: %s", esp_err_to_name(ret));
        return;
    }

    nvs_handle_t h;
    ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao abrir namespace NVS '%s': %s", NVS_NS, esp_err_to_name(ret));
        return;
    }

    esp_err_t erase_ssid_ret = nvs_erase_key(h, NVS_KEY_SSID);
    esp_err_t erase_pass_ret = nvs_erase_key(h, NVS_KEY_PASS);

    if ((erase_ssid_ret != ESP_OK && erase_ssid_ret != ESP_ERR_NVS_NOT_FOUND) ||
        (erase_pass_ret != ESP_OK && erase_pass_ret != ESP_ERR_NVS_NOT_FOUND)) {
        ESP_LOGE(TAG, "Falha ao apagar chaves NVS (ssid=%s, password=%s)",
                 esp_err_to_name(erase_ssid_ret), esp_err_to_name(erase_pass_ret));
        nvs_close(h);
        return;
    }

    ret = nvs_commit(h);
    nvs_close(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao confirmar reset no NVS: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Credenciais apagadas. Seguindo sem reiniciar.");
}

bool wifi_portal_reset_credentials_oneshot(const char *marker) {
    if (marker == NULL || marker[0] == '\0') {
        ESP_LOGE(TAG, "Marker invalido para reset one-shot");
        return false;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS inconsistente, apagando particao...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar NVS para reset one-shot: %s", esp_err_to_name(ret));
        return false;
    }

    nvs_handle_t h;
    ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao abrir namespace NVS '%s': %s", NVS_NS, esp_err_to_name(ret));
        return false;
    }

    char stored_marker[96] = {0};
    size_t stored_marker_len = sizeof(stored_marker);
    ret = nvs_get_str(h, NVS_KEY_RESET_MARKER, stored_marker, &stored_marker_len);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "One-shot marker atual: %s", marker);
        ESP_LOGI(TAG, "One-shot marker salvo:  %s", stored_marker);
    }

    if (ret == ESP_OK && strcmp(stored_marker, marker) == 0) {
        ESP_LOGI(TAG, "Reset one-shot ja aplicado para este firmware.");
        nvs_close(h);
        return false;
    }

    esp_err_t erase_ssid_ret = nvs_erase_key(h, NVS_KEY_SSID);
    esp_err_t erase_pass_ret = nvs_erase_key(h, NVS_KEY_PASS);

    if ((erase_ssid_ret != ESP_OK && erase_ssid_ret != ESP_ERR_NVS_NOT_FOUND) ||
        (erase_pass_ret != ESP_OK && erase_pass_ret != ESP_ERR_NVS_NOT_FOUND)) {
        ESP_LOGE(TAG, "Falha ao apagar chaves NVS (ssid=%s, password=%s)",
                 esp_err_to_name(erase_ssid_ret), esp_err_to_name(erase_pass_ret));
        nvs_close(h);
        return false;
    }

    ret = nvs_set_str(h, NVS_KEY_RESET_MARKER, marker);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao salvar marker one-shot no NVS: %s", esp_err_to_name(ret));
        nvs_close(h);
        return false;
    }

    ret = nvs_commit(h);
    nvs_close(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao confirmar reset one-shot no NVS: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "Reset one-shot aplicado. Credenciais limpas para marker: %s", marker);
    return true;
}
