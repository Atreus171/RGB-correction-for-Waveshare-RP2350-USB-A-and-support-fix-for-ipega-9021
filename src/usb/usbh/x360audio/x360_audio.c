// x360_audio.c - Xbox 360 wired controller headset audio host driver (diagnostic)
//
// Claims Interface 1 (vendor 0xFF / sub 0x5D / proto 0x03) of a wired Xbox 360
// controller, opens its audio/status endpoints and logs every frame verbatim.
// Purpose: characterize the byte-level protocol before building the actual
// G.726 audio pipeline (see docs/protocols/xbox360_headset.md).
//
// Physical endpoint layout on a real Xbox 360 wired controller:
//   0x82 IN interrupt 32B 2ms  -> mic (continuous ADPCM codes when active)
//   0x02 OUT interrupt 32B 4ms -> phone (ADPCM codes to the headset)
//   0x83 IN interrupt 32B 64ms -> status (unverified role)
//   0x03 OUT interrupt 32B 16ms-> config (unverified role)
//
// The driver is endpoint-address tolerant: unknown endpoints are assigned by
// order (first IN = mic, second IN = status, first OUT = phone, second OUT =
// config) so third-party boards with different addresses still enumerate.

#include "tusb.h"
#include <stdio.h>
#include <string.h>
#include "host/usbh_pvt.h"
#include "platform/platform.h"
#include "x360_audio.h"

#if CFG_TUH_X360_AUDIO

// Interface signature of the Xbox 360 wired controller headset/audio interface.
#define X360_AUDIO_IF_CLASS    0xFF
#define X360_AUDIO_IF_SUBCLASS 0x5D
#define X360_AUDIO_IF_PROTOCOL 0x03

// Logging throttle: first N frames always logged, then 1 per interval.
#define X360_AUDIO_ALWAYS_LOG  6
#define X360_AUDIO_LOG_MS      200
#define X360_AUDIO_STAT_MS     1000
#define X360_AUDIO_REEARM_MS   100

typedef struct
{
    bool    claimed;            // interface claimed (open succeeded)
    bool    configured;         // set_config completed
    uint8_t itf_num;
    // Endpoints. Role names follow the physical Xbox 360 layout; values may
    // differ on third-party hardware (order-based fallback).
    uint8_t ep_mic_in;          // 0x82 expected
    uint8_t ep_phone_out;       // 0x02 expected
    uint8_t ep_status_in;       // 0x83 expected
    uint8_t ep_cfg_out;         // 0x03 expected

    bool    mic_xfer_active;
    bool    status_xfer_active;
    bool    phone_xfer_active;
    bool    cfg_xfer_active;

    uint8_t mic_buf[X360_AUDIO_FRAME_SIZE];
    uint8_t status_buf[X360_AUDIO_FRAME_SIZE];
    uint8_t phone_tx[X360_AUDIO_FRAME_SIZE];
    uint8_t cfg_tx[X360_AUDIO_FRAME_SIZE];

    bool    mic_frame_new;                          // last mic frame changed
    uint8_t last_mic[X360_AUDIO_FRAME_SIZE];        // for change detection

    uint32_t mic_count;
    uint32_t status_count;
    uint32_t last_log_ms;
    uint32_t last_stat_ms;
} x360_audio_dev_t;

static x360_audio_dev_t x360_dev[CFG_TUH_DEVICE_MAX + 1];

static x360_audio_dev_t* slot_get(uint8_t dev_addr)
{
    if (dev_addr == 0 || dev_addr > CFG_TUH_DEVICE_MAX) return NULL;
    return &x360_dev[dev_addr];
}

//--------------------------------------------------------------------+
// Diagnostics
//--------------------------------------------------------------------+

static void x360_log_hex(uint8_t dev_addr, const char* tag, uint8_t const* buf, uint16_t len)
{
    printf("[x360audio] d=%02u %s (%u): ", dev_addr, tag, len);
    for (uint16_t i = 0; i < len && i < X360_AUDIO_FRAME_SIZE; i++) printf("%02X ", buf[i]);
    printf("\n");
}

static void x360_maybe_log_frame(x360_audio_dev_t* s, uint8_t dev_addr,
                                 const char* tag, uint8_t const* buf, uint16_t len,
                                 uint32_t frame_no)
{
    uint32_t now = platform_time_ms();

    // Always log the first frames after mount; afterwards throttle.
    if (frame_no <= X360_AUDIO_ALWAYS_LOG || now - s->last_log_ms >= X360_AUDIO_LOG_MS)
    {
        s->last_log_ms = now;
        x360_log_hex(dev_addr, tag, buf, len);
    }
}

//--------------------------------------------------------------------+
// Transfer handling
//--------------------------------------------------------------------+

