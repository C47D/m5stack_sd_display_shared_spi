/* Shared SPI for the M5Stack
 * 
 * The code halts when sharing the spi bus between the m5 display and the
 * sd card adapter.
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "driver/gpio.h"

#include <sys/unistd.h>
#include <sys/stat.h>
#include <sys/dirent.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

/* NOTE: Choose the board you are working on */

#define BOARD_WROVER_KIT_V41
// #define BOARD_M5STACK

#ifdef BOARD_WROVER_KIT_V41
#define SD_CARD_MOSI	15
#define SD_CARD_MISO	2
#define SD_CARD_CLK	14
#define SD_CARD_CS	13
#endif

#ifdef BOARD_M5STACK
#define SD_CARD_MOSI	23
#define SD_CARD_MISO	19
#define SD_CARD_CLK	18
#define SD_CARD_CS	4
#endif

#define SD_CARD_DMA_CHANNEL	1

#define MOUNT_POINT     "/sdcard"

#define TEST_SD_CARD
// #define TEST_LVGL

#ifdef TEST_SD_CARD
void test_sd_card(void);
#endif

/* Littlevgl specific */
#include "lvgl/lvgl.h"
#include "lvgl_driver.h"
#include "lv_examples/lv_apps/demo/demo.h"

static const char *TAG = "SD-CARD";

sdmmc_card_t *card;

//Creates a semaphore to handle concurrent call to lvgl stuff
//If you wish to call *any* lvgl function from other threads/tasks
//you should lock on the very same semaphore!
SemaphoreHandle_t xGuiSemaphore;

static void IRAM_ATTR lv_tick_task(void *arg);

void guiTask(void *pvParameters);

