// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stddef.h>

#include "esp_err.h"

// ip must have space for an IPv4 address including its terminator.
esp_err_t sonos_get_target(char *ip, size_t len);
esp_err_t sonos_set_target(const char *ip);
