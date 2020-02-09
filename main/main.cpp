#pragma GCC diagnostic warning "-fpermissive"

#include <M5Stack.h>
#include <stdio.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "buffer.h"
#include "esp_log.h"

#include "esp_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "soc/timer_group_struct.h"
#include "driver/periph_ctrl.h"
#include "driver/timer.h"

static const char* TAG = "Printer";
xQueueHandle timer_queue;

#define GPIOP_SCK GPIO_NUM_2
#define GPIOP_SIN GPIO_NUM_5
#define GPIOP_SOUT GPIO_NUM_36

#define ESP_INTR_FLAG_DEFAULT 0

// 変換後の画像データの設定
// 初期値は何でもいい
#define PICTURE_WIDTH  160
#define PICTURE_HEIGHT 144
uint16_t bmp[PICTURE_WIDTH * PICTURE_HEIGHT] = {ORANGE};

const char printer_magic[] = {0x88, 0x33};

// 受信データを貯めておく領域の設定
#define MAX_DATA_LENGTH 5984
unsigned char data[6000] = {0};
unsigned int data_ptr = 0;

enum printer_state
{
    PS_MAGIC0,
    PS_MAGIC1,
    PS_CMD,
    PS_ARG0,
    PS_LEN_LOW,
    PS_LEN_HIGH,
    PS_DATA,
    PS_CHECKSUM0,
    PS_CHECKSUM1,
    PS_ACK,
    PS_STATUS
};
enum printer_state printer_state;
enum printer_state printer_state_prev;
uint16_t printer_data_len;

volatile uint8_t gb_sin, gb_sout;
volatile uint8_t gb_bit;

struct circular_buf recv_buf;

static void printer_state_reset()
{
    printer_data_len = 0;
    printer_state = PS_MAGIC0;
    printer_state_prev = printer_state;
}

static void printer_state_update(uint8_t b)
{
    printer_state_prev = printer_state;
    switch (printer_state)
    {
    case PS_MAGIC0:
        if (b == printer_magic[0])
        {
            printer_state = PS_MAGIC1;
        }
        break;
    case PS_MAGIC1:
        if (b == printer_magic[1])
        {
            printer_state = PS_CMD;
        }
        else
        {
            printer_state = PS_MAGIC0;
        }
        break;
    case PS_CMD:
        printer_state = PS_ARG0;
        break;
    case PS_ARG0:
        printer_state = PS_LEN_LOW;
        break;
    case PS_LEN_LOW:
        printer_data_len = b;
        printer_state = PS_LEN_HIGH;
        break;
    case PS_LEN_HIGH:
        printer_data_len |= b << 8;
        if (printer_data_len != 0)
        {
            printer_state = PS_DATA;
        }
        else
        {
            printer_state = PS_CHECKSUM0;
        }
        break;
    case PS_DATA:
        printer_data_len--;
        printer_state = (printer_data_len == 0) ? PS_CHECKSUM0 : PS_DATA;
        break;
    case PS_CHECKSUM0:
        printer_state = PS_CHECKSUM1;
        break;
    case PS_CHECKSUM1:
        printer_state = PS_ACK;
        break;
    case PS_ACK:
        printer_state = PS_STATUS;
        break;
    case PS_STATUS:
        printer_state = PS_MAGIC0;
        break;
    default:
        break;
    }
}

inline static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    if (gpio_get_level(GPIOP_SCK) == 0)
    { // FALLING
        gb_sout |= gpio_get_level(GPIOP_SOUT) ? 1 : 0;
        gb_bit++;
        if (gb_bit == 8)
        {
            // 1バイトがまとまったので送信する
            //usart_send_blocking(USART2, gb_sout);
            data[data_ptr++] = gb_sout;
            printer_state_update(gb_sout);
            switch (printer_state)
            {
            case PS_ACK:
                buf_push(&recv_buf, 0x81);
                break;
            case PS_STATUS:
                buf_push(&recv_buf, 0x00);
                break;
            default:
                break;
            }

            // Reset state
            gb_bit = 0;
            gb_sout = 0;

            // Prepare next gb_sin
            if (buf_empty(&recv_buf))
            {
                gb_sin = 0x00;
            }
            else
            {
                gb_sin = buf_pop(&recv_buf);
            }
        }
        else
        {
            gb_sin <<= 1;
            gb_sout <<= 1;
        }
    }
    else
    { // RISING
        (gb_sin & 0x80) ? gpio_set_level(GPIOP_SIN, 1) : gpio_set_level(GPIOP_SIN, 0);
    }

    if(data_ptr == MAX_DATA_LENGTH){
        xQueueSendFromISR(timer_queue, NULL, NULL);
    }
}

