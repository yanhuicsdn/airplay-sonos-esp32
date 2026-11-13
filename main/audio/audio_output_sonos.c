// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio_output.h"

#include "audio_receiver.h"
#include "audio_resample.h"
#include "esp_aac_enc.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "playback_events.h"
#include "rtsp_server.h"
#include "sonos_discovery.h"
#include "wifi.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define TAG "sonos"
#define PORT 8080
#define RATE CONFIG_OUTPUT_SAMPLE_RATE_HZ
#define CHANNELS 2
#define PCM_BLOCK 352
#define MAX_RENDER_FRAMES 810
#define AAC_CAP 2048
#define ENCODER_STACK_BYTES (56 * 1024)
#define QUEUE_FRAMES 64
#define PCM_QUEUE_BLOCKS 24
#define SERVICE "urn:schemas-upnp-org:service:AVTransport:1"
#define CONTROL_PATH ":1400/MediaRenderer/AVTransport/Control"
#define STREAM_PATH "/live.aac"
#define RADIO_PREFIX "x-rincon-mp3radio://"

typedef struct {
  unsigned generation;
  size_t size;
  uint8_t data[AAC_CAP];
} aac_frame_t;

typedef struct {
  unsigned generation;
  int rate;
  int16_t samples[PCM_BLOCK * CHANNELS];
} pcm_block_t;

typedef struct {
  bool playing;
  unsigned session;
} transport_state_t;

static QueueHandle_t s_pcm;
static QueueHandle_t s_audio;
static QueueHandle_t s_control;
static TaskHandle_t s_renderer;
static TaskHandle_t s_encoding_task;
static TaskHandle_t s_http;
static TaskHandle_t s_controller;
static atomic_bool s_playing;
static atomic_uint s_generation = 1;
static atomic_uint s_session = 1;
static atomic_uint s_target_addr;
static atomic_int s_source_rate = 44100;
static atomic_uint s_pcm_drops;
static atomic_uint s_aac_drops;
static atomic_uint s_encode_errors;
static void *s_encoder;
static int16_t *s_input;
static int s_input_bytes;
static int s_output_bytes;

static int transport(const char *ip, const char *action, const char *args) {
  char url[100], soap_action[112], body[1220];
  snprintf(url, sizeof(url), "http://%s%s", ip, CONTROL_PATH);
  snprintf(soap_action, sizeof(soap_action), "\"%s#%s\"", SERVICE, action);
  const int size = snprintf(body, sizeof(body),
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
      "<u:%s xmlns:u=\"%s\"><InstanceID>0</InstanceID>%s</u:%s>"
      "</s:Body></s:Envelope>", action, SERVICE, args, action);
  if (size < 0 || (size_t)size >= sizeof(body)) return -1;
  const esp_http_client_config_t config = {
      .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 5000,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return -1;
  esp_http_client_set_header(client, "Content-Type", "text/xml; charset=\"utf-8\"");
  esp_http_client_set_header(client, "SOAPACTION", soap_action);
  esp_http_client_set_post_field(client, body, size);
  const esp_err_t error = esp_http_client_perform(client);
  const int status = error == ESP_OK ? esp_http_client_get_status_code(client) : -1;
  esp_http_client_cleanup(client);
  ESP_LOGI(TAG, "%s HTTP %d", action, status);
  return status;
}

static int set_stream_uri(const char *target) {
  char ip[16], uri[90], args[810];
  if (wifi_get_ip_str(ip, sizeof(ip)) != ESP_OK) return -1;
  snprintf(uri, sizeof(uri), RADIO_PREFIX "http://%s:%d" STREAM_PATH, ip, PORT);
  snprintf(args, sizeof(args),
      "<CurrentURI>%s</CurrentURI><CurrentURIMetaData>"
      "&lt;DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
      "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\"&gt;"
      "&lt;item id=\"1\" parentID=\"0\" restricted=\"1\"&gt;"
      "&lt;dc:title&gt;AirPlay ESP32&lt;/dc:title&gt;"
      "&lt;upnp:class&gt;object.item.audioItem.audioBroadcast&lt;/upnp:class&gt;"
      "&lt;res protocolInfo=\"http-get:*:audio/aac:*\"&gt;%s&lt;/res&gt;"
      "&lt;/item&gt;&lt;/DIDL-Lite&gt;</CurrentURIMetaData>", uri, uri);
  return transport(target, "SetAVTransportURI", args);
}

static void control_task(void *arg) {
  char active_ip[INET_ADDRSTRLEN] = "";
  transport_state_t state;
  TickType_t wait = pdMS_TO_TICKS(2000);
  for (;;) {
    if (xQueueReceive(s_control, &state, wait) != pdTRUE) {
      sonos_target_t target;
      if (sonos_load_target(&target) == ESP_OK && !target.id[0]) {
        sonos_resolve_target(&target);
      }
      wait = portMAX_DELAY;
      continue;
    }
    wait = portMAX_DELAY;
    if (state.session != atomic_load(&s_session)) continue;
    if (active_ip[0]) {
      atomic_store(&s_target_addr, 0);
      transport(active_ip, "Stop", "");
      active_ip[0] = '\0';
    }
    if (!state.playing || state.session != atomic_load(&s_session)) continue;

    sonos_target_t target;
    const esp_err_t err = sonos_resolve_target(&target);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Sonos unavailable or ambiguous (%s). Check the setup page.",
               esp_err_to_name(err));
      continue;
    }
    char ip[INET_ADDRSTRLEN];
    if (state.session != atomic_load(&s_session) ||
        !inet_ntop(AF_INET, &target.addr, ip, sizeof(ip))) continue;
    atomic_store(&s_target_addr, target.addr.s_addr);
    if (set_stream_uri(ip) == 200 && state.session == atomic_load(&s_session) &&
        transport(ip, "Play", "<Speed>1</Speed>") == 200) {
      memcpy(active_ip, ip, strlen(ip) + 1);
    } else {
      atomic_store(&s_target_addr, 0);
    }
  }
}

