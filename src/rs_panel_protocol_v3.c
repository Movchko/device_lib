#include "rs_panel_protocol_v3.h"
#include <string.h>

static uint16_t rs_v3_crc16_ccitt_update(uint16_t crc, uint8_t b)
{
    uint8_t i;
    crc ^= (uint16_t)b << 8;
    for (i = 0u; i < 8u; i++) {
        if ((crc & 0x8000u) != 0u) {
            crc = (uint16_t)((crc << 1) ^ 0x1021u);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t RsPanelV3_Crc16Ccitt(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i;
    if (data == 0) {
        return crc;
    }
    for (i = 0u; i < len; i++) {
        crc = rs_v3_crc16_ccitt_update(crc, data[i]);
    }
    return crc;
}

uint16_t RsPanelV3_FaultListCrc(const char (*lines)[RS_PANEL_V3_FAULT_TEXT_LEN], uint16_t count)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i;
    if (lines == 0) {
        return crc;
    }
    for (i = 0u; i < count; i++) {
        const char *s = lines[i];
        while (*s != '\0') {
            crc = rs_v3_crc16_ccitt_update(crc, (uint8_t)(*s++));
        }
        crc = rs_v3_crc16_ccitt_update(crc, 0u);
    }
    return crc;
}

static void rs_v3_crc_feed_u8(uint16_t *crc, uint8_t v)
{
    *crc = rs_v3_crc16_ccitt_update(*crc, v);
}

static void rs_v3_crc_feed_u16le(uint16_t *crc, uint16_t v)
{
    rs_v3_crc_feed_u8(crc, (uint8_t)(v & 0xFFu));
    rs_v3_crc_feed_u8(crc, (uint8_t)(v >> 8));
}

static void rs_v3_crc_feed_u32le(uint16_t *crc, uint32_t v)
{
    rs_v3_crc_feed_u8(crc, (uint8_t)(v & 0xFFu));
    rs_v3_crc_feed_u8(crc, (uint8_t)((v >> 8) & 0xFFu));
    rs_v3_crc_feed_u8(crc, (uint8_t)((v >> 16) & 0xFFu));
    rs_v3_crc_feed_u8(crc, (uint8_t)((v >> 24) & 0xFFu));
}

static void rs_v3_crc_feed_i16le(uint16_t *crc, int16_t v)
{
    rs_v3_crc_feed_u16le(crc, (uint16_t)v);
}

static void rs_v3_crc_feed_mcu_key(uint16_t *crc, const RsPanelV3McuKey *k)
{
    if (k == 0) {
        return;
    }
    rs_v3_crc_feed_u8(crc, k->h_adr);
    rs_v3_crc_feed_u8(crc, k->l_adr);
    rs_v3_crc_feed_u32le(crc, k->uid0);
    rs_v3_crc_feed_u32le(crc, k->uid1);
    rs_v3_crc_feed_u32le(crc, k->uid2);
}

uint16_t RsPanelV3_FaultEvtEncodeBody(uint8_t *dst, uint16_t dst_size,
                                      const RsPanelV3FaultEvtItem *item)
{
    uint16_t pos = 0u;
    if (dst == 0 || item == 0) {
        return 0u;
    }
    if (pos >= dst_size) {
        return 0u;
    }
    dst[pos++] = item->code;
    if (pos >= dst_size) {
        return 0u;
    }
    dst[pos++] = item->flags;
    if ((item->flags & RS_PANEL_V3_FAULT_FLAG_HAS_MCU) != 0u) {
        if ((uint16_t)(pos + 14u) > dst_size) {
            return 0u;
        }
        dst[pos++] = item->mcu.h_adr;
        dst[pos++] = item->mcu.l_adr;
        dst[pos++] = (uint8_t)(item->mcu.uid0 & 0xFFu);
        dst[pos++] = (uint8_t)((item->mcu.uid0 >> 8) & 0xFFu);
        dst[pos++] = (uint8_t)((item->mcu.uid0 >> 16) & 0xFFu);
        dst[pos++] = (uint8_t)((item->mcu.uid0 >> 24) & 0xFFu);
        dst[pos++] = (uint8_t)(item->mcu.uid1 & 0xFFu);
        dst[pos++] = (uint8_t)((item->mcu.uid1 >> 8) & 0xFFu);
        dst[pos++] = (uint8_t)((item->mcu.uid1 >> 16) & 0xFFu);
        dst[pos++] = (uint8_t)((item->mcu.uid1 >> 24) & 0xFFu);
        dst[pos++] = (uint8_t)(item->mcu.uid2 & 0xFFu);
        dst[pos++] = (uint8_t)((item->mcu.uid2 >> 8) & 0xFFu);
        dst[pos++] = (uint8_t)((item->mcu.uid2 >> 16) & 0xFFu);
        dst[pos++] = (uint8_t)((item->mcu.uid2 >> 24) & 0xFFu);
    }
    if ((item->flags & RS_PANEL_V3_FAULT_FLAG_HAS_CHANNEL) != 0u) {
        if ((uint16_t)(pos + 2u) > dst_size) {
            return 0u;
        }
        dst[pos++] = item->ch_type;
        dst[pos++] = item->ch_l_adr;
    }
    if ((item->flags & RS_PANEL_V3_FAULT_FLAG_HAS_CAN) != 0u) {
        if (pos >= dst_size) {
            return 0u;
        }
        dst[pos++] = item->can_idx;
    }
    if ((item->flags & RS_PANEL_V3_FAULT_FLAG_HAS_EXTRA) != 0u) {
        if ((uint16_t)(pos + 2u) > dst_size) {
            return 0u;
        }
        dst[pos++] = (uint8_t)((uint16_t)item->extra & 0xFFu);
        dst[pos++] = (uint8_t)(((uint16_t)item->extra >> 8) & 0xFFu);
    }
    if (item->code == (uint8_t)RS_PANEL_V3_FAULT_PANEL_JOURNAL) {
        if (pos >= dst_size) {
            return 0u;
        }
        dst[pos++] = item->panel_addr;
    }
    return pos;
}

uint8_t RsPanelV3_FaultEvtDecodeBody(const uint8_t *src, uint16_t len,
                                     RsPanelV3FaultEvtItem *out)
{
    uint16_t pos = 0u;
    if (src == 0 || out == 0 || len < 2u) {
        return 0u;
    }
    memset(out, 0, sizeof(*out));
    out->code = src[pos++];
    out->flags = src[pos++];
    if ((out->flags & RS_PANEL_V3_FAULT_FLAG_HAS_MCU) != 0u) {
        if ((uint16_t)(pos + 14u) > len) {
            return 0u;
        }
        out->mcu.h_adr = src[pos++];
        out->mcu.l_adr = src[pos++];
        out->mcu.uid0 = (uint32_t)src[pos] | ((uint32_t)src[pos + 1u] << 8) |
                        ((uint32_t)src[pos + 2u] << 16) | ((uint32_t)src[pos + 3u] << 24);
        pos = (uint16_t)(pos + 4u);
        out->mcu.uid1 = (uint32_t)src[pos] | ((uint32_t)src[pos + 1u] << 8) |
                        ((uint32_t)src[pos + 2u] << 16) | ((uint32_t)src[pos + 3u] << 24);
        pos = (uint16_t)(pos + 4u);
        out->mcu.uid2 = (uint32_t)src[pos] | ((uint32_t)src[pos + 1u] << 8) |
                        ((uint32_t)src[pos + 2u] << 16) | ((uint32_t)src[pos + 3u] << 24);
        pos = (uint16_t)(pos + 4u);
    }
    if ((out->flags & RS_PANEL_V3_FAULT_FLAG_HAS_CHANNEL) != 0u) {
        if ((uint16_t)(pos + 2u) > len) {
            return 0u;
        }
        out->ch_type = src[pos++];
        out->ch_l_adr = src[pos++];
    }
    if ((out->flags & RS_PANEL_V3_FAULT_FLAG_HAS_CAN) != 0u) {
        if (pos >= len) {
            return 0u;
        }
        out->can_idx = src[pos++];
    }
    if ((out->flags & RS_PANEL_V3_FAULT_FLAG_HAS_EXTRA) != 0u) {
        if ((uint16_t)(pos + 2u) > len) {
            return 0u;
        }
        out->extra = (int16_t)((uint16_t)src[pos] | ((uint16_t)src[pos + 1u] << 8));
        pos = (uint16_t)(pos + 2u);
    }
    if (out->code == (uint8_t)RS_PANEL_V3_FAULT_PANEL_JOURNAL && pos < len) {
        out->panel_addr = src[pos++];
    }
    return 1u;
}

uint8_t RsPanelV3_FaultEvtEqual(const RsPanelV3FaultEvtItem *a, const RsPanelV3FaultEvtItem *b)
{
    if (a == 0 || b == 0) {
        return 0u;
    }
    if (a->code != b->code || a->flags != b->flags) {
        return 0u;
    }
    if ((a->flags & RS_PANEL_V3_FAULT_FLAG_HAS_MCU) != 0u) {
        if (a->mcu.h_adr != b->mcu.h_adr || a->mcu.l_adr != b->mcu.l_adr ||
            a->mcu.uid0 != b->mcu.uid0 || a->mcu.uid1 != b->mcu.uid1 ||
            a->mcu.uid2 != b->mcu.uid2) {
            return 0u;
        }
    }
    if ((a->flags & RS_PANEL_V3_FAULT_FLAG_HAS_CHANNEL) != 0u) {
        if (a->ch_type != b->ch_type || a->ch_l_adr != b->ch_l_adr) {
            return 0u;
        }
    }
    if ((a->flags & RS_PANEL_V3_FAULT_FLAG_HAS_CAN) != 0u && a->can_idx != b->can_idx) {
        return 0u;
    }
    if ((a->flags & RS_PANEL_V3_FAULT_FLAG_HAS_EXTRA) != 0u && a->extra != b->extra) {
        return 0u;
    }
    if (a->code == (uint8_t)RS_PANEL_V3_FAULT_PANEL_JOURNAL &&
        a->panel_addr != b->panel_addr) {
        return 0u;
    }
    return 1u;
}

uint16_t RsPanelV3_FaultEvtCrc(const RsPanelV3FaultEvtItem *items, uint16_t count)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i;
    uint8_t buf[32];
    if (items == 0) {
        return crc;
    }
    for (i = 0u; i < count; i++) {
        const RsPanelV3FaultEvtItem *it = &items[i];
        uint16_t n = RsPanelV3_FaultEvtEncodeBody(buf, sizeof(buf), it);
        uint16_t j;
        for (j = 0u; j < n; j++) {
            rs_v3_crc_feed_u8(&crc, buf[j]);
        }
    }
    return crc;
}

uint16_t RsPanelV3_DeviceCatalogCrc(const RsPanelV3DeviceItem *items, uint16_t count)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i;
    if (items == 0) {
        return crc;
    }
    for (i = 0u; i < count; i++) {
        crc = RsPanelV3_DeviceCatalogCrc_Add(crc, &items[i]);
    }
    return crc;
}

