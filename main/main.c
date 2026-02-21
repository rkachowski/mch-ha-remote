/*
 * MCH2022 HA Remote Control
 * MQTT-based remote with joystick menu navigation.
 */

#include "main.h"

#include <esp_log.h>
#include <string.h>

static const char *TAG = "mch-ha-remote";

static pax_buf_t buf;
xQueueHandle buttonQueue;

// MQTT state.
static esp_mqtt_client_handle_t mqtt_client;
static esp_event_loop_handle_t  mqtt_event_loop;
static volatile bool mqtt_connected = false;
static volatile bool exiting        = false;

// Menu definition.
#define MENU_ITEM_COUNT 3
static const char *menu_labels[MENU_ITEM_COUNT] = {
    "Item 1",
    "Item 2",
    "Item 3",
};
static const char *menu_topics[MENU_ITEM_COUNT] = {
    "home/remote/item1",
    "home/remote/item2",
    "home/remote/item3",
};
static const char *menu_payload = "PRESS";
static int selected_item = 0;

// Updates the screen with the latest buffer.
void disp_flush() {
    ili9341_write(get_ili9341(), buf.buf);
}

// Exits the app, returning to the launcher.
void exit_to_launcher() {
    REG_WRITE(RTC_CNTL_STORE0_REG, 0);
    esp_restart();
}

// Draw the disconnected screen.
static void draw_connecting_screen() {
    pax_background(&buf, 0xff000000);

    const pax_font_t *font = pax_font_saira_condensed;

    char *text = "Connecting...";
    pax_vec1_t dims = pax_text_size(font, font->default_size, text);
    pax_draw_text(
        &buf, 0xffffffff, font, font->default_size,
        (buf.width  - dims.x) / 2.0,
        (buf.height - dims.y) / 2.0,
        text
    );

    char *sub = "mqtt.apps.lan";
    pax_vec1_t sub_dims = pax_text_size(font, 18, sub);
    pax_draw_text(
        &buf, 0xff888888, font, 18,
        (buf.width  - sub_dims.x) / 2.0,
        buf.height - 30,
        sub
    );
}

// Draw the connected menu screen.
static void draw_menu_screen() {
    pax_background(&buf, 0xff1a1a2e);

    const pax_font_t *font = pax_font_saira_condensed;

    // Title bar.
    pax_draw_rect(&buf, 0xff16213e, 0, 0, buf.width, 36);
    pax_draw_text(&buf, 0xffffffff, font, 24, 10, 6, "HA Remote");

    // Menu items.
    float row_height = 48;
    float menu_y     = 46;

    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        float y = menu_y + i * row_height;

        if (i == selected_item) {
            pax_draw_rect(&buf, 0xff4a90d9, 0, y, buf.width, row_height);
        }

        pax_col_t text_col = (i == selected_item) ? 0xffffffff : 0xffcccccc;
        pax_draw_text(&buf, text_col, font, 28, 16, y + 10, menu_labels[i]);
    }

    // Footer hint.
    pax_draw_rect(&buf, 0xff16213e, 0, buf.height - 28, buf.width, 28);
    pax_draw_text(&buf, 0xff888888, font, 16, 10, buf.height - 24, "A: Send  HOME: Exit");
}

void app_main() {
    ESP_LOGI(TAG, "Starting HA Remote");

    // Initialize hardware.
    bsp_init();
    bsp_rp2040_init();
    buttonQueue = get_rp2040()->queue;

    // Initialize graphics.
    pax_buf_init(&buf, NULL, 320, 240, PAX_BUF_16_565RGB);

    // Initialize NVS.
    ESP_ERROR_CHECK(nvs_flash_init());

    // Initialize and connect WiFi.
    wifi_init();
    wifi_connect_to_stored();

    // Create MQTT event loop.
    const esp_event_loop_args_t event_args = {
        .queue_size      = 32,
        .task_core_id    = 1,
        .task_name       = "mqtt-events",
        .task_priority   = 1,
        .task_stack_size = 2048,
    };
    esp_event_loop_create(&event_args, &mqtt_event_loop);

    // Initialize MQTT client.
    const esp_mqtt_client_config_t mqtt_cfg = {
        .event_loop_handle = mqtt_event_loop,
        .uri               = "mqtt://mqtt.apps.lan:1883/",
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));

    // Main loop.
    while (1) {
        // Draw appropriate screen.
        if (mqtt_connected) {
            draw_menu_screen();
        } else {
            draw_connecting_screen();
        }
        disp_flush();

        // Wait for button input; use short timeout when disconnected for redraw.
        rp2040_input_message_t message;
        TickType_t timeout = mqtt_connected ? portMAX_DELAY : pdMS_TO_TICKS(500);

        if (xQueueReceive(buttonQueue, &message, timeout)) {
            if (message.state) {
                switch (message.input) {
                    case RP2040_INPUT_BUTTON_HOME:
                        exiting = true;
                        exit_to_launcher();
                        break;

                    case RP2040_INPUT_JOYSTICK_UP:
                        selected_item = (selected_item - 1 + MENU_ITEM_COUNT) % MENU_ITEM_COUNT;
                        break;

                    case RP2040_INPUT_JOYSTICK_DOWN:
                        selected_item = (selected_item + 1) % MENU_ITEM_COUNT;
                        break;

                    case RP2040_INPUT_BUTTON_ACCEPT:
                        if (mqtt_connected) {
                            esp_mqtt_client_publish(
                                mqtt_client,
                                menu_topics[selected_item],
                                menu_payload,
                                0,  // len (0 = auto from null-terminated string)
                                1,  // qos
                                0   // retain
                            );
                            ESP_LOGI(TAG, "Published to %s", menu_topics[selected_item]);
                        }
                        break;

                    default:
                        break;
                }
            }
        }
    }
}

void mqtt_event_handler(void *event_handler_arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data) {
    if (event_id == MQTT_EVENT_CONNECTED) {
        mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
    } else if (event_id == MQTT_EVENT_DISCONNECTED && !exiting) {
        mqtt_connected = false;
        ESP_LOGI(TAG, "MQTT disconnected, reconnecting WiFi...");
        wifi_connect_to_stored();
    } else if (event_id == MQTT_EVENT_ERROR) {
        ESP_LOGE(TAG, "MQTT error");
    }
}
