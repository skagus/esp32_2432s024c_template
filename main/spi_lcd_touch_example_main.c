/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch_cst816s.h"

static const char* TAG = "MAIN";
// Using SPI2 in the example
#define LCD_HOST		SPI2_HOST
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define LCD_PIXEL_CLOCK_HZ			(20 * 1000 * 1000)
#define LCD_BK_LIGHT_ON_LEVEL		1
#define LCD_BK_LIGHT_OFF_LEVEL		!LCD_BK_LIGHT_ON_LEVEL

#define SPI_PIN_NUM_MISO			12
#define SPI_PIN_NUM_MOSI			13
#define SPI_PIN_NUM_SCLK			14
#define PIN_NUM_LCD_CS				15
#define PIN_NUM_LCD_DC				2 //
#define PIN_NUM_LCD_RST				-1 //3
#define PIN_NUM_BK_LIGHT			27

#define TOUCH_I2C_NUM				(0)
#define TOUCH_I2C_SDA				(33)
#define TOUCH_I2C_SCL				(32)
#define TOUCH_PIN_RST				(25)
#define TOUCH_PIN_INT				(21)


// The pixel number in horizontal and vertical
#define LCD_H_RES					240
#define LCD_V_RES					320
// Bit number used to represent command and parameter
#define LCD_CMD_BITS				8
#define LCD_PARAM_BITS				8

#define LVGL_TICK_PERIOD_MS			100			// Refresh period.
#define LVGL_TASK_MAX_DELAY_MS		500	// 
#define LVGL_TASK_MIN_DELAY_MS		1
#define LVGL_TASK_STACK_SIZE		(4 * 1024)
#define LVGL_TASK_PRIORITY			2

// to sync other task that related to GUI.
static SemaphoreHandle_t gstLvglMutex = NULL;

#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED
esp_lcd_touch_handle_t gstTouchHandle = NULL;
#endif

extern void example_lvgl_demo_ui(lv_disp_t* disp);

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,esp_lcd_panel_io_event_data_t* edata,void* user_ctx)
{
	lv_disp_drv_t* disp_driver = (lv_disp_drv_t*)user_ctx;
	lv_disp_flush_ready(disp_driver);
	return false;
}