static void x360_arm_mic_read(x360_audio_dev_t* s, uint8_t dev_addr)
{
    if (!s->claimed || s->mic_xfer_active) return;
    if (!usbh_edpt_xfer(dev_addr, s->ep_mic_in, s->mic_buf, X360_AUDIO_FRAME_SIZE))
    {
        printf("[x360audio] d=%02u mic read submit failed\n", dev_addr);
        return;
    }
    s->mic_xfer_active = true;
}

static void x360_arm_status_read(x360_audio_dev_t* s, uint8_t dev_addr)
{
    if (!s->claimed || s->status_xfer_active) return;
    if (!usbh_edpt_xfer(dev_addr, s->ep_status_in, s->status_buf, X360_AUDIO_FRAME_SIZE))
    {
        printf("[x360audio] d=%02u status read submit failed\n", dev_addr);
        return;
    }
    s->status_xfer_active = true;
}

//--------------------------------------------------------------------+
// usbh_class_driver_t callbacks
//--------------------------------------------------------------------+

static bool x360_audio_init(void)
{
    tu_memclr(x360_dev, sizeof(x360_dev));
    return true;
}

static bool x360_audio_deinit(void)
{
    return true;
}

static bool x360_audio_open(uint8_t rhport, uint8_t dev_addr,
                            tusb_desc_interface_t const* desc_itf, uint16_t max_len)
{
    (void)rhport;

    // Only claim the wired 360 headset/audio interface signature.
    if (desc_itf->bInterfaceClass    != X360_AUDIO_IF_CLASS ||
        desc_itf->bInterfaceSubClass != X360_AUDIO_IF_SUBCLASS ||
        desc_itf->bInterfaceProtocol != X360_AUDIO_IF_PROTOCOL)
    {
        return false;
    }

    x360_audio_dev_t* s = slot_get(dev_addr);
    if (!s) return false;

    // Idempotent: this driver is tried per-interface; only one 0x5D/0x03
    // interface per device should ever exist.
    if (s->claimed)
    {
        return s->itf_num == desc_itf->bInterfaceNumber;
    }

    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    printf("[x360audio] claiming if%u d=%02u VID=%04X PID=%04X EPs=%u\n",
           desc_itf->bInterfaceNumber, dev_addr, vid, pid, desc_itf->bNumEndpoints);

    s->itf_num = desc_itf->bInterfaceNumber;

    // Parse endpoints in order: IN #1 = mic, IN #2 = status, OUT #1 = phone,
    // OUT #2 = config. Known physical addresses are matched exactly when seen.
    const uint8_t* p_desc = (const uint8_t*)desc_itf;
    const uint8_t* desc_end = p_desc + max_len;
    uint8_t in_idx = 0;
    uint8_t out_idx = 0;

    p_desc = tu_desc_next(p_desc); // skip interface descriptor

    while (p_desc < desc_end && (in_idx + out_idx) < desc_itf->bNumEndpoints)
    {
        if (tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT)
        {
            const tusb_desc_endpoint_t* ep = (const tusb_desc_endpoint_t*)p_desc;

            // Bail out if any endpoint is not interrupt (audio iface is interrupt-only).
            if (ep->bmAttributes.xfer != TUSB_XFER_INTERRUPT)
            {
                printf("[x360audio] if%u EP 0x%02X not interrupt, not claiming\n",
                       desc_itf->bInterfaceNumber, ep->bEndpointAddress);
                return false;
            }

            if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN)
            {
                if (ep->bEndpointAddress == 0x82)      s->ep_mic_in = ep->bEndpointAddress;
                else if (ep->bEndpointAddress == 0x83) s->ep_status_in = ep->bEndpointAddress;
                else if (in_idx == 0)                  s->ep_mic_in = ep->bEndpointAddress;
                else if (in_idx == 1)                  s->ep_status_in = ep->bEndpointAddress;
                in_idx++;
            }
            else
            {
                if (ep->bEndpointAddress == 0x02)        s->ep_phone_out = ep->bEndpointAddress;
                else if (ep->bEndpointAddress == 0x03)   s->ep_cfg_out = ep->bEndpointAddress;
                else if (out_idx == 0)                   s->ep_phone_out = ep->bEndpointAddress;
                else if (out_idx == 1)                   s->ep_cfg_out = ep->bEndpointAddress;
                out_idx++;
            }

            printf("[x360audio]   EP 0x%02X size=%u interval=%u\n",
                   ep->bEndpointAddress, tu_edpt_packet_size(ep), ep->bInterval);
        }
        else if (tu_desc_type(p_desc) == TUSB_DESC_INTERFACE)
        {
            break; // next interface descriptor
        }
        p_desc = tu_desc_next(p_desc);
    }

    if (!s->ep_mic_in || !s->ep_phone_out)
    {
        printf("[x360audio] if%u requires mic IN + phone OUT endpoints, not claiming\n",
               desc_itf->bInterfaceNumber);
        return false;
    }

    // Re-walk to actually open every endpoint we will touch.
    p_desc = (const uint8_t*)desc_itf;
    p_desc = tu_desc_next(p_desc);
    bool all_open = true;
    while (p_desc < desc_end)
    {
        if (tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT)
        {
            const tusb_desc_endpoint_t* ep = (const tusb_desc_endpoint_t*)p_desc;
            uint8_t a = ep->bEndpointAddress;
            if (a != s->ep_mic_in && a != s->ep_phone_out &&
                a != s->ep_status_in && a != s->ep_cfg_out)
            {
                p_desc = tu_desc_next(p_desc);
                continue;
            }
            if (!tuh_edpt_open(dev_addr, ep))
            {
                printf("[x360audio] d=%02u failed to open EP 0x%02X\n", dev_addr, a);
                all_open = false;
            }
        }
        else if (tu_desc_type(p_desc) == TUSB_DESC_INTERFACE)
        {
            break;
        }
        p_desc = tu_desc_next(p_desc);
    }

    if (!all_open || !s->ep_mic_in)
    {
        printf("[x360audio] not claiming if%u: endpoint open failure\n",
               desc_itf->bInterfaceNumber);
        tu_memclr(s, sizeof(*s));
        return false;
    }

    s->claimed = true;
    printf("[x360audio] claimed d=%02u mic=0x%02X phone=0x%02X status=0x%02X cfg=0x%02X\n",
           dev_addr, s->ep_mic_in, s->ep_phone_out, s->ep_status_in, s->ep_cfg_out);
    return true;
}

