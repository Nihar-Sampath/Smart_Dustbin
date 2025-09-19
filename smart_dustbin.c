

/*
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/touch_pad.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_netif_ip_addr.h"  // Needed for static IP

#define SERVO_GPIO      13
#define MOISTURE_ADC    ADC1_CHANNEL_6  // GPIO34
#define TOUCH_GPIO      TOUCH_PAD_NUM0  // GPIO4

#define TRIG_RIGHT      32
#define ECHO_RIGHT      35
#define TRIG_LEFT       23
#define ECHO_LEFT       22

#define WIFI_SSID "ACT107699447924"
#define WIFI_PASS "123456789101112"

#define WET_THRESHOLD       2200
#define LEFT_FULL_THRESH_CM 6
#define RIGHT_FULL_THRESH_CM 7
#define MAX_DISTANCE_CM     20

#define TOUCH_THRESHOLD         190
#define TOUCH_RELEASE_THRESHOLD 320

static const char *TAG = "SMART_DUSTBIN";
char latest_status[32] = "ok";

typedef enum {
    SERVO_LEFT,
    SERVO_NEUTRAL,
    SERVO_RIGHT
} servo_state_t;

static servo_state_t current_state = SERVO_LEFT;

void servo_write_angle(int angle) {
    uint32_t duty = (angle * 500) / 180 + 250;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

int read_distance_cm(gpio_num_t trig, gpio_num_t echo) {
    gpio_set_level(trig, 0);
    ets_delay_us(2);
    gpio_set_level(trig, 1);
    ets_delay_us(10);
    gpio_set_level(trig, 0);

    int64_t start = esp_timer_get_time();
    while (!gpio_get_level(echo)) {
        if ((esp_timer_get_time() - start) > 10000) return -1;
    }

    int64_t echo_start = esp_timer_get_time();
    while (gpio_get_level(echo)) {
        if ((esp_timer_get_time() - echo_start) > 30000) return -1;
    }

    int64_t duration = esp_timer_get_time() - echo_start;
    return duration * 0.034 / 2;
}

esp_err_t status_get_handler(httpd_req_t *req) {
    httpd_resp_send(req, latest_status, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

httpd_uri_t status_uri = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = status_get_handler,
    .user_ctx = NULL
};

httpd_handle_t start_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &status_uri);
    }
    return server;
}

void wifi_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Access via: http://" IPSTR "/status", IP2STR(&event->ip_info.ip));
        httpd_handle_t handle = start_webserver();
        if (handle == NULL) {
            ESP_LOGE(TAG, "Failed to start HTTP server!");
        }
    }
}

// 📌 ONLY THIS FUNCTION IS MODIFIED TO USE STATIC IP
void wifi_init_sta() {
    esp_netif_init();
    esp_event_loop_create_default();

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    // Stop DHCP client before setting static IP
    esp_netif_dhcpc_stop(sta_netif);

    // Set static IP
    esp_netif_ip_info_t ip_info;
    ip4addr_aton("192.168.1.100", &ip_info.ip);
    ip4addr_aton("192.168.1.1", &ip_info.gw);
    ip4addr_aton("255.255.255.0", &ip_info.netmask);
    esp_netif_set_ip_info(sta_netif, &ip_info);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();
}

void setup_gpio() {
    gpio_set_direction(TRIG_RIGHT, GPIO_MODE_OUTPUT);
    gpio_set_direction(ECHO_RIGHT, GPIO_MODE_INPUT);
    gpio_set_direction(TRIG_LEFT, GPIO_MODE_OUTPUT);
    gpio_set_direction(ECHO_LEFT, GPIO_MODE_INPUT);

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .channel = LEDC_CHANNEL_0,
        .duty = 0,
        .gpio_num = SERVO_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .hpoint = 0,
        .timer_sel = LEDC_TIMER_0
    };
    ledc_channel_config(&ledc_channel);
}

void dustbin_task(void *pvParam) {
    bool touch_active = false;
    uint16_t touch_value = 0;

    while (true) {
        touch_pad_read(TOUCH_GPIO, &touch_value);

        if (touch_value < TOUCH_THRESHOLD && !touch_active) {
            ESP_LOGI(TAG, "Touch Detected");

            // Collect moisture for 2 seconds, pick lowest value
            int min_moisture = 4095;
            uint32_t start = esp_log_timestamp();
            while ((esp_log_timestamp() - start) < 2000) {
                int reading = adc1_get_raw(MOISTURE_ADC);
                if (reading < min_moisture) min_moisture = reading;
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            ESP_LOGI(TAG, "Moisture reading = %d", min_moisture);

            int angle = (min_moisture < WET_THRESHOLD) ? 10 : 180;
            ESP_LOGI(TAG, "Classified as %s waste. Rotating servo to %d°", 
                     (angle == 10) ? "WET" : "DRY", angle);

            servo_write_angle(angle);
            vTaskDelay(pdMS_TO_TICKS(1500));
            servo_write_angle(110); // Reset

            // Read ultrasonic sensors
            int left_cm = read_distance_cm(TRIG_LEFT, ECHO_LEFT);
            int right_cm = read_distance_cm(TRIG_RIGHT, ECHO_RIGHT);
            ESP_LOGI(TAG, "Bin Distances - Right: %d cm, Left: %d cm", left_cm, right_cm);

            if (left_cm < LEFT_FULL_THRESH_CM && right_cm < RIGHT_FULL_THRESH_CM) {
                strcpy(latest_status, "both_full");
                ESP_LOGI(TAG, "Status Sent: both_full");
            } else if (left_cm < LEFT_FULL_THRESH_CM) {
                strcpy(latest_status, "wet_full");
                ESP_LOGI(TAG, "Status Sent: wet_full");
            } else if (right_cm < RIGHT_FULL_THRESH_CM) {
                strcpy(latest_status, "dry_full");
                ESP_LOGI(TAG, "Status Sent: dry_full");
            } else if ((left_cm > 0 && left_cm < MAX_DISTANCE_CM) ||
                       (right_cm > 0 && right_cm < MAX_DISTANCE_CM)) {
                strcpy(latest_status, "not_full");
                ESP_LOGI(TAG, "Status Sent: not_full");
            } else {
                strcpy(latest_status, "sensor_error");
                ESP_LOGW(TAG, "Status Sent: sensor_error");
            }

            touch_active = true;
        } else if (touch_value > TOUCH_RELEASE_THRESHOLD && touch_active) {
            touch_active = false;
        }

        vTaskDelay(pdMS_TO_TICKS(200)); // debounce
    }
}

void app_main() {
    nvs_flash_init();
    setup_gpio();
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(MOISTURE_ADC, ADC_ATTEN_DB_12);
    touch_pad_init();
    touch_pad_config(TOUCH_GPIO, 0);
    ESP_LOGI(TAG, "Touch pad initialized (GPIO4)");
    wifi_init_sta();
    xTaskCreate(dustbin_task, "dustbin_task", 4096, NULL, 5, NULL);
}

*/