uint16_t RsPanelV3_ZoneCatalogCrc(const uint8_t (*names)[RS_PANEL_V3_ZONE_NAME_LEN],
                                  const uint8_t *zone_ids,
                                  uint16_t count)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i;
    if (names == 0 || zone_ids == 0) {
        return crc;
    }
    for (i = 0u; i < count; i++) {
        crc = RsPanelV3_ZoneCatalogCrc_Add(crc, zone_ids[i], (const char *)names[i]);
    }
    return crc;
}

uint16_t RsPanelV3_DeviceCatalogCrc_Add(uint16_t crc, const RsPanelV3DeviceItem *item)
{
    uint8_t j;
    if (item == 0) {
        return crc;
    }
    rs_v3_crc_feed_u8(&crc, item->d_type);
    rs_v3_crc_feed_u8(&crc, item->h_adr);
    rs_v3_crc_feed_u8(&crc, item->l_adr);
    rs_v3_crc_feed_u8(&crc, item->zone);
    rs_v3_crc_feed_u32le(&crc, item->uid0);
    rs_v3_crc_feed_u32le(&crc, item->uid1);
    rs_v3_crc_feed_u32le(&crc, item->uid2);
    rs_v3_crc_feed_u8(&crc, item->n_ch);
    for (j = 0u; j < item->n_ch && j < RS_PANEL_V3_MAX_MCU_CHANNELS; j++) {
        rs_v3_crc_feed_u8(&crc, item->ch[j].ch_type);
        rs_v3_crc_feed_u8(&crc, item->ch[j].ch_l_adr);
    }
    return crc;
}

