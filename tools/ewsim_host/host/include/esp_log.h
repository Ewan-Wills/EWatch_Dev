#pragma once
#include <stdint.h>
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
#ifdef __cplusplus
extern "C" {
#endif
void esp_log_level_set(const char *tag, esp_log_level_t level);
esp_log_level_t esp_log_level_get(const char *tag);
#ifdef __cplusplus
}
#endif
#define ESP_LOGE(tag, ...) do {} while (0)
#define ESP_LOGW(tag, ...) do {} while (0)
#define ESP_LOGI(tag, ...) do {} while (0)
#define ESP_LOGD(tag, ...) do {} while (0)
#define ESP_LOGV(tag, ...) do {} while (0)