void app_main() {

#ifdef TEST_LVGL
    /* lvgl task */
    xTaskCreatePinnedToCore((TaskFunction_t) guiTask /* pvTaskCode */,
	"gui" /* pcName */,
	4096 * 2 /* ulStackDepth */,
	NULL /* pvParameters */,
	0 /* uxPriority */,
	NULL /* pvCreatedTask */,
	1 /* xCoreID */
    );
#endif

#ifdef TEST_SD_CARD
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    const char mount_point[] = MOUNT_POINT;

    ESP_LOGI(TAG, "Init SPI Bus");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

#ifdef BOARD_WROVER_KIT_V41
#pragma message "Wrover kit v4.1 board"
    /* Wrover kit v41 have the display and sd spi pins mapped to different spi hosts */
#if TFT_SPI_HOST == HSPI_HOST
#pragma message "SPI for display is HSPI_HOST, for sd card is VSPI_HOST"
    host.slot = VSPI_HOST;
#else
#pragma message "SPI for display is VSPI_HOST, for sd card is HSPI_HOST"
    host.slot = HSPI_HOST;
#endif

#else /* M5STACK have the display and sd spi on the same spi host */
#pragma message "M5Stack board"    
#if TFT_SPI_HOST == HSPI_HOST
#pragma message "SPI for display is HSPI_HOST, for sd card is VSPI_HOST"
    host.slot = VSPI_HOST;
#else
#pragma message "SPI for display is VSPI_HOST, for sd card is HSPI_HOST"
    host.slot = HSPI_HOST;
#endif
#endif

    esp_err_t err;

/* We only init a new spi bus when working on the wrover kit because
 * the spi host is not shared between sd spi and display spi */
#ifdef BOARD_WROVER_KIT_V41
    /* Shared SPI bus configuration
	 * 
	 * NOTE: SD_CARD_MISO must be the same as the DISPLAY_MISO or -1
	 *       SD_CARD_MOSI must be the same as DISPLAY_MOSI
	 *       SD_CARD_SCLK must be the same as DISPLAY_SCLK
	 */
    spi_bus_config_t buscfg = {
            .miso_io_num = SD_CARD_MISO,
            .mosi_io_num = SD_CARD_MOSI,
            .sclk_io_num = SD_CARD_CLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = DISP_BUF_SIZE,
    };

    ret = spi_bus_initialize(host.slot,
        &buscfg, SD_CARD_DMA_CHANNEL);
    assert(ret == ESP_OK);
#endif

    // This init the slot without CD (Card Detect) and WP (Write Protect)
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CARD_CS;
    slot_config.host_id = host.slot;

	/* esp_vfs_fat_sdspi_mount is a convenience function that setup the FatFs and
	 * vsf. It does:
	 * 1. Call esp_vfs_fat_register()
	 * 2. Call ff_diskio_register() 
	 * 3. Call the FatFs function f_mount() and optionally f_fdisk, f_mkfs to mount
	 *    the file system using the same driver that was passed to esp_vfs_fat_register
	 * 4. Call POSIX API for files */
    ret = esp_vfs_fat_sdspi_mount(mount_point,
        &host, &slot_config, &mount_config, &card);

    if (ESP_OK != ret) {
		if (ESP_FAIL == ret) {
			// ESP_LOGE(TAG, "Failed to mount filesystem. If you want the card to be formatted, set format_if_mount_failed = true".);
		} else {
			// ESP_LOGE(TAG, "Failed to initialize the card (%s). Make sure SD card lines have pull-up resistors in place", esp_err_to_name(ret));
		}
    }
    
    ESP_LOGI(TAG, "Screen is working...\n");
    ESP_LOGI(TAG, "Writing to the card in...\n");

    for (size_t i = 10; i > 0; i--) {
	ESP_LOGI(TAG, "..%d\n", i);
	vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Writing to card\n");
    test_sd_card();
    ESP_LOGI(TAG, "SD card routine complete\n");
    ESP_LOGI(TAG, "Screen is now frozen\n");
#endif
}

#ifdef TEST_SD_CARD
/* Mostly from test/test_sdio */
void test_sd_card(void)
{
    sdmmc_card_print_info(stdout, card);

    ESP_LOGI(TAG, "Opening file");

    FILE *f = fopen("/sdcard/hello.txt", "w");
    if (NULL == f){
	ESP_LOGE(TAG, "Failed to open file for writing");
	return;
    }

    fprintf(f, "Hello %s!", card->cid.name);
    fclose(f);
    ESP_LOGI(TAG, "File written");

    // Check if destination file exists before renaming
    struct stat st;
    if (stat("/sdcard/foo.txt", &st) == 0) {
	// Delete it if exists
	unlink("/sdcard/foo.txt");
    }

    // Rename original file
    ESP_LOGI(TAG, "Renaming file");
    if (rename("/sdcard/hello.txt", "/sdcard/foo.txt") != 0) {
	ESP_LOGE(TAG, "Rename failed");
	return;
    }

    // Open renamed file for reading
    ESP_LOGI(TAG, "Reading file");
    f = fopen("/sdcard/foo.txt", "r");
    if (NULL == f) {
	ESP_LOGE(TAG, "Failed to open file for reading");
	return;
    }

    char line[64] = "";
    fgets(line, sizeof line, f);
    fclose(f);

    // Strip new line
    char *pos = strchr(line, '\n');
    if (pos) {
	*pos = '\0';
    }

    ESP_LOGI(TAG, "Read from file: %s", line);

    // All done, unmount partition and disable SDMMC or SPI peripheral
    esp_vfs_fat_sdmmc_unmount();
    ESP_LOGI(TAG, "Card unmounted");
}
#endif

static void IRAM_ATTR lv_tick_task(void *arg) {
    (void) arg;

    lv_tick_inc(portTICK_RATE_MS);
}

void some_random_task(void)
{
    ESP_LOGI(TAG, "lvgl task still ticks but screen freezes\n");
}

/* Task that runs the lvgl code */
void guiTask(void *pvParameters) {
    (void) pvParameters;

    xGuiSemaphore = xSemaphoreCreateMutex();

    lv_init();

    lvgl_driver_init();

    static lv_color_t buf1[DISP_BUF_SIZE];
    static lv_color_t buf2[DISP_BUF_SIZE];
    static lv_disp_buf_t disp_buf;
    lv_disp_buf_init(&disp_buf, buf1, buf2, DISP_BUF_SIZE);

    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = disp_driver_flush;
    disp_drv.buffer = &disp_buf;
    lv_disp_drv_register(&disp_drv);

#if CONFIG_LVGL_TOUCH_CONTROLLER != TOUCH_CONTROLLER_NONE
    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.read_cb = touch_driver_read;
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    lv_indev_drv_register(&indev_drv);
#endif

    /* PERIODIC TIMER *******************************
     * On ESP32 it's better to create a periodic task
     * instead of esp_register_freertos_tick_hook */

    esp_timer_handle_t periodic_timer;
    const esp_timer_create_args_t periodic_timer_args = {
            .callback = &lv_tick_task,
            /* help identify the timer when debugging */
            .name = "periodic_gui"
    };
    /* esp_timer_start_periodic expects the period in us,
     * we want a 10ms tick */
    const uint64_t lv_timer_period = 10 * 1000; 

    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, lv_timer_period));

    demo_create();

    // lv_task_create(some_random_task, 2000, LV_TASK_PRIO_LOWEST, NULL);

    while (1) {
        vTaskDelay(1);
        //Try to lock the semaphore, if success, call lvgl stuff
        if (xSemaphoreTake(xGuiSemaphore, (TickType_t)10) == pdTRUE) {
            lv_task_handler();
            xSemaphoreGive(xGuiSemaphore);
        }
    }

    //A task should NEVER return
    vTaskDelete(NULL);
}
