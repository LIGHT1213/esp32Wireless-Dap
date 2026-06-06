#include "wdap_protocol.h"

#include <stdio.h>
#include <string.h>
#include "esp_crc.h"

uint32_t wdap_protocol_crc32(const wdap_hdr_t *hdr, const uint8_t *payload)
{
    wdap_hdr_t tmp;
    memcpy(&tmp, hdr, sizeof(tmp));
    tmp.crc32 = 0;
    uint32_t crc = esp_crc32_le(UINT32_MAX, (const uint8_t *)&tmp, sizeof(tmp));
    if (payload != NULL && tmp.payload_len > 0) {
        crc = esp_crc32_le(crc, payload, tmp.payload_len);
    }
    return crc;
}

bool wdap_protocol_validate(const wdap_hdr_t *hdr, const uint8_t *payload, size_t frame_len)
{
    if (hdr == NULL || frame_len < sizeof(wdap_hdr_t)) {
        return false;
    }
    if (hdr->magic != WDAP_MAGIC || hdr->version != WDAP_VERSION || hdr->header_len != sizeof(wdap_hdr_t)) {
        return false;
    }
    if ((size_t)hdr->payload_len + sizeof(wdap_hdr_t) != frame_len) {
        return false;
    }
    return wdap_protocol_crc32(hdr, payload) == hdr->crc32;
}

void wdap_protocol_format_mac(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
