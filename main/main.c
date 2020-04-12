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

#define SD_CARD_MOSI	23
#define SD_CARD_MISO	19
#define SD_CARD_CLK	18
#define SD_CARD_CS	4

#define DMA_CHANNEL	2
#define MAX_BUFSIZE	(16 * 1024)

void test_sd_card(void);

/* Littlevgl specific */
#include "lvgl/lvgl.h"
#include "lvgl_driver.h"
#include "lv_examples/lv_apps/demo/demo.h"

static const char *TAG = "SD-CARD";

static void IRAM_ATTR lv_tick_task(void *arg);
void guiTask();

void app_main() {
    /* lvgl task */
    xTaskCreatePinnedToCore(guiTask, "gui", 4096*2, NULL, 0, NULL, 1);

    printf("Screen is working...\n");
    printf("Writing to the card in...\n");

    for (size_t i = 10; i > 0; i--) {
	printf("..%d\n", i);
	vTaskDelay(pdMS_TO_TICKS(1000));
    }

    printf("Writing to card\n");
    test_sd_card();
    printf("SD card routine complete\n");
    printf("Screen is now frozen\n");
}

/* Mostly from test/test_sdio */
void test_sd_card(void)
{
#if 0 /* Setting the spi bus, not needed, we do it already on lvgl */
    spi_bus_config_t bus_cfg = {
	.mosi_io_num = SD_CARD_MOSI,
	.miso_io_num = SD_CARD_MISO,
	.sclk_io_num = SD_CARD_CLK,
	.quadwp_io_num = -1,
	.quadhd_io_num = -1,
	.max_transfer_sz = 4000,
    };
    esp_err_t spi_bus_ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CHAN);
    if (ESP_OK != spi_bus_ret) {
	ESP_LOGE(TAG, "Failed to initialize bus.");
	return;
    }
#endif

    sdspi_device_config_t device_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    device_config.host_id = /* Same as the display */;
    device_config.gpio_cs = SD_CARD_CS;
    device_config.gpio_int = SDSPI_SLOT_NO_INT /* Interrupt line */;
    device_config.gpio_cd = SDSPI_SLOT_NO_CD /* Card detect line */;
    device_config.gpio_wp = SDSPI_SLOT_NO_WP /* Write protect line  */;

    esp_err_t int_err = gpio_install_isr_service(0);
    assert(ESP_OK == int_err);

#if 0
    /* Extra configuration for SPI host
     * NOTE: SPI Host already configured, so we don't use this */
    sdspi_slot_config_t slot_config = SDSPI_SLOT_CONFIG_DEFAULT();
    slot_config.gpio_miso = SD_CARD_MISO;
    slot_config.gpio_mosi = SD_CARD_MOSI;
    slot_config.gpio_sck = SD_CARD_CLK;
    slot_config.gpio_cs = SD_CARD_CS;
    slot_config.dma_channel = DMA_CHANNEL;
#endif

    /* Init the SD SPI device and attach to its bus
     * 
     * NOTE: Init the spi bus by spi_bus_initialize before calling this
     * function. */

    sdspi_dev_handle_t sdspi_handle;
    /* XXX: Check we really need this */
    sdspi_host_init();

    /* TODO: Change slot config by device_config */
    esp_err_t sdspi_host_err = sdspi_host_init_device(&device_config,
	&sdspi_handle
    );
    assert(sdspi_host_err == ESP_OK);

    /* Store the SD SPI device state and configurations of
     * upper layer (SD/SDIO/MMC driver)
     *
     * Modify the slot parameter of the structure to the SD SPI
     * device handle just returned from sdspi_host_init_device.
     * Call sdmmc_card_init with the sdmmc_host_t to probe and init
     * the SD Card. */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = sdspi_handle;

    /* Mount configuration */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
	.format_if_mount_failed = false,
	.max_files = 5,
	.allocation_unit_size = MAX_BUFSIZE,
    };

    /* Use configuration and mount the file system */
    sdmmc_card_t *card = NULL;
    
    /* Wait for at least 5 seconds */
    int retry_times = 5;
    do {
	if (ESP_OK == sdmmc_card_init(&config, card)) {
	    break;
	}

	vTaskDelay(1000 / portTICK_PERIOD_MS);
    } while (--retry_times);
    
    
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard",
	&host,		// Host configuration
	&slot_config,	// Slot configuration
	&mount_config,	// Mount configuration
	&card		// out card
    );

    if (ESP_OK != ret) {
	if (ESP_FAIL == ret) {
	    ESP_LOGE(TAG, "Failed to mount filesystem. "
		"If you want the card to be formatted, set format_if_mount_failed = true".);
	} else {
	    ESP_LOGE(TAG, "Failed to initialize the card (%s). "
		"Make sure SD card lines have pull-up resistors in place",
		esp_err_to_name(ret));
	}
    }

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

static void IRAM_ATTR lv_tick_task(void *arg) {
    (void) arg;

    lv_tick_inc(portTICK_RATE_MS);
}

void some_random_task(void)
{
    printf("lvgl task still ticks but screen freezes\n");
}

//Creates a semaphore to handle concurrent call to lvgl stuff
//If you wish to call *any* lvgl function from other threads/tasks
//you should lock on the very same semaphore!
SemaphoreHandle_t xGuiSemaphore;

void guiTask() {
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

    const esp_timer_create_args_t periodic_timer_args = {
            .callback = &lv_tick_task,
            /* name is optional, but may help identify the timer when debugging */
            .name = "periodic_gui"
    };

    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    //On ESP32 it's better to create a periodic task instead of esp_register_freertos_tick_hook
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 10*1000)); //10ms (expressed as microseconds)

    demo_create();

    lv_task_create(some_random_task, 2000, LV_TASK_PRIO_LOWEST, NULL);

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
