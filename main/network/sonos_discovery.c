// SPDX-License-Identifier: GPL-3.0-or-later

#include "sonos_discovery.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "wifi.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ZONE_PLAYER "urn:schemas-upnp-org:device:ZonePlayer:1"
#define DESCRIPTION ":1400/xml/device_description.xml"
#define SSDP_GROUP "239.255.255.250"
#define DESCRIPTION_LIMIT 16384
#define CANDIDATE_LIMIT 12
#define SETTINGS_NAMESPACE "sonos"
#define TARGET_KEY "target"
#define LEGACY_IP_KEY "target_ip"

static pthread_mutex_t s_settings_lock = PTHREAD_MUTEX_INITIALIZER;

static bool valid_address(struct in_addr address) {
  unsigned first = ntohl(address.s_addr) >> 24;
  return first > 0 && first != 127 && first < 224;
}

static bool valid_id(const char *id, size_t size) {
  static const char prefix[] = "uuid:RINCON_";
  const size_t prefix_len = sizeof(prefix) - 1;
  if (size <= prefix_len || size >= SONOS_ID_LEN ||
      memcmp(id, prefix, prefix_len) != 0) return false;
  return strspn(id + prefix_len, "0123456789ABCDEFabcdef") == size - prefix_len;
}

static esp_err_t load_target(sonos_target_t *target) {
  memset(target, 0, sizeof(*target));
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;

  size_t size = sizeof(*target);
  err = nvs_get_blob(nvs, TARGET_KEY, target, &size);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    err = nvs_get_u32(nvs, LEGACY_IP_KEY, &target->addr.s_addr);
  } else if (err == ESP_OK && (size != sizeof(*target) ||
                             !memchr(target->id, 0, sizeof(target->id)))) {
    err = ESP_ERR_INVALID_SIZE;
  }
  nvs_close(nvs);
  return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t sonos_load_target(sonos_target_t *target) {
  pthread_mutex_lock(&s_settings_lock);
  esp_err_t err = load_target(target);
  pthread_mutex_unlock(&s_settings_lock);
  return err;
}

static esp_err_t save_target(const sonos_target_t *target) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(nvs, TARGET_KEY, target, sizeof(*target));
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}

static bool xml_field(const char *xml, const char *tag, char *out, size_t size) {
  char open[32], close[32];
  snprintf(open, sizeof(open), "<%s>", tag);
  snprintf(close, sizeof(close), "</%s>", tag);
  const char *start = strstr(xml, open);
  if (!start) return false;
  start += strlen(open);
  const char *end = strstr(start, close);
  if (!end || end == start || (size_t)(end - start) >= size) return false;
  memcpy(out, start, end - start);
  out[end - start] = '\0';
  return true;
}

static bool describe(struct in_addr addr, char id[SONOS_ID_LEN], bool play3_only) {
  char ip[INET_ADDRSTRLEN], url[80];
  if (!valid_address(addr) || !inet_ntop(AF_INET, &addr, ip, sizeof(ip))) return false;
  snprintf(url, sizeof(url), "http://%s" DESCRIPTION, ip);
  const esp_http_client_config_t config = {
      .url = url,
      .timeout_ms = 700,
      .disable_auto_redirect = true,
  };
  char *xml = heap_caps_malloc(DESCRIPTION_LIMIT + 1, MALLOC_CAP_SPIRAM);
  if (!xml) return false;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  int size = -1;
  if (client && esp_http_client_open(client, 0) == ESP_OK &&
      esp_http_client_fetch_headers(client) >= 0 &&
      esp_http_client_get_status_code(client) == 200) {
    size = esp_http_client_read_response(client, xml, DESCRIPTION_LIMIT);
  }
  if (client) esp_http_client_cleanup(client);
  bool found = false;
  if (size > 0) {
    xml[size] = '\0';
    char *nested = strstr(xml, "<deviceList>");
    if (nested) *nested = '\0';
    char type[64], manufacturer[32], model[16];
    found = xml_field(xml, "deviceType", type, sizeof(type)) &&
            strcmp(type, ZONE_PLAYER) == 0 &&
            xml_field(xml, "manufacturer", manufacturer, sizeof(manufacturer)) &&
            strncmp(manufacturer, "Sonos", 5) == 0 &&
            xml_field(xml, "UDN", id, SONOS_ID_LEN) && valid_id(id, strlen(id)) &&
            (!play3_only || (xml_field(xml, "modelNumber", model, sizeof(model)) &&
                             strcmp(model, "S3") == 0));
  }
  free(xml);
  return found;
}

esp_err_t sonos_set_target(const char *ip) {
  sonos_target_t target = {0};
  if (ip[0]) {
    if (inet_pton(AF_INET, ip, &target.addr) != 1 || !valid_address(target.addr)) {
      return ESP_ERR_INVALID_ARG;
    }
    char id[SONOS_ID_LEN];
    if (describe(target.addr, id, false)) strcpy(target.id, id);
  }
  pthread_mutex_lock(&s_settings_lock);
  esp_err_t err = save_target(&target);
  pthread_mutex_unlock(&s_settings_lock);
  return err;
}

