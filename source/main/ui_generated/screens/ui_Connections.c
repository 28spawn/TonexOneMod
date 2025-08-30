// This file was generated manually for connection settings screen
// LVGL version: 8.3.11

#include "../ui.h"

void ui_Connections_screen_init(void)
{
    ui_Connections = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Connections, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Connections, lv_color_hex(0x1F1F1F), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Connections, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_HomeButton = lv_btn_create(ui_Connections);
    lv_obj_set_size(ui_HomeButton, 80, 40);
    lv_obj_set_pos(ui_HomeButton, 10, 10);
    lv_obj_add_event_cb(ui_HomeButton, ui_event_HomeButton, LV_EVENT_ALL, NULL);
    lv_obj_t * ui_HomeLabel = lv_label_create(ui_HomeButton);
    lv_label_set_text(ui_HomeLabel, LV_SYMBOL_HOME);

    ui_WiFiLabel = lv_label_create(ui_Connections);
    lv_label_set_text(ui_WiFiLabel, "WiFi Settings");
    lv_obj_set_pos(ui_WiFiLabel, 20, 70);

    ui_WiFiSwitch = lv_switch_create(ui_Connections);
    lv_obj_set_pos(ui_WiFiSwitch, 250, 65);

    ui_BluetoothLabel = lv_label_create(ui_Connections);
    lv_label_set_text(ui_BluetoothLabel, "Bluetooth Settings");
    lv_obj_set_pos(ui_BluetoothLabel, 20, 120);

    ui_BTSwitch = lv_switch_create(ui_Connections);
    lv_obj_set_pos(ui_BTSwitch, 250, 115);

    ui_USBLabel = lv_label_create(ui_Connections);
    lv_label_set_text(ui_USBLabel, "USB Settings");
    lv_obj_set_pos(ui_USBLabel, 20, 170);

    ui_USBSwitch = lv_switch_create(ui_Connections);
    lv_obj_set_pos(ui_USBSwitch, 250, 165);
}

