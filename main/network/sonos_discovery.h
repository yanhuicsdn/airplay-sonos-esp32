// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "esp_err.h"
#include "lwip/inet.h"

#define SONOS_ID_LEN 32

typedef struct {
  struct in_addr addr;
  char id[SONOS_ID_LEN];
} sonos_target_t;

esp_err_t sonos_load_target(sonos_target_t *target);
esp_err_t sonos_set_target(const char *ip);
esp_err_t sonos_resolve_target(sonos_target_t *target);