static void on_playback(playback_source_t source, playback_event_t event,
                        const playback_event_data_t *data, void *ctx) {
  if (source != PLAYBACK_SOURCE_AIRPLAY) return;
  if (event != PLAYBACK_EVENT_PLAYING && event != PLAYBACK_EVENT_PAUSED &&
      event != PLAYBACK_EVENT_DISCONNECTED) return;

  const bool playing = event == PLAYBACK_EVENT_PLAYING;
  if (atomic_exchange(&s_playing, playing) == playing) return;
  const transport_state_t state = {
      .playing = playing,
      .session = atomic_fetch_add(&s_session, 1) + 1,
  };
  atomic_fetch_add(&s_generation, 1);
  xQueueOverwrite(s_control, &state);
}

static bool send_all(int fd, const void *buf, size_t length) {
  const uint8_t *cursor = buf;
  while (length) {
    const ssize_t count = send(fd, cursor, length, 0);
    if (count <= 0) return false;
    cursor += count;
    length -= count;
  }
  return true;
}

static void serve_stream(int fd, const struct sockaddr_in *peer) {
  if (peer->sin_addr.s_addr != atomic_load(&s_target_addr)) return;
  const struct timeval timeout = { .tv_sec = 2 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  char request[2048] = {0};
  size_t used = 0;
  while (!strstr(request, "\r\n\r\n")) {
    if (used == sizeof(request) - 1) return;
    const ssize_t count = recv(fd, request + used, sizeof(request) - 1 - used, 0);
    if (count <= 0) return;
    used += count;
    request[used] = 0;
  }
  const bool get = strncmp(request, "GET " STREAM_PATH " ", sizeof("GET " STREAM_PATH " ") - 1) == 0;
  const bool head = strncmp(request, "HEAD " STREAM_PATH " ", sizeof("HEAD " STREAM_PATH " ") - 1) == 0;
  if (!get && !head) return;

  const unsigned session = atomic_load(&s_session);
  if (!atomic_load(&s_playing) ||
      peer->sin_addr.s_addr != atomic_load(&s_target_addr)) return;
  static const char headers[] =
      "HTTP/1.0 200 OK\r\nContent-Type: audio/aac\r\n"
      "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
  if (!send_all(fd, headers, sizeof(headers) - 1) || head) return;
  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof(ip));
  ESP_LOGI(TAG, "Audio GET from %s", ip);
  unsigned long sent = 0;
  int64_t report_at = esp_timer_get_time() + 10000000;
  unsigned pcm_drops = atomic_load(&s_pcm_drops);
  unsigned aac_drops = atomic_load(&s_aac_drops);
  unsigned encode_errors = atomic_load(&s_encode_errors);
  while (atomic_load(&s_playing) && atomic_load(&s_session) == session &&
         peer->sin_addr.s_addr == atomic_load(&s_target_addr)) {
    aac_frame_t frame;
    if (xQueueReceive(s_audio, &frame, pdMS_TO_TICKS(200)) != pdTRUE) continue;
    if (frame.generation != atomic_load(&s_generation)) continue;
    if (!send_all(fd, frame.data, frame.size)) break;
    sent += frame.size;
    const int64_t now = esp_timer_get_time();
    if (now >= report_at) {
      const unsigned new_pcm_drops = atomic_load(&s_pcm_drops);
      const unsigned new_aac_drops = atomic_load(&s_aac_drops);
      const unsigned new_encode_errors = atomic_load(&s_encode_errors);
      ESP_LOGI(TAG, "Stream bytes=%lu PCM drops=%u AAC drops=%u encoder errors=%u",
               sent, new_pcm_drops - pcm_drops, new_aac_drops - aac_drops,
               new_encode_errors - encode_errors);
      pcm_drops = new_pcm_drops;
      aac_drops = new_aac_drops;
      encode_errors = new_encode_errors;
      report_at = now + 10000000;
    }
  }
  ESP_LOGI(TAG, "Audio sent %lu bytes to %s", sent, ip);
}