uint16_t RsPanelV3_ZoneCatalogCrc_Add(uint16_t crc, uint8_t zone_id, const char *name)
{
    const char *s = (name != 0) ? name : "";
    rs_v3_crc_feed_u8(&crc, zone_id);
    while (*s != '\0') {
        rs_v3_crc_feed_u8(&crc, (uint8_t)(*s++));
    }
    rs_v3_crc_feed_u8(&crc, 0u);
    return crc;
}

static uint8_t rs_v3_put_u8(uint8_t *dst, uint16_t *pos, uint16_t cap, uint8_t v)
{
    if (*pos >= cap) {
        return 0u;
    }
    dst[(*pos)++] = v;
    return 1u;
}

static uint8_t rs_v3_put_u16le(uint8_t *dst, uint16_t *pos, uint16_t cap, uint16_t v)
{
    if ((uint16_t)(*pos + 2u) > cap) {
        return 0u;
    }
    dst[(*pos)++] = (uint8_t)(v & 0xFFu);
    dst[(*pos)++] = (uint8_t)(v >> 8);
    return 1u;
}

static uint8_t rs_v3_put_u32le(uint8_t *dst, uint16_t *pos, uint16_t cap, uint32_t v)
{
    if ((uint16_t)(*pos + 4u) > cap) {
        return 0u;
    }
    dst[(*pos)++] = (uint8_t)(v & 0xFFu);
    dst[(*pos)++] = (uint8_t)((v >> 8) & 0xFFu);
    dst[(*pos)++] = (uint8_t)((v >> 16) & 0xFFu);
    dst[(*pos)++] = (uint8_t)((v >> 24) & 0xFFu);
    return 1u;
}