#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/touch_pad.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_timer.h"

#define SERVO_GPIO      13
#define MOISTURE_ADC    ADC1_CHANNEL_6  // GPIO34
#define TOUCH_GPIO      TOUCH_PAD_NUM0  // GPIO4

#define TRIG_RIGHT      32
#define ECHO_RIGHT      35
#define TRIG_LEFT       23
#define ECHO_LEFT       22

#define WIFI_SSID "ACT107699447924"
#define WIFI_PASS "123456789101112"

#define WET_THRESHOLD       2200
#define LEFT_FULL_THRESH_CM 6
#define RIGHT_FULL_THRESH_CM 7
#define MAX_DISTANCE_CM     20

#define TOUCH_THRESHOLD         190
#define TOUCH_RELEASE_THRESHOLD 320

static const char *TAG = "SMART_DUSTBIN";
char latest_status[32] = "ok";

typedef enum {
    SERVO_LEFT,
    SERVO_NEUTRAL,
    SERVO_RIGHT
} servo_state_t;

static servo_state_t current_state = SERVO_LEFT;

void servo_write_angle(int angle) {
    uint32_t duty = (angle * 500) / 180 + 250;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

int read_distance_cm(gpio_num_t trig, gpio_num_t echo) {
    gpio_set_level(trig, 0);
    ets_delay_us(2);
    gpio_set_level(trig, 1);
    ets_delay_us(10);
    gpio_set_level(trig, 0);

    int64_t start = esp_timer_get_time();
    while (!gpio_get_level(echo)) {
        if ((esp_timer_get_time() - start) > 10000) return -1;
    }

    int64_t echo_start = esp_timer_get_time();
    while (gpio_get_level(echo)) {
        if ((esp_timer_get_time() - echo_start) > 30000) return -1;
    }

    int64_t duration = esp_timer_get_time() - echo_start;
    return duration * 0.034 / 2;
}

esp_err_t status_get_handler(httpd_req_t *req) {
    httpd_resp_send(req, latest_status, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

httpd_uri_t status_uri = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = status_get_handler,
    .user_ctx = NULL
};

httpd_handle_t start_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &status_uri);
    }
    return server;
}

void wifi_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Access via: http://" IPSTR "/status", IP2STR(&event->ip_info.ip));
        start_webserver();
    }
}

void wifi_init_sta() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();
}

