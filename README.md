| Supported Targets | ESP32 |
| ----------------- | ----- |

# ESP-IDF NimBLE API

## I. Overview

Component that simplifies NimBLE peripheral implementation with:
- BLE advertising/scan response configuration
- Connection management (multiple simultaneous connections)
- GATT service setup (including Nordic UART Service)
- Security Management (bonding, MITM protection)
- Notification/Indication support
- Event handling through callbacks

## II. Integration into Your Project

1. Add Component Dependency

   Include the NimBLE Peripheral component in your project via:

   **Terminal command:**

   ```bash
   idf.py add-dependency --git https://github.com/<yourusername>/esp-nimble-api-component.git --git-ref main
   ```

   Or in idf_component.yml:

   ```yml
   dependencies:
   <yourgithub>/esp-nimble-api:
      git: https://github.com/<yourusername>/esp-nimble-api-component.git
      version: main
   ```

2. Essential Configuration

   1. Enable Bluetooth Stack:

      ```bash
      idf.py menuconfig
      ```

      - Navigate to:
      - Component config → Bluetooth → Enable Bluetooth
      - Bluetooth Host → NimBLE (BLE only)

   2. Configure Connection Limits:
      Set CONFIG_BT_NIMBLE_MAX_CONNECTIONS (default 3) according to your needs.

3. Hardware Considerations

   While this is a BLE software component, ensure:

   - Stable power supply for reliable radio operations
   - Antenna placement follows ESP32 hardware guidelines
   - No physical obstructions in operational environment (>1m clearance recommended)

4. Security Setup (Optional)

   For bonded connections:

   ```c
   // In your app code:
   nimble_peripheral_config_t config = {
      .sm_bonding = 1,
      .sm_mitm = 1,  // Enable MITM protection
      .sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY
   };
   ```

5. Service Declaration

   Define your GATT services structure:

   ```c
   static const struct ble_gatt_svc_def my_services[] = {
      { 
         .type = BLE_GATT_SVC_TYPE_PRIMARY,
         .uuid = &custom_svc_uuid,
         .characteristics = (struct ble_gatt_chr_def[]) { 
               { /* Characteristic definition */ },
               {0}
         }
      },
      {0}
   };
   ```

This maintains your existing structure while adding explicit dependency management and hardware/environment considerations specific to BLE operations.

## III. Workflow

1. Initialize with nimble_peripheral_init()

2. Configure advertising parameters

3. Handle connection events via callbacks:

   - On connect/disconnect
   - Subscription changes (notify/indicate)
   - MTU updates

4. Send data via nimble_peripheral_notificate()

## IV. Basic Example

