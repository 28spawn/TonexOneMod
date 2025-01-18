/*
 Copyright (C) 2024  Greg Smith

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
 
*/

#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "lvgl.h"
#include "demos/lv_demos.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_ota_ops.h"
#include "sys/param.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_crc.h"
#include "esp_now.h"
#include "driver/i2c.h"
#include "soc/lldesc.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_gc9107.h"
#include "esp_intr_alloc.h"
#include "main.h"
#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_169 || CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B || CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_M5ATOMS3R
    #include "ui.h"
#endif
#include "usb/usb_host.h"
#include "usb_comms.h"
#include "usb_tonex_one.h"
#include "display.h"
#include "CH422G.h"
#include "control.h"
#include "task_priorities.h"
#include "midi_control.h"

static const char *TAG = "app_display";

#define DISPLAY_TASK_STACK_SIZE   (6 * 1024)

#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B
    // LCD panel config
    #define DISPLAY_LCD_PIXEL_CLOCK_HZ     (18 * 1000 * 1000)
    #define DISPLAY_LCD_BK_LIGHT_ON_LEVEL  1
    #define DISPLAY_LCD_BK_LIGHT_OFF_LEVEL !DISPLAY_LCD_BK_LIGHT_ON_LEVEL
    
    // The pixel number in horizontal and vertical
    #define DISPLAY_LCD_H_RES              800
    #define DISPLAY_LCD_V_RES              480

    #if CONFIG_DISPLAY_DOUBLE_FB
    #define DISPLAY_LCD_NUM_FB             2
    #else
    #define DISPLAY_LCD_NUM_FB             1
    #endif // CONFIG_DISPLAY_DOUBLE_FB
  
#endif

#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_169
    #define WAVESHARE_240_280_LCD_H_RES               (240)
    #define WAVESHARE_240_280_LCD_V_RES               (280)

    /* LCD settings */
    #define WAVESHARE_240_280_LCD_SPI_NUM             (SPI3_HOST)
    #define WAVESHARE_240_280_LCD_PIXEL_CLK_HZ        (40 * 1000 * 1000)
    #define WAVESHARE_240_280_LCD_CMD_BITS            (8)
    #define WAVESHARE_240_280_LCD_PARAM_BITS          (8)
    #define WAVESHARE_240_280_LCD_COLOR_SPACE         (ESP_LCD_COLOR_SPACE_RGB)
    #define WAVESHARE_240_280_LCD_BITS_PER_PIXEL      (16)
    #define WAVESHARE_240_280_LCD_DRAW_BUFF_DOUBLE    (1)
    #define WAVESHARE_240_280_LCD_DRAW_BUFF_HEIGHT    (50)
    #define WAVESHARE_240_280_LCD_BL_ON_LEVEL         (1)

    static esp_lcd_panel_io_handle_t lcd_io = NULL;
    static esp_lcd_panel_handle_t lcd_panel = NULL;
#endif

#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_M5ATOMS3R
    #define ATOM3SR_LCD_H_RES               (128)
    #define ATOM3SR_LCD_V_RES               (128)

    /* LCD settings */
    #define ATOM3SR_LCD_SPI_NUM             (SPI3_HOST)
    #define ATOM3SR_LCD_PIXEL_CLK_HZ        (40 * 1000 * 1000)
    #define ATOM3SR_LCD_CMD_BITS            (8)
    #define ATOM3SR_LCD_PARAM_BITS          (8)
    #define ATOM3SR_LCD_COLOR_SPACE         (ESP_LCD_COLOR_SPACE_RGB)
    #define ATOM3SR_LCD_BITS_PER_PIXEL      (16)
    #define ATOM3SR_LCD_DRAW_BUFF_DOUBLE    (1)
    #define ATOM3SR_LCD_DRAW_BUFF_HEIGHT    (50)
    #define ATOM3SR_LCD_BL_ON_LEVEL         (1)

    static esp_lcd_panel_io_handle_t lcd_io = NULL;
    static esp_lcd_panel_handle_t lcd_panel = NULL;
#endif

#define DISPLAY_LVGL_TICK_PERIOD_MS     2
#define DISPLAY_LVGL_TASK_MAX_DELAY_MS  500
#define DISPLAY_LVGL_TASK_MIN_DELAY_MS  1
#define BUF_SIZE                        (1024)
#define I2C_MASTER_TIMEOUT_MS           1000
#define MAX_UI_TEXT                     130

enum UIElements
{
    UI_ELEMENT_USB_STATUS,
    UI_ELEMENT_BT_STATUS,
    UI_ELEMENT_PRESET_NAME,
    UI_ELEMENT_AMP_SKIN,
    UI_ELEMENT_PRESET_DESCRIPTION,
    UI_ELEMENT_PARAMETERS
};

enum UIAction
{
    UI_ACTION_SET_STATE,
    UI_ACTION_SET_LABEL_TEXT,
    UI_ACTION_SET_ENTRY_TEXT
};

typedef struct 
{
    uint8_t ElementID;
    uint8_t Action;
    uint32_t Value;
    char Text[MAX_UI_TEXT];
} tUIUpdate;

static QueueHandle_t ui_update_queue;
static SemaphoreHandle_t I2CMutexHandle;
static SemaphoreHandle_t lvgl_mux = NULL;
static tTonexParameter TonexParametersCopy[TONEX_PARAM_LAST];

#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_169 || CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B || CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_M5ATOMS3R
    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
    static lv_disp_drv_t disp_drv;      // contains callback functions
#endif

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void __attribute__((unused)) display_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
#if CONFIG_DISPLAY_AVOID_TEAR_EFFECT_WITH_SEM
    xSemaphoreGive(sem_gui_ready);
    xSemaphoreTake(sem_vsync_end, portMAX_DELAY);
#endif
    // pass the draw buffer to the driver
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
    lv_disp_flush_ready(drv);
}