static bool ssdp_target(const char *response, struct in_addr addr,
                        sonos_target_t *target) {
  if (strncmp(response, "HTTP/1.1 200 ", 13) != 0 || !valid_address(addr)) return false;
  char ip[INET_ADDRSTRLEN], expected[80];
  if (!inet_ntop(AF_INET, &addr, ip, sizeof(ip))) return false;
  snprintf(expected, sizeof(expected), "http://%s" DESCRIPTION, ip);

  bool location_matches = false;
  const char *id = NULL;
  size_t id_len = 0;
  for (const char *line = strstr(response, "\r\n"); line;) {
    line += 2;
    const char *end = strstr(line, "\r\n");
    if (!end || end == line) break;
    if (strncasecmp(line, "LOCATION:", 9) == 0) {
      const char *value = line + 9;
      while (value < end && (*value == ' ' || *value == '\t')) ++value;
      location_matches = (size_t)(end - value) == strlen(expected) &&
                         memcmp(value, expected, end - value) == 0;
    } else if (strncasecmp(line, "USN:", 4) == 0) {
      id = line + 4;
      while (id < end && (*id == ' ' || *id == '\t')) ++id;
      const char *separator = strstr(id, "::");
      id_len = separator && separator < end ? separator - id : end - id;
    }
    line = end;
  }
  if (!location_matches || !id || !id_len || id_len >= sizeof(target->id)) return false;
  memset(target, 0, sizeof(*target));
  memcpy(target->id, id, id_len);
  if (!valid_id(target->id, id_len)) return false;
  target->addr = addr;
  return true;
}

static esp_err_t discover(const char *wanted, sonos_target_t *target) {
  static const char query[] =
      "M-SEARCH * HTTP/1.1\r\nHOST: " SSDP_GROUP ":1900\r\n"
      "MAN: \"ssdp:discover\"\r\nMX: 1\r\nST: " ZONE_PLAYER "\r\n\r\n";

  char ip[INET_ADDRSTRLEN];
  struct in_addr local;
  if (!wifi_is_connected() || wifi_get_ip_str(ip, sizeof(ip)) != ESP_OK ||
      inet_pton(AF_INET, ip, &local) != 1 || !valid_address(local)) return ESP_ERR_INVALID_STATE;
  const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) return ESP_FAIL;

  const struct sockaddr_in from = {.sin_family = AF_INET, .sin_addr = local};
  struct sockaddr_in group = {.sin_family = AF_INET, .sin_port = htons(1900)};
  inet_pton(AF_INET, SSDP_GROUP, &group.sin_addr);
  if (bind(fd, (const struct sockaddr *)&from, sizeof(from)) < 0 ||
      setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &local, sizeof(local)) < 0 ||
      sendto(fd, query, sizeof(query) - 1, 0,
             (const struct sockaddr *)&group, sizeof(group)) < 0) {
    close(fd);
    return ESP_FAIL;
  }

  sonos_target_t candidates[CANDIDATE_LIMIT];
  size_t count = 0;
  const int64_t deadline = esp_timer_get_time() + 1500000;
  while (count < CANDIDATE_LIMIT) {
    int64_t remaining = deadline - esp_timer_get_time();
    if (remaining <= 0) break;
    struct timeval timeout = {
        .tv_sec = remaining / 1000000,
        .tv_usec = remaining % 1000000,
    };
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(fd, &read_set);
    if (select(fd + 1, &read_set, NULL, NULL, &timeout) <= 0) break;

    char response[1536];
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    ssize_t bytes = recvfrom(fd, response, sizeof(response) - 1, 0,
                             (struct sockaddr *)&peer, &peer_len);
    if (bytes <= 0) continue;
    response[bytes] = '\0';
    sonos_target_t candidate;
    if (!ssdp_target(response, peer.sin_addr, &candidate) ||
        (*wanted && strcmp(candidate.id, wanted) != 0)) continue;
    size_t i = 0;
    while (i < count && candidates[i].addr.s_addr != candidate.addr.s_addr) ++i;
    if (i == count) candidates[count++] = candidate;
  }
  close(fd);

  bool found = false;
  for (size_t i = 0; i < count; ++i) {
    char id[SONOS_ID_LEN];
    if (!describe(candidates[i].addr, id, !*wanted) ||
        strcmp(candidates[i].id, id) != 0) continue;
    if (found) return ESP_ERR_INVALID_STATE;
    *target = candidates[i];
    found = true;
  }
  return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t sonos_resolve_target(sonos_target_t *target) {
  sonos_target_t saved;
  esp_err_t err = sonos_load_target(&saved);
  if (err != ESP_OK) return err;
  *target = saved;

  char id[SONOS_ID_LEN];
  if (describe(saved.addr, id, false) && (!saved.id[0] || strcmp(saved.id, id) == 0)) {
    strcpy(target->id, id);
  } else if (saved.id[0] || !saved.addr.s_addr) {
    err = discover(saved.id, target);
    if (err != ESP_OK) return err;
  } else {
    // Preserve a manually configured address until the player can be identified.
    return ESP_OK;
  }

  pthread_mutex_lock(&s_settings_lock);
  sonos_target_t current;
  err = load_target(&current);
  if (err == ESP_OK) {
    if (current.addr.s_addr != saved.addr.s_addr || strcmp(current.id, saved.id) != 0) {
      err = ESP_ERR_INVALID_STATE;
    } else if (target->addr.s_addr != saved.addr.s_addr || strcmp(target->id, saved.id) != 0) {
      err = save_target(target);
    }
  }
  pthread_mutex_unlock(&s_settings_lock);
  return err;
}