```c
#include <stdio.h>
#include <nvs_flash.h>
#include <esp_nimble_api.h>
#include <esp_random.h>

#define MAIN_TAG "MAIN"

/* NimBLE Peripheral */
static nimble_peripheral_handle_t nimble_peripheral;

/* Nordic UART Service (NUS) */
static const ble_uuid128_t nus_svc_uuid = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static uint16_t nus_tx_chr_val_handle;
static const ble_uuid128_t nus_tx_chr_uuid = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);
static uint16_t nus_rx_chr_val_handle;
static const ble_uuid128_t nus_rx_chr_uuid = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static char nus_tx_buffer[512];
static char nus_rx_buffer[512];

static int nus_rx_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        ESP_LOGE(MAIN_TAG, "NUS RX: Operation not supported, opcode: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (attr_handle != nus_rx_chr_val_handle)
    {
        ESP_LOGE(ESP_NIMBLE_API_TAG, "NUS RX: Invalid attribute handle: %d", attr_handle);
        return BLE_ATT_ERR_UNLIKELY;
    }

    esp_err_t err = nus_process_rx_data(ctxt->om, nus_rx_buffer, sizeof(nus_rx_buffer));
    if (err != ESP_OK)
    {
        ESP_LOGE(MAIN_TAG, "BLE data processing failure | Status: %s", esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }

    ESP_LOGI(ESP_NIMBLE_API_TAG, "NUS RX: %s (len=%d); conn_handle=%d", nus_rx_buffer, strlen(nus_rx_buffer), conn_handle);

    return 0;
}

/* IO GPIO Service */
static const ble_uuid16_t auto_io_svc_uuid = BLE_UUID16_INIT(0x1815);
static uint16_t led_chr_val_handle;
static const ble_uuid128_t led_chr_uuid = BLE_UUID128_INIT(0x23, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15, 0xDE, 0xEF, 0x12, 0x12, 0x25, 0x15, 0x00, 0x00);

static int led_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        ESP_LOGE(MAIN_TAG, "Operation not supported, opcode: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (attr_handle != led_chr_val_handle)
    {
        ESP_LOGE(MAIN_TAG, "Invalid attribute handle: %d", attr_handle);
        return BLE_ATT_ERR_ATTR_NOT_FOUND;
    }

    struct os_mbuf *om = ctxt->om;
    if (!om || om->om_len != 1)
    {
        ESP_LOGE(MAIN_TAG, "Invalid attribute value length: %d", om ? om->om_len : 0);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    if (conn_handle != BLE_HS_CONN_HANDLE_NONE)
    {
        ESP_LOGI(MAIN_TAG, "Write on handle %d from remote device (connection: %d)", attr_handle, conn_handle);
    }
    else
    {
        ESP_LOGI(MAIN_TAG, "Write on handle %d from internal stack", attr_handle);
    }

    uint8_t led_state = om->om_data[0];
    if (led_state)
    {
        ESP_LOGI(MAIN_TAG, "LED ON");
    }
    else
    {
        ESP_LOGI(MAIN_TAG, "LED OFF");
    }

    return 0;
}

/* NimBLE Peripheral Services & Callbacks */
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &nus_tx_chr_uuid.u,
                .access_cb = nus_rx_chr_access_cb,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &nus_tx_chr_val_handle,
            },
            {
                .uuid = &nus_rx_chr_uuid.u,
                .access_cb = nus_rx_chr_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &nus_rx_chr_val_handle,
            },
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &auto_io_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &led_chr_uuid.u,
                .access_cb = led_chr_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &led_chr_val_handle,
            },
            {0},
        },
    },
    {0}};

static void on_connect_cb(struct ble_gap_event *event, void *arg, int conn_index)
{
    ESP_LOGI(MAIN_TAG, "%s connected (Total active %d)", nimble_peripheral.peripheral_conn[conn_index].conn_addr_str, nimble_peripheral.peripheral_conn_active_count);
}

static void on_disconnect(struct ble_gap_event *event, void *arg, nimble_peripheral_conn_t peripheral_conn)
{
    ESP_LOGI(MAIN_TAG, "%s disconnected (Total active %d)", peripheral_conn.conn_addr_str, nimble_peripheral.peripheral_conn_active_count);
}

static void on_notify_subscribe(struct ble_gap_event *event, void *arg, int conn_index)
{
    ESP_LOGI(MAIN_TAG, "%s subscribed to notify %d. (Total subscriptions %d)", nimble_peripheral.peripheral_conn[conn_index].conn_addr_str, event->subscribe.attr_handle, nimble_peripheral.peripheral_conn[conn_index].notify_subscription_count + nimble_peripheral.peripheral_conn[conn_index].indicate_subscription_count);
}

static void on_notify_unsubscribe(struct ble_gap_event *event, void *arg, int conn_index)
{
    ESP_LOGI(MAIN_TAG, "%s unsubscribed from notify %d. (Total subscriptions %d)", nimble_peripheral.peripheral_conn[conn_index].conn_addr_str, event->subscribe.attr_handle, nimble_peripheral.peripheral_conn[conn_index].notify_subscription_count + nimble_peripheral.peripheral_conn[conn_index].indicate_subscription_count);
}

static void on_subscribe_indicate(struct ble_gap_event *event, void *arg, int conn_index)
{
    ESP_LOGI(MAIN_TAG, "%s subscribed to indicate %d. (Total subscriptions %d)", nimble_peripheral.peripheral_conn[conn_index].conn_addr_str, event->subscribe.attr_handle, nimble_peripheral.peripheral_conn[conn_index].notify_subscription_count + nimble_peripheral.peripheral_conn[conn_index].indicate_subscription_count);
}

static void on_unsubscribe_indicate(struct ble_gap_event *event, void *arg, int conn_index)
{
    ESP_LOGI(MAIN_TAG, "%s unsubscribed from indicate %d. (Total subscriptions %d)", nimble_peripheral.peripheral_conn[conn_index].conn_addr_str, event->subscribe.attr_handle, nimble_peripheral.peripheral_conn[conn_index].notify_subscription_count + nimble_peripheral.peripheral_conn[conn_index].indicate_subscription_count);
}

static nimble_peripheral_config_t nimble_peripheral_config = {
    .device_name = "ESP32-NimBLE",
    .sm_io_cap = BLE_SM_IO_CAP_NO_IO,
    .sm_bonding = false,
    .sm_mitm = false,
    .sm_sc = false,
    .sm_random_address = false,
    .sm_resolve_peer_address = false,
    .ble_gatt_services = (struct ble_gatt_svc_def *)gatt_svcs,
    .nimble_peripheral_on_connect_cb = on_connect_cb,
    .nimble_peripheral_on_disconnect_cb = on_disconnect,
    .nimble_peripheral_on_subscribe_notify_cb = on_notify_subscribe,
    .nimble_peripheral_on_unsubscribe_notify_cb = on_notify_unsubscribe,
    .nimble_peripheral_on_subscribe_indicate_cb = on_subscribe_indicate,
    .nimble_peripheral_on_unsubscribe_indicate_cb = on_unsubscribe_indicate};

/* DHT11 Simulation */
int dht11_temperature;
int dht11_humidity;

/* NVS */
static esp_err_t nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    return err;
}

static void dht11_task(void *param)
{
    ESP_LOGI(MAIN_TAG, "DHT11 task started successfully.");

    while (1)
    {
        uint8_t dht11_temperature = 15 + (uint8_t)(esp_random() % 6);
        uint8_t dht11_humidity = 40 + (uint8_t)(esp_random() % 11);

        int ret = snprintf(nus_tx_buffer, sizeof(nus_tx_buffer), "T:%d°C, H:%d%%", dht11_temperature, dht11_humidity);
        if (ret < 0 || (size_t)ret >= sizeof(nus_tx_buffer))
        {
            ESP_LOGE(MAIN_TAG, "Failed to format sensor data: message is too long or an encoding error occurred.");
        }
        else
        {
            esp_err_t res = nimble_peripheral_notificate(nus_tx_chr_val_handle, nus_tx_buffer, sizeof(nus_tx_buffer), nus_tx_buffer);
            if (res != ESP_OK)
            {
                ESP_LOGE(MAIN_TAG, "Notification process failed: check previous logs for details.");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    /* NVS */
    ESP_ERROR_CHECK(nvs_init());
    /* NimBLE Utils */
    ESP_ERROR_CHECK(nimble_peripheral_init(&nimble_peripheral_config, &nimble_peripheral));
    /* Temperature & Humidity Reporter */
    xTaskCreate(dht11_task, "DHT11 Task", 4 * 1024, NULL, 5, NULL);
}
```

## V. Key Functions

- nimble_peripheral_init(): Main initialization
- nimble_peripheral_notificate(): Send notifications
- nus_process_rx_data(): Handle received data (NUS)
- GAP event handler: Manage connections/subscriptions

## VI. Event Handling

Implement these callbacks in your config:

- nimble_peripheral_on_connect_cb
- nimble_peripheral_on_disconnect_cb
- nimble_peripheral_on_subscribe_notify_cb

## VII. Questions/Issues

Report any issues with:

- Connection stability
- Notification reliability
- Security handshake problems
- Memory allocation errors