void setup_gpio() {
    gpio_set_direction(TRIG_RIGHT, GPIO_MODE_OUTPUT);
    gpio_set_direction(ECHO_RIGHT, GPIO_MODE_INPUT);
    gpio_set_direction(TRIG_LEFT, GPIO_MODE_OUTPUT);
    gpio_set_direction(ECHO_LEFT, GPIO_MODE_INPUT);

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .channel = LEDC_CHANNEL_0,
        .duty = 0,
        .gpio_num = SERVO_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .hpoint = 0,
        .timer_sel = LEDC_TIMER_0
    };
    ledc_channel_config(&ledc_channel);
}

void dustbin_task(void *pvParam) {
    bool touch_active = false;
    uint16_t touch_value = 0;

    while (true) {
        touch_pad_read(TOUCH_GPIO, &touch_value);

        if (touch_value < TOUCH_THRESHOLD && !touch_active) {
            ESP_LOGI(TAG, "Touch Detected");

            // Collect moisture for 2 seconds, pick lowest value
            int min_moisture = 4095;
            uint32_t start = esp_log_timestamp();
            while ((esp_log_timestamp() - start) < 2000) {
                int reading = adc1_get_raw(MOISTURE_ADC);
                if (reading < min_moisture) min_moisture = reading;
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            ESP_LOGI(TAG, "Moisture reading = %d", min_moisture);

            // Rotate servo to correct bin
            // Change this line (inside dustbin_task)
int angle = (min_moisture < WET_THRESHOLD) ? 10 : 180;
ESP_LOGI(TAG, "Classified as %s waste. Rotating servo to %d°", 
         (angle == 10) ? "WET" : "DRY", angle);

            servo_write_angle(angle);
            vTaskDelay(pdMS_TO_TICKS(1500));
            servo_write_angle(110); // Reset

            // Read ultrasonic sensors
            int left_cm = read_distance_cm(TRIG_LEFT, ECHO_LEFT);
            int right_cm = read_distance_cm(TRIG_RIGHT, ECHO_RIGHT);
            ESP_LOGI(TAG, "Bin Distances - Right: %d cm, Left: %d cm", left_cm, right_cm);

            // Determine bin status and log
            // Change this block (bin status detection)

if (left_cm < LEFT_FULL_THRESH_CM && right_cm < RIGHT_FULL_THRESH_CM) {
    strcpy(latest_status, "both_full");
    ESP_LOGI(TAG, "Status Sent: both_full");
} else if (left_cm < LEFT_FULL_THRESH_CM) {
    strcpy(latest_status, "wet_full");  // LEFT is now WET
    ESP_LOGI(TAG, "Status Sent: wet_full");
} else if (right_cm < RIGHT_FULL_THRESH_CM) {
    strcpy(latest_status, "dry_full");  // RIGHT is now DRY
    ESP_LOGI(TAG, "Status Sent: dry_full");
} else if ((left_cm > 0 && left_cm < MAX_DISTANCE_CM) ||
           (right_cm > 0 && right_cm < MAX_DISTANCE_CM)) {
    strcpy(latest_status, "not_full");
    ESP_LOGI(TAG, "Status Sent: not_full");
} else {
    strcpy(latest_status, "sensor_error");
    ESP_LOGW(TAG, "Status Sent: sensor_error");
}


            touch_active = true;
        } else if (touch_value > TOUCH_RELEASE_THRESHOLD && touch_active) {
            touch_active = false;
        }

        vTaskDelay(pdMS_TO_TICKS(200)); // debounce
    }
}

void app_main() {
    nvs_flash_init();
    setup_gpio();
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(MOISTURE_ADC, ADC_ATTEN_DB_12);
    touch_pad_init();
    touch_pad_config(TOUCH_GPIO, 0);
    ESP_LOGI(TAG, "Touch pad initialized (GPIO4)");
    wifi_init_sta();
    xTaskCreate(dustbin_task, "dustbin_task", 4096, NULL, 5, NULL);
}








/*
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/touch_pad.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_timer.h"


#define SERVO_GPIO      13
#define MOISTURE_ADC    ADC1_CHANNEL_6  // GPIO34
#define TOUCH_GPIO      TOUCH_PAD_NUM0  // GPIO4

#define TRIG_RIGHT      32
#define ECHO_RIGHT      35
#define TRIG_LEFT       23
#define ECHO_LEFT       22


#define WIFI_SSID "ACT107699447924"
#define WIFI_PASS "123456789101112"


#define WET_THRESHOLD       2100
#define LEFT_FULL_THRESH_CM 6
#define RIGHT_FULL_THRESH_CM 7
#define MAX_DISTANCE_CM     20

#define TOUCH_THRESHOLD         190
#define TOUCH_RELEASE_THRESHOLD 320

static const char *TAG = "SMART_DUSTBIN";


static char latest_status[32] = "not_full";


typedef enum {
    SERVO_LEFT,
    SERVO_NEUTRAL,
    SERVO_RIGHT
} servo_state_t;

static servo_state_t current_state = SERVO_LEFT;

static void servo_write_angle(int angle) {
    // 13-bit duty; ~5%..10% => 250..750 @ 50Hz
    uint32_t duty = (angle * 500) / 180 + 250;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}


static int read_distance_cm(gpio_num_t trig, gpio_num_t echo) {
    gpio_set_level(trig, 0);
    ets_delay_us(2);
    gpio_set_level(trig, 1);
    ets_delay_us(10);
    gpio_set_level(trig, 0);

    int64_t start = esp_timer_get_time();
    while (!gpio_get_level(echo)) {
        if ((esp_timer_get_time() - start) > 10000) return -1; // timeout waiting for rising edge
    }

    int64_t echo_start = esp_timer_get_time();
    while (gpio_get_level(echo)) {
        if ((esp_timer_get_time() - echo_start) > 30000) return -1; // timeout pulse too long
    }

    int64_t duration = esp_timer_get_time() - echo_start; // us
    return (int)(duration * 0.034f / 2.0f); // cm
}


static esp_err_t status_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, latest_status, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static httpd_uri_t status_uri = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = status_get_handler,
    .user_ctx = NULL
};

static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &status_uri);
    }
    return server;
}


static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Access via: http://" IPSTR "/status", IP2STR(&event->ip_info.ip));
        start_webserver();
    }
}

static void wifi_init_sta(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();
}


static void setup_gpio(void) {
    gpio_set_direction(TRIG_RIGHT, GPIO_MODE_OUTPUT);
    gpio_set_direction(ECHO_RIGHT, GPIO_MODE_INPUT);
    gpio_set_direction(TRIG_LEFT, GPIO_MODE_OUTPUT);
    gpio_set_direction(ECHO_LEFT, GPIO_MODE_INPUT);

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .channel = LEDC_CHANNEL_0,
        .duty = 0,
        .gpio_num = SERVO_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .hpoint = 0,
        .timer_sel = LEDC_TIMER_0
    };
    ledc_channel_config(&ledc_channel);
}


static void dustbin_task(void *pvParam) {
    bool touch_active = false;
    uint16_t touch_value = 0;

    while (true) {
        touch_pad_read(TOUCH_GPIO, &touch_value);

        if (touch_value < TOUCH_THRESHOLD && !touch_active) {
            ESP_LOGI(TAG, "Touch Detected");

            // 1) Moisture check
            int min_moisture = 4095;
            uint32_t t0 = esp_log_timestamp();
            while ((esp_log_timestamp() - t0) < 2000) {
                int reading = adc1_get_raw(MOISTURE_ADC);
                if (reading < min_moisture) min_moisture = reading;
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            ESP_LOGI(TAG, "Moisture = %d", min_moisture);

            // 2) Servo routing
            int angle = (min_moisture > WET_THRESHOLD) ? 170 : 10;
            servo_write_angle(angle);
            vTaskDelay(pdMS_TO_TICKS(1500));
            servo_write_angle(90);

            // 3) Distances (with 100ms gap)
            int left_cm  = read_distance_cm(TRIG_LEFT, ECHO_LEFT);
            vTaskDelay(pdMS_TO_TICKS(100));  // <-- increased delay
            int right_cm = read_distance_cm(TRIG_RIGHT, ECHO_RIGHT);

            ESP_LOGI(TAG, "Raw distances: Left=%d cm, Right=%d cm", left_cm, right_cm);

            // 4) Status decision (with -1 treated as full)
            bool left_full  = (left_cm != -1 && left_cm < LEFT_FULL_THRESH_CM) || (left_cm == -1);
            bool right_full = (right_cm != -1 && right_cm < RIGHT_FULL_THRESH_CM) || (right_cm == -1);

            if (left_full && right_full) {
                strcpy(latest_status, "both_full");
            } else if (left_full) {
                strcpy(latest_status, "dry_full");
            } else if (right_full) {
                strcpy(latest_status, "wet_full");
            } else if ((left_cm > 0 && left_cm < MAX_DISTANCE_CM) ||
                       (right_cm > 0 && right_cm < MAX_DISTANCE_CM)) {
                strcpy(latest_status, "not_full");
            } else {
                strcpy(latest_status, "sensor_error");
            }

            ESP_LOGI(TAG, "Sent status: %s", latest_status);  // <-- log what is sent

            touch_active = true;
        } else if (touch_value > TOUCH_RELEASE_THRESHOLD && touch_active) {
            touch_active = false;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}


void app_main(void) {
    nvs_flash_init();
    setup_gpio();

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(MOISTURE_ADC, ADC_ATTEN_DB_12);

    touch_pad_init();
    touch_pad_config(TOUCH_GPIO, 0);
    ESP_LOGI(TAG, "Touch pad initialized (GPIO4)");

    wifi_init_sta();
    xTaskCreate(dustbin_task, "dustbin_task", 4096, NULL, 5, NULL);
}
*/