static uint8_t rs_v3_put_bytes(uint8_t *dst, uint16_t *pos, uint16_t cap,
                               const void *src, uint16_t n)
{
    if ((uint16_t)(*pos + n) > cap) {
        return 0u;
    }
    memcpy(&dst[*pos], src, n);
    *pos = (uint16_t)(*pos + n);
    return 1u;
}

static uint8_t rs_v3_get_u8(const uint8_t *src, uint16_t *pos, uint16_t len, uint8_t *o)
{
    if (*pos >= len || o == 0) {
        return 0u;
    }
    *o = src[(*pos)++];
    return 1u;
}

static uint8_t rs_v3_get_u16le(const uint8_t *src, uint16_t *pos, uint16_t len, uint16_t *o)
{
    if ((uint16_t)(*pos + 2u) > len || o == 0) {
        return 0u;
    }
    *o = (uint16_t)src[*pos] | ((uint16_t)src[*pos + 1u] << 8);
    *pos = (uint16_t)(*pos + 2u);
    return 1u;
}

uint16_t RsPanelV3_EncodePoll(uint8_t *dst, uint16_t dst_size, const RsPanelV3Poll *poll)
{
    uint16_t pos = 0u;
    if (dst == 0 || poll == 0 || dst_size < 1u) {
        return 0u;
    }
    if (rs_v3_put_u8(dst, &pos, dst_size, RS_PANEL_V3_VERSION) == 0u) {
        return 0u;
    }

    if (poll->ack.result != (uint8_t)RS_PANEL_V3_ACK_NONE) {
        if (rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)RS_PANEL_V3_TAG_ACK) == 0u ||
            rs_v3_put_u8(dst, &pos, dst_size, 2u) == 0u ||
            rs_v3_put_u8(dst, &pos, dst_size, poll->ack.seq) == 0u ||
            rs_v3_put_u8(dst, &pos, dst_size, poll->ack.result) == 0u) {
            return 0u;
        }
    }

    if (rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)RS_PANEL_V3_TAG_SYS) == 0u ||
        rs_v3_put_u8(dst, &pos, dst_size, 2u) == 0u ||
        rs_v3_put_u16le(dst, &pos, dst_size, poll->sys.flags) == 0u) {
        return 0u;
    }

    if (poll->has_time != 0u) {
        uint8_t t[6];
        t[0] = poll->time.hour;
        t[1] = poll->time.min;
        t[2] = poll->time.sec;
        t[3] = poll->time.day;
        t[4] = poll->time.month;
        t[5] = poll->time.year;
        if (rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)RS_PANEL_V3_TAG_TIME) == 0u ||
            rs_v3_put_u8(dst, &pos, dst_size, 6u) == 0u ||
            rs_v3_put_bytes(dst, &pos, dst_size, t, 6u) == 0u) {
            return 0u;
        }
    }

    if (poll->has_zones != 0u) {
        uint8_t n = poll->zones.count;
        uint8_t i;
        uint16_t body_pos;
        uint16_t body_len;
        if (n > RS_PANEL_V3_MAX_ZONES) {
            n = RS_PANEL_V3_MAX_ZONES;
        }
        if (rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)RS_PANEL_V3_TAG_ZONES) == 0u) {
            return 0u;
        }
        body_pos = pos;
        if (rs_v3_put_u16le(dst, &pos, dst_size, 0u) == 0u) {
            return 0u;
        }
        if (rs_v3_put_u8(dst, &pos, dst_size, n) == 0u) {
            return 0u;
        }
        for (i = 0u; i < n; i++) {
            const RsPanelV3ZoneItem *z = &poll->zones.items[i];
            uint8_t name_len = 0u;
            while (name_len < (RS_PANEL_V3_ZONE_NAME_LEN - 1u) && z->name[name_len] != '\0') {
                name_len++;
            }
            if (rs_v3_put_u8(dst, &pos, dst_size, z->zone) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, z->status) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, z->remaining_s) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, name_len) == 0u ||
                rs_v3_put_bytes(dst, &pos, dst_size, z->name, name_len) == 0u) {
                return 0u;
            }
        }
        body_len = (uint16_t)(pos - body_pos - 2u);
        dst[body_pos] = (uint8_t)(body_len & 0xFFu);
        dst[body_pos + 1u] = (uint8_t)(body_len >> 8);
    }

    if (poll->has_zone_names != 0u) {
        const RsPanelV3ZoneNameItem *zn = &poll->zone_names;
        uint8_t name_len = 0u;
        while (name_len < (RS_PANEL_V3_ZONE_NAME_LEN - 1u) && zn->name[name_len] != '\0') {
            name_len++;
        }
        if (zn->op == (uint8_t)RS_PANEL_V3_STREAM_OP_CLEAR) {
            if (rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)RS_PANEL_V3_TAG_ZONE_NAMES) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, 1u) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, zn->op) == 0u) {
                return 0u;
            }
        } else {
            if (rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)RS_PANEL_V3_TAG_ZONE_NAMES) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)(3u + name_len)) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, zn->op) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, zn->zone) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, name_len) == 0u ||
                rs_v3_put_bytes(dst, &pos, dst_size, zn->name, name_len) == 0u) {
                return 0u;
            }
        }
    }

    if (poll->has_devices != 0u) {
        const RsPanelV3DeviceItem *d = &poll->device;
        uint8_t i;
        uint8_t body_len;
        if (d->op == (uint8_t)RS_PANEL_V3_STREAM_OP_CLEAR) {
            if (rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)RS_PANEL_V3_TAG_DEVICES) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, 1u) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, d->op) == 0u) {
                return 0u;
            }
        } else {
            body_len = (uint8_t)(15u + d->n_ch * 2u);
            if (rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)RS_PANEL_V3_TAG_DEVICES) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, body_len) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, d->op) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, d->d_type) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, d->h_adr) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, d->l_adr) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, d->zone) == 0u ||
                rs_v3_put_u32le(dst, &pos, dst_size, d->uid0) == 0u ||
                rs_v3_put_u32le(dst, &pos, dst_size, d->uid1) == 0u ||
                rs_v3_put_u32le(dst, &pos, dst_size, d->uid2) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, d->n_ch) == 0u) {
                return 0u;
            }
            for (i = 0u; i < d->n_ch && i < RS_PANEL_V3_MAX_MCU_CHANNELS; i++) {
                if (rs_v3_put_u8(dst, &pos, dst_size, d->ch[i].ch_type) == 0u ||
                    rs_v3_put_u8(dst, &pos, dst_size, d->ch[i].ch_l_adr) == 0u) {
                    return 0u;
                }
            }
        }
    }

    if (poll->has_fault_evt != 0u) {
        const RsPanelV3FaultEvtItem *fe = &poll->fault_evt;
        uint8_t body[36];
        uint16_t body_n = 0u;
        if (fe->op == (uint8_t)RS_PANEL_V3_FAULT_EVT_CLEAR_ALL) {
            if (rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)RS_PANEL_V3_TAG_FAULT_EVT) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, 1u) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, fe->op) == 0u) {
                return 0u;
            }
        } else {
            body_n = RsPanelV3_FaultEvtEncodeBody(body, sizeof(body), fe);
            if (body_n == 0u) {
                return 0u;
            }
            if (rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)RS_PANEL_V3_TAG_FAULT_EVT) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)(1u + body_n)) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, fe->op) == 0u ||
                rs_v3_put_bytes(dst, &pos, dst_size, body, body_n) == 0u) {
                return 0u;
            }
        }
    }

    if (poll->has_fault_op != 0u) {
        const RsPanelV3FaultOpItem *fo = &poll->fault_op;
        uint8_t tlen = 0u;
        while (tlen < (RS_PANEL_V3_FAULT_TEXT_LEN - 1u) && fo->text[tlen] != '\0') {
            tlen++;
        }
        if (fo->op == (uint8_t)RS_PANEL_V3_FAULT_OP_CLEAR) {
            if (rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)RS_PANEL_V3_TAG_FAULT_OP) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, 1u) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, fo->op) == 0u) {
                return 0u;
            }
        } else if (tlen > 0u) {
            if (rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)RS_PANEL_V3_TAG_FAULT_OP) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)(1u + tlen)) == 0u ||
                rs_v3_put_u8(dst, &pos, dst_size, fo->op) == 0u ||
                rs_v3_put_bytes(dst, &pos, dst_size, fo->text, tlen) == 0u) {
                return 0u;
            }
        }
    }

    if (poll->has_config != 0u) {
        if (rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)RS_PANEL_V3_TAG_CONFIG) == 0u ||
            rs_v3_put_u8(dst, &pos, dst_size, 2u) == 0u ||
            rs_v3_put_u8(dst, &pos, dst_size, poll->config.phase) == 0u ||
            rs_v3_put_u8(dst, &pos, dst_size, poll->config.percent) == 0u) {
            return 0u;
        }
    }

    if (poll->has_event_reply != 0u) {
        const RsPanelV3EventReply *er = &poll->event_reply;
        uint16_t plen = er->payload_len;
        uint16_t body_pos;
        uint16_t body_len;
        if (plen > RS_PANEL_V3_EVENT_REPLY_MAX) {
            plen = RS_PANEL_V3_EVENT_REPLY_MAX;
        }
        if (rs_v3_put_u8(dst, &pos, dst_size, (uint8_t)RS_PANEL_V3_TAG_EVENT_REPLY) == 0u) {
            return 0u;
        }
        body_pos = pos;
        if (rs_v3_put_u16le(dst, &pos, dst_size, 0u) == 0u) {
            return 0u;
        }
        if (rs_v3_put_u8(dst, &pos, dst_size, er->seq) == 0u ||
            rs_v3_put_u8(dst, &pos, dst_size, er->type) == 0u ||
            rs_v3_put_u8(dst, &pos, dst_size, er->result) == 0u ||
            rs_v3_put_u16le(dst, &pos, dst_size, plen) == 0u ||
            rs_v3_put_bytes(dst, &pos, dst_size, er->payload, plen) == 0u) {
            return 0u;
        }
        body_len = (uint16_t)(pos - body_pos - 2u);
        dst[body_pos] = (uint8_t)(body_len & 0xFFu);
        dst[body_pos + 1u] = (uint8_t)(body_len >> 8);
    }

    return pos;
}