#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B
// we use two semaphores to sync the VSYNC event and the LVGL task, to avoid potential tearing effect
#if CONFIG_DISPLAY_AVOID_TEAR_EFFECT_WITH_SEM
SemaphoreHandle_t sem_vsync_end;
SemaphoreHandle_t sem_gui_ready;
#endif

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static bool display_on_vsync_event(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *event_data, void *user_data)
{
    BaseType_t high_task_awoken = pdFALSE;
#if CONFIG_DISPLAY_AVOID_TEAR_EFFECT_WITH_SEM
    if (xSemaphoreTakeFromISR(sem_gui_ready, &high_task_awoken) == pdTRUE) {
        xSemaphoreGiveFromISR(sem_vsync_end, &high_task_awoken);
    }
#endif
    return high_task_awoken == pdTRUE;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void display_lvgl_touch_cb(lv_indev_drv_t * drv, lv_indev_data_t * data)
{
    uint16_t touchpad_x[1] = {0};
    uint16_t touchpad_y[1] = {0};
    uint8_t touchpad_cnt = 0;
    bool touchpad_pressed = false;

    if (xSemaphoreTake(I2CMutexHandle, (TickType_t)10) == pdTRUE)
    {
        /* Read touch controller data */
        esp_lcd_touch_read_data(drv->user_data);

        /* Get coordinates */
        touchpad_pressed = esp_lcd_touch_get_coordinates(drv->user_data, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);

        xSemaphoreGive(I2CMutexHandle);
    }
    else
    {
        ESP_LOGE(TAG, "Touch cb mutex timeout");
    }

    if (touchpad_pressed && touchpad_cnt > 0) 
    {
        data->point.x = touchpad_x[0];
        data->point.y = touchpad_y[0];
        data->state = LV_INDEV_STATE_PR;
    } 
    else 
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void PreviousClicked(lv_event_t * e)
{
    // called from LVGL 
    ESP_LOGI(TAG, "UI Previous Clicked");      

    control_request_preset_down();      
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void NextClicked(lv_event_t * e)
{
    // called from LVGL 
    ESP_LOGI(TAG, "UI Next Clicked");    

    control_request_preset_up();        
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void AmpSkinPrevious(lv_event_t * e)
{
    control_set_skin_previous();
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void AmpSkinNext(lv_event_t * e)
{
    control_set_skin_next();
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void SaveUserDataRequest(lv_event_t * e)
{
    control_save_user_data(0);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void BTBondsClearRequest(lv_event_t * e)
{
    // request to clear bluetooth bonds
    midi_delete_bluetooth_bonds();
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void PresetDescriptionChanged(lv_event_t * e)
{
    char* text = (char*)lv_textarea_get_text(ui_PresetDetailsTextArea);

    ESP_LOGI(TAG, "PresetDescriptionChanged: %s", text);

    control_set_user_text(text);      
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void ParameterChanged(lv_event_t * e)
{
    // get the object that was changed
    lv_obj_t* obj = lv_event_get_current_target(e);

    ESP_LOGI(TAG, "Parameter changed");

    // see what it was, and update the pedal
    if (obj == ui_NoiseGateSwitch)
    {
        usb_modify_parameter(TONEX_PARAM_NOISE_GATE_ENABLE, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
    }
    else if (obj == ui_NoiseGatePostSwitch)
    {
        usb_modify_parameter(TONEX_PARAM_NOISE_GATE_POST, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
    }
    else if (obj == ui_NoiseGateThresholdSlider)
    {
        usb_modify_parameter(TONEX_PARAM_NOISE_GATE_THRESHOLD, lv_slider_get_value(obj));
    }
    else if (obj == ui_NoiseGateReleaseSlider)
    {
        usb_modify_parameter(TONEX_PARAM_NOISE_GATE_RELEASE, lv_slider_get_value(obj));
    }
    else if (obj == ui_NoiseGateDepthSlider)
    {
        usb_modify_parameter(TONEX_PARAM_NOISE_GATE_DEPTH, lv_slider_get_value(obj));
    }
    else if (obj == ui_CompressorEnableSwitch)
    {
        usb_modify_parameter(TONEX_PARAM_COMP_ENABLE, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
    }
    else if (obj == ui_CompressorPostSwitch)
    {
        usb_modify_parameter(TONEX_PARAM_COMP_POST, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
    }
    else if (obj == ui_CompressorThresholdSlider)
    {
        usb_modify_parameter(TONEX_PARAM_COMP_THRESHOLD, lv_slider_get_value(obj));
    }
    else if (obj == ui_CompresorAttackSlider)
    {
        usb_modify_parameter(TONEX_PARAM_COMP_ATTACK, lv_slider_get_value(obj));
    }
    else if (obj == ui_CompressorGainSlider)
    {
        usb_modify_parameter(TONEX_PARAM_COMP_MAKE_UP, lv_slider_get_value(obj));
    }
    else if (obj == ui_EQPostSwitch)
    {
        usb_modify_parameter(TONEX_PARAM_EQ_POST, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
    }
    else if (obj == ui_EQBassSlider)
    {
        usb_modify_parameter(TONEX_PARAM_EQ_BASS, lv_slider_get_value(obj));
    }
    else if (obj == ui_EQMidSlider)
    {
        usb_modify_parameter(TONEX_PARAM_EQ_MID, lv_slider_get_value(obj));
    }
    else if (obj == ui_EQTrebleSlider)
    {
        usb_modify_parameter(TONEX_PARAM_EQ_TREBLE, lv_slider_get_value(obj));
    }
    else if (obj == ui_ReverbEnableSwitch)
    {
        usb_modify_parameter(TONEX_PARAM_REVERB_ENABLE, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
    }
    else if (obj == ui_ReverbPostSwitch)
    {
        usb_modify_parameter(TONEX_PARAM_REVERB_POSITION, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
    }
    else if (obj == ui_ReverbModelDropdown)
    {
        usb_modify_parameter(TONEX_PARAM_REVERB_MODEL, lv_dropdown_get_selected(obj));
    }
    else if (obj == ui_ReverbMixSlider)
    {
        // check which model is set
        switch (lv_dropdown_get_selected(ui_ReverbModelDropdown))
        {
            case TONEX_REVERB_SPRING_1:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING1_MIX, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_SPRING_2:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING2_MIX, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_SPRING_3:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING3_MIX, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_SPRING_4:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING4_MIX, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_ROOM:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_ROOM_MIX, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_PLATE:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_PLATE_MIX, lv_slider_get_value(obj));
            } break;
        }        
    }
    else if (obj == ui_ReverbTimeSlider)
    {
        // check which model is set
        switch (lv_dropdown_get_selected(ui_ReverbModelDropdown))
        {
            case TONEX_REVERB_SPRING_1:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING1_TIME, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_SPRING_2:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING2_TIME, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_SPRING_3:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING3_TIME, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_SPRING_4:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING4_TIME, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_ROOM:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_ROOM_TIME, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_PLATE:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_PLATE_TIME, lv_slider_get_value(obj));
            } break;
        }        
    }
    else if (obj == ui_ReverbPredelaySlider)
    {
        // check which model is set
        switch (lv_dropdown_get_selected(ui_ReverbModelDropdown))
        {
            case TONEX_REVERB_SPRING_1:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING1_PREDELAY, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_SPRING_2:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING2_PREDELAY, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_SPRING_3:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING3_PREDELAY, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_SPRING_4:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING4_PREDELAY, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_ROOM:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_ROOM_PREDELAY, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_PLATE:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_PLATE_PREDELAY, lv_slider_get_value(obj));
            } break;
        }        
    }
    else if (obj == ui_ReverbColorSlider)
    {
        // check which model is set
        switch (lv_dropdown_get_selected(ui_ReverbModelDropdown))
        {
            case TONEX_REVERB_SPRING_1:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING1_COLOR, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_SPRING_2:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING2_COLOR, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_SPRING_3:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING3_COLOR, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_SPRING_4:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_SPRING4_COLOR, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_ROOM:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_ROOM_COLOR, lv_slider_get_value(obj));
            } break;

            case TONEX_REVERB_PLATE:
            {
                usb_modify_parameter(TONEX_PARAM_REVERB_PLATE_COLOR, lv_slider_get_value(obj));
            } break;
        }        
    }
    else if (obj == ui_ModulationEnableSwitch)
    {
        usb_modify_parameter(TONEX_PARAM_MODULATION_ENABLE, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
    }
    else if (obj == ui_ModulationPostSwitch)
    {
        usb_modify_parameter(TONEX_PARAM_MODULATION_POST, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
    }
    else if (obj == ui_ModulationModelDropdown)
    {
        usb_modify_parameter(TONEX_PARAM_MODULATION_MODEL, lv_dropdown_get_selected(obj));
    }
    else if (obj == ui_ModulationSyncSwitch)
    {
        // check which model is set
        switch (lv_dropdown_get_selected(ui_ModulationModelDropdown))
        {
            case TONEX_MODULATION_CHORUS:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_CHORUS_SYNC, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
            } break;

            case TONEX_MODULATION_TREMOLO:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_TREMOLO_SYNC, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
            } break;

            case TONEX_MODULATION_PHASER:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_PHASER_SYNC, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
            } break;

            case TONEX_MODULATION_FLANGER:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_FLANGER_SYNC, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
            } break;

            case TONEX_MODULATION_ROTARY:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_ROTARY_SYNC, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
            } break;
        }
    }
    else if (obj == ui_ModulationParam1Slider)
    {
        // check which model is set
        switch (lv_dropdown_get_selected(ui_ModulationModelDropdown))
        {
            case TONEX_MODULATION_CHORUS:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_CHORUS_RATE, lv_slider_get_value(obj));
            } break;

            case TONEX_MODULATION_TREMOLO:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_TREMOLO_RATE, lv_slider_get_value(obj));
            } break;

            case TONEX_MODULATION_PHASER:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_PHASER_RATE, lv_slider_get_value(obj));
            } break;

            case TONEX_MODULATION_FLANGER:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_FLANGER_RATE, lv_slider_get_value(obj));
            } break;

            case TONEX_MODULATION_ROTARY:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_ROTARY_SPEED, lv_slider_get_value(obj));
            } break;
        }
    }
    else if (obj == ui_ModulationParam2Slider)
    {
        // check which model is set
        switch (lv_dropdown_get_selected(ui_ModulationModelDropdown))
        {
            case TONEX_MODULATION_CHORUS:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_CHORUS_DEPTH, lv_slider_get_value(obj));
            } break;

            case TONEX_MODULATION_TREMOLO:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_TREMOLO_SHAPE, lv_slider_get_value(obj));
            } break;

            case TONEX_MODULATION_PHASER:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_PHASER_DEPTH, lv_slider_get_value(obj));
            } break;

            case TONEX_MODULATION_FLANGER:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_FLANGER_DEPTH, lv_slider_get_value(obj));
            } break;

            case TONEX_MODULATION_ROTARY:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_ROTARY_RADIUS, lv_slider_get_value(obj));
            } break;
        }
    }
    else if (obj == ui_ModulationParam3Slider)
    {
        // check which model is set
        switch (lv_dropdown_get_selected(ui_ModulationModelDropdown))
        {
            case TONEX_MODULATION_CHORUS:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_CHORUS_LEVEL, lv_slider_get_value(obj));
            } break;

            case TONEX_MODULATION_TREMOLO:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_TREMOLO_SPREAD, lv_slider_get_value(obj));
            } break;

            case TONEX_MODULATION_PHASER:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_PHASER_LEVEL, lv_slider_get_value(obj));
            } break;

            case TONEX_MODULATION_FLANGER:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_FLANGER_FEEDBACK, lv_slider_get_value(obj));
            } break;

            case TONEX_MODULATION_ROTARY:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_ROTARY_SPREAD, lv_slider_get_value(obj));
            } break;
        }
    }
    else if (obj == ui_ModulationParam4Slider)
    {
        // check which model is set
        switch (lv_dropdown_get_selected(ui_ModulationModelDropdown))
        {
            case TONEX_MODULATION_CHORUS:
            {
                // not used
            } break;

            case TONEX_MODULATION_TREMOLO:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_TREMOLO_LEVEL, lv_slider_get_value(obj));
            } break;

            case TONEX_MODULATION_PHASER:
            {
                // not used
            } break;

            case TONEX_MODULATION_FLANGER:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_FLANGER_LEVEL, lv_slider_get_value(obj));
            } break;

            case TONEX_MODULATION_ROTARY:
            {
                usb_modify_parameter(TONEX_PARAM_MODULATION_ROTARY_LEVEL, lv_slider_get_value(obj));
            } break;
        }
    }
    else if (obj == ui_DelayEnableSwitch)
    {
        usb_modify_parameter(TONEX_PARAM_DELAY_ENABLE, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
    }
    else if (obj == ui_DelayPostSwitch)
    {
        usb_modify_parameter(TONEX_PARAM_DELAY_POST, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
    }
    else if (obj == ui_DelayModelDropdown)
    {
        usb_modify_parameter(TONEX_PARAM_DELAY_MODEL, lv_dropdown_get_selected(obj));
    }
    else if (obj == ui_DelaySyncSwitch)
    {
        // check which model is set
        switch (lv_dropdown_get_selected(ui_DelayModelDropdown))
        {
            case TONEX_DELAY_DIGITAL:
            {
                usb_modify_parameter(TONEX_PARAM_DELAY_DIGITAL_SYNC, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
            } break;

            case TONEX_DELAY_TAPE:
            {
                usb_modify_parameter(TONEX_PARAM_DELAY_TAPE_SYNC, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
            } break;
        }
    }
    else if (obj == ui_DelayPingPongSwitch)
    {
        // check which model is set
        switch (lv_dropdown_get_selected(ui_DelayModelDropdown))
        {
            case TONEX_DELAY_DIGITAL:
            {
                usb_modify_parameter(TONEX_PARAM_DELAY_DIGITAL_MODE, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
            } break;

            case TONEX_DELAY_TAPE:
            {
                usb_modify_parameter(TONEX_PARAM_DELAY_TAPE_MODE, lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0);
            } break;
        }
    }
    else if (obj == ui_DelayTSSlider)
    {
        // check which model is set
        switch (lv_dropdown_get_selected(ui_DelayModelDropdown))
        {
            case TONEX_DELAY_DIGITAL:
            {
                usb_modify_parameter(TONEX_PARAM_DELAY_DIGITAL_TIME, lv_slider_get_value(obj));
            } break;

            case TONEX_DELAY_TAPE:
            {
                usb_modify_parameter(TONEX_PARAM_DELAY_TAPE_TIME, lv_slider_get_value(obj));
            } break;
        }
    }
    else if (obj == ui_DelayFeedbackSlider)
    {
        // check which model is set
        switch (lv_dropdown_get_selected(ui_DelayModelDropdown))
        {
            case TONEX_DELAY_DIGITAL:
            {
                usb_modify_parameter(TONEX_PARAM_DELAY_DIGITAL_FEEDBACK, lv_slider_get_value(obj));
            } break;

            case TONEX_DELAY_TAPE:
            {
                usb_modify_parameter(TONEX_PARAM_DELAY_TAPE_FEEDBACK, lv_slider_get_value(obj));
            } break;
        }        
    }
    else if (obj == ui_DelayMixSlider)
    {
        // check which model is set
        switch (lv_dropdown_get_selected(ui_DelayModelDropdown))
        {
            case TONEX_DELAY_DIGITAL:
            {
                usb_modify_parameter(TONEX_PARAM_DELAY_DIGITAL_MIX, lv_slider_get_value(obj));
            } break;

            case TONEX_DELAY_TAPE:
            {
                usb_modify_parameter(TONEX_PARAM_DELAY_TAPE_MIX, lv_slider_get_value(obj));
            } break;
        }     
    }
    else if (obj == ui_AmplifierGainSlider)
    {
        usb_modify_parameter(TONEX_PARAM_MODEL_GAIN, lv_slider_get_value(obj));
    }
    else if (obj == ui_AmplifierVolumeSlider)
    {
        usb_modify_parameter(TONEX_PARAM_MODEL_VOLUME, lv_slider_get_value(obj));
    }
    else if (obj == ui_AmplifierPresenseSlider)
    {
        //?? usb_modify_parameter(TONEX_PARAM_, lv_slider_get_value(obj));
    }
    else
    {
        ESP_LOGW(TAG, "Unknown Parameter changed");    
    }


    
    // to do
}
#endif

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static void __attribute__((unused)) display_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(DISPLAY_LVGL_TICK_PERIOD_MS);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
bool display_lvgl_lock(int timeout_ms)
{
    // Convert timeout in milliseconds to FreeRTOS ticks
    // If `timeout_ms` is set to -1, the program will block until the condition is met
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks) == pdTRUE;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void display_lvgl_unlock(void)
{
    xSemaphoreGiveRecursive(lvgl_mux);
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void UI_SetUSBStatus(uint8_t state)
{
    tUIUpdate ui_update;

    // build command
    ui_update.ElementID = UI_ELEMENT_USB_STATUS;
    ui_update.Action = UI_ACTION_SET_STATE;
    ui_update.Value = state;

    // send to queue
    if (xQueueSend(ui_update_queue, (void*)&ui_update, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "UI Update queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void UI_SetBTStatus(uint8_t state)
{
    tUIUpdate ui_update;

    // build command
    ui_update.ElementID = UI_ELEMENT_BT_STATUS;
    ui_update.Action = UI_ACTION_SET_STATE;
    ui_update.Value = state;

    // send to queue
    if (xQueueSend(ui_update_queue, (void*)&ui_update, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "UI Update queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void UI_SetPresetLabel(char* text)
{
    tUIUpdate ui_update;

    // build command
    ui_update.ElementID = UI_ELEMENT_PRESET_NAME;
    ui_update.Action = UI_ACTION_SET_LABEL_TEXT;
    strncpy(ui_update.Text, text, MAX_UI_TEXT - 1);

    // send to queue
    if (xQueueSend(ui_update_queue, (void*)&ui_update, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "UI Update queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void UI_SetAmpSkin(uint16_t index)
{
    tUIUpdate ui_update;

    // build commands
    ui_update.ElementID = UI_ELEMENT_AMP_SKIN;
    ui_update.Action = UI_ACTION_SET_STATE;
    ui_update.Value = index;

    // send to queue
    if (xQueueSend(ui_update_queue, (void*)&ui_update, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "UI Update queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void UI_SetPresetDescription(char* text)
{
    tUIUpdate ui_update;

    // build command
    ui_update.ElementID = UI_ELEMENT_PRESET_DESCRIPTION;
    ui_update.Action = UI_ACTION_SET_ENTRY_TEXT;
    strncpy(ui_update.Text, text, MAX_UI_TEXT - 1);

    // send to queue
    if (xQueueSend(ui_update_queue, (void*)&ui_update, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "UI Update queue send failed!");            
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void UI_SetCurrentParameterValues(tTonexParameter* params)
{
    tUIUpdate ui_update;

    // make a local copy of the current params, for thread safety
    memcpy((void*)TonexParametersCopy, (void*)params, sizeof(TonexParametersCopy));

    // build command
    ui_update.ElementID = UI_ELEMENT_PARAMETERS;
    
    // send to queue
    if (xQueueSend(ui_update_queue, (void*)&ui_update, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "UI Update parameters send failed!");            
    }
}

#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static lv_obj_t* ui_get_skin_image(uint16_t index)
{
    lv_obj_t* result;

    switch (index)
    {
#if CONFIG_TONEX_CONTROLLER_SKINS_AMP        
        // amps
        case AMP_SKIN_JCM800:
        {
            result = (lv_obj_t*)&ui_img_skin_jcm800_png;            
        } break;

        case AMP_SKIN_TWIN_REVERB:
        {
            result = (lv_obj_t*)&ui_img_skin_twinreverb_png;
        } break;

        case AMP_SKIN_2001RB:
        {
            result = (lv_obj_t*)&ui_img_skin_2001rb_png;
        } break;

        case AMP_SKIN_5150:
        {
            result = (lv_obj_t*)&ui_img_skin_5150_png;
        } break;

        case AMP_SKIN_B18N:
        {
            result = (lv_obj_t*)&ui_img_skin_b18n_png;
        } break;

        case AMP_SKIN_BLUES_DELUXE:
        {
            result = (lv_obj_t*)&ui_img_skin_bluesdeluxe_png;
        } break;

        case AMP_SKIN_DEVILLE:
        {
            result = (lv_obj_t*)&ui_img_skin_deville_png;
        } break;

        case AMP_SKIN_DUAL_RECTIFIER:
        {
            result = (lv_obj_t*)&ui_img_skin_dualrectifier_png;
        } break;

        case AMP_SKIN_GOLD_FINGER:
        {
            result = (lv_obj_t*)&ui_img_skin_goldfinger_png;
        } break;

        case AMP_SKIN_INVADER:
        {
            result = (lv_obj_t*)&ui_img_skin_invader_png;
        } break;

        case AMP_SKIN_JAZZ_CHORUS:
        {
            result = (lv_obj_t*)&ui_img_skin_jazzchorus_png;
        } break;

        case AMP_SKIN_OR_50:
        {
            result = (lv_obj_t*)&ui_img_skin_or50_png;
        } break;

        case AMP_SKIN_POWERBALL:
        {
            result = (lv_obj_t*)&ui_img_skin_powerball_png;
        } break;

        case AMP_SKIN_PRINCETON:
        {
            result = (lv_obj_t*)&ui_img_skin_princeton_png;
        } break;

        case AMP_SKIN_SVTCL:
        {
            result = (lv_obj_t*)&ui_img_skin_svtcl_png;
        } break;

        case AMP_SKIN_MAVERICK:
        {
            result = (lv_obj_t*)&ui_img_skin_maverick_png;
        } break;

        case AMP_SKIN_MK3:
        {
            result = (lv_obj_t*)&ui_img_skin_mk3_png;
        } break;

        case AMP_SKIN_SUPERBASS:
        {
            result = (lv_obj_t*)&ui_img_skin_superbass_png;
        } break;

        case AMP_SKIN_DUMBLE:
        {
            result = (lv_obj_t*)&ui_img_skin_dumble_png;
        } break;

        case AMP_SKIN_JETCITY:
        {
            result = (lv_obj_t*)&ui_img_skin_jetcity_png;
        } break;

        case AMP_SKIN_AC30:
        {
            result = (lv_obj_t*)&ui_img_skin_ac30_png;
        } break;

        case AMP_SKIN_EVH5150:
        {
            result = (lv_obj_t*)&ui_img_skin_evh5150_png;
        } break;

        case AMP_SKIN_2020:
        {
            result = (lv_obj_t*)&ui_img_skin_2020_png;
        } break;

        case AMP_SKIN_PINK_TACO:
        {
            result = (lv_obj_t*)&ui_img_skin_pinktaco_png;
        } break;

        case AMP_SKIN_SUPRO_50:
        {
            result = (lv_obj_t*)&ui_img_skin_supro50_png;
        } break;

        case AMP_SKIN_DIEZEL:
        {
            result = (lv_obj_t*)&ui_img_skin_diezel_png;
        } break;
#endif  //CONFIG_TONEX_CONTROLLER_SKINS_AMP

#if CONFIG_TONEX_CONTROLLER_SKINS_PEDAL
        // pedals
        case PEDAL_SKIN_ARION:
        {
            result = (lv_obj_t*)&ui_img_pskin_arion_png;
        } break;

        case PEDAL_SKIN_BIGMUFF:
        {
            result = (lv_obj_t*)&ui_img_pskin_bigmuff_png;
        } break;

        case PEDAL_SKIN_DARKGLASS:
        {
            result = (lv_obj_t*)&ui_img_pskin_darkglass_png;
        } break;

        case PEDAL_SKIN_DOD:
        {
            result = (lv_obj_t*)&ui_img_pskin_dod_png;
        } break;

        case PEDAL_SKIN_EHX:
        {
            result = (lv_obj_t*)&ui_img_pskin_ehx_png;
        } break;

        case PEDAL_SKIN_FENDER:
        {
            result = (lv_obj_t*)&ui_img_pskin_fender_png;
        } break;

        case PEDAL_SKIN_FULLTONE:
        {
            result = (lv_obj_t*)&ui_img_pskin_fulltone_png;
        } break;

        case PEDAL_SKIN_FZS:
        {
            result = (lv_obj_t*)&ui_img_pskin_fzs_png;
        } break;

        case PEDAL_SKIN_JHS:
        {
            result = (lv_obj_t*)&ui_img_pskin_jhs_png;
        } break;

        case PEDAL_SKIN_KLON:
        {
            result = (lv_obj_t*)&ui_img_pskin_klon_png;
        } break;

        case PEDAL_SKIN_LANDGRAF:
        {
            result = (lv_obj_t*)&ui_img_pskin_landgraf_png;
        } break;

        case PEDAL_SKIN_MXR:
        {
            result = (lv_obj_t*)&ui_img_pskin_mxr_png;
        } break;

        case PEDAL_SKIN_MXR2:
        {
            result = (lv_obj_t*)&ui_img_pskin_mxr2_png;
        } break;

        case PEDAL_SKIN_OD1:
        {
            result = (lv_obj_t*)&ui_img_pskin_od1_png;
        } break;

        case PEDAL_SKIN_PLIMSOUL:
        {
            result = (lv_obj_t*)&ui_img_pskin_plimsoul_png;
        } break;

        case PEDAL_SKIN_ROGERMAYER:
        {
            result = (lv_obj_t*)&ui_img_pskin_rogermayer_png;
        } break;

        case PEDAL_SKIN_SEYMOUR:
        {
            result = (lv_obj_t*)&ui_img_pskin_seymour_png;
        } break;

        case PEDAL_SKIN_STRYMON:
        {
            result = (lv_obj_t*)&ui_img_pskin_strymon_png;
        } break;

        case PEDAL_SKIN_TREX:
        {
            result = (lv_obj_t*)&ui_img_pskin_trex_png;
        } break;

        case PEDAL_SKIN_TUBESCREAMER:
        {
            result = (lv_obj_t*)&ui_img_pskin_tubescreamer_png;
        } break;

        case PEDAL_SKIN_WAMPLER:
        {
            result = (lv_obj_t*)&ui_img_pskin_wampler_png;
        } break;

        case PEDAL_SKIN_ZVEX:
        {
            result = (lv_obj_t*)&ui_img_pskin_zvex_png;
        } break;
#endif //CONFIG_TONEX_CONTROLLER_SKINS_PEDAL

        default:
        {
            result = (lv_obj_t*)&ui_img_skin_jcm800_png;            
        } break;
    }

    return result;
}
#endif 

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
static uint8_t update_ui_element(tUIUpdate* update)
{
#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_169 || CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B || CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_M5ATOMS3R
    lv_obj_t* element_1 = NULL;

    switch (update->ElementID)
    {
        case UI_ELEMENT_USB_STATUS:
        {
            element_1 = ui_USBStatusFail;
        } break;

        case UI_ELEMENT_BT_STATUS:
        {
            element_1 = ui_BTStatusConn;
        } break;

        case UI_ELEMENT_PRESET_NAME:
        {
            element_1 = ui_PresetHeadingLabel;
        } break;

        case UI_ELEMENT_AMP_SKIN:
        {
#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B
            element_1 = ui_SkinImage;
#endif            
        } break;

        case UI_ELEMENT_PRESET_DESCRIPTION:
        {
#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B            
            element_1 = ui_PresetDetailsTextArea;
#endif            
        } break;

        case UI_ELEMENT_PARAMETERS:
        {
#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B     
            ESP_LOGI(TAG, "Syncing params to UI");

            for (uint16_t param = 0; param < TONEX_PARAM_LAST; param++)
            {                
                tTonexParameter* param_entry = &TonexParametersCopy[param];

                // debug
                //ESP_LOGI(TAG, "Param %d: val: %02f, min: %02f, max: %02f", param, param_entry->Value, param_entry->Min, param_entry->Max);

                switch (param)
                {
                    case TONEX_PARAM_NOISE_GATE_POST:
                    {
                        if (param_entry->Value)
                        {
                            lv_obj_add_state(ui_NoiseGatePostSwitch, LV_STATE_CHECKED);
                        }
                        else
                        {
                            lv_obj_clear_state(ui_NoiseGatePostSwitch, LV_STATE_CHECKED);
                        }
                    } break;

                    case TONEX_PARAM_NOISE_GATE_ENABLE:
                    {
                        if (param_entry->Value)
                        {
                            lv_obj_add_state(ui_NoiseGateSwitch, LV_STATE_CHECKED);
                        }
                        else
                        {
                            lv_obj_clear_state(ui_NoiseGateSwitch, LV_STATE_CHECKED);
                        }
                    } break;

                    case TONEX_PARAM_NOISE_GATE_THRESHOLD:
                    {
                        lv_slider_set_value(ui_NoiseGateThresholdSlider, param_entry->Value, LV_ANIM_OFF);
                        lv_slider_set_range(ui_NoiseGateThresholdSlider, param_entry->Min, param_entry->Max);
                    } break;

                    case TONEX_PARAM_NOISE_GATE_RELEASE:
                    {
                        lv_slider_set_value(ui_NoiseGateReleaseSlider, param_entry->Value, LV_ANIM_OFF);
                        lv_slider_set_range(ui_NoiseGateReleaseSlider, param_entry->Min, param_entry->Max);
                    } break;

                    case TONEX_PARAM_NOISE_GATE_DEPTH:
                    {
                        lv_slider_set_value(ui_NoiseGateDepthSlider, param_entry->Value, LV_ANIM_OFF);
                        lv_slider_set_range(ui_NoiseGateDepthSlider, param_entry->Min, param_entry->Max);
                    } break;

                    case TONEX_PARAM_COMP_POST:
                    {
                        if (param_entry->Value)
                        {
                            lv_obj_add_state(ui_CompressorPostSwitch, LV_STATE_CHECKED);
                        }
                        else
                        {
                            lv_obj_clear_state(ui_CompressorPostSwitch, LV_STATE_CHECKED);
                        }
                    } break;

                    case TONEX_PARAM_COMP_ENABLE:
                    {
                        if (param_entry->Value)
                        {
                            lv_obj_add_state(ui_CompressorEnableSwitch, LV_STATE_CHECKED);
                        }
                        else
                        {
                            lv_obj_clear_state(ui_CompressorEnableSwitch, LV_STATE_CHECKED);
                        }
                    } break;

                    case TONEX_PARAM_COMP_THRESHOLD:
                    {
                        lv_slider_set_value(ui_CompressorThresholdSlider, param_entry->Value, LV_ANIM_OFF);
                        lv_slider_set_range(ui_CompressorThresholdSlider, param_entry->Min, param_entry->Max);
                    } break;

                    case TONEX_PARAM_COMP_MAKE_UP:
                    {
                        lv_slider_set_value(ui_CompressorGainSlider, param_entry->Value, LV_ANIM_OFF);
                        lv_slider_set_range(ui_CompressorGainSlider, param_entry->Min, param_entry->Max);
                    } break;

                    case TONEX_PARAM_COMP_ATTACK:
                    {
                        lv_slider_set_value(ui_CompresorAttackSlider, param_entry->Value, LV_ANIM_OFF);
                        lv_slider_set_range(ui_CompresorAttackSlider, param_entry->Min, param_entry->Max);
                    } break;

                    case TONEX_PARAM_EQ_POST:
                    {
                        if (param_entry->Value)
                        {
                            lv_obj_add_state(ui_EQPostSwitch, LV_STATE_CHECKED);
                        }
                        else
                        {
                            lv_obj_clear_state(ui_EQPostSwitch, LV_STATE_CHECKED);
                        }
                    } break;

                    case TONEX_PARAM_EQ_BASS:
                    {
                        lv_slider_set_value(ui_EQBassSlider, param_entry->Value, LV_ANIM_OFF);
                        lv_slider_set_range(ui_EQBassSlider, param_entry->Min, param_entry->Max);
                    } break;

                    case TONEX_PARAM_EQ_BASS_FREQ:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_EQ_MID:
                    {
                        lv_slider_set_value(ui_EQMidSlider, param_entry->Value, LV_ANIM_OFF);
                        lv_slider_set_range(ui_EQMidSlider, param_entry->Min, param_entry->Max);
                    } break;

                    case TONEX_PARAM_EQ_MIDQ:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_EQ_MID_FREQ:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_EQ_TREBLE:
                    {
                        lv_slider_set_value(ui_EQTrebleSlider, param_entry->Value, LV_ANIM_OFF);
                        lv_slider_set_range(ui_EQTrebleSlider, param_entry->Min, param_entry->Max);
                    } break;

                    case TONEX_PARAM_EQ_TREBLE_FREQ:
                    {
                        // not exposed via UI    
                    } break;
                    
                    case TONEX_PARAM_UNKNOWN_1:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_UNKNOWN_2:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_MODEL_GAIN:
                    {
                        lv_slider_set_value(ui_AmplifierGainSlider, param_entry->Value, LV_ANIM_OFF);
                        lv_slider_set_range(ui_AmplifierGainSlider, param_entry->Min, param_entry->Max);
                    } break;

                    case TONEX_PARAM_MODEL_VOLUME:
                    {
                        lv_slider_set_value(ui_AmplifierVolumeSlider, param_entry->Value, LV_ANIM_OFF);
                        lv_slider_set_range(ui_AmplifierVolumeSlider, param_entry->Min, param_entry->Max);
                    } break;

                    case TONEX_PARAM_MODEX_MIX:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_UNKNOWN_3:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_UNKNOWN_4:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_UNKNOWN_5:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_UNKNOWN_6:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_UNKNOWN_7:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_UNKNOWN_8:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_UNKNOWN_9:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_UNKNOWN_10:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_UNKNOWN_11:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_UNKNOWN_12:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_UNKNOWN_13:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_UNKNOWN_14:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_UNKNOWN_15:
                    {
                        // not exposed via UI
                    } break;
                    
                    case TONEX_PARAM_REVERB_POSITION:
                    {
                        if (param_entry->Value)
                        {
                            lv_obj_add_state(ui_ReverbPostSwitch, LV_STATE_CHECKED);
                        }
                        else
                        {
                            lv_obj_clear_state(ui_ReverbPostSwitch, LV_STATE_CHECKED);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_ENABLE:
                    {
                        if (param_entry->Value)
                        {
                            lv_obj_add_state(ui_ReverbEnableSwitch, LV_STATE_CHECKED);
                        }
                        else
                        {
                            lv_obj_clear_state(ui_ReverbEnableSwitch, LV_STATE_CHECKED);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_MODEL:
                    {
                        lv_dropdown_set_selected(ui_ReverbModelDropdown, param_entry->Value);
                    } break;

                    case TONEX_PARAM_REVERB_SPRING1_TIME:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_1)
                        {                            
                            lv_slider_set_value(ui_ReverbTimeSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbTimeSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING1_PREDELAY:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_1)
                        {                            
                            lv_slider_set_value(ui_ReverbPredelaySlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbPredelaySlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING1_COLOR:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_1)
                        {                            
                            lv_slider_set_value(ui_ReverbColorSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbColorSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING1_MIX:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_1)
                        {                            
                            lv_slider_set_value(ui_ReverbMixSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbMixSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING2_TIME:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_2)
                        {                            
                            lv_slider_set_value(ui_ReverbTimeSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbTimeSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING2_PREDELAY:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_2)
                        {                            
                            lv_slider_set_value(ui_ReverbPredelaySlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbPredelaySlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING2_COLOR:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_2)
                        {                            
                            lv_slider_set_value(ui_ReverbColorSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbColorSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING2_MIX:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_2)
                        {                            
                            lv_slider_set_value(ui_ReverbMixSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbMixSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING3_TIME:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_3)
                        {                            
                            lv_slider_set_value(ui_ReverbTimeSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbTimeSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING3_PREDELAY:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_3)
                        {                            
                            lv_slider_set_value(ui_ReverbPredelaySlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbPredelaySlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING3_COLOR:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_3)
                        {                            
                            lv_slider_set_value(ui_ReverbColorSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbColorSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING3_MIX:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_3)
                        {                            
                            lv_slider_set_value(ui_ReverbMixSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbMixSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING4_TIME:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_4)
                        {                            
                            lv_slider_set_value(ui_ReverbTimeSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbTimeSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING4_PREDELAY:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_4)
                        {                            
                            lv_slider_set_value(ui_ReverbPredelaySlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbPredelaySlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING4_COLOR:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_4)
                        {                            
                            lv_slider_set_value(ui_ReverbColorSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbColorSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_SPRING4_MIX:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_SPRING_4)
                        {                            
                            lv_slider_set_value(ui_ReverbMixSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbMixSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_ROOM_TIME:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_ROOM)
                        {                            
                            lv_slider_set_value(ui_ReverbTimeSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbTimeSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_ROOM_PREDELAY:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_ROOM)
                        {                            
                            lv_slider_set_value(ui_ReverbPredelaySlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbPredelaySlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_ROOM_COLOR:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_ROOM)
                        {                            
                            lv_slider_set_value(ui_ReverbColorSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbColorSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_ROOM_MIX:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_ROOM)
                        {                            
                            lv_slider_set_value(ui_ReverbMixSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbMixSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_PLATE_TIME:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_PLATE)
                        {                            
                            lv_slider_set_value(ui_ReverbTimeSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbTimeSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_PLATE_PREDELAY:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_PLATE)
                        {                            
                            lv_slider_set_value(ui_ReverbPredelaySlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbPredelaySlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_PLATE_COLOR:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_PLATE)
                        {                            
                            lv_slider_set_value(ui_ReverbColorSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbColorSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_REVERB_PLATE_MIX:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_REVERB_MODEL].Value == TONEX_REVERB_PLATE)
                        {                            
                            lv_slider_set_value(ui_ReverbMixSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ReverbMixSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_POST:
                    {
                        if (param_entry->Value)
                        {
                            lv_obj_add_state(ui_ModulationPostSwitch, LV_STATE_CHECKED);
                        }
                        else
                        {
                            lv_obj_clear_state(ui_ModulationPostSwitch, LV_STATE_CHECKED);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_ENABLE:
                    {
                        if (param_entry->Value)
                        {
                            lv_obj_add_state(ui_ModulationEnableSwitch, LV_STATE_CHECKED);
                        }
                        else
                        {
                            lv_obj_clear_state(ui_ModulationEnableSwitch, LV_STATE_CHECKED);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_MODEL:
                    {
                        lv_dropdown_set_selected(ui_ModulationModelDropdown, param_entry->Value);

                        // configure the variable UI items
                        switch ((int)param_entry->Value)
                        {
                            case TONEX_MODULATION_CHORUS:
                            {
                                lv_label_set_text(ui_ModulationParam1Label, "Rate");
                                lv_label_set_text(ui_ModulationParam2Label, "Depth");
                                lv_label_set_text(ui_ModulationParam3Label, "Level");
                                lv_obj_add_flag(ui_ModulationParam4Label, LV_OBJ_FLAG_HIDDEN);
                                lv_obj_add_flag(ui_ModulationParam4Slider, LV_OBJ_FLAG_HIDDEN);
                            } break;

                            case TONEX_MODULATION_TREMOLO:
                            {
                                lv_label_set_text(ui_ModulationParam1Label, "Rate");
                                lv_label_set_text(ui_ModulationParam2Label, "Shape");
                                lv_label_set_text(ui_ModulationParam3Label, "Spread");
                                lv_label_set_text(ui_ModulationParam4Label, "Level");
                                lv_obj_clear_flag(ui_ModulationParam4Label, LV_OBJ_FLAG_HIDDEN);
                                lv_obj_clear_flag(ui_ModulationParam4Slider, LV_OBJ_FLAG_HIDDEN);
                            } break;

                            case TONEX_MODULATION_PHASER:
                            {
                                lv_label_set_text(ui_ModulationParam1Label, "Rate");
                                lv_label_set_text(ui_ModulationParam2Label, "Depth");
                                lv_label_set_text(ui_ModulationParam3Label, "Level");
                                lv_obj_add_flag(ui_ModulationParam4Label, LV_OBJ_FLAG_HIDDEN);
                                lv_obj_add_flag(ui_ModulationParam4Slider, LV_OBJ_FLAG_HIDDEN);
                            } break;

                            case TONEX_MODULATION_FLANGER:
                            {
                                lv_label_set_text(ui_ModulationParam1Label, "Rate");
                                lv_label_set_text(ui_ModulationParam2Label, "Depth");
                                lv_label_set_text(ui_ModulationParam3Label, "Feedback");
                                lv_label_set_text(ui_ModulationParam4Label, "Level");
                                lv_obj_clear_flag(ui_ModulationParam4Label, LV_OBJ_FLAG_HIDDEN);
                                lv_obj_clear_flag(ui_ModulationParam4Slider, LV_OBJ_FLAG_HIDDEN);
                            } break;

                            case TONEX_MODULATION_ROTARY:
                            {
                                lv_label_set_text(ui_ModulationParam1Label, "Speed");
                                lv_label_set_text(ui_ModulationParam2Label, "Radius");
                                lv_label_set_text(ui_ModulationParam3Label, "Spread");
                                lv_label_set_text(ui_ModulationParam4Label, "Level");
                                lv_obj_clear_flag(ui_ModulationParam4Label, LV_OBJ_FLAG_HIDDEN);
                                lv_obj_clear_flag(ui_ModulationParam4Slider, LV_OBJ_FLAG_HIDDEN);
                            } break;

                            default:
                            {
                                ESP_LOGW(TAG, "Unknown modulation model: %d", (int)param_entry->Value);
                            } break;
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_CHORUS_SYNC:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_CHORUS)
                        {      
                            if (param_entry->Value)
                            {
                                lv_obj_add_state(ui_ModulationSyncSwitch, LV_STATE_CHECKED);
                            }
                            else
                            {
                                lv_obj_clear_state(ui_ModulationSyncSwitch, LV_STATE_CHECKED);
                            }                        
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_CHORUS_TS:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_MODULATION_CHORUS_RATE:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_CHORUS)
                        { 
                            lv_slider_set_value(ui_ModulationParam1Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam1Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_CHORUS_DEPTH:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_CHORUS)
                        { 
                            lv_slider_set_value(ui_ModulationParam2Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam2Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_CHORUS_LEVEL:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_CHORUS)
                        { 
                            lv_slider_set_value(ui_ModulationParam3Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam3Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_TREMOLO_SYNC:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_TREMOLO)
                        {      
                            if (param_entry->Value)
                            {
                                lv_obj_add_state(ui_ModulationSyncSwitch, LV_STATE_CHECKED);
                            }
                            else
                            {
                                lv_obj_clear_state(ui_ModulationSyncSwitch, LV_STATE_CHECKED);
                            }                        
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_TREMOLO_TS:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_MODULATION_TREMOLO_RATE:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_TREMOLO)
                        { 
                            lv_slider_set_value(ui_ModulationParam1Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam1Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_TREMOLO_SHAPE:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_TREMOLO)
                        { 
                            lv_slider_set_value(ui_ModulationParam2Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam2Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_TREMOLO_SPREAD:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_TREMOLO)
                        { 
                            lv_slider_set_value(ui_ModulationParam3Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam3Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_TREMOLO_LEVEL:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_TREMOLO)
                        { 
                            lv_slider_set_value(ui_ModulationParam4Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam4Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_PHASER_SYNC:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_PHASER)
                        {      
                            if (param_entry->Value)
                            {
                                lv_obj_add_state(ui_ModulationSyncSwitch, LV_STATE_CHECKED);
                            }
                            else
                            {
                                lv_obj_clear_state(ui_ModulationSyncSwitch, LV_STATE_CHECKED);
                            }                        
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_PHASER_TS:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_MODULATION_PHASER_RATE:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_PHASER)
                        { 
                            lv_slider_set_value(ui_ModulationParam1Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam1Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_PHASER_DEPTH:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_PHASER)
                        { 
                            lv_slider_set_value(ui_ModulationParam2Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam2Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_PHASER_LEVEL:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_PHASER)
                        { 
                            lv_slider_set_value(ui_ModulationParam3Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam3Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_FLANGER_SYNC:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_FLANGER)
                        {      
                            if (param_entry->Value)
                            {
                                lv_obj_add_state(ui_ModulationSyncSwitch, LV_STATE_CHECKED);
                            }
                            else
                            {
                                lv_obj_clear_state(ui_ModulationSyncSwitch, LV_STATE_CHECKED);
                            }                        
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_FLANGER_TS:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_MODULATION_FLANGER_RATE:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_FLANGER)
                        { 
                            lv_slider_set_value(ui_ModulationParam1Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam1Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_FLANGER_DEPTH:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_FLANGER)
                        { 
                            lv_slider_set_value(ui_ModulationParam2Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam2Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_FLANGER_FEEDBACK:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_FLANGER)
                        { 
                            lv_slider_set_value(ui_ModulationParam3Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam3Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_FLANGER_LEVEL:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_FLANGER)
                        { 
                            lv_slider_set_value(ui_ModulationParam4Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam4Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_ROTARY_SYNC:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_ROTARY)
                        {      
                            if (param_entry->Value)
                            {
                                lv_obj_add_state(ui_ModulationSyncSwitch, LV_STATE_CHECKED);
                            }
                            else
                            {
                                lv_obj_clear_state(ui_ModulationSyncSwitch, LV_STATE_CHECKED);
                            }                        
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_ROTARY_TS:
                    {
                        // not exposed via UI
                    } break;

                    case TONEX_PARAM_MODULATION_ROTARY_SPEED:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_ROTARY)
                        { 
                            lv_slider_set_value(ui_ModulationParam1Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam1Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_ROTARY_RADIUS:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_ROTARY)
                        { 
                            lv_slider_set_value(ui_ModulationParam2Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam2Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_ROTARY_SPREAD:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_ROTARY)
                        { 
                            lv_slider_set_value(ui_ModulationParam3Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam3Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_MODULATION_ROTARY_LEVEL:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_MODULATION_MODEL].Value == TONEX_MODULATION_ROTARY)
                        { 
                            lv_slider_set_value(ui_ModulationParam4Slider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_ModulationParam4Slider, param_entry->Min, param_entry->Max);
                        }
                    } break;
                    
                    case TONEX_PARAM_DELAY_POST:
                    {
                        if (param_entry->Value)
                        {
                            lv_obj_add_state(ui_DelayPostSwitch, LV_STATE_CHECKED);
                        }
                        else
                        {
                            lv_obj_clear_state(ui_DelayPostSwitch, LV_STATE_CHECKED);
                        }
                    } break;

                    case TONEX_PARAM_DELAY_ENABLE:
                    {
                        if (param_entry->Value)
                        {
                            lv_obj_add_state(ui_DelayEnableSwitch, LV_STATE_CHECKED);
                        }
                        else
                        {
                            lv_obj_clear_state(ui_DelayEnableSwitch, LV_STATE_CHECKED);
                        }
                    } break;

                    case TONEX_PARAM_DELAY_MODEL:
                    {
                        lv_dropdown_set_selected(ui_DelayModelDropdown, param_entry->Value);
                    } break;

                    case TONEX_PARAM_DELAY_DIGITAL_SYNC:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_DELAY_MODEL].Value == TONEX_DELAY_DIGITAL)
                        {
                            if (param_entry->Value)
                            {
                                lv_obj_add_state(ui_DelaySyncSwitch, LV_STATE_CHECKED);
                            }
                            else
                            {
                                lv_obj_clear_state(ui_DelaySyncSwitch, LV_STATE_CHECKED);
                            }
                        }
                    } break;

                    case TONEX_PARAM_DELAY_DIGITAL_TS:
                    {
                         // not exposed via UI
                    } break;

                    case TONEX_PARAM_DELAY_DIGITAL_TIME:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_DELAY_MODEL].Value == TONEX_DELAY_DIGITAL)
                        { 
                            lv_slider_set_value(ui_DelayTSSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_DelayTSSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_DELAY_DIGITAL_FEEDBACK:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_DELAY_MODEL].Value == TONEX_DELAY_DIGITAL)
                        { 
                            lv_slider_set_value(ui_DelayFeedbackSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_DelayFeedbackSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_DELAY_DIGITAL_MODE:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_DELAY_MODEL].Value == TONEX_DELAY_DIGITAL)
                        {
                            if (param_entry->Value)
                            {
                                lv_obj_add_state(ui_DelayPingPongSwitch, LV_STATE_CHECKED);
                            }
                            else
                            {
                                lv_obj_clear_state(ui_DelayPingPongSwitch, LV_STATE_CHECKED);
                            }
                        }
                    } break;

                    case TONEX_PARAM_DELAY_DIGITAL_MIX:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_DELAY_MODEL].Value == TONEX_DELAY_DIGITAL)
                        { 
                            lv_slider_set_value(ui_DelayMixSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_DelayMixSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_DELAY_TAPE_SYNC:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_DELAY_MODEL].Value == TONEX_DELAY_TAPE)
                        {
                            if (param_entry->Value)
                            {
                                lv_obj_add_state(ui_DelaySyncSwitch, LV_STATE_CHECKED);
                            }
                            else
                            {
                                lv_obj_clear_state(ui_DelaySyncSwitch, LV_STATE_CHECKED);
                            }
                        }
                    } break;

                    case TONEX_PARAM_DELAY_TAPE_TS:
                    {
                        // not exposed via UI   
                    } break;

                    case TONEX_PARAM_DELAY_TAPE_TIME:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_DELAY_MODEL].Value == TONEX_DELAY_TAPE)
                        { 
                            lv_slider_set_value(ui_DelayTSSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_DelayTSSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_DELAY_TAPE_FEEDBACK:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_DELAY_MODEL].Value == TONEX_DELAY_TAPE)
                        { 
                            lv_slider_set_value(ui_DelayFeedbackSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_DelayFeedbackSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;

                    case TONEX_PARAM_DELAY_TAPE_MODE:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_DELAY_MODEL].Value == TONEX_DELAY_TAPE)
                        {
                            if (param_entry->Value)
                            {
                                lv_obj_add_state(ui_DelayPingPongSwitch, LV_STATE_CHECKED);
                            }
                            else
                            {
                                lv_obj_clear_state(ui_DelayPingPongSwitch, LV_STATE_CHECKED);
                            }
                        }
                    } break;
                    
                    case TONEX_PARAM_DELAY_TAPE_MIX:
                    {
                        if (TonexParametersCopy[TONEX_PARAM_DELAY_MODEL].Value == TONEX_DELAY_TAPE)
                        { 
                            lv_slider_set_value(ui_DelayMixSlider, param_entry->Value, LV_ANIM_OFF);
                            lv_slider_set_range(ui_DelayMixSlider, param_entry->Min, param_entry->Max);
                        }
                    } break;
                }                
            }
#endif            
        } break;

        default:
        {
            ESP_LOGE(TAG, "Unknown display elment %d", update->ElementID);     
            return 0;        
        } break;
    }
    
    // check the action
    switch (update->Action)
    {
        case UI_ACTION_SET_STATE:
        {
            // check the element
            if (element_1 == ui_USBStatusFail)
            {
                if (update->Value == 0)
                {
                    // show the USB disconnected image
                    lv_obj_add_flag(ui_USBStatusOK, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(ui_USBStatusFail, LV_OBJ_FLAG_HIDDEN);
                }
                else
                {
                    // show the USB connected image
                    lv_obj_add_flag(ui_USBStatusFail, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(ui_USBStatusOK, LV_OBJ_FLAG_HIDDEN);
                }
            }
            else if (element_1 == ui_BTStatusConn)
            {
                if (update->Value == 0)
                {
                    // show the BT disconnected image
                    lv_obj_add_flag(ui_BTStatusConn, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(ui_BTStatusDisconn, LV_OBJ_FLAG_HIDDEN);
                }
                else
                {
                    // show the BT connected image
                    lv_obj_add_flag(ui_BTStatusDisconn, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(ui_BTStatusConn, LV_OBJ_FLAG_HIDDEN);
                }
            }
#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B            
            else if (element_1 == ui_SkinImage)
            {
                // set skin
                lv_img_set_src(ui_SkinImage, ui_get_skin_image(update->Value));
            }
#endif            
        } break;

        case UI_ACTION_SET_LABEL_TEXT:
        {
#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B
            lv_label_set_text(element_1, update->Text);
#elif CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_169 || CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_M5ATOMS3R
            if (element_1 == ui_PresetHeadingLabel)
            {
                // split up preset into 2 text lines.
                // incoming has "XX: Name"
                char preset_index[16];
                char preset_name[33];

                // get the preset number
                sprintf(preset_index, "%d", atoi(update->Text));
                lv_label_set_text(ui_PresetHeadingLabel, preset_index);

                // get the preset name
                for (uint8_t loop = 0; loop < 4; loop++)
                {
                    if (update->Text[loop] == ':')
                    {
                        strncpy(preset_name, (const char*)&update->Text[loop + 2], sizeof(preset_name) - 1);
                        lv_label_set_text(ui_PresetHeadingLabel2, preset_name);
                        break;
                    }
                }
            }
            else
            {
                lv_label_set_text(element_1, update->Text);
            }
#endif            
        } break;

        case UI_ACTION_SET_ENTRY_TEXT:
        {
#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B
            lv_textarea_set_text(element_1, update->Text);
#endif            
        } break;

        default:
        {
            ESP_LOGE(TAG, "Unknown display action");
        } break;
    }
#endif 

    return 1;
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void display_task(void *arg)
{
    tUIUpdate ui_update;
    ESP_LOGI(TAG, "Display task start");

    while (1) 
    {
        // Lock the mutex due to the LVGL APIs are not thread-safe
        if (display_lvgl_lock(-1)) 
        {
            lv_task_handler();

            // check for any UI update messages
            if (xQueueReceive(ui_update_queue, (void*)&ui_update, 0) == pdPASS)
            {
                // process it
                update_ui_element(&ui_update);
            }

            // Release the mutex
            display_lvgl_unlock();
	    }
        
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/****************************************************************************
* NAME:        
* DESCRIPTION: 
* PARAMETERS:  
* RETURN:      
* NOTES:       
*****************************************************************************/
void display_init(i2c_port_t I2CNum, SemaphoreHandle_t I2CMutex)
{    
    I2CMutexHandle = I2CMutex;

    // create queue for UI updates from other threads
    ui_update_queue = xQueueCreate(25, sizeof(tUIUpdate));
    if (ui_update_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create UI update queue!");
    }

    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    assert(lvgl_mux);

#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B    
    uint8_t touch_ok = 0;
    esp_err_t ret = ESP_OK;
    gpio_config_t gpio_config_struct;

#if CONFIG_DISPLAY_AVOID_TEAR_EFFECT_WITH_SEM
    ESP_LOGI(TAG, "Create semaphores");
    sem_vsync_end = xSemaphoreCreateBinary();
    assert(sem_vsync_end);
    sem_gui_ready = xSemaphoreCreateBinary();
    assert(sem_gui_ready);
#endif

#if DISPLAY_PIN_NUM_BK_LIGHT >= 0
    ESP_LOGI(TAG, "Turn off LCD backlight");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DISPLAY_PIN_NUM_BK_LIGHT
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
#endif

    ESP_LOGI(TAG, "Install RGB LCD panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_rgb_panel_config_t panel_config = {
        .data_width = 16, // RGB565 in parallel mode, thus 16bit in width
        .psram_trans_align = 64,
        .num_fbs = DISPLAY_LCD_NUM_FB,
#if CONFIG_DISPLAY_USE_BOUNCE_BUFFER
        .bounce_buffer_size_px = 10 * DISPLAY_LCD_H_RES,
#endif
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .disp_gpio_num = DISPLAY_PIN_NUM_DISP_EN,
        .pclk_gpio_num = DISPLAY_PIN_NUM_PCLK,
        .vsync_gpio_num = DISPLAY_PIN_NUM_VSYNC,
        .hsync_gpio_num = DISPLAY_PIN_NUM_HSYNC,
        .de_gpio_num = DISPLAY_PIN_NUM_DE,
        .data_gpio_nums = {
            DISPLAY_PIN_NUM_DATA0,
            DISPLAY_PIN_NUM_DATA1,
            DISPLAY_PIN_NUM_DATA2,
            DISPLAY_PIN_NUM_DATA3,
            DISPLAY_PIN_NUM_DATA4,
            DISPLAY_PIN_NUM_DATA5,
            DISPLAY_PIN_NUM_DATA6,
            DISPLAY_PIN_NUM_DATA7,
            DISPLAY_PIN_NUM_DATA8,
            DISPLAY_PIN_NUM_DATA9,
            DISPLAY_PIN_NUM_DATA10,
            DISPLAY_PIN_NUM_DATA11,
            DISPLAY_PIN_NUM_DATA12,
            DISPLAY_PIN_NUM_DATA13,
            DISPLAY_PIN_NUM_DATA14,
            DISPLAY_PIN_NUM_DATA15,
        },
        .timings = {
            .pclk_hz = DISPLAY_LCD_PIXEL_CLOCK_HZ,
            .h_res = DISPLAY_LCD_H_RES,
            .v_res = DISPLAY_LCD_V_RES,
            // The following parameters should refer to LCD spec
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .hsync_pulse_width = 4,
            .vsync_back_porch = 16,
            .vsync_front_porch = 16,
            .vsync_pulse_width = 4,
            .flags.pclk_active_neg = true,
        },
        .flags.fb_in_psram = true, // allocate frame buffer in PSRAM
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));

    ESP_LOGI(TAG, "Register event callbacks");
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = display_on_vsync_event,
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, &disp_drv));

    ESP_LOGI(TAG, "Initialize RGB LCD panel");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

#if DISPLAY_PIN_NUM_BK_LIGHT >= 0
    ESP_LOGI(TAG, "Turn on LCD backlight");
    gpio_set_level(DISPLAY_PIN_NUM_BK_LIGHT, DISPLAY_LCD_BK_LIGHT_ON_LEVEL);
#endif

    esp_lcd_touch_handle_t tp = NULL;
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    
    // set Int pin to output temporarily
    gpio_config_struct.pin_bit_mask = (uint64_t)1 << TOUCH_INT;
    gpio_config_struct.mode = GPIO_MODE_OUTPUT;
    gpio_config_struct.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config_struct.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&gpio_config_struct);

    // reset low
	CH422G_write_output(TOUCH_RESET, 0);
    esp_rom_delay_us(100 * 1000);

    // set Int to low/output
    gpio_set_level(TOUCH_INT, 0);
    esp_rom_delay_us(100 * 1000);

    // release reset
    CH422G_write_output(TOUCH_RESET, 1);
    esp_rom_delay_us(200 * 1000);

    // set interrupt to tristate
    gpio_config_struct.mode = GPIO_MODE_INPUT;
    gpio_config_struct.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&gpio_config_struct);

    // Touch IO handle
    if (esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2CNum, &tp_io_config, &tp_io_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Touch reset 3 failed!");
    }
    
    esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_LCD_V_RES,
            .y_max = DISPLAY_LCD_H_RES,
            .rst_gpio_num = -1,
            .int_gpio_num = -1,
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };

    /* Initialize touch */
    ESP_LOGI(TAG, "Initialize touch controller GT911");

    // try a few times
    for (int loop = 0; loop < 5; loop++)
    {
        ret = ESP_FAIL;

        if (xSemaphoreTake(I2CMutexHandle, (TickType_t)10000) == pdTRUE)
        {
            ret = (esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp));

            xSemaphoreGive(I2CMutexHandle);
        }
        else
        {
            ESP_LOGE(TAG, "Initialize touch loop mutex timeout");
        }
        
        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "Touch controller init OK");
            touch_ok = 1;
            break;
        }
        else
        {
            ESP_LOGI(TAG, "Touch controller init retry %s", esp_err_to_name(ret));

            // reset I2C bus
            i2c_master_reset();
        }
           
        vTaskDelay(pdMS_TO_TICKS(25));    
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init touch screen");
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    //??? lv_fs_if_init();
    
    void *buf1 = NULL;
    void *buf2 = NULL;
#if CONFIG_DISPLAY_DOUBLE_FB
    ESP_LOGI(TAG, "Use frame buffers as LVGL draw buffers");
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &buf1, &buf2));
    // initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, DISPLAY_LCD_H_RES * DISPLAY_LCD_V_RES);
#else
    ESP_LOGI(TAG, "Allocate separate LVGL draw buffers from PSRAM");
    buf1 = heap_caps_malloc(DISPLAY_LCD_H_RES * 100 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(buf1);

    // initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, DISPLAY_LCD_H_RES * 100);
#endif // CONFIG_DISPLAY_DOUBLE_FB

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = DISPLAY_LCD_H_RES;
    disp_drv.ver_res = DISPLAY_LCD_V_RES;
    disp_drv.flush_cb = display_lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
#if CONFIG_DISPLAY_DOUBLE_FB
    disp_drv.full_refresh = true; // the full_refresh mode can maintain the synchronization between the two frame buffers
#endif
    lv_disp_t* __attribute__((unused)) disp = lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    
    if (touch_ok)
    {
        static lv_indev_drv_t indev_drv;    // Input device driver (Touch)
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.disp = disp;
        indev_drv.read_cb = display_lvgl_touch_cb;
        indev_drv.user_data = tp;

        lv_indev_drv_register(&indev_drv);
    }
#endif  //CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B

#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_169
    gpio_config_t gpio_config_struct;

    // switch off the buzzer. 
    ESP_LOGI(TAG, "Buzzer off");
    gpio_config_struct.pin_bit_mask = (uint64_t)1 << WAVESHARE_240_280_BUZZER;
    gpio_config_struct.mode = GPIO_MODE_OUTPUT;
    gpio_config_struct.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config_struct.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&gpio_config_struct);
    gpio_set_level(GPIO_NUM_42, 0);

    // LCD backlight
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << WAVESHARE_240_280_LCD_GPIO_BL};
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    /* LCD initialization */
    ESP_LOGD(TAG, "Initialize SPI bus");
    const spi_bus_config_t buscfg = {
        .sclk_io_num = WAVESHARE_240_280_LCD_GPIO_SCLK,
        .mosi_io_num = WAVESHARE_240_280_LCD_GPIO_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        // note here: this value needs to be: WAVESHARE_240_280_LCD_H_RES * WAVESHARE_240_280_LCD_DRAW_BUFF_HEIGHT * sizeof(uint16_t)
        // however, the ESP framework uses multiples of 4092 for DMA (LLDESC_MAX_NUM_PER_DESC).
        // this theoretical number is 49.9 times the DMA size, which gets rounded down and ends up too small.
        // so instead, manually setting it to a little larger (50 rather than 49.9)
        .max_transfer_sz = 50 * LLDESC_MAX_NUM_PER_DESC,
    };
    spi_bus_initialize(WAVESHARE_240_280_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO);

    ESP_LOGD(TAG, "Install panel IO");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = WAVESHARE_240_280_LCD_GPIO_DC,
        .cs_gpio_num = WAVESHARE_240_280_LCD_GPIO_CS,
        .pclk_hz = WAVESHARE_240_280_LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = WAVESHARE_240_280_LCD_CMD_BITS,
        .lcd_param_bits = WAVESHARE_240_280_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)WAVESHARE_240_280_LCD_SPI_NUM, &io_config, &lcd_io);

    ESP_LOGD(TAG, "Install LCD driver");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = WAVESHARE_240_280_LCD_GPIO_RST,
        .color_space = WAVESHARE_240_280_LCD_COLOR_SPACE,
        .bits_per_pixel = WAVESHARE_240_280_LCD_BITS_PER_PIXEL,
    };
    esp_lcd_new_panel_st7789(lcd_io, &panel_config, &lcd_panel);

    esp_lcd_panel_reset(lcd_panel);
    esp_lcd_panel_init(lcd_panel);
    esp_lcd_panel_mirror(lcd_panel, true, true);
    esp_lcd_panel_disp_on_off(lcd_panel, true);

    // LCD backlight on 
    ESP_ERROR_CHECK(gpio_set_level(WAVESHARE_240_280_LCD_GPIO_BL, WAVESHARE_240_280_LCD_BL_ON_LEVEL));

    esp_lcd_panel_set_gap(lcd_panel, 0, 20);
    esp_lcd_panel_invert_color(lcd_panel, true);

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    void *buf1 = NULL;
    void *buf2 = NULL;
    ESP_LOGI(TAG, "Allocate separate LVGL draw buffers from PSRAM");
    buf1 = heap_caps_malloc(WAVESHARE_240_280_LCD_H_RES * 32 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(buf1);
    buf2 = heap_caps_malloc(WAVESHARE_240_280_LCD_H_RES * 32 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(buf2);
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, WAVESHARE_240_280_LCD_H_RES * 32);

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = WAVESHARE_240_280_LCD_H_RES;
    disp_drv.ver_res = WAVESHARE_240_280_LCD_V_RES;
    disp_drv.flush_cb = display_lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = lcd_panel;

    lv_disp_t* __attribute__((unused)) disp = lv_disp_drv_register(&disp_drv);

#endif //CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_169

#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_M5ATOMS3R
    // LCD backlight
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << ATOM3SR_LCD_GPIO_BL};
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    /* LCD initialization */
    ESP_LOGD(TAG, "Initialize SPI bus");
    const spi_bus_config_t buscfg = {
        .sclk_io_num = ATOM3SR_LCD_GPIO_SCLK,
        .mosi_io_num = ATOM3SR_LCD_GPIO_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        // note here: this value needs to be: WAVESHARE_240_280_LCD_H_RES * WAVESHARE_240_280_LCD_DRAW_BUFF_HEIGHT * sizeof(uint16_t)
        // however, the ESP framework uses multiples of 4092 for DMA (LLDESC_MAX_NUM_PER_DESC).
        // this theoretical number is 49.9 times the DMA size, which gets rounded down and ends up too small.
        // so instead, manually setting it to a little larger (50 rather than 49.9)
        .max_transfer_sz = 50 * LLDESC_MAX_NUM_PER_DESC,
    };
    spi_bus_initialize(ATOM3SR_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO);

    ESP_LOGD(TAG, "Install panel IO");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = ATOM3SR_LCD_GPIO_DC,
        .cs_gpio_num = ATOM3SR_LCD_GPIO_CS,
        .pclk_hz = ATOM3SR_LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = ATOM3SR_LCD_CMD_BITS,
        .lcd_param_bits = ATOM3SR_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)ATOM3SR_LCD_SPI_NUM, &io_config, &lcd_io);

    ESP_LOGD(TAG, "Install LCD driver");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = ATOM3SR_LCD_GPIO_RST,
        .color_space = ATOM3SR_LCD_COLOR_SPACE,
        .bits_per_pixel = ATOM3SR_LCD_BITS_PER_PIXEL,
    };
    esp_lcd_new_panel_gc9107(lcd_io, &panel_config, &lcd_panel);

    esp_lcd_panel_reset(lcd_panel);
    esp_lcd_panel_init(lcd_panel);
    esp_lcd_panel_mirror(lcd_panel, true, true);
    esp_lcd_panel_disp_on_off(lcd_panel, true);

    // LCD backlight on 
    ESP_ERROR_CHECK(gpio_set_level(ATOM3SR_LCD_GPIO_BL, ATOM3SR_LCD_BL_ON_LEVEL));

    esp_lcd_panel_set_gap(lcd_panel, 0, 20);
    esp_lcd_panel_invert_color(lcd_panel, true);

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    void *buf1 = NULL;
    void *buf2 = NULL;
    ESP_LOGI(TAG, "Allocate separate LVGL draw buffers from PSRAM");
    buf1 = heap_caps_malloc(ATOM3SR_LCD_H_RES * 32 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(buf1);
    buf2 = heap_caps_malloc(ATOM3SR_LCD_H_RES * 32 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(buf2);
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, ATOM3SR_LCD_H_RES * 32);

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = ATOM3SR_LCD_H_RES;
    disp_drv.ver_res = ATOM3SR_LCD_V_RES;
    disp_drv.flush_cb = display_lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = lcd_panel;

    lv_disp_t* __attribute__((unused)) disp = lv_disp_drv_register(&disp_drv);
#endif

#if CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_169 || CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_WAVESHARE_43B || CONFIG_TONEX_CONTROLLER_HARDWARE_PLATFORM_M5ATOMS3R
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &display_increase_lvgl_tick,
        .name = "lvgl_tick"
    };

    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, DISPLAY_LVGL_TICK_PERIOD_MS * 1000));

    vTaskDelay(pdMS_TO_TICKS(10));

    // init GUI
    ESP_LOGI(TAG, "Init scene");
    ui_init();

    // create display task
    xTaskCreatePinnedToCore(display_task, "Dsp", DISPLAY_TASK_STACK_SIZE, NULL, DISPLAY_TASK_PRIORITY, NULL, 1);
#endif
}
