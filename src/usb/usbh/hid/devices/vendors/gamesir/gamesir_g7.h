#ifndef GAMESIR_G7_H
#define GAMESIR_G7_H

#include "../../../hid_device.h"
#include "../../../hid_utils.h"
#include "tusb.h"

#define GAMESIR_G7_VID     0x320F
#define GAMESIR_G7_PID     0x5041

typedef struct TU_ATTR_PACKED {
    uint8_t report_id;
    uint8_t buttons1;
    uint8_t buttons2;
    uint8_t dpad;
    uint8_t left_x;
    uint8_t left_y;
    uint8_t right_x;
    uint8_t right_y;
    uint8_t lt;
    uint8_t rt;
} gamesir_g7_report_t;

#define G7_BTN_A       0x01
#define G7_BTN_B       0x02
#define G7_BTN_X       0x04
#define G7_BTN_Y       0x08
#define G7_BTN_LB      0x10
#define G7_BTN_RB      0x20
#define G7_BTN_BACK    0x40
#define G7_BTN_START   0x80

#define G7_BTN_GUIDE   0x01
#define G7_BTN_LSB     0x02
#define G7_BTN_RSB     0x04

extern DeviceInterface gamesir_g7_interface;

#endif