static void http_task(void *arg) {
  const int server = (int)(intptr_t)arg;
  for (;;) {
    struct sockaddr_in peer;
    socklen_t length = sizeof(peer);
    const int fd = accept(server, (struct sockaddr *)&peer, &length);
    if (fd >= 0) {
      serve_stream(fd, &peer);
      close(fd);
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

static void encode_pcm(const int16_t *pcm, size_t bytes, unsigned generation,
                       size_t *used) {
  const uint8_t *source = (const uint8_t *)pcm;
  while (bytes) {
    const size_t room = (size_t)s_input_bytes - *used;
    const size_t count = bytes < room ? bytes : room;
    memcpy((uint8_t *)s_input + *used, source, count);
    *used += count;
    source += count;
    bytes -= count;
    if (*used != (size_t)s_input_bytes) continue;

    aac_frame_t frame = { .generation = generation };
    esp_audio_enc_in_frame_t input = {
        .buffer = (uint8_t *)s_input, .len = s_input_bytes,
    };
    esp_audio_enc_out_frame_t output = {
        .buffer = frame.data, .len = s_output_bytes,
    };
    if (esp_aac_enc_process(s_encoder, &input, &output) == ESP_AUDIO_ERR_OK &&
        output.encoded_bytes > 0 && output.encoded_bytes <= AAC_CAP) {
      frame.size = output.encoded_bytes;
      if (xQueueSend(s_audio, &frame, 0) != pdTRUE) {
        atomic_fetch_add(&s_aac_drops, 1);
        aac_frame_t stale;
        xQueueReceive(s_audio, &stale, 0);
        xQueueSend(s_audio, &frame, 0);
      }
    } else {
      atomic_fetch_add(&s_encode_errors, 1);
    }
    *used = 0;
  }
}

static void encoding_task(void *arg) {
  pcm_block_t block;
  int16_t converted[MAX_RENDER_FRAMES * CHANNELS];
  int rate = 0;
  unsigned generation = 0;
  size_t used = 0;
  for (;;) {
    xQueueReceive(s_pcm, &block, portMAX_DELAY);
    if (block.generation != atomic_load(&s_generation)) continue;
    if (block.rate != rate || block.generation != generation) {
      rate = block.rate;
      generation = block.generation;
      used = 0;
      if (!audio_resample_init(rate, RATE, CHANNELS)) {
        rate = 0;
        atomic_fetch_add(&s_encode_errors, 1);
        continue;
      }
      const esp_audio_err_t reset = esp_aac_enc_reset(s_encoder);
      if (reset != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "AAC encoder reset returned %d", reset);
      }
    }
    if (rate == RATE) {
      encode_pcm(block.samples, sizeof(block.samples), generation, &used);
    } else {
      const size_t samples =
          audio_resample_process(block.samples, PCM_BLOCK, converted, MAX_RENDER_FRAMES);
      encode_pcm(converted, samples * CHANNELS * sizeof(int16_t), generation, &used);
    }
  }
}

static void render_task(void *arg) {
  pcm_block_t block;
  int64_t next = 0;
  uint32_t remainder = 0;
  unsigned generation = 0;
  for (;;) {
    const int64_t now = esp_timer_get_time();
    if (!atomic_load(&s_playing)) {
      next = 0;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    block.generation = atomic_load(&s_generation);
    block.rate = atomic_load(&s_source_rate);
    if (!next || block.generation != generation || now - next > 100000) {
      next = now;
      remainder = 0;
      generation = block.generation;
    }
    if (now < next) {
      vTaskDelay(1);
      continue;
    }

    const size_t frames = audio_receiver_read_at(block.samples, PCM_BLOCK, next);
    memset(block.samples + frames * CHANNELS, 0,
           (PCM_BLOCK - frames) * CHANNELS * sizeof(int16_t));
    const int32_t gain = airplay_get_volume_q15();
    for (size_t i = 0; i < PCM_BLOCK * CHANNELS; ++i) {
      block.samples[i] = (int16_t)((int32_t)block.samples[i] * gain / 32768);
    }
    if (xQueueSend(s_pcm, &block, 0) != pdTRUE) {
      atomic_fetch_add(&s_pcm_drops, 1);
      pcm_block_t stale;
      xQueueReceive(s_pcm, &stale, 0);
      xQueueSend(s_pcm, &block, 0);
    }
    const uint32_t ticks = PCM_BLOCK * 1000000U + remainder;
    next += ticks / block.rate;
    remainder = ticks % block.rate;
  }
}

esp_err_t audio_output_init(void) {
  esp_aac_enc_config_t config = {
      .sample_rate = RATE, .channel = CHANNELS, .bits_per_sample = 16,
      .bitrate = 96000, .adts_used = true,
  };
  if (esp_aac_enc_open(&config, sizeof(config), &s_encoder) != ESP_AUDIO_ERR_OK ||
      esp_aac_enc_get_frame_size(s_encoder, &s_input_bytes, &s_output_bytes) != ESP_AUDIO_ERR_OK ||
      s_input_bytes <= 0 || s_output_bytes <= 0 || s_output_bytes > AAC_CAP) {
    if (s_encoder) esp_aac_enc_close(s_encoder);
    return ESP_FAIL;
  }
  s_input = heap_caps_malloc(s_input_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s_pcm = xQueueCreateWithCaps(PCM_QUEUE_BLOCKS, sizeof(pcm_block_t), MALLOC_CAP_SPIRAM);
  s_audio = xQueueCreateWithCaps(QUEUE_FRAMES, sizeof(aac_frame_t), MALLOC_CAP_SPIRAM);
  s_control = xQueueCreate(1, sizeof(transport_state_t));

  const struct sockaddr_in addr = {
      .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = INADDR_ANY,
  };
  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  const int reuse = 1;
  if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  if (!s_input || !s_pcm || !s_audio || !s_control || fd < 0 ||
      bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 2) < 0) {
    goto fail;
  }
  // Leave core 0 to buffered AirPlay ingest and decoding. The render task
  // preempts the encoder on core 1 to keep the PCM clock steady.
  if (xTaskCreatePinnedToCore(encoding_task, "sonos_aac", ENCODER_STACK_BYTES,
                             NULL, 8, &s_encoding_task, 1) != pdPASS ||
      xTaskCreate(http_task, "sonos_http", 6144, (void *)(intptr_t)fd, 5, &s_http) != pdPASS ||
      xTaskCreate(control_task, "sonos_control", 6144, NULL, 5, &s_controller) != pdPASS ||
      playback_events_register(on_playback, NULL) != 0) {
    goto fail;
  }
  ESP_LOGI(TAG, "AAC HTTP output ready on port %d", PORT);
  return ESP_OK;

fail:
  if (s_encoding_task) vTaskDelete(s_encoding_task);
  if (s_http) vTaskDelete(s_http);
  if (s_controller) vTaskDelete(s_controller);
  if (fd >= 0) close(fd);
  if (s_audio) vQueueDeleteWithCaps(s_audio);
  if (s_pcm) vQueueDeleteWithCaps(s_pcm);
  if (s_control) vQueueDelete(s_control);
  free(s_input);
  esp_aac_enc_close(s_encoder);
  return ESP_FAIL;
}

void audio_output_start(void) {
  if (!s_renderer && xTaskCreatePinnedToCore(render_task, "sonos_render", 8192, NULL,
        AUDIO_PLAYBACK_TASK_PRIORITY, &s_renderer, 1) != pdPASS) {
    ESP_LOGE(TAG, "Could not start AAC renderer");
  }
}

void audio_output_flush(void) {
  atomic_fetch_add(&s_generation, 1);
}

void audio_output_set_source_rate(int rate) {
  if (rate >= 24000 && rate <= 48000) atomic_store(&s_source_rate, rate);
}

void audio_output_set_sample_rate(uint32_t rate) {
  audio_output_set_source_rate((int)rate);
}

uint32_t audio_output_get_hardware_latency_us(void) {
  return 0;
}

esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait) {
  return ESP_ERR_NOT_SUPPORTED;
}
