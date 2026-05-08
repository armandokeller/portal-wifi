#include "esp_log.h"
#include "esp_app_desc.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_portal.h"

#define RESET_WIFI 1

static void get_firmware_marker(char *out, size_t out_len)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (out == NULL || out_len < 65 || app_desc == NULL) {
        if (out != NULL && out_len > 0) {
            out[0] = '\0';
        }
        return;
    }

    for (int i = 0; i < 32; i++) {
        snprintf(&out[i * 2], 3, "%02x", app_desc->app_elf_sha256[i]);
    }
    out[64] = '\0';
}

static const char *TAG = "main";

void app_main(void)
{
    // Altere o nome da rede temporaria no arquivo wifi_portal.h para evitar conflitos

    #if RESET_WIFI
    // Aplica reset de credenciais uma vez por build com a flag ativa.
    char reset_marker[65];
    get_firmware_marker(reset_marker, sizeof(reset_marker));
    if (wifi_portal_reset_credentials_oneshot(reset_marker)) {
        ESP_LOGI(TAG, "RESET_WIFI one-shot aplicado neste boot");
    }
    #endif

    // Inicializa o Portal Wi-Fi (NVS já é tratado internamente agora)
    wifi_portal_init();

    // Lógica principal simplificada
    while (1) {
        if (wifi_portal_is_connected()) {
            if (wifi_portal_has_internet()) {
                ESP_LOGI(TAG, "Conectado: Online");
            } else {
                ESP_LOGW(TAG, "Conectado: Sem Internet");
            }
        } else {
            ESP_LOGI(TAG, "Portal Ativo: Aguardando Wi-Fi...");
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
