#pragma once

#include <esp_err.h>
#include <esp_log.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_gap.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
#include <services/ans/ble_svc_ans.h>
#include <host/util/util.h>

#define ESP_NIMBLE_API_TAG "NimBLE API"

#define MAX_SUBSCRIPTIONS_PER_CONN 10
#define BLE_GAP_APPEARANCE_GENERIC_TAG 0x0200
#define BLE_GAP_LE_ROLE_PERIPHERAL 0x00

/**
 * @brief BLE connection context storage
 *
 * Tracks active connection state including subscribed characteristics
 */
typedef struct
{
    uint16_t conn_handle;
    uint8_t conn_addr_val[6];
    char conn_addr_str[18];
    uint16_t notify_subscriptions[MAX_SUBSCRIPTIONS_PER_CONN];
    int notify_subscription_count;
    uint16_t indicate_subscriptions[MAX_SUBSCRIPTIONS_PER_CONN];
    int indicate_subscription_count;
} nimble_peripheral_conn_t;

/**
 * @brief NimBLE peripheral configuration parameters
 *
 * Contains security settings, service definitions, and event callbacks
 */
typedef struct
{
    const char *device_name;
    uint8_t sm_io_cap;
    bool sm_bonding;
    bool sm_mitm;
    bool sm_sc;
    bool sm_random_address;
    bool sm_resolve_peer_address;
    struct ble_gatt_svc_def *ble_gatt_services;
    void (*nimble_peripheral_on_connect_cb)(struct ble_gap_event *event, void *arg, int conn_index);
    void (*nimble_peripheral_on_disconnect_cb)(struct ble_gap_event *event, void *arg, nimble_peripheral_conn_t peripheral_conn);
    void (*nimble_peripheral_on_subscribe_notify_cb)(struct ble_gap_event *event, void *arg, int conn_index);
    void (*nimble_peripheral_on_unsubscribe_notify_cb)(struct ble_gap_event *event, void *arg, int conn_index);
    void (*nimble_peripheral_on_subscribe_indicate_cb)(struct ble_gap_event *event, void *arg, int conn_index);
    void (*nimble_peripheral_on_unsubscribe_indicate_cb)(struct ble_gap_event *event, void *arg, int conn_index);
} nimble_peripheral_config_t;

/**
 * @brief Active peripheral state container
 *
 * Maintains runtime BLE connection information and device addressing
 */
typedef struct
{
    uint8_t peripheral_addr_type;
    uint8_t peripheral_addr_val[6];
    uint8_t peripheral_addr_str[18];
    uint16_t peripheral_conn_handle;
    int peripheral_conn_active_count;
    nimble_peripheral_conn_t peripheral_conn[CONFIG_BT_NIMBLE_MAX_CONNECTIONS];
} nimble_peripheral_handle_t;

/**
 * @brief Initialize NimBLE persistent storage configuration
 *
 * Required for bond storage and security material preservation
 */
void ble_store_config_init(void);

/**
 * @brief Initialize NimBLE peripheral stack
 *
 * @param nimble_peripheral_config Configuration parameters struct
 * @param nimble_peripheral Handle for peripheral state management
 * @return esp_err_t
 *  - ESP_OK: Success
 *  - ESP_ERR_INVALID_ARG: Null parameters
 *  - ESP_FAIL: GATT service registration failed
 */
esp_err_t nimble_peripheral_init(nimble_peripheral_config_t *nimble_peripheral_config, nimble_peripheral_handle_t *nimble_peripheral);

/**
 * @brief Send BLE notification to subscribed clients
 *
 * @param attr_handle Characteristic handle for notification
 * @param buffer Pre-allocated output buffer
 * @param buffer_size Buffer capacity
 * @param message Null-terminated string to send
 * @return esp_err_t
 *  - ESP_OK: Notification queued
 *  - ESP_FAIL: No subscriptions or allocation failure
 */
esp_err_t nimble_peripheral_notificate(uint16_t attr_handle, char *buffer, size_t buffer_size, const char *message);

/**
 * @brief Process received data from Nordic UART Service (NUS)
 *
 * @param om Received data mbuf
 * @param buffer Destination buffer for processed data
 * @param buffer_size Buffer capacity
 * @return esp_err_t
 *  - ESP_OK: Data copied successfully
 *  - ESP_ERR_INVALID_ARG: Null parameters
 *  - ESP_ERR_INVALID_SIZE: Data exceeds buffer
 */
esp_err_t nus_process_rx_data(struct os_mbuf *om, char *buffer, size_t buffer_size);