static void lvgl_flush_cb(lv_disp_drv_t* drv,const lv_area_t* area,lv_color_t* color_map)
{
	esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
	int offsetx1 = area->x1;
	int offsetx2 = area->x2;
	int offsety1 = area->y1;
	int offsety2 = area->y2;
	// copy a buffer's content to a specific area of the display
	esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

void rotate_touch()
{
#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED
	// Rotate LCD touch
	esp_lcd_touch_set_mirror_y(gstTouchHandle,false);
	esp_lcd_touch_set_mirror_x(gstTouchHandle,false);
#endif
}

/* Rotate display and touch, when rotated screen in LVGL. Called when driver parameters are updated. */
static void lvgl_port_update_callback(lv_disp_drv_t* drv)
{
	esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;

	switch(drv->rotated)
	{
		case LV_DISP_ROT_NONE:
		{
			// Rotate LCD display
			esp_lcd_panel_swap_xy(panel_handle,false);
			esp_lcd_panel_mirror(panel_handle,true,false);
			rotate_touch();
			break;
		}
		case LV_DISP_ROT_90:
		{
			// Rotate LCD display
			esp_lcd_panel_swap_xy(panel_handle,true);
			esp_lcd_panel_mirror(panel_handle,true,true);
			rotate_touch();
			break;
		}
		case LV_DISP_ROT_180:
		{
			// Rotate LCD display
			esp_lcd_panel_swap_xy(panel_handle,false);
			esp_lcd_panel_mirror(panel_handle,false,true);
			rotate_touch();
			break;
		}
		case LV_DISP_ROT_270:
		{
			// Rotate LCD display
			esp_lcd_panel_swap_xy(panel_handle,true);
			esp_lcd_panel_mirror(panel_handle,false,false);
			rotate_touch();
			break;
		}
	}
}

#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED
static void lvgl_touch_cb(lv_indev_drv_t* drv,lv_indev_data_t* data)
{
	uint16_t touchpad_x[1] = {0};
	uint16_t touchpad_y[1] = {0};
	uint8_t touchpad_cnt = 0;

	/* Read touch controller data */
	esp_lcd_touch_read_data(drv->user_data);

	/* Get coordinates */
	bool touchpad_pressed = esp_lcd_touch_get_coordinates(drv->user_data,touchpad_x,touchpad_y,NULL,&touchpad_cnt,1);

	if(touchpad_pressed && touchpad_cnt > 0)
	{
		data->point.x = touchpad_x[0];
		data->point.y = touchpad_y[0];
		data->state = LV_INDEV_STATE_PRESSED;
		printf("Pressed: %d, %x\n",touchpad_x[0],touchpad_y[0]);
	}
	else
	{
		data->state = LV_INDEV_STATE_RELEASED;
	}
}
#endif

static void increase_lvgl_tick(void* arg)
{
	/* Tell LVGL how many milliseconds has elapsed */
	lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

bool lvgl_lock(int timeout_ms)
{
	// Convert timeout in milliseconds to FreeRTOS ticks
	// If `timeout_ms` is set to -1, the program will block until the condition is met
	const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
	return xSemaphoreTakeRecursive(gstLvglMutex,timeout_ticks) == pdTRUE;
}

void lvgl_unlock(void)
{
	xSemaphoreGiveRecursive(gstLvglMutex);
}

static void lvgl_port_task(void* arg)
{
	ESP_LOGI(TAG,"Starting LVGL task");
	uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
	while(1)
	{
		// Lock the mutex due to the LVGL APIs are not thread-safe
		if(lvgl_lock(-1))
		{
			task_delay_ms = lv_timer_handler();
			// Release the mutex
			lvgl_unlock();
		}

		if(task_delay_ms > LVGL_TASK_MAX_DELAY_MS)
		{
			task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
		}
		else if(task_delay_ms < LVGL_TASK_MIN_DELAY_MS)
		{
			task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
		}
		vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
	}
}

void tp_cb(esp_lcd_touch_handle_t hTP)
{
	printf("TP CB\n");
}

static esp_lcd_panel_handle_t init_lcd(lv_disp_drv_t* pst_disp_drv)
{
	ESP_LOGI(TAG,"Turn off LCD backlight");
	gpio_config_t bk_gpio_config =
	{
		.mode = GPIO_MODE_OUTPUT,
		.pin_bit_mask = 1ULL << PIN_NUM_BK_LIGHT
	};
	ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

	ESP_LOGI(TAG,"Initialize SPI bus");
	spi_bus_config_t buscfg =
	{
		.sclk_io_num = SPI_PIN_NUM_SCLK,
		.mosi_io_num = SPI_PIN_NUM_MOSI,
		.miso_io_num = SPI_PIN_NUM_MISO,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
	};
	ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST,&buscfg,SPI_DMA_CH_AUTO));

	ESP_LOGI(TAG,"Install panel IO");
	esp_lcd_panel_io_handle_t io_handle = NULL;
	esp_lcd_panel_io_spi_config_t io_config =
	{
		.dc_gpio_num = PIN_NUM_LCD_DC,
		.cs_gpio_num = PIN_NUM_LCD_CS,
		.pclk_hz = LCD_PIXEL_CLOCK_HZ,
		.lcd_cmd_bits = LCD_CMD_BITS,
		.lcd_param_bits = LCD_PARAM_BITS,
		.spi_mode = 0,
		.trans_queue_depth = 10,
		.on_color_trans_done = notify_lvgl_flush_ready,
		.user_ctx = pst_disp_drv,
	};
	// Attach the LCD to the SPI bus
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

	esp_lcd_panel_handle_t panel_handle = NULL;
	esp_lcd_panel_dev_config_t panel_config =
	{
		.reset_gpio_num = PIN_NUM_LCD_RST,
		.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
		.bits_per_pixel = 16,
	};
	ESP_LOGI(TAG,"Install ILI9341 panel driver");
	ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle,&panel_config,&panel_handle));

	ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
	ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
	ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));

	ESP_LOGI(TAG,"Turn on LCD backlight");
	// user can flush pre-defined pattern to the screen before we turn on the screen or backlight
	ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
	gpio_set_level(PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_ON_LEVEL);

	return panel_handle;
}

