#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "protocol_examples_common.h"
#include "tcp_server.h"

#include "usb/usb_host.h"
#include "esp_log.h"
#include "string.h"
#include "nvs_flash.h"
#include "wifi.h"
#include "usbip.h"

#include "lwip/sockets.h"


static usb_host_client_handle_t client_hdl;
static usb_device_handle_t dev_hdl;

void usb_host_client_loop() {
    while (1) {
        uint32_t event_flags_ret;
        esp_err_t retval = usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags_ret);
        if(retval != ESP_OK && retval != ESP_ERR_TIMEOUT) {
            printf("host lib handle events error %s\n", esp_err_to_name(retval));
        };
        retval = usb_host_client_handle_events(client_hdl, pdMS_TO_TICKS(100));
        if(retval != ESP_OK && retval != ESP_ERR_TIMEOUT) {
            printf("host client handle events error %s\n", esp_err_to_name(retval));
        };
    }
}

void usb_write_callback(usb_transfer_t *transfer) {
    ESP_LOGI("", "write transfer, length %d", transfer->actual_num_bytes); 
    for (int i=0;i<transfer->actual_num_bytes;i++) {
        printf("%02x ", transfer->data_buffer[i]);
    }
    printf("\n");
    usb_host_transfer_free(transfer);
}

void usb_host_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg) {
    ESP_LOGI("", "host client callback %d", event_msg->event);
    if(event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        ESP_LOGI("", "new dev %d", event_msg->new_dev.address);
        if(usb_host_device_open(client_hdl, event_msg->new_dev.address, &dev_hdl) == ESP_OK) {
            const usb_device_desc_t *device_desc;
            if(usb_host_get_device_descriptor(dev_hdl, &device_desc) == ESP_OK) {
                ESP_LOGI("", "PID 0x%x, VID 0x%x", device_desc->idProduct, device_desc->idVendor);
                struct usbip_usb_device usbipdev = {
                    .path = "1",
                    .busid = "1",
                    .busnum = 1,
                    .devnum = 1,
                    .speed = 1,
                    .idVendor = device_desc->idVendor,
                    .idProduct = device_desc->idProduct,
                    .bcdDevice=device_desc->bcdUSB,
                    .bDeviceClass = device_desc->bDeviceClass,
                    .bDeviceSubClass = device_desc->bDeviceSubClass,
                    .bDeviceProtocol = device_desc->bDeviceProtocol,
                    .bConfigurationValue = 0,
                    .bNumConfigurations = device_desc->bNumConfigurations,
                    .bNumInterfaces = 1,
                    };
                usbip_add_device(&usbipdev);
                // if (device_desc->idVendor == 0x3293 && device_desc->idProduct == 0x100) {
                //     ESP_LOGI("", "Unhuman motor controller");
                //     // note skipping parsing of configuration descriptor, this device is already known
                //     if (usb_host_interface_claim(client_hdl, dev_hdl, 0, 0) == ESP_OK) {
                //         ESP_LOGI("", "Claimed interface");
                //         interface_claimed = true;
                //     }
                // }
            }
        }
    } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGI("", "device gone %d", event_msg->new_dev.address);
        if(usb_host_device_close(client_hdl, dev_hdl) == ESP_OK) {
            ESP_LOGI("", "device closed");
        }
    }
}


void app_main(void)
{
    //usb

    usb_host_config_t host_config = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
    if(usb_host_install(&host_config) ==ESP_OK) {
        printf("usb_host_install ok\n");
    }

    usb_host_client_config_t client_config = {
        .max_num_event_msg = 3,
        .client_event_callback = usb_host_client_event_cb,
    };
    if(usb_host_client_register(&client_config, &client_hdl) == ESP_OK) {
         printf("usb_host_client_register ok\n");
    }

    xTaskCreate(usb_host_client_loop, "usb_host_client_loop", 4*1024, NULL, 10, NULL);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI("wifi", "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    //ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
   // ESP_ERROR_CHECK(example_connect());

#ifdef CONFIG_EXAMPLE_IPV4
    xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*)AF_INET, 5, NULL);
#endif
#ifdef CONFIG_EXAMPLE_IPV6
    xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*)AF_INET6, 5, NULL);
#endif

    esp_log_level_set("*", ESP_LOG_DEBUG);  
    for (unsigned int i=0;;i++) {

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }


}