static bool x360_audio_set_config(uint8_t dev_addr, uint8_t itf_num)
{
    x360_audio_dev_t* s = slot_get(dev_addr);
    if (!s || !s->claimed || s->itf_num != itf_num) return false;

    s->configured = true;
    s->mic_count = 0;
    s->status_count = 0;
    s->last_log_ms = 0;
    s->last_stat_ms = platform_time_ms();
    printf("[x360audio] d=%02u configured, starting reads (mic 0x%02X, status 0x%02X)\n",
           dev_addr, s->ep_mic_in, s->ep_status_in);

    x360_arm_mic_read(s, dev_addr);
    if (s->ep_status_in) x360_arm_status_read(s, dev_addr);

    // Continue set-config chain so the gamepad interface completes too.
    usbh_driver_set_config_complete(dev_addr, itf_num);
    return true;
}

static bool x360_audio_xfer_cb(uint8_t dev_addr, uint8_t ep_addr,
                               xfer_result_t result, uint32_t xferred_bytes)
{
    x360_audio_dev_t* s = slot_get(dev_addr);
    if (!s || !s->claimed) return false;

    if (ep_addr == s->ep_mic_in)
    {
        s->mic_xfer_active = false;
        if (result != XFER_RESULT_SUCCESS)
        {
            printf("[x360audio] d=%02u mic xfer failed result=%d\n", dev_addr, result);
            return true; // re-arm handled by task()
        }
        s->mic_count++;
        x360_maybe_log_frame(s, dev_addr, "mic", s->mic_buf, (uint16_t)xferred_bytes, s->mic_count);

        // Detect frame change for the summary line.
        s->mic_frame_new = (xferred_bytes != X360_AUDIO_FRAME_SIZE ||
                            memcmp(s->mic_buf, s->last_mic, X360_AUDIO_FRAME_SIZE) != 0);
        if (s->mic_frame_new)
        {
            memcpy(s->last_mic, s->mic_buf, X360_AUDIO_FRAME_SIZE);
        }

        x360_arm_mic_read(s, dev_addr);
        return true;
    }

    if (ep_addr == s->ep_status_in)
    {
        s->status_xfer_active = false;
        if (result != XFER_RESULT_SUCCESS)
        {
            printf("[x360audio] d=%02u status xfer failed result=%d\n", dev_addr, result);
            return true;
        }
        s->status_count++;
        x360_maybe_log_frame(s, dev_addr, "status", s->status_buf, (uint16_t)xferred_bytes, s->status_count);
        x360_arm_status_read(s, dev_addr);
        return true;
    }

    if (ep_addr == s->ep_phone_out)
    {
        s->phone_xfer_active = false;
        printf("[x360audio] d=%02u phone out done result=%d/%u bytes\n",
               dev_addr, result, (unsigned)xferred_bytes);
        return true;
    }

    if (ep_addr == s->ep_cfg_out)
    {
        s->cfg_xfer_active = false;
        printf("[x360audio] d=%02u cfg out done result=%d/%u bytes\n",
               dev_addr, result, (unsigned)xferred_bytes);
        return true;
    }

    return false;
}

