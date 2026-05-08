#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

#include "esp_err.h"
#include <stdbool.h>


#define SSID_PORTAL "ESP-CONFIG"

/**
 * @brief Inicializa o sistema de Wi-Fi com Portal Cativo.
 * Tenta conectar às credenciais salvas no NVS. Se falhar ou não houver, 
 * abre o portal "ESP-CONFIG" para configuração.
 */
void wifi_portal_init(void);

/**
 * @brief Verifica se o dispositivo está conectado à rede Wi-Fi (recebeu IP).
 * @return true Se estiver conectado
 */
bool wifi_portal_is_connected(void);

/**
 * @brief Verifica se há conectividade real com a Internet (via HTTP Ping).
 * @return true Se conseguiu acessar o servidor de teste
 */
bool wifi_portal_has_internet(void);

/**
 * @brief Apaga as credenciais de Wi-Fi salvas no NVS.
 * Esta função pode ser chamada antes de wifi_portal_init().
 * Nao reinicia o dispositivo.
 */
void wifi_portal_reset_credentials(void);

/**
 * @brief Apaga credenciais apenas uma vez por marcador de firmware.
 *
 * Quando o marker ainda nao foi aplicado, apaga ssid/password e salva o marker no NVS.
 * Nos proximos boots com o mesmo marker, nao apaga novamente.
 *
 * @param marker Identificador do build/firmware (ex.: data e hora de compilacao).
 * @return true Se o reset foi aplicado neste boot.
 * @return false Se ja havia sido aplicado para o marker informado ou em caso de erro.
 */
bool wifi_portal_reset_credentials_oneshot(const char *marker);

/**
 * @brief Inicia o console de configuração (Portal AP) manualmente se necessário.
 */
void wifi_portal_start_config(void);

#endif // WIFI_PORTAL_H