static uint8_t rs_v3_get_u32le(const uint8_t *src, uint16_t *pos, uint16_t len, uint32_t *o)
{
    if ((uint16_t)(*pos + 4u) > len || o == 0) {
        return 0u;
    }
    *o = (uint32_t)src[*pos] | ((uint32_t)src[*pos + 1u] << 8) |
         ((uint32_t)src[*pos + 2u] << 16) | ((uint32_t)src[*pos + 3u] << 24);
    *pos = (uint16_t)(*pos + 4u);
    return 1u;
}

uint8_t RsPanelV3_DecodePoll(const uint8_t *src, uint16_t src_len, RsPanelV3Poll *out)
{
    uint16_t pos = 0u;
    uint8_t tag;
    uint8_t len_u8;
    uint16_t len_u16;
    uint16_t body_start;
    uint16_t body_end;
    if (src == 0 || out == 0 || src_len < 1u) {
        return 0u;
    }
    memset(out, 0, sizeof(*out));
    if (src[pos++] != RS_PANEL_V3_VERSION) {
        return 0u;
    }
    while (pos < src_len) {
        if (rs_v3_get_u8(src, &pos, src_len, &tag) == 0u) {
            return 0u;
        }
        len_u16 = 0u;
        len_u8 = 0u;
        /* Длину читаем сначала; body_start — первый байт тела (не поле len). */
        if (tag == (uint8_t)RS_PANEL_V3_TAG_ZONES ||
            tag == (uint8_t)RS_PANEL_V3_TAG_EVENT_REPLY) {
            if (rs_v3_get_u16le(src, &pos, src_len, &len_u16) == 0u) {
                return 0u;
            }
            body_start = pos;
            body_end = (uint16_t)(body_start + len_u16);
        } else {
            if (rs_v3_get_u8(src, &pos, src_len, &len_u8) == 0u) {
                return 0u;
            }
            body_start = pos;
            body_end = (uint16_t)(body_start + len_u8);
        }
        if (body_end > src_len) {
            return 0u;
        }

        switch (tag) {
        case (uint8_t)RS_PANEL_V3_TAG_ACK:
            if ((uint16_t)(body_end - body_start) >= 2u) {
                out->ack.seq = src[body_start];
                out->ack.result = src[body_start + 1u];
            }
            break;
        case (uint8_t)RS_PANEL_V3_TAG_SYS:
            if ((uint16_t)(body_end - body_start) >= 2u) {
                out->sys.flags = (uint16_t)src[body_start] | ((uint16_t)src[body_start + 1u] << 8);
            }
            break;
        case (uint8_t)RS_PANEL_V3_TAG_TIME:
            if ((uint16_t)(body_end - body_start) >= 6u) {
                out->has_time = 1u;
                out->time.hour = src[body_start];
                out->time.min = src[body_start + 1u];
                out->time.sec = src[body_start + 2u];
                out->time.day = src[body_start + 3u];
                out->time.month = src[body_start + 4u];
                out->time.year = src[body_start + 5u];
            }
            break;
        case (uint8_t)RS_PANEL_V3_TAG_ZONES: {
            uint16_t zp = body_start;
            uint8_t n;
            uint8_t i;
            if (rs_v3_get_u8(src, &zp, body_end, &n) == 0u) {
                return 0u;
            }
            out->has_zones = 1u;
            if (n > RS_PANEL_V3_MAX_ZONES) {
                n = RS_PANEL_V3_MAX_ZONES;
            }
            out->zones.count = n;
            for (i = 0u; i < n; i++) {
                RsPanelV3ZoneItem *z = &out->zones.items[i];
                uint8_t name_len;
                if (rs_v3_get_u8(src, &zp, body_end, &z->zone) == 0u ||
                    rs_v3_get_u8(src, &zp, body_end, &z->status) == 0u ||
                    rs_v3_get_u8(src, &zp, body_end, &z->remaining_s) == 0u ||
                    rs_v3_get_u8(src, &zp, body_end, &name_len) == 0u) {
                    return 0u;
                }
                memset(z->name, 0, sizeof(z->name));
                if (name_len > 0u) {
                    uint16_t copy = name_len;
                    if (copy >= RS_PANEL_V3_ZONE_NAME_LEN) {
                        copy = (uint16_t)(RS_PANEL_V3_ZONE_NAME_LEN - 1u);
                    }
                    if ((uint16_t)(zp + copy) > body_end) {
                        return 0u;
                    }
                    memcpy(z->name, &src[zp], copy);
                    zp = (uint16_t)(zp + name_len);
                }
            }
            break;
        }
        case (uint8_t)RS_PANEL_V3_TAG_FAULT_OP: {
            uint16_t fp = body_start;
            uint8_t op;
            if (rs_v3_get_u8(src, &fp, body_end, &op) == 0u) {
                return 0u;
            }
            out->has_fault_op = 1u;
            out->fault_op.op = op;
            out->fault_op.text[0] = '\0';
            if (op != (uint8_t)RS_PANEL_V3_FAULT_OP_CLEAR && fp < body_end) {
                uint16_t tlen = (uint16_t)(body_end - fp);
                if (tlen >= RS_PANEL_V3_FAULT_TEXT_LEN) {
                    tlen = (uint16_t)(RS_PANEL_V3_FAULT_TEXT_LEN - 1u);
                }
                memcpy(out->fault_op.text, &src[fp], tlen);
                out->fault_op.text[tlen] = '\0';
            }
            break;
        }
        case (uint8_t)RS_PANEL_V3_TAG_CONFIG:
            if ((uint16_t)(body_end - body_start) >= 2u) {
                out->has_config = 1u;
                out->config.phase = src[body_start];
                out->config.percent = src[body_start + 1u];
            }
            break;
        case (uint8_t)RS_PANEL_V3_TAG_EVENT_REPLY: {
            uint16_t ep = body_start;
            uint16_t plen;
            out->has_event_reply = 1u;
            if (rs_v3_get_u8(src, &ep, body_end, &out->event_reply.seq) == 0u ||
                rs_v3_get_u8(src, &ep, body_end, &out->event_reply.type) == 0u ||
                rs_v3_get_u8(src, &ep, body_end, &out->event_reply.result) == 0u ||
                rs_v3_get_u16le(src, &ep, body_end, &plen) == 0u) {
                return 0u;
            }
            if (plen > RS_PANEL_V3_EVENT_REPLY_MAX) {
                plen = RS_PANEL_V3_EVENT_REPLY_MAX;
            }
            out->event_reply.payload_len = plen;
            if (plen > 0u) {
                if ((uint16_t)(ep + plen) > body_end) {
                    return 0u;
                }
                memcpy(out->event_reply.payload, &src[ep], plen);
            }
            break;
        }
        case (uint8_t)RS_PANEL_V3_TAG_ZONE_NAMES: {
            uint16_t zp = body_start;
            uint8_t op;
            if (rs_v3_get_u8(src, &zp, body_end, &op) == 0u) {
                return 0u;
            }
            out->has_zone_names = 1u;
            out->zone_names.op = op;
            out->zone_names.zone = 0u;
            out->zone_names.name[0] = '\0';
            if (op == (uint8_t)RS_PANEL_V3_STREAM_OP_ADD) {
                uint8_t name_len;
                if (rs_v3_get_u8(src, &zp, body_end, &out->zone_names.zone) == 0u ||
                    rs_v3_get_u8(src, &zp, body_end, &name_len) == 0u) {
                    return 0u;
                }
                if (name_len > 0u) {
                    uint16_t copy = name_len;
                    if (copy >= RS_PANEL_V3_ZONE_NAME_LEN) {
                        copy = (uint16_t)(RS_PANEL_V3_ZONE_NAME_LEN - 1u);
                    }
                    if ((uint16_t)(zp + name_len) > body_end) {
                        return 0u;
                    }
                    memcpy(out->zone_names.name, &src[zp], copy);
                    out->zone_names.name[copy] = '\0';
                }
            }
            break;
        }
        case (uint8_t)RS_PANEL_V3_TAG_DEVICES: {
            uint16_t dp = body_start;
            RsPanelV3DeviceItem *d = &out->device;
            uint8_t i;
            if (rs_v3_get_u8(src, &dp, body_end, &d->op) == 0u) {
                return 0u;
            }
            out->has_devices = 1u;
            if (d->op == (uint8_t)RS_PANEL_V3_STREAM_OP_ADD) {
                if (rs_v3_get_u8(src, &dp, body_end, &d->d_type) == 0u ||
                    rs_v3_get_u8(src, &dp, body_end, &d->h_adr) == 0u ||
                    rs_v3_get_u8(src, &dp, body_end, &d->l_adr) == 0u ||
                    rs_v3_get_u8(src, &dp, body_end, &d->zone) == 0u ||
                    rs_v3_get_u32le(src, &dp, body_end, &d->uid0) == 0u ||
                    rs_v3_get_u32le(src, &dp, body_end, &d->uid1) == 0u ||
                    rs_v3_get_u32le(src, &dp, body_end, &d->uid2) == 0u ||
                    rs_v3_get_u8(src, &dp, body_end, &d->n_ch) == 0u) {
                    return 0u;
                }
                if (d->n_ch > RS_PANEL_V3_MAX_MCU_CHANNELS) {
                    d->n_ch = RS_PANEL_V3_MAX_MCU_CHANNELS;
                }
                for (i = 0u; i < d->n_ch; i++) {
                    if (rs_v3_get_u8(src, &dp, body_end, &d->ch[i].ch_type) == 0u ||
                        rs_v3_get_u8(src, &dp, body_end, &d->ch[i].ch_l_adr) == 0u) {
                        return 0u;
                    }
                }
            }
            break;
        }
        case (uint8_t)RS_PANEL_V3_TAG_FAULT_EVT: {
            uint16_t fp = body_start;
            RsPanelV3FaultEvtItem *fe = &out->fault_evt;
            uint8_t op;
            uint16_t blen;
            if (rs_v3_get_u8(src, &fp, body_end, &op) == 0u) {
                return 0u;
            }
            out->has_fault_evt = 1u;
            fe->op = op;
            if (op == (uint8_t)RS_PANEL_V3_FAULT_EVT_CLEAR_ALL) {
                break;
            }
            blen = (uint16_t)(body_end - fp);
            if (RsPanelV3_FaultEvtDecodeBody(&src[fp], blen, fe) == 0u) {
                return 0u;
            }
            /* DecodeBody делает memset всего item — вернуть op (SET/CLEAR). */
            fe->op = op;
            break;
        }
        default:
            break;
        }
        pos = body_end;
    }
    return 1u;
}