static void gblink_slave_gpio_setup()
{
    gpio_config_t io_conf;
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = (1ULL<<GPIOP_SIN);
    //disable pull-down mode
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    //disable pull-up mode
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    //configure GPIO with the given settings
    gpio_config(&io_conf);
    gpio_set_level(GPIOP_SIN, 0);

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL<<GPIOP_SOUT);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL<<GPIOP_SCK);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(GPIOP_SCK, gpio_isr_handler, (void*) GPIOP_SCK);
}

extern "C" {
	int app_main(void);
}

/*
 * The main task of this example program
 */

static uint8_t rows=0;

inline uint16_t convertColor(uint8_t dot){
    switch(dot) {
        case 0x00:
            return WHITE;
            break;
        case 0x01:
            return LIGHTGREY;
            break;
        case 0x10:
            return DARKGREY;
            break;
        case 0x11:
            return BLACK;
            break;
        default:
            return 0x00;
            break;
    }
    return ORANGE;
}

static void timer_example_evt_task(void *arg)
{
    while (1) {
        xQueueReceive(timer_queue, NULL, portMAX_DELAY);

        /* Print information that the timer reported an event */
        rows = 0;
        data_ptr = 0;
        printf("This is Test\n");

        // ここで受信データから画像部分を取り出し、ビットマップに変換したあと、
        // M5Stackの画像表示を一気に呼ぶ
        // if(rows < 18){
            for(int i = 0; i < MAX_DATA_LENGTH-15; i++){
                // 受信データがif文の条件の順で並んでいるとき、それ以降の640バイトがドットデータとなる。
                // https://dhole.github.io/post/gameboy_serial_2/
                if(data[i] == 0x88 && data[i+1] == 0x33 && data[i+2] == 0x04 && data[i+3] == 0x00 && data[i+4] == 0x80 && data[i+5] == 0x02){
                    // 640バイトで160*16ドットを表現
                    // 8x8ドットごとに並べていく
                    // 1ドットあたり2ビット :: 2バイトで8ドット
                    uint8_t *payload = data+i+6;

                    for(uint8_t cols = 0; cols < 20; cols++){
                        // タイル位置データのコピーを実施
                        uint8_t *tile;
                        tile = payload+cols*16;
                        uint8_t tile_x, tile_y;
                        for(tile_y = 0; tile_y < 16; tile_y+=2){
                            for(tile_x = 0; tile_x < 8; tile_x++){
                                uint8_t mask = 0x01 << (7-tile_x) ;
                                uint8_t dot;
                                dot =  (tile[tile_y  ] & mask) ? 1 : 0;
                                dot += (tile[tile_y+1] & mask) ? 2 : 0;
                                bmp[(rows*8+tile_y/2)*160+(cols*8+tile_x)] = convertColor(dot);
                            }
                        }
                    }

                    rows++;

                    payload = data+i+6+320;

                    for(uint8_t cols = 0; cols < 20; cols++){
                        // タイル位置データのコピーを実施
                        uint8_t *tile;
                        tile = payload+cols*16;
                        uint16_t tile_x, tile_y;
                        for(tile_y = 0; tile_y < 16; tile_y+=2){
                            for(tile_x = 0; tile_x < 8; tile_x++){
                                uint8_t mask = 0x01 << (7-tile_x) ;
                                uint8_t dot;
                                dot =  (tile[tile_y  ] & mask) ? 1 : 0;
                                dot += (tile[tile_y+1] & mask) ? 2 : 0;
                                bmp[(rows*8+tile_y/2)*160+(cols*8+tile_x)] = convertColor(dot);
                            }
                        }
                    }
                    rows++;
                }
            }
            M5.Lcd.drawBitmap(0, 0, PICTURE_WIDTH, PICTURE_HEIGHT, (uint16_t*)bmp);
        // }
    }
}


int app_main()
{
    // put your setup code here, to run once:
    M5.begin();
    M5.Lcd.printf("hello world");
    ESP_LOGI(TAG, "Start");

    printer_state_reset();
    gblink_slave_gpio_setup();

    timer_queue = xQueueCreate(10, 0);
    xTaskCreate(timer_example_evt_task, "timer_evt_task", 8192*3, NULL, 5, NULL);

    while (1){
        // ESP_LOGI(TAG, "ptr: %d", data_ptr);
        delay(1);
    }
}