esp_lcd_touch_handle_t init_touch()
{
	const i2c_config_t i2c_conf =
	{
		.mode = I2C_MODE_MASTER,
		.sda_io_num = TOUCH_I2C_SDA,
		.scl_io_num = TOUCH_I2C_SCL,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = 400000,
	};
	ESP_LOGI(TAG,"Initializing I2C for display touch");
	/* Initialize I2C */
	ESP_ERROR_CHECK(i2c_param_config(TOUCH_I2C_NUM, &i2c_conf));
	ESP_ERROR_CHECK(i2c_driver_install(TOUCH_I2C_NUM, i2c_conf.mode, 0, 0, 0));

	//////////////////
	esp_lcd_panel_io_handle_t tp_io_handle = NULL;
	esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
	ESP_LOGI(TAG,"esp_lcd_new_panel_io_i2c");
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)TOUCH_I2C_NUM, &tp_io_config, &tp_io_handle));

	esp_lcd_touch_config_t tp_cfg =
	{
		.x_max = LCD_H_RES,
		.y_max = LCD_V_RES,
		.rst_gpio_num = -1, // CONFIG_LCD_TOUCH_RST,
		.int_gpio_num = TOUCH_PIN_INT,
		.levels =
		{
			.reset = 0,
			.interrupt = 0,
		},
		.flags =
		{
			.swap_xy = 0,
			.mirror_x = 0,
			.mirror_y = 0,
		},
		.interrupt_callback = tp_cb,
	};
	esp_lcd_touch_handle_t hTP;
	ESP_LOGI(TAG,"Initialize touch controller CST816s");
	esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &hTP);
	return hTP;
}


static lv_disp_t* init_lvgl(esp_lcd_panel_handle_t panel_handle,lv_disp_drv_t* pst_disp_drv)
{
	static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)

	ESP_LOGI(TAG,"Initialize LVGL library");
	lv_init();
	// alloc draw buffers used by LVGL
	// it's recommended to choose the size of the draw buffer(s) to be at least 1/10 screen sized
	lv_color_t* buf1 = heap_caps_malloc(LCD_H_RES * 20 * sizeof(lv_color_t),MALLOC_CAP_DMA);
	assert(buf1);
	lv_color_t* buf2 = heap_caps_malloc(LCD_H_RES * 20 * sizeof(lv_color_t),MALLOC_CAP_DMA);
	assert(buf2);
	// initialize LVGL draw buffers
	lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LCD_H_RES * 20);

	ESP_LOGI(TAG,"Register display driver to LVGL");
	lv_disp_drv_init(pst_disp_drv);
	pst_disp_drv->hor_res = LCD_H_RES;
	pst_disp_drv->ver_res = LCD_V_RES;
	pst_disp_drv->flush_cb = lvgl_flush_cb;
	pst_disp_drv->drv_update_cb = lvgl_port_update_callback;
	pst_disp_drv->draw_buf = &disp_buf;
	pst_disp_drv->user_data = panel_handle;
	lv_disp_t* disp = lv_disp_drv_register(pst_disp_drv);

	ESP_LOGI(TAG,"Install LVGL tick timer");
	// Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
	const esp_timer_create_args_t lvgl_tick_timer_args = 
	{
		.callback = &increase_lvgl_tick,
		.name = "lvgl_tick"
	};
	esp_timer_handle_t lvgl_tick_timer = NULL;
	ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args,&lvgl_tick_timer));
	ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer,LVGL_TICK_PERIOD_MS * 1000));

#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED
	static lv_indev_drv_t indev_drv;    // Input device driver (Touch)
	lv_indev_drv_init(&indev_drv);
	indev_drv.type = LV_INDEV_TYPE_POINTER;
	indev_drv.disp = disp;
	indev_drv.read_cb = lvgl_touch_cb;
	indev_drv.user_data = gstTouchHandle;
	lv_indev_drv_register(&indev_drv);
#endif

	gstLvglMutex = xSemaphoreCreateRecursiveMutex();
	assert(gstLvglMutex);

	ESP_LOGI(TAG,"Create LVGL task");
	xTaskCreate(lvgl_port_task,"LVGL",LVGL_TASK_STACK_SIZE,NULL,LVGL_TASK_PRIORITY,NULL);
	return disp;
}

void app_main(void)
{
	static lv_disp_drv_t disp_drv;      // contains callback functions
	esp_lcd_panel_handle_t panel_handle = init_lcd(&disp_drv);

#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED
	gstTouchHandle = init_touch();
#endif // CONFIG_EXAMPLE_LCD_TOUCH_ENABLED

	lv_disp_t* disp = init_lvgl(panel_handle, &disp_drv);

	ESP_LOGI(TAG,"Display LVGL Meter Widget");
	// Lock the mutex due to the LVGL APIs are not thread-safe
	if(lvgl_lock(-1))
	{
		example_lvgl_demo_ui(disp);
		// Release the mutex
		lvgl_unlock();
	}
}