uint16_t RsPanelV3_EncodeRsp(uint8_t *dst, uint16_t dst_size, const RsPanelV3Rsp *rsp)
{
    uint16_t pos = 0u;
    if (dst == 0 || rsp == 0 || dst_size < 12u) {
        return 0u;
    }
    if (rs_v3_put_u8(dst, &pos, dst_size, RS_PANEL_V3_VERSION) == 0u ||
        rs_v3_put_u8(dst, &pos, dst_size, rsp->event.seq) == 0u ||
        rs_v3_put_u8(dst, &pos, dst_size, rsp->event.type) == 0u ||
        rs_v3_put_u8(dst, &pos, dst_size, rsp->event.zone) == 0u ||
        rs_v3_put_u8(dst, &pos, dst_size, rsp->event.u8_a) == 0u ||
        rs_v3_put_u16le(dst, &pos, dst_size, rsp->event.u16_a) == 0u ||
        rs_v3_put_u16le(dst, &pos, dst_size, rsp->event.u16_b) == 0u ||
        rs_v3_put_u16le(dst, &pos, dst_size, rsp->devices_crc) == 0u ||
        rs_v3_put_u16le(dst, &pos, dst_size, rsp->faults_crc) == 0u ||
        rs_v3_put_u8(dst, &pos, dst_size, rsp->flags) == 0u) {
        return 0u;
    }
    return pos;
}

