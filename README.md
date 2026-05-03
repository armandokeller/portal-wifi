# Base para portal Wi-Fi para ESP32 usando ESP-IDF

Este projeto demonstra como criar um [portal Wi-Fi](https://en.wikipedia.org/wiki/Captive_portal) usando o ESP32 e o framework ESP-IDF. O portal Wi-Fi permite que os usuários se conectem a um ponto de acesso criado pelo ESP32 para configurar as credenciais de Wi-Fi. Pode servir de base para projetos que precisam de uma configuração inicial de rede sem fio, como dispositivos IoT.

## Funcionalidades
- Criação de um ponto de acesso (AP) para configuração.
- Interface web para inserir as credenciais de Wi-Fi.
- Armazenamento seguro das credenciais usando NVS (Non-Volatile Storage).
- Verificação da conexão com a internet.
- Opção para resetar as credenciais de Wi-Fi e reiniciar o dispositivo.
- Suporte para múltiplas tentativas de conexão e feedback ao usuário.
- Implementação de um sistema de timeout para o portal AP, garantindo que ele não fique ativo indefinidamente.

## Estrutura do Projeto
- `main/`: Contém o código-fonte principal do projeto.
  - `main.c`: Ponto de entrada do aplicativo, gerencia o ciclo de vida do portal Wi-Fi.
  - `wifi_portal.c`: Implementação das funções relacionadas ao portal Wi-Fi, incluindo a criação do AP, interface web e gerenciamento de credenciais.
  - `wifi_portal.h`: Declaração das funções e definições relacionadas ao portal Wi-Fi.

## Funções Principais
A biblioteca `wifi_portal` oferece as seguintes funções principais:
- `wifi_portal_init()`: Inicializa o portal Wi-Fi, configurando o AP e preparando a interface web.
- `wifi_portal_has_internet()`: Verifica se o dispositivo está conectado à internet.
- `wifi_portal_reset_credentials()`: Apaga as credenciais de Wi-Fi salvas no NVS e reinicia o dispositivo.

Para alterar o SSID do portal AP, basta modificar a definição `SSID_PORTAL` no arquivo `wifi_portal.h`.


## Exemplo de Uso
No arquivo `main.c`, o portal Wi-Fi é inicializado e o dispositivo tenta se conectar à rede Wi-Fi usando as credenciais salvas. Se a conexão falhar, o portal AP é ativado para permitir que o usuário configure as credenciais.

```c
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_portal.h"

static const char *TAG = "main";

void app_main(void)
{
    // Inicializa o Portal Wi-Fi
    wifi_portal_init();

    // Lógica principal
    while (1) {
        if (wifi_portal_is_connected()) { // Verifica se está conectado à Wi-Fi
            if (wifi_portal_has_internet()) { // Verifica se tem acesso à internet
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
```

### TODO
 - [ ] Carregar o HTML a partir de um arquivo html armazenado no SPIFFS(Sistema de Arquivos) ou embutido como recurso, em vez de usar uma string literal para facilitar a manutenção e personalização da interface web.
