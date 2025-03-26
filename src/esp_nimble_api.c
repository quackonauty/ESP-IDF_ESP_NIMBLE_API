#include <esp_nimble_api.h>

static uint8_t esp_uri[] = {0x17, '/', '/', 'e', 's', 'p', 'r', 'e', 's', 's', 'i', 'f', '.', 'c', 'o', 'm'};
static nimble_peripheral_config_t *g_nimble_peripheral_config = NULL;
static nimble_peripheral_handle_t *g_nimble_peripheral = NULL;

static esp_err_t ble_app_set_addr()
{
    int rc;
    ble_addr_t addr;

    rc = ble_hs_id_gen_rnd(0, &addr);
    if (rc != 0)
    {
        ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to generate random address");
        return ESP_FAIL;
    }

    rc = ble_hs_id_set_rnd(addr.val);
    if (rc != 0)
    {
        switch (rc)
        {
        case BLE_HS_EINVAL:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to set random address, invalid address specified");
            break;
        default:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to set random address, error code: %d", rc);
            break;
        }
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void nimble_peripheral_advertise(void);
static void nimble_peripheral_ext_advertise(void);

static int nimble_peripheral_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    int rc = 0;
    esp_err_t err;
    uint16_t conn_handle;
    int conn_index;

    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        conn_handle = event->connect.conn_handle;
        if (event->connect.status == 0)
        {
            struct ble_gap_conn_desc desc;
            rc = ble_gap_conn_find(conn_handle, &desc);
            if (rc != 0)
            {
                ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to find connection descriptor (handle %d)", conn_handle);
                break;
            }

            if (g_nimble_peripheral->peripheral_conn_active_count >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS)
            {
                ESP_LOGW(ESP_NIMBLE_API_TAG, "Maximum connections reached; cannot register connection handle %d", conn_handle);
                break;
            }

            conn_index = g_nimble_peripheral->peripheral_conn_active_count;
            nimble_peripheral_conn_t *peripheral_conn = &g_nimble_peripheral->peripheral_conn[conn_index];

            peripheral_conn->conn_handle = event->connect.conn_handle;
            memcpy(peripheral_conn->conn_addr_val, desc.peer_id_addr.val, sizeof(peripheral_conn->conn_addr_val));
            sprintf(peripheral_conn->conn_addr_str, "%02X:%02X:%02X:%02X:%02X:%02X", desc.peer_id_addr.val[5], desc.peer_id_addr.val[4], desc.peer_id_addr.val[3], desc.peer_id_addr.val[2], desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
            memset(peripheral_conn->notify_subscriptions, 0, sizeof(peripheral_conn->notify_subscriptions));
            memset(peripheral_conn->indicate_subscriptions, 0, sizeof(peripheral_conn->indicate_subscriptions));
            peripheral_conn->notify_subscription_count = 0;
            peripheral_conn->indicate_subscription_count = 0;

            g_nimble_peripheral->peripheral_conn_active_count++;

            if (g_nimble_peripheral_config->nimble_peripheral_on_connect_cb)
            {
                g_nimble_peripheral_config->nimble_peripheral_on_connect_cb(event, arg, conn_index);
            }
        }
        else
        {
            if (g_nimble_peripheral->peripheral_conn_active_count < CONFIG_BT_NIMBLE_MAX_CONNECTIONS)
            {
                nimble_peripheral_advertise();
            }
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        conn_handle = event->disconnect.conn.conn_handle;
        bool found = false;
        int foundIndex;
        nimble_peripheral_conn_t peripheral_conn;

        for (int i = 0; i < g_nimble_peripheral->peripheral_conn_active_count; i++)
        {
            if (g_nimble_peripheral->peripheral_conn[i].conn_handle == conn_handle)
            {
                foundIndex = i;
                peripheral_conn = g_nimble_peripheral->peripheral_conn[i];
                found = true;
                break;
            }
        }

        if (!found)
        {
            ESP_LOGW(ESP_NIMBLE_API_TAG, "Disconnect event for unknown connection: Handle=%d", conn_handle);
            break;
        }

        int last_index = g_nimble_peripheral->peripheral_conn_active_count - 1;
        if (foundIndex != last_index)
        {
            g_nimble_peripheral->peripheral_conn[foundIndex] = g_nimble_peripheral->peripheral_conn[last_index];
        }
        memset(&g_nimble_peripheral->peripheral_conn[last_index], 0, sizeof(nimble_peripheral_conn_t));
        g_nimble_peripheral->peripheral_conn_active_count--;

        if (g_nimble_peripheral_config->nimble_peripheral_on_disconnect_cb)
        {
            g_nimble_peripheral_config->nimble_peripheral_on_disconnect_cb(event, arg, peripheral_conn);
        }

        if (g_nimble_peripheral->peripheral_conn_active_count < CONFIG_BT_NIMBLE_MAX_CONNECTIONS)
        {
            nimble_peripheral_advertise();
        }
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(ESP_NIMBLE_API_TAG, "Advertise complete; reason=%d, readvertising...", event->adv_complete.reason);
        nimble_peripheral_advertise();
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if ((event->notify_tx.status != 0) && (event->notify_tx.status != BLE_HS_EDONE))
        {
            ESP_LOGI(ESP_NIMBLE_API_TAG, "Notify event; conn_handle=%d attr_handle=%d status=%d is_indication=%d", event->notify_tx.conn_handle, event->notify_tx.attr_handle, event->notify_tx.status, event->notify_tx.indication);
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        conn_handle = event->subscribe.conn_handle;
        bool found_conn = false;
        int conn_index;

        for (int i = 0; i < g_nimble_peripheral->peripheral_conn_active_count; i++)
        {
            if (g_nimble_peripheral->peripheral_conn[i].conn_handle == conn_handle)
            {
                conn_index = i;
                found_conn = true;
                break;
            }
        }

        if (!found_conn)
        {
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Subscribe event connection handle %d not found", conn_handle);
            break;
        }

        nimble_peripheral_conn_t *conn = &g_nimble_peripheral->peripheral_conn[conn_index];

        if (event->subscribe.prev_notify != event->subscribe.cur_notify)
        {
            if (event->subscribe.cur_notify)
            {
                if (conn->notify_subscription_count < MAX_SUBSCRIPTIONS_PER_CONN)
                {
                    conn->notify_subscriptions[conn->notify_subscription_count] = event->subscribe.attr_handle;
                    conn->notify_subscription_count++;
                }
                else
                {
                    ESP_LOGW(ESP_NIMBLE_API_TAG, "Max notification subscriptions reached on connection %d", conn_handle);
                }

                if (g_nimble_peripheral_config->nimble_peripheral_on_subscribe_notify_cb)
                {
                    g_nimble_peripheral_config->nimble_peripheral_on_subscribe_notify_cb(event, arg, conn_index);
                }
            }
            else
            {
                for (int j = 0; j < conn->notify_subscription_count; j++)
                {
                    if (conn->notify_subscriptions[j] == event->subscribe.attr_handle)
                    {
                        for (int k = j; k < conn->notify_subscription_count - 1; k++)
                        {
                            conn->notify_subscriptions[k] = conn->notify_subscriptions[k + 1];
                        }
                        conn->notify_subscription_count--;
                        break;
                    }
                }

                if (g_nimble_peripheral_config->nimble_peripheral_on_unsubscribe_notify_cb)
                {
                    g_nimble_peripheral_config->nimble_peripheral_on_unsubscribe_notify_cb(event, arg, conn_index);
                }
            }
        }

        if (event->subscribe.prev_indicate != event->subscribe.cur_indicate)
        {
            if (event->subscribe.cur_indicate)
            {
                if (conn->indicate_subscription_count < MAX_SUBSCRIPTIONS_PER_CONN)
                {
                    conn->indicate_subscriptions[conn->indicate_subscription_count] = event->subscribe.attr_handle;
                    conn->indicate_subscription_count++;
                }
                else
                {
                    ESP_LOGW(ESP_NIMBLE_API_TAG, "Max indication subscriptions reached on connection %d", conn_handle);
                }
                if (g_nimble_peripheral_config->nimble_peripheral_on_subscribe_indicate_cb)
                {
                    g_nimble_peripheral_config->nimble_peripheral_on_subscribe_indicate_cb(event, arg, conn_index);
                }
            }
            else
            {
                for (int j = 0; j < conn->indicate_subscription_count; j++)
                {
                    if (conn->indicate_subscriptions[j] == event->subscribe.attr_handle)
                    {
                        for (int k = j; k < conn->indicate_subscription_count - 1; k++)
                        {
                            conn->indicate_subscriptions[k] = conn->indicate_subscriptions[k + 1];
                        }
                        conn->indicate_subscription_count--;
                        break;
                    }
                }

                if (g_nimble_peripheral_config->nimble_peripheral_on_unsubscribe_indicate_cb)
                {
                    g_nimble_peripheral_config->nimble_peripheral_on_unsubscribe_indicate_cb(event, arg, conn_index);
                }
            }
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(ESP_NIMBLE_API_TAG, "mtu update event; conn_handle=%d cid=%d mtu=%d", event->mtu.conn_handle, event->mtu.channel_id, event->mtu.value);
        break;
    }

    return rc;
}

static void nimble_peripheral_advertise(void)
{
    ESP_LOGI(ESP_NIMBLE_API_TAG, "Starting advertising...");

    struct ble_hs_adv_fields adv_fields = {0};
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields rsp_fields = {0};

    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    adv_fields.tx_pwr_lvl_is_present = 1;
    adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char *name = ble_svc_gap_device_name();
    adv_fields.name = (uint8_t *)name;
    adv_fields.name_len = strlen(name);
    adv_fields.name_is_complete = 1;

    adv_fields.appearance_is_present = 1;
    adv_fields.appearance = BLE_GAP_APPEARANCE_GENERIC_TAG;

    adv_fields.le_role_is_present = 1;
    adv_fields.le_role = BLE_GAP_LE_ROLE_PERIPHERAL;

    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0)
    {
        switch (rc)
        {
        case BLE_HS_EBUSY:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to set advertising data, advertising is in progress");
            break;
        case BLE_HS_EMSGSIZE:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to set advertising data, data is too large to fit into an advertisement");
            break;
        default:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to set advertising data, error code: %d", rc);
            break;
        }
        return;
    }

    rsp_fields.device_addr = g_nimble_peripheral->peripheral_addr_val;
    rsp_fields.device_addr_type = g_nimble_peripheral->peripheral_addr_type;
    rsp_fields.device_addr_is_present = 1;

    rsp_fields.uri = esp_uri;
    rsp_fields.uri_len = sizeof(esp_uri);

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0)
    {
        switch (rc)
        {
        case BLE_HS_EBUSY:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to set scan response data, advertising is in progress");
            break;
        case BLE_HS_EMSGSIZE:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to set scan response data, data is too large to fit into a scan response");
            break;
        default:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to set scan response data, error code: %d", rc);
            break;
        }
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(g_nimble_peripheral->peripheral_addr_type, NULL, BLE_HS_FOREVER, &adv_params, nimble_peripheral_gap_event_cb, NULL);
    if (rc != 0)
    {
        ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to start advertising, error code: %d", rc);
        return;
    }

    ESP_LOGI(ESP_NIMBLE_API_TAG, "Advertising started successfully");
}

static void nimble_peripheral_ext_advertise(void)
{
    // struct ble_gap_ext_adv_params params;
    // struct os_mbuf *data;
    // uint8_t instance = 0;
    // int rc;

    // /* First check if any instance is already active */
    // if (ble_gap_ext_adv_active(instance))
    // {
    //     return;
    // }

    // /* use defaults for non-set params */
    // memset(&params, 0, sizeof(params));

    // /* enable connectable advertising */
    // params.connectable = 1;

    // /* advertise using random addr */
    // params.own_addr_type = BLE_OWN_ADDR_PUBLIC;

    // params.primary_phy = BLE_HCI_LE_PHY_1M;
    // params.secondary_phy = BLE_HCI_LE_PHY_2M;
    // // params.tx_power = 127;
    // params.sid = 1;

    // params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    // params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MIN;

    // /* configure instance 0 */
    // rc = ble_gap_ext_adv_configure(instance, &params, NULL,
    //                                bleprph_gap_event, NULL);
    // assert(rc == 0);

    // /* in this case only scan response is allowed */

    // /* get mbuf for scan rsp data */
    // data = os_msys_get_pkthdr(sizeof(ext_adv_pattern_1), 0);
    // assert(data);

    // /* fill mbuf with scan rsp data */
    // rc = os_mbuf_append(data, ext_adv_pattern_1, sizeof(ext_adv_pattern_1));
    // assert(rc == 0);

    // rc = ble_gap_ext_adv_set_data(instance, data);
    // assert(rc == 0);

    // /* start advertising */
    // rc = ble_gap_ext_adv_start(instance, 0, 0);
    // assert(rc == 0);
}

static void host_controller_reset_cb(int err)
{
    ESP_LOGI(ESP_NIMBLE_API_TAG, "Host and controller reset, error code: %d", err);
}

static void host_controller_sync_cb(void)
{
    int rc;

    if (g_nimble_peripheral_config->sm_random_address)
    {
        ESP_ERROR_CHECK(ble_app_set_addr());
        rc = ble_hs_util_ensure_addr(1);
    }
    else
    {
        rc = ble_hs_util_ensure_addr(0);
    }
    if (rc != 0)
    {
        switch (rc)
        {
        case BLE_HS_ENOADDR:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to ensure address, no available address");
            break;
        default:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to ensure address, error code: %d", rc);
            break;
        }
        abort();
    }

    rc = ble_hs_id_infer_auto(0, &g_nimble_peripheral->peripheral_addr_type);
    if (rc != 0)
    {
        switch (rc)
        {
        case BLE_HS_ENOADDR:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to infer address type, no suitable address to infer");
            break;
        default:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to infer address type, error code: %d", rc);
            break;
        }
        abort();
    }

    rc = ble_hs_id_copy_addr(g_nimble_peripheral->peripheral_addr_type, g_nimble_peripheral->peripheral_addr_val, NULL);
    if (rc != 0)
    {
        switch (rc)
        {
        case BLE_HS_EINVAL:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to copy address, invalid address specified");
            break;
        case BLE_HS_ENOADDR:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to copy address, no identity address of the requested type");
            break;
        default:
            ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to copy address, error code: %d", rc);
            break;
        }
        abort();
    }
    ESP_LOGI(ESP_NIMBLE_API_TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x", g_nimble_peripheral->peripheral_addr_val[5], g_nimble_peripheral->peripheral_addr_val[4], g_nimble_peripheral->peripheral_addr_val[3], g_nimble_peripheral->peripheral_addr_val[2], g_nimble_peripheral->peripheral_addr_val[1], g_nimble_peripheral->peripheral_addr_val[0]);

    memset(g_nimble_peripheral->peripheral_conn, 0, sizeof(g_nimble_peripheral->peripheral_conn));
    g_nimble_peripheral->peripheral_conn_active_count = 0;

    nimble_peripheral_advertise();
}

static void nimble_peripheral_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t nimble_peripheral_init(nimble_peripheral_config_t *nimble_peripheral_config, nimble_peripheral_handle_t *nimble_peripheral)
{
    ESP_LOGI(ESP_NIMBLE_API_TAG, "Initializating NimBLE...");

    if (!nimble_peripheral_config || !nimble_peripheral)
    {
        ESP_LOGE(ESP_NIMBLE_API_TAG, "Invalid configuration or peripheral handle");
        return ESP_ERR_INVALID_ARG;
    }
    g_nimble_peripheral_config = nimble_peripheral_config;
    g_nimble_peripheral = nimble_peripheral;

    if (g_nimble_peripheral_config->sm_random_address)
    {
        g_nimble_peripheral->peripheral_addr_type = BLE_OWN_ADDR_RANDOM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to initialize controller and host stack, error code: %d", err);
        return err;
    }

    ble_hs_cfg.reset_cb = host_controller_reset_cb;
    ble_hs_cfg.sync_cb = host_controller_sync_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_hs_cfg.sm_io_cap = g_nimble_peripheral_config->sm_io_cap;

    if (g_nimble_peripheral_config->sm_bonding)
    {
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
        ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    }

    if (nimble_peripheral_config->sm_mitm)
    {
        ble_hs_cfg.sm_mitm = 1;
    }

    if (nimble_peripheral_config->sm_sc)
    {
        ble_hs_cfg.sm_sc = 1;
    }
    else
    {
        ble_hs_cfg.sm_sc = 0;
    }

    if (nimble_peripheral_config->sm_resolve_peer_address)
    {
        ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ID;
    }

    ble_svc_gap_init();
    int rc = ble_svc_gap_device_name_set(nimble_peripheral_config->device_name);
    if (rc != 0)
    {
        ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to set service gap decive name");
        return ESP_FAIL;
    }
    rc = ble_svc_gap_device_appearance_set(BLE_GAP_APPEARANCE_GENERIC_TAG);
    if (rc != 0)
    {
        ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to set service gap device appearance");
        return ESP_FAIL;
    }

    ble_svc_gatt_init();
    rc = ble_gatts_count_cfg(nimble_peripheral_config->ble_gatt_services);
    if (rc != 0)
    {
        ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to count GATT server configuration, error code: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(nimble_peripheral_config->ble_gatt_services);
    if (rc != 0)
    {
        ESP_LOGE(ESP_NIMBLE_API_TAG, "Failed to add GATT services, error code: %d", rc);
        return ESP_FAIL;
    }

    ble_svc_ans_init();

    ble_store_config_init();

    nimble_port_freertos_init(nimble_peripheral_host_task);

    return err;
}

esp_err_t nimble_peripheral_notificate(uint16_t attr_handle, char *buffer, size_t buffer_size, const char *message)
{
    size_t len = strnlen(message, buffer_size);
    if (len == buffer_size)
    {
        ESP_LOGE(ESP_NIMBLE_API_TAG, "Message is too long or not null-terminated.");
        return ESP_FAIL;
    }

    if (!g_nimble_peripheral || g_nimble_peripheral->peripheral_conn_active_count == 0)
    {
        ESP_LOGW(ESP_NIMBLE_API_TAG, "No active BLE connections to send notifications.");
        return ESP_FAIL;
    }

    for (int i = 0; i < g_nimble_peripheral->peripheral_conn_active_count; i++)
    {
        nimble_peripheral_conn_t *conn = &g_nimble_peripheral->peripheral_conn[i];

        for (int j = 0; j < conn->notify_subscription_count; j++)
        {
            if (conn->notify_subscriptions[j] == attr_handle)
            {
                struct os_mbuf *om = ble_hs_mbuf_from_flat((uint8_t *)message, len);
                if (!om)
                {
                    ESP_LOGE(ESP_NIMBLE_API_TAG, "Memory allocation failed for notification.");
                    return ESP_FAIL;
                }

                int rc = ble_gatts_notify_custom(conn->conn_handle, attr_handle, om);
                if (rc != 0)
                {
                    ESP_LOGE(ESP_NIMBLE_API_TAG, "Notification failed: conn_handle=%d, attr_handle=%d, error=%d", conn->conn_handle, attr_handle, rc);
                    return ESP_FAIL;
                }

                ESP_LOGI(ESP_NIMBLE_API_TAG, "Notification sent: conn_handle=%d, attr_handle=%d", conn->conn_handle, attr_handle);
            }
        }
    }

    return ESP_OK;
}

esp_err_t nus_process_rx_data(struct os_mbuf *om, char *buffer, size_t buffer_size)
{
    if (!om || !buffer || buffer_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int buf_len = om->om_len;
    if (buf_len == 0 || buf_len >= buffer_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    int rc = os_mbuf_copydata(om, 0, buf_len, buffer);
    if (rc != 0)
    {
        return ESP_FAIL;
    }
    buffer[buf_len] = '\0';

    return ESP_OK;
}