#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/i2c.h"     
#include "driver/ledc.h" 
#include "driver/gpio.h" 
#include "mqtt_client.h"

static const char *TAG = "PELTIER_SYSTEM_WEB";

#define I2C_MASTER_SDA_IO           8      
#define I2C_MASTER_SCL_IO           9     
#define I2C_MASTER_NUM              I2C_NUM_0                          
#define I2C_MASTER_TIMEOUT_MS       1000

#define SHT31_SENSOR_ADDR           0x44 
#define SHT31_CMD_MEASURE_MSB       0x24        
#define SHT31_CMD_MEASURE_LSB       0x00

#define PIN_PWM_LANH                4   
#define PIN_PWM_NONG                5   

typedef enum { DANG_TAT, DANG_LANH, DANG_NONG } TrangThaiPeltier;
static TrangThaiPeltier trang_thai_hien_tai = DANG_TAT;

typedef struct {
    float Kp, Ki, Kd;
    float setpoint;
    float integral_err;
    float prev_err;
    float out_min, out_max;
} PID_Controller;

// Bộ thông số PID
static PID_Controller pid = {
    .Kp = 2000.0f,        
    .Ki = 25.0f,         
    .Kd = 5.0f,          
    .setpoint = 30.0f,   
    .integral_err = 0.0f,
    .prev_err = 0.0f,
    .out_min = -4095.0f, 
    .out_max = 4095.0f   
};

float temp = 25.0f; 
int current_pwm = 0;
esp_mqtt_client_handle_t client = NULL;

static esp_err_t init_i2c_driver_clean(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000, 
    };
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) return err;
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

static esp_err_t sht31_read_temp(float *actual_temp)
{
    uint8_t cmd[2] = { SHT31_CMD_MEASURE_MSB, SHT31_CMD_MEASURE_LSB };
    
    esp_err_t err = i2c_master_write_to_device(I2C_MASTER_NUM, SHT31_SENSOR_ADDR, cmd, sizeof(cmd), pdMS_TO_TICKS(1000));
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t data[6];
    err = i2c_master_read_from_device(I2C_MASTER_NUM, SHT31_SENSOR_ADDR, data, sizeof(data), pdMS_TO_TICKS(1000));
    if (err != ESP_OK) return err;

    uint16_t raw_temp = (data[0] << 8) | data[1];
    *actual_temp = -45.0f + (175.0f * ((float)raw_temp / 65535.0f));

    return ESP_OK;
}

static void init_pwm_drv8833(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_12_BIT, 
        .freq_hz          = 18000,             
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel_lanh = {
        .gpio_num       = PIN_PWM_LANH,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .duty           = 0,
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_lanh));

    ledc_channel_config_t ledc_channel_nong = {
        .gpio_num       = PIN_PWM_NONG,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_1,
        .timer_sel      = LEDC_TIMER_0,
        .duty           = 0,
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_nong));
}

static void set_peltier_pwm(int duty_lanh, int duty_nong)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_lanh);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty_nong);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}


static float compute_pid(PID_Controller *p, float current_temp, float dt)
{
    float error = p->setpoint - current_temp;
    if (error > 2.0f) {
        p->integral_err = 0.0f; 
        p->prev_err = error;
        return p->out_max;      
    } 
    else if (error < -2.0f) {
        p->integral_err = 0.0f; 
        p->prev_err = error;
        return p->out_min;      
    }

    //Logic PID cho vùng an toàn
    if ((error > 0.0f && p->prev_err < 0.0f) || (error < 0.0f && p->prev_err > 0.0f)) {
        p->integral_err = 0.0f;
    }

    p->integral_err += error * dt;
    if (p->integral_err > 200.0f)  p->integral_err = 200.0f;
    if (p->integral_err < -200.0f) p->integral_err = -200.0f;

    float derivative = (error - p->prev_err) / dt;
    p->prev_err = error;

    float output = (p->Kp * error) + (p->Ki * p->integral_err) + (p->Kd * derivative);

    if (output > p->out_max) return p->out_max;
    if (output < p->out_min) return p->out_min;
    return output;
}