uint8_t RsPanelV3_DecodeRsp(const uint8_t *src, uint16_t src_len, RsPanelV3Rsp *out)
{
    uint16_t pos = 0u;
    if (src == 0 || out == 0 || src_len < 12u) {
        return 0u;
    }
    memset(out, 0, sizeof(*out));
    if (src[pos++] != RS_PANEL_V3_VERSION) {
        return 0u;
    }
    if (rs_v3_get_u8(src, &pos, src_len, &out->event.seq) == 0u ||
        rs_v3_get_u8(src, &pos, src_len, &out->event.type) == 0u ||
        rs_v3_get_u8(src, &pos, src_len, &out->event.zone) == 0u ||
        rs_v3_get_u8(src, &pos, src_len, &out->event.u8_a) == 0u ||
        rs_v3_get_u16le(src, &pos, src_len, &out->event.u16_a) == 0u ||
        rs_v3_get_u16le(src, &pos, src_len, &out->event.u16_b) == 0u) {
        return 0u;
    }
    if (src_len >= 14u) {
        if (rs_v3_get_u16le(src, &pos, src_len, &out->devices_crc) == 0u ||
            rs_v3_get_u16le(src, &pos, src_len, &out->faults_crc) == 0u ||
            rs_v3_get_u8(src, &pos, src_len, &out->flags) == 0u) {
            return 0u;
        }
    } else if (src_len >= 12u) {
        /* legacy RSP: single fault_crc */
        uint16_t legacy_crc;
        if (rs_v3_get_u16le(src, &pos, src_len, &legacy_crc) == 0u) {
            return 0u;
        }
        out->faults_crc = legacy_crc;
        if (pos < src_len) {
            (void)rs_v3_get_u8(src, &pos, src_len, &out->flags);
        }
    }
    return 1u;
}
