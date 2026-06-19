/*
 * web_service.h — Companion API minima (Layer 2, Etapa 15.1)
 *
 * Expõe um servidor HTTP local após NB_EVT_WIFI_IP_ACQUIRED:
 *   GET  /api/status — JSON: state, expression, attention, health, uptime, fps
 *   GET  /api/persona — JSON: warmth, energy, curiosity, trust
 *   GET  /api/camera/status — JSON: camera, bloqueio de bridge e heap
 *   GET  /api/camera/snapshot — JPEG QQVGA sob demanda, se camera pronta
 *   GET  /api/config  — JSON com todas as chaves NVS relevantes
 *   POST /api/config  — Atualiza chave NVS (body: {"key":"vol","value":50})
 *   POST /api/command — Injeta ação (body: {"type":"ACTION","value":"GREET"})
 *
 * Sem autenticação — LAN local, sem exposição externa.
 * Dashboard rico roda no bridge; firmware mantém apenas APIs REST leves.
 * Máximo 3 conexões HTTP simultâneas.
 *
 * Inicia somente após NB_EVT_WIFI_IP_ACQUIRED — nunca bloqueia o boot.
 */

#ifndef NB_WEB_SERVICE_H
#define NB_WEB_SERVICE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Registra subscriptions de evento e prepara o serviço.
 *
 * O servidor HTTP só inicia após NB_EVT_WIFI_IP_ACQUIRED.
 * Sem WiFi: sem efeito, sistema opera normalmente.
 *
 * Deve ser chamado em PHASE_SERVICES após event_bus_init().
 *
 * @return ESP_OK sempre.
 */
esp_err_t web_service_init(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_WEB_SERVICE_H */
