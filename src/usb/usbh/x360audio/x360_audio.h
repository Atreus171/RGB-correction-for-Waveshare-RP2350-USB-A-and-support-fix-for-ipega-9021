// x360_audio.h - Xbox 360 wired controller headset audio (host side)
//
// TinyUSB host class driver for Interface 1 (vendor 0xFF / sub 0x5D / proto 0x03)
// of an Xbox 360 wired controller. Diagnostic mode:
//   - claims interface 1, opens the mic/phone/status/config endpoints
//   - queues continuous reads on the mic IN + status IN endpoints
//   - logs frames verbatim so the packet format can be characterized
//     (see docs/protocols/xbox360_headset.md)
//
// Integration: registered in usbh_app_driver_get_cb() (src/usb/usbh/xinput/xinput.c)
// exactly like usbh_xinput_driver. No submodule changes required.

#ifndef X360_AUDIO_H
#define X360_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#if CFG_TUH_X360_AUDIO

#include "host/usbh_pvt.h"

// Fixed frame size of the audio interrupt endpoints (32 bytes on real hardware).
#define X360_AUDIO_FRAME_SIZE 32

// Registered small periodic handler (called from usbh_task).
void x360_audio_task(void);

// True once interface 1 of dev_addr has been claimed and configured.
bool x360_audio_is_active(uint8_t dev_addr);

// First device with a claimed audio interface, or 0 if none (for CDC probes).
uint8_t x360_audio_active_addr(void);

// Diagnostic counters / last-frame accessors.
uint32_t x360_audio_mic_frame_count(uint8_t dev_addr);
uint32_t x360_audio_status_frame_count(uint8_t dev_addr);
bool x360_audio_last_mic_frame(uint8_t dev_addr, uint8_t buf[X360_AUDIO_FRAME_SIZE]);

// Send a config/status probe on the 0x03 OUT endpoint. len <= X360_AUDIO_FRAME_SIZE.
bool x360_audio_send_config(uint8_t dev_addr, uint8_t const* cmd, uint16_t len);

// Enqueue a config/status probe on the 0x03 OUT endpoint. Drained one frame at
// a time by x360_audio_task() (safer than send_config for multi-frame scripts).
bool x360_audio_queue_config(uint8_t dev_addr, uint8_t const* cmd, uint16_t len);

// Send a raw frame on the 0x02 OUT (phone) endpoint. len must be 32.
bool x360_audio_send_phone(uint8_t dev_addr, uint8_t const* frame, uint16_t len);

// TinyUSB host class driver registration (consumed by usbh_app_driver_get_cb).
extern const usbh_class_driver_t usbh_x360audio_driver;

#endif // CFG_TUH_X360_AUDIO

#endif // X360_AUDIO_H