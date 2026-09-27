#ifndef ULV_DECODER_PLUGIN_H
#define ULV_DECODER_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define ULV_PLUGIN_API __declspec(dllexport)
#else
#define ULV_PLUGIN_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ULV_PLUGIN_ABI_VERSION 2u

typedef struct ulv_point_v1 {
  float x;
  float y;
  float z;
  float intensity;
  uint16_t ring;
  uint8_t return_type;
  uint8_t flags;
} ulv_point_v1;

#ifdef __cplusplus
static_assert(sizeof(ulv_point_v1) == 20, "ulv_point_v1 must match the browser wire point");
#endif

typedef struct ulv_frame_v1 {
  uint32_t struct_size;
  uint32_t device_id;
  uint64_t sequence;
  uint64_t timestamp_ns;
  const char* vendor;
  const char* model;
  const ulv_point_v1* points;
  size_t point_count;
} ulv_frame_v1;

typedef void (*ulv_frame_callback_v1)(const ulv_frame_v1* frame, void* user_data);

typedef struct ulv_udp_packet_v1 {
  uint32_t struct_size;
  const char* source_ip;
  uint16_t source_port;
  uint16_t destination_port;
  uint64_t receive_timestamp_ns;
  const uint8_t* payload;
  size_t payload_size;
} ulv_udp_packet_v1;

/* Every vendor adapter exports this small, stable C ABI. The host owns packet
 * and callback memory; an adapter must copy anything it retains. */
ULV_PLUGIN_API uint32_t ulv_plugin_abi_version(void);
ULV_PLUGIN_API const char* ulv_plugin_manifest_json(void);
ULV_PLUGIN_API void* ulv_plugin_create(const char* config_json,
                                       ulv_frame_callback_v1 callback,
                                       void* user_data);
ULV_PLUGIN_API int ulv_plugin_feed_udp(void* instance,
                                      const ulv_udp_packet_v1* packet);
ULV_PLUGIN_API void ulv_plugin_destroy(void* instance);

/* Optional ABI-v2 diagnostic hook. Hosts must probe for the symbol so older
 * third-party plugins remain compatible. The returned JSON is plugin-owned. */
ULV_PLUGIN_API const char* ulv_plugin_instance_status_json(void* instance);

/* Optional high-rate hook. Hosts must probe for this symbol and fall back to
 * ulv_plugin_feed_udp. It is useful for MEMS sensors that emit many small
 * packets; every element has the same ownership rules as the scalar call. */
ULV_PLUGIN_API int ulv_plugin_feed_udp_batch(void* instance,
                                            const ulv_udp_packet_v1* packets,
                                            size_t packet_count);

/* Optional fixed-size contiguous batch hook for very high packet rates. */
ULV_PLUGIN_API int ulv_plugin_feed_udp_payload_batch(
    void* instance,
    const char* source_ip,
    uint16_t source_port,
    uint16_t destination_port,
    uint64_t receive_timestamp_ns,
    const uint8_t* payloads,
    size_t payload_size,
    size_t packet_count);

#ifdef __cplusplus
}
#endif

#endif