static void dieu_khien_peltier(int pid_output)
{
    current_pwm = abs(pid_output);

    if (pid_output < 0) {
        if (trang_thai_hien_tai == DANG_NONG) {
            set_peltier_pwm(0, 0);
            vTaskDelay(pdMS_TO_TICKS(5000)); 
        }
        trang_thai_hien_tai = DANG_LANH;
        set_peltier_pwm(current_pwm, 0);
    } 
    else if (pid_output > 0) {
        if (trang_thai_hien_tai == DANG_LANH) {
            set_peltier_pwm(0, 0);
            vTaskDelay(pdMS_TO_TICKS(5000)); 
        }
        trang_thai_hien_tai = DANG_NONG;
        set_peltier_pwm(0, current_pwm);
    } 
    else {
        trang_thai_hien_tai = DANG_TAT;
        current_pwm = 0;
        set_peltier_pwm(0, 0);
    }
}


static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Đã kết nối Broker MQTT");
        esp_mqtt_client_subscribe(client, "/hethong/set_from_web", 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Mất kết nối MQTT");
        break;
    case MQTT_EVENT_DATA:
        if (strncmp(event->topic, "/hethong/set_from_web", event->topic_len) == 0) {
            char buf[16];
            int len = event->data_len;
            if (len < 16) {
                memcpy(buf, event->data, len);
                buf[len] = '\0';
                
                float web_setpoint = atof(buf);
                if (web_setpoint >= 10.0f && web_setpoint <= 40.0f) {
                    if (fabs(pid.setpoint - web_setpoint) > 0.1f) { 
                        pid.setpoint = web_setpoint;
                        ESP_LOGI(TAG, "Đã cập nhật Setpoint từ Web: %.1f", pid.setpoint);
                    }
                }
            }
        }
        break;
    default:
        break;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://172.20.10.12:1883",
    };
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}


static void task_dieu_khien_va_gui_du_lieu(void *arg)
{
    const float dt = 1.0f; 

    while(1)
    {
        float doc_nhiet_do = 0.0f;
        
        if (sht31_read_temp(&doc_nhiet_do) == ESP_OK) {
            temp = doc_nhiet_do; 
            ESP_LOGI(TAG, "Nhiệt độ: %.2f C | Mục tiêu: %.1f C | PWM: %d", temp, pid.setpoint, current_pwm);
        } else {
            ESP_LOGE(TAG, "LỖI SHT31! Kích hoạt chế độ bảo vệ phần cứng.");
            temp = -45.0f;
            
            //Ngắt khẩn cấp
            set_peltier_pwm(0, 0);
            trang_thai_hien_tai = DANG_TAT;
            current_pwm = 0;
            pid.integral_err = 0.0f;
        }

        float pid_out = compute_pid(&pid, temp, dt);
        dieu_khien_peltier((int)pid_out);

        const char *mode_str = (trang_thai_hien_tai == DANG_LANH) ? "COOL" : 
                               (trang_thai_hien_tai == DANG_NONG) ? "HEAT" : "OFF ";
        
        if(client != NULL) {
            char chuoi_json[128];
            // Định dạng JSON
            sprintf(chuoi_json, "{\"temp\": %.2f, \"set\": %.1f, \"pwm\": %d, \"mode\": \"%s\"}", 
                    temp, pid.setpoint, current_pwm, mode_str);
            esp_mqtt_client_publish(client, "/hethong/temp_control", chuoi_json, 0, 1, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(1000)); // Chu kỳ 1 giây
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_i2c_driver_clean());
    init_pwm_drv8833();

    xTaskCreate(task_dieu_khien_va_gui_du_lieu, "Task_Control", 8192, NULL, 5, NULL);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "Đang kết nối Wi-Fi...");
    ESP_ERROR_CHECK(example_connect()); 

    mqtt_app_start();
    ESP_LOGI(TAG, "Đã khởi động xong mạng và MQTT!");
}