static void x360_audio_close(uint8_t dev_addr)
{
    x360_audio_dev_t* s = slot_get(dev_addr);
    if (!s) return;
    if (s->claimed)
    {
        printf("[x360audio] d=%02u closed (mic=%u status=%u)\n",
               dev_addr, (unsigned)s->mic_count, (unsigned)s->status_count);
    }
    tu_memclr(s, sizeof(*s));
}

usbh_class_driver_t const usbh_x360audio_driver =
{
    .name       = "X360AUDIO",
    .init       = x360_audio_init,
    .deinit     = x360_audio_deinit,
    .open       = x360_audio_open,
    .set_config = x360_audio_set_config,
    .xfer_cb    = x360_audio_xfer_cb,
    .close      = x360_audio_close
};

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

bool x360_audio_is_active(uint8_t dev_addr)
{
    x360_audio_dev_t* s = slot_get(dev_addr);
    return s && s->claimed && s->configured;
}

uint32_t x360_audio_mic_frame_count(uint8_t dev_addr)
{
    x360_audio_dev_t* s = slot_get(dev_addr);
    return s ? s->mic_count : 0;
}

uint32_t x360_audio_status_frame_count(uint8_t dev_addr)
{
    x360_audio_dev_t* s = slot_get(dev_addr);
    return s ? s->status_count : 0;
}

bool x360_audio_last_mic_frame(uint8_t dev_addr, uint8_t buf[X360_AUDIO_FRAME_SIZE])
{
    x360_audio_dev_t* s = slot_get(dev_addr);
    if (!s || !s->mic_count) return false;
    memcpy(buf, s->mic_buf, X360_AUDIO_FRAME_SIZE);
    return true;
}

bool x360_audio_send_config(uint8_t dev_addr, uint8_t const* cmd, uint16_t len)
{
    x360_audio_dev_t* s = slot_get(dev_addr);
    if (!s || !s->claimed || !s->ep_cfg_out) return false;
    if (len > X360_AUDIO_FRAME_SIZE) return false;
    if (s->cfg_xfer_active) return false;

    tu_memclr(s->cfg_tx, sizeof(s->cfg_tx));
    memcpy(s->cfg_tx, cmd, len);

    if (!usbh_edpt_xfer(dev_addr, s->ep_cfg_out, s->cfg_tx, X360_AUDIO_FRAME_SIZE))
    {
        return false;
    }
    s->cfg_xfer_active = true;
    printf("[x360audio] d=%02u cfg out sent (%u bytes)\n", dev_addr, len);
    return true;
}

bool x360_audio_send_phone(uint8_t dev_addr, uint8_t const* frame, uint16_t len)
{
    x360_audio_dev_t* s = slot_get(dev_addr);
    if (!s || !s->claimed || !s->ep_phone_out) return false;
    // The controller only produces sound with 32-byte phone frames
    // (16-byte frames are silent, per xboxdrv notes).
    if (len != X360_AUDIO_FRAME_SIZE) return false;
    if (s->phone_xfer_active) return false;

    // Keep a dedicated tx buffer so callers can use immediates.
    memcpy(s->phone_tx, frame, X360_AUDIO_FRAME_SIZE);

    if (!usbh_edpt_xfer(dev_addr, s->ep_phone_out, s->phone_tx, X360_AUDIO_FRAME_SIZE))
    {
        return false;
    }
    s->phone_xfer_active = true;
    return true;
}

void x360_audio_task(void)
{
    for (uint8_t dev_addr = 1; dev_addr <= CFG_TUH_DEVICE_MAX; dev_addr++)
    {
        x360_audio_dev_t* s = slot_get(dev_addr);
        if (!s || !s->claimed) continue;

        uint32_t now = platform_time_ms();

        // Re-arm reads after an error (throttled).
        if (!s->mic_xfer_active && now - s->last_stat_ms > X360_AUDIO_REEARM_MS)
        {
            x360_arm_mic_read(s, dev_addr);
        }
        if (s->ep_status_in && !s->status_xfer_active && now - s->last_stat_ms > X360_AUDIO_REEARM_MS)
        {
            x360_arm_status_read(s, dev_addr);
        }

        // Periodic diagnostics summary.
        if (now - s->last_stat_ms >= X360_AUDIO_STAT_MS)
        {
            s->last_stat_ms = now;
            if (s->mic_count || s->status_count)
            {
                printf("[x360audio] d=%02u mic=%u status=%u last="
                       "%02X %02X %02X %02X %02X %02X %02X %02X\n",
                       dev_addr, (unsigned)s->mic_count, (unsigned)s->status_count,
                       s->mic_buf[0], s->mic_buf[1], s->mic_buf[2], s->mic_buf[3],
                       s->mic_buf[4], s->mic_buf[5], s->mic_buf[6], s->mic_buf[7]);
            }
        }
    }
}

#endif // CFG_TUH_X360_AUDIO