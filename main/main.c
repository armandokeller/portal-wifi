#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_portal.h"

static const char *TAG = "main";

void app_main(void)
{
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
