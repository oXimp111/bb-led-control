#pragma once
#include <cstdint>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 0x105
typedef struct { char label[17]; } esp_partition_t;

esp_err_t esp_partition_get_sha256(const esp_partition_t* p, uint8_t* sha);
