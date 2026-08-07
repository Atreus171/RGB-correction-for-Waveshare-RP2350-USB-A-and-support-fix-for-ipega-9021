#include "gamesir_g7.h"
#include <stdio.h>
#include "core/buttons.h"
#include "core/router/router.h"
#include "core/input_event.h"

#define GAMESIR_VID      0x320F
#define GAMESIR_G7_PID   0x5041

static bool is_gamesir_g7(uint16_t vid, uint16_t pid) {
    return (vid == GAMESIR_VID && pid == GAMESIR_G7_PID);
}

static bool init_gamesir_g7(uint8_t dev_addr, uint8_t instance) {
    printf("[GameSir G7] Mounted: dev_addr=%d, instance=%d\n", dev_addr, instance);
    return true;
}

static void process_gamesir_g7(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
    gamesir_g7_report_t g7 = {0};

    if (len == sizeof(gamesir_g7_report_t)) {
        // Report includes report ID byte
        memcpy(&g7, report, sizeof(gamesir_g7_report_t));
    } else if (len == sizeof(gamesir_g7_report_t) - 1) {
        // Report without report ID — data starts at buttons1
        memcpy(&g7.buttons1, report, len);
    } else {
        return;
    }

    bool dpad_up    = (g7.dpad == 0 || g7.dpad == 1 || g7.dpad == 7);
    bool dpad_right = (g7.dpad >= 1 && g7.dpad <= 3);
    bool dpad_down  = (g7.dpad >= 3 && g7.dpad <= 5);
    bool dpad_left  = (g7.dpad >= 5 && g7.dpad <= 7);

    uint32_t buttons = 0;

    if (dpad_up)    buttons |= JP_BUTTON_DU;
    if (dpad_down)  buttons |= JP_BUTTON_DD;
    if (dpad_left)  buttons |= JP_BUTTON_DL;
    if (dpad_right) buttons |= JP_BUTTON_DR;

    if (g7.buttons1 & G7_BTN_A)     buttons |= JP_BUTTON_B1;
    if (g7.buttons1 & G7_BTN_B)     buttons |= JP_BUTTON_B2;
    if (g7.buttons1 & G7_BTN_X)     buttons |= JP_BUTTON_B3;
    if (g7.buttons1 & G7_BTN_Y)     buttons |= JP_BUTTON_B4;
    if (g7.buttons1 & G7_BTN_LB)    buttons |= JP_BUTTON_L1;
    if (g7.buttons1 & G7_BTN_RB)    buttons |= JP_BUTTON_R1;
    if (g7.buttons1 & G7_BTN_BACK)  buttons |= JP_BUTTON_S1;
    if (g7.buttons1 & G7_BTN_START) buttons |= JP_BUTTON_S2;

    if (g7.buttons2 & G7_BTN_GUIDE) buttons |= JP_BUTTON_A1;
    if (g7.buttons2 & G7_BTN_LSB)   buttons |= JP_BUTTON_L3;
    if (g7.buttons2 & G7_BTN_RSB)   buttons |= JP_BUTTON_R3;

    uint8_t axis_lx = g7.left_x;
    uint8_t axis_ly = g7.left_y;
    uint8_t axis_rx = g7.right_x;
    uint8_t axis_ry = g7.right_y;

    ensureAllNonZero(&axis_lx, &axis_ly, &axis_rx, &axis_ry);

    input_event_t event = {
        .dev_addr = dev_addr,
        .instance = instance,
        .type = INPUT_TYPE_GAMEPAD,
        .transport = INPUT_TRANSPORT_USB,
        .buttons = buttons,
        .button_count = 14,
        .analog = {axis_lx, axis_ly, axis_rx, axis_ry, g7.lt, g7.rt},
        .keys = 0,
    };
    router_submit_input(&event);
}

static void unmount_gamesir_g7(uint8_t dev_addr, uint8_t instance) {
    printf("[GameSir G7] Unmounted: dev_addr=%d, instance=%d\n", dev_addr, instance);
}

DeviceInterface gamesir_g7_interface = {
    .name = "GameSir G7",
    .is_device = is_gamesir_g7,
    .init = init_gamesir_g7,
    .process = process_gamesir_g7,
    .task = NULL,
    .unmount = unmount_gamesir_g7,
};
