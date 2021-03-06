/*
 * Copyright (C) 2018 Gunar Schorcht
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     cpu_esp_common_esp_now
 * @{
 *
 * @file
 * @brief       Netdev interface for the ESP-NOW WiFi P2P protocol
 *
 * @author      Gunar Schorcht <gunar@schorcht.net>
 * @author      Timo Rothenpieler <timo.rothenpieler@uni-bremen.de>
 */

#include "log.h"
#include "tools.h"

#include <string.h>
#include <assert.h>
#include <errno.h>

#include "net/gnrc.h"
#include "xtimer.h"

#include "esp_common.h"
#include "esp_attr.h"
#include "esp_event_loop.h"
#include "esp_now.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "irq_arch.h"
#include "od.h"

#include "nvs_flash/include/nvs_flash.h"

#include "esp_now_params.h"
#include "esp_now_netdev.h"

#define ENABLE_DEBUG             (0)
#include "debug.h"

#define ESP_NOW_UNICAST          (1)

#define ESP_NOW_WIFI_STA         (1)
#define ESP_NOW_WIFI_SOFTAP      (2)
#define ESP_NOW_WIFI_STA_SOFTAP  (ESP_NOW_WIFI_STA + ESP_NOW_WIFI_SOFTAP)

#define ESP_NOW_AP_PREFIX        "RIOT_ESP_"
#define ESP_NOW_AP_PREFIX_LEN    (strlen(ESP_NOW_AP_PREFIX))

/**
 * There is only one ESP-NOW device. We define it as static device variable
 * to have accesss to the device inside ESP-NOW interrupt routines which do
 * not provide an argument that could be used as pointer to the ESP-NOW
 * device which triggers the interrupt.
 */
static esp_now_netdev_t _esp_now_dev = { 0 };
static const netdev_driver_t _esp_now_driver;

static bool _esp_now_add_peer(const uint8_t* bssid, uint8_t channel, uint8_t* key)
{
    if (esp_now_is_peer_exist(bssid)) {
        return false;
    }

    esp_now_peer_info_t peer = {};

    memcpy(peer.peer_addr, bssid, ESP_NOW_ETH_ALEN);
    peer.channel = channel;
    peer.ifidx = ESP_IF_WIFI_AP;

    if (esp_now_params.key) {
        peer.encrypt = true;
        memcpy(peer.lmk, esp_now_params.key, ESP_NOW_KEY_LEN);
    }

    esp_err_t ret = esp_now_add_peer(&peer);
    DEBUG("esp_now_add_peer node %02x:%02x:%02x:%02x:%02x:%02x "
          "added with return value %d\n",
          bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], ret);
    return (ret == ESP_OK);
}

#if ESP_NOW_UNICAST

static xtimer_t _esp_now_scan_peers_timer;
static bool _esp_now_scan_peers_done = false;

#define ESP_NOW_APS_BLOCK_SIZE 8 /* has to be power of two */

static wifi_ap_record_t* aps = NULL;
static uint32_t aps_size = 0;

static const wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = ESP_NOW_CHANNEL,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 0,
        .scan_time.active.max = 120 /* TODO tune value */
};

static void IRAM_ATTR esp_now_scan_peers_done(void)
{
    mutex_lock(&_esp_now_dev.dev_lock);

    esp_err_t ret;
    uint16_t ap_num;

    ret = esp_wifi_scan_get_ap_num(&ap_num);
    DEBUG("wifi_scan_get_ap_num ret=%d num=%d\n", ret ,ap_num);

    if (ret == ESP_OK && ap_num) {
        uint32_t state;
        /* reallocation of memory must not be disturbed */
        critical_enter_var(state);
        /* allocate memory for APs record list blockwise and fetch them the list */
        if (ap_num > aps_size) {
            if (aps) {
                /* free allocated AP record list memory */
                aps_size = 0;
                free (aps);
            }
            /* allocate new memory */
            aps_size = (ap_num & ~(ESP_NOW_APS_BLOCK_SIZE - 1)) + ESP_NOW_APS_BLOCK_SIZE;
            aps = malloc(sizeof(wifi_ap_record_t) * aps_size);
            ap_num = aps_size;
        }
        critical_exit_var(state);

        ret = esp_wifi_scan_get_ap_records(&ap_num, aps);

        DEBUG("wifi_scan_get_aps ret=%d num=%d\n", ret, ap_num);

        critical_enter_var(state);
        /* iterate over APs records */
        for (uint16_t i = 0; i < ap_num; i++) {

            /* check whether the AP is an ESP_NOW node */
            if (strncmp((char*)aps[i].ssid, ESP_NOW_AP_PREFIX, ESP_NOW_AP_PREFIX_LEN) == 0) {
                /* add the AP as peer */
                _esp_now_add_peer(aps[i].bssid, aps[i].primary, esp_now_params.key);
            }
        }
        critical_exit_var(state);
    }

#if ENABLE_DEBUG
    esp_now_peer_num_t peer_num;
    esp_now_get_peer_num(&peer_num);
    DEBUG("associated peers total=%d, encrypted=%d\n",
          peer_num.total_num, peer_num.encrypt_num);
#endif

    _esp_now_scan_peers_done = true;

    /* set the time for next scan */
    xtimer_set(&_esp_now_scan_peers_timer, esp_now_params.scan_period);

    mutex_unlock(&_esp_now_dev.dev_lock);
}

static void esp_now_scan_peers_start(void)
{
    DEBUG("%s\n", __func__);

    esp_wifi_scan_start(&scan_cfg, false);
}

static void IRAM_ATTR esp_now_scan_peers_timer_cb(void* arg)
{
    DEBUG("%s\n", __func__);

    esp_now_netdev_t* dev = (esp_now_netdev_t*)arg;

    if (dev->netdev.event_callback) {
        dev->scan_event = true;
        dev->netdev.event_callback((netdev_t*)dev, NETDEV_EVENT_ISR);
    }
}

#else

static const uint8_t _esp_now_mac[6] = { 0x82, 0x73, 0x79, 0x84, 0x79, 0x83 }; /* RIOTOS */

#endif /* ESP_NOW_UNICAST */

static IRAM_ATTR void esp_now_recv_cb(const uint8_t *mac, const uint8_t *data, int len)
{
#if ESP_NOW_UNICAST
    if (!_esp_now_scan_peers_done) {
        /* if peers are not scanned, we cannot receive anything */
        return;
    }
#endif

    mutex_lock(&_esp_now_dev.rx_lock);
    critical_enter();

    /*
     * The ring buffer uses a single byte for the pkt length, followed by the mac address,
     * followed by the actual packet data. The MTU for ESP-NOW is 250 bytes, so len will never
     * exceed the limits of a byte as the mac address length is not included.
     */
    if ((int)ringbuffer_get_free(&_esp_now_dev.rx_buf) < 1 + ESP_NOW_ADDR_LEN + len) {
        critical_exit();
        mutex_unlock(&_esp_now_dev.rx_lock);
        DEBUG("%s: buffer full, dropping incoming packet of %d bytes\n", __func__, len);
        return;
    }

#if 0 /* don't printf anything in ISR */
    printf("%s\n", __func__);
    printf("%s: received %d byte from %02x:%02x:%02x:%02x:%02x:%02x\n",
           __func__, len,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    od_hex_dump(data, len, OD_WIDTH_DEFAULT);
#endif

    ringbuffer_add_one(&_esp_now_dev.rx_buf, len);
    ringbuffer_add(&_esp_now_dev.rx_buf, (char*)mac, ESP_NOW_ADDR_LEN);
    ringbuffer_add(&_esp_now_dev.rx_buf, (char*)data, len);

    if (_esp_now_dev.netdev.event_callback) {
        _esp_now_dev.recv_event = true;
        _esp_now_dev.netdev.event_callback((netdev_t*)&_esp_now_dev, NETDEV_EVENT_ISR);
    }

    critical_exit();
    mutex_unlock(&_esp_now_dev.rx_lock);
}

static volatile int _esp_now_sending = 0;

static void IRAM_ATTR esp_now_send_cb(const uint8_t *mac, esp_now_send_status_t status)
{
    DEBUG("%s: sent to %02x:%02x:%02x:%02x:%02x:%02x with status %d\n",
          __func__,
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], status);

    if (_esp_now_sending) {
        _esp_now_sending--;
    }
}

/*
 * Event handler for esp system events.
 */
static esp_err_t IRAM_ATTR _esp_system_event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
        case SYSTEM_EVENT_STA_START:
            DEBUG("%s WiFi started\n", __func__);
            break;
        case SYSTEM_EVENT_SCAN_DONE:
            DEBUG("%s WiFi scan done\n", __func__);
#if ESP_NOW_UNICAST
            esp_now_scan_peers_done();
#endif
            break;
        default:
            break;
    }
    return ESP_OK;
}

/**
 * Default WiFi configuration, overwrite them with your configs
 */
#ifndef CONFIG_WIFI_STA_SSID
#define CONFIG_WIFI_STA_SSID        "RIOT_AP"
#endif
#ifndef CONFIG_WIFI_STA_PASSWORD
#define CONFIG_WIFI_STA_PASSWORD    "ThisistheRIOTporttoESP"
#endif
#ifndef CONFIG_WIFI_STA_CHANNEL
#define CONFIG_WIFI_STA_CHANNEL     0
#endif

#define CONFIG_WIFI_STA_SCAN_METHOD WIFI_ALL_CHANNEL_SCAN
#define CONFIG_WIFI_STA_SORT_METHOD WIFI_CONNECT_AP_BY_SIGNAL
#define CONFIG_WIFI_STA_RSSI        -127
#define CONFIG_WIFI_STA_AUTHMODE    WIFI_AUTH_WPA_WPA2_PSK

#define CONFIG_WIFI_AP_PASSWORD     ESP_NOW_SOFT_AP_PASSPHRASE
#define CONFIG_WIFI_AP_CHANNEL      ESP_NOW_CHANNEL
#define CONFIG_WIFI_AP_AUTH         WIFI_AUTH_WPA_WPA2_PSK
#define CONFIG_WIFI_AP_HIDDEN       false
#define CONFIG_WIFI_AP_BEACON       100
#define CONFIG_WIFI_AP_MAX_CONN     4

extern esp_err_t esp_system_event_add_handler(system_event_cb_t handler,
                                              void *arg);

esp_now_netdev_t *netdev_esp_now_setup(void)
{
    esp_now_netdev_t* dev = &_esp_now_dev;

    DEBUG("%s: %p\n", __func__, dev);

    if (dev->netdev.driver) {
        DEBUG("%s: early returning previously initialized device\n", __func__);
        return dev;
    }

    /*
     * Init the WiFi driver. TODO It is not only required before ESP_NOW is
     * initialized but also before other WiFi functions are used. Once other
     * WiFi functions are realized it has to be moved to a more common place.
     */
    extern portMUX_TYPE g_intr_lock_mux;
    mutex_init(&g_intr_lock_mux);

    ringbuffer_init(&dev->rx_buf, (char*)dev->rx_mem, sizeof(dev->rx_mem));

    esp_system_event_add_handler(_esp_system_event_handler, NULL);

    esp_err_t result;
#if CONFIG_ESP32_WIFI_NVS_ENABLED
    result = nvs_flash_init();
    if (result != ESP_OK) {
        LOG_TAG_ERROR("esp_now",
                      "nfs_flash_init failed with return value %d\n", result);
        return NULL;
    }
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&cfg);
    if (result != ESP_OK) {
        LOG_TAG_ERROR("esp_now",
                      "esp_wifi_init failed with return value %d\n", result);
        return NULL;
    }

#ifdef CONFIG_WIFI_COUNTRY
    /* TODO */
#endif

    /* we use predefined station configuration */
    wifi_config_t wifi_config_sta = {
        .sta = {
            .ssid = CONFIG_WIFI_STA_SSID,
            .password = CONFIG_WIFI_STA_PASSWORD,
            .channel = CONFIG_WIFI_STA_CHANNEL,
            .scan_method = CONFIG_WIFI_STA_SCAN_METHOD,
            .sort_method = CONFIG_WIFI_STA_SORT_METHOD,
            .threshold.rssi = CONFIG_WIFI_STA_RSSI,
            .threshold.authmode = CONFIG_WIFI_STA_AUTHMODE
        }
    };

    /* get SoftAP interface mac address and store it as device addresss */
    esp_read_mac(dev->addr, ESP_MAC_WIFI_SOFTAP);

    /* prepare the ESP_NOW configuration for SoftAP */
    wifi_config_t wifi_config_ap = {};

    strcpy ((char*)wifi_config_ap.ap.password, esp_now_params.softap_pass);
    sprintf((char*)wifi_config_ap.ap.ssid, "%s%02x%02x%02x%02x%02x%02x",
            ESP_NOW_AP_PREFIX,
            dev->addr[0], dev->addr[1], dev->addr[2],
            dev->addr[3], dev->addr[4], dev->addr[5]);
    wifi_config_ap.ap.ssid_len = strlen((char*)wifi_config_ap.ap.ssid);

    wifi_config_ap.ap.channel = esp_now_params.channel;
    wifi_config_ap.ap.authmode = CONFIG_WIFI_AP_AUTH;
    wifi_config_ap.ap.ssid_hidden = CONFIG_WIFI_AP_HIDDEN;
    wifi_config_ap.ap.max_connection = CONFIG_WIFI_AP_MAX_CONN;
    wifi_config_ap.ap.beacon_interval = CONFIG_WIFI_AP_BEACON;

    /* set the WiFi interface to Station + SoftAP mode without DHCP */
    result = esp_wifi_set_mode(WIFI_MODE_STA | WIFI_MODE_AP);
    if (result != ESP_OK) {
        LOG_TAG_ERROR("esp_now",
                      "esp_wifi_set_mode failed with return value %d\n",
                      result);
        return NULL;
    }

    /* set the Station and SoftAP configuration */
    result = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config_sta);
    if (result != ESP_OK) {
        LOG_TAG_ERROR("esp_now", "esp_wifi_set_config station failed with "
                      "return value %d\n", result);
        return NULL;
    }
    result = esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config_ap);
    if (result != ESP_OK) {
        LOG_TAG_ERROR("esp_now",
                      "esp_wifi_set_mode softap failed with return value %d\n",
                      result);
        return NULL;
    }

    /* start the WiFi driver */
    result = esp_wifi_start();
    if (result != ESP_OK) {
        LOG_TAG_ERROR("esp_now",
                      "esp_wifi_start failed with return value %d\n", result);
        return NULL;
    }

#if !ESP_NOW_UNICAST
    /* all ESP-NOW nodes get the shared mac address on their station interface */
    esp_wifi_set_mac(ESP_IF_WIFI_STA, (uint8_t*)_esp_now_mac);
#endif

    /* set the netdev driver */
    dev->netdev.driver = &_esp_now_driver;

    /* initialize netdev data structure */
    dev->recv_event = false;
    dev->scan_event = false;

    mutex_init(&dev->dev_lock);
    mutex_init(&dev->rx_lock);

    /* initialize ESP-NOW and register callback functions */
    result = esp_now_init();
    if (result != ESP_OK) {
        LOG_TAG_ERROR("esp_now", "esp_now_init failed with return value %d\n",
                      result);
        return NULL;
    }
    esp_now_register_send_cb(esp_now_send_cb);
    esp_now_register_recv_cb(esp_now_recv_cb);

#if ESP_NOW_UNICAST
    /* timer for peer scan initialization */
    _esp_now_scan_peers_done = false;
    _esp_now_scan_peers_timer.callback = &esp_now_scan_peers_timer_cb;
    _esp_now_scan_peers_timer.arg = dev;

    /* execute the first scan */
    esp_now_scan_peers_start();

#else /* ESP_NOW_UNICAST */
    bool res = _esp_now_add_peer(_esp_now_mac, esp_now_params.channel,
                                               esp_now_params.key);
    DEBUG("%s: multicast node add %s\n", __func__, res ? "success" : "error");
#endif /* ESP_NOW_UNICAST */

    return dev;
}

static int _init(netdev_t *netdev)
{
    DEBUG("%s: %p\n", __func__, netdev);

#ifdef MODULE_NETSTATS_L2
    memset(&netdev->stats, 0x00, sizeof(netstats_t));
#endif

    return 0;
}

static int _send(netdev_t *netdev, const iolist_t *iolist)
{
#if ESP_NOW_UNICAST
    if (!_esp_now_scan_peers_done) {
        return -ENODEV;
    }
#endif

    DEBUG("%s: %p %p\n", __func__, netdev, iolist);

    CHECK_PARAM_RET(netdev != NULL, -ENODEV);
    CHECK_PARAM_RET(iolist != NULL && iolist->iol_len == ESP_NOW_ADDR_LEN, -EINVAL);
    CHECK_PARAM_RET(iolist->iol_next != NULL, -EINVAL);

    esp_now_netdev_t *dev = (esp_now_netdev_t*)netdev;

    mutex_lock(&dev->dev_lock);

#if ESP_NOW_UNICAST
    uint8_t* _esp_now_dst = NULL;

    for (uint8_t i = 0; i < ESP_NOW_ADDR_LEN; i++) {
        if (((uint8_t*)iolist->iol_base)[i] != 0xff) {
            _esp_now_dst = iolist->iol_base;
            break;
        }
    }
#else
   const uint8_t* _esp_now_dst = _esp_now_mac;
#endif
    iolist = iolist->iol_next;

    uint8_t *data_pos = dev->tx_mem;
    uint8_t data_len = 0;

    while (iolist) {
        if (((int)data_len + iolist->iol_len) > ESP_NOW_MAX_SIZE_RAW) {
            DEBUG("%s: payload length exceeds maximum(%u>%u)\n", __func__,
                  data_len + iolist->iol_len, ESP_NOW_MAX_SIZE_RAW);
            return -EBADMSG;
        }

        memcpy(data_pos, iolist->iol_base, iolist->iol_len);
        data_pos += iolist->iol_len;
        data_len += iolist->iol_len;

        iolist = iolist->iol_next;
    }

    DEBUG("%s: send %u byte\n", __func__, (unsigned)data_len);
#if defined(MODULE_OD) && ENABLE_DEBUG
    od_hex_dump(dev->tx_mem, data_len, OD_WIDTH_DEFAULT);
#endif

    if (_esp_now_dst) {
        DEBUG("%s: send to esp_now addr %02x:%02x:%02x:%02x:%02x:%02x\n", __func__,
              _esp_now_dst[0], _esp_now_dst[1], _esp_now_dst[2],
              _esp_now_dst[3], _esp_now_dst[4], _esp_now_dst[5]);
    } else {
        DEBUG("%s: send esp_now broadcast\n", __func__);
    }

    _esp_now_sending = 1;

    /* send the packet to the peer(s) mac address */
    if (esp_now_send(_esp_now_dst, dev->tx_mem, data_len) == ESP_OK) {
        while (_esp_now_sending > 0) {
            thread_yield_higher();
        }

#ifdef MODULE_NETSTATS_L2
        netdev->stats.tx_bytes += data_len;
        netdev->event_callback(netdev, NETDEV_EVENT_TX_COMPLETE);
#endif

        mutex_unlock(&dev->dev_lock);
        return data_len;
    } else {
        _esp_now_sending = 0;

#ifdef MODULE_NETSTATS_L2
        netdev->stats.tx_failed++;
#endif
    }

    mutex_unlock(&dev->dev_lock);
    return -EIO;
}

static int _recv(netdev_t *netdev, void *buf, size_t len, void *info)
{
    DEBUG("%s: %p %p %u %p\n", __func__, netdev, buf, len, info);

    CHECK_PARAM_RET(netdev != NULL, -ENODEV);

    esp_now_netdev_t* dev = (esp_now_netdev_t*)netdev;

    mutex_lock(&dev->rx_lock);

    uint16_t size = ringbuffer_empty(&dev->rx_buf)
        ? 0
        : (ringbuffer_peek_one(&dev->rx_buf) + ESP_NOW_ADDR_LEN);

    if (size && dev->rx_buf.avail < size) {
        /* this should never happen unless this very function messes up */
        mutex_unlock(&dev->rx_lock);
        return -EIO;
    }

    if (!buf && !len) {
        /* return the size without dropping received data */
        mutex_unlock(&dev->rx_lock);
        return size;
    }

    if (!buf && len) {
        /* return the size and drop received data */
        if (size) {
            ringbuffer_remove(&dev->rx_buf, 1 + size);
        }
        mutex_unlock(&dev->rx_lock);
        return size;
    }

    if (buf && len && !size) {
        mutex_unlock(&dev->rx_lock);
        return 0;
    }

    if (buf && len && size) {
        if (size > len) {
            DEBUG("[esp_now] No space in receive buffers\n");
            mutex_unlock(&dev->rx_lock);
            return -ENOBUFS;
        }

        /* remove already peeked size byte */
        ringbuffer_remove(&dev->rx_buf, 1);
        ringbuffer_get(&dev->rx_buf, buf, size);

        uint8_t *mac = buf;

        DEBUG("%s: received %d byte from %02x:%02x:%02x:%02x:%02x:%02x\n",
              __func__, size - ESP_NOW_ADDR_LEN,
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#if defined(MODULE_OD) && ENABLE_DEBUG
        od_hex_dump(buf + ESP_NOW_ADDR_LEN, size - ESP_NOW_ADDR_LEN, OD_WIDTH_DEFAULT);
#endif

#if ESP_NOW_UNICAST
        if (esp_now_is_peer_exist(mac) <= 0) {
            _esp_now_add_peer(mac, esp_now_params.channel, esp_now_params.key);
        }
#endif

#ifdef MODULE_NETSTATS_L2
        netdev->stats.rx_count++;
        netdev->stats.rx_bytes += size;
#endif

        mutex_unlock(&dev->rx_lock);
        return size;
    }

    mutex_unlock(&dev->rx_lock);
    return -EINVAL;
}

static inline int _get_iid(esp_now_netdev_t *dev, eui64_t *value, size_t max_len)
{
    CHECK_PARAM_RET(max_len >= sizeof(eui64_t), -EOVERFLOW);

    /* interface id according to */
    /* https://tools.ietf.org/html/rfc4291#section-2.5.1 */
    value->uint8[0] = dev->addr[0] ^ 0x02; /* invert bit1 */
    value->uint8[1] = dev->addr[1];
    value->uint8[2] = dev->addr[2];
    value->uint8[3] = 0xff;
    value->uint8[4] = 0xfe;
    value->uint8[5] = dev->addr[3];
    value->uint8[6] = dev->addr[4];
    value->uint8[7] = dev->addr[5];

    return sizeof(eui64_t);
}

static int _get(netdev_t *netdev, netopt_t opt, void *val, size_t max_len)
{
    DEBUG("%s: %s %p %p %u\n", __func__, netopt2str(opt), netdev, val, max_len);

    CHECK_PARAM_RET(netdev != NULL, -ENODEV);
    CHECK_PARAM_RET(val != NULL, -EINVAL);

    esp_now_netdev_t *dev = (esp_now_netdev_t*)netdev;
    int res = -ENOTSUP;

    switch (opt) {

        case NETOPT_DEVICE_TYPE:
            CHECK_PARAM_RET(max_len >= sizeof(uint16_t), -EOVERFLOW);
            *((uint16_t *)val) = NETDEV_TYPE_ESP_NOW;
            res = sizeof(uint16_t);
            break;

#ifdef MODULE_GNRC
        case NETOPT_PROTO:
            CHECK_PARAM_RET(max_len == sizeof(gnrc_nettype_t), -EOVERFLOW);
            *((gnrc_nettype_t *)val) = dev->proto;
            res = sizeof(gnrc_nettype_t);
            break;
#endif

        case NETOPT_MAX_PACKET_SIZE:
            CHECK_PARAM_RET(max_len >= sizeof(uint16_t), -EOVERFLOW);
            *((uint16_t *)val) = ESP_NOW_MAX_SIZE;
            res = sizeof(uint16_t);
            break;

        case NETOPT_ADDR_LEN:
        case NETOPT_SRC_LEN:
            CHECK_PARAM_RET(max_len >= sizeof(uint16_t), -EOVERFLOW);
            *((uint16_t *)val) = sizeof(dev->addr);
            res = sizeof(uint16_t);
            break;

        case NETOPT_ADDRESS:
            CHECK_PARAM_RET(max_len >= sizeof(dev->addr), -EOVERFLOW);
            memcpy(val, dev->addr, sizeof(dev->addr));
            res = sizeof(dev->addr);
            break;

        case NETOPT_IPV6_IID:
            res = _get_iid(dev, val, max_len);
            break;

#ifdef MODULE_NETSTATS_L2
        case NETOPT_STATS:
            CHECK_PARAM_RET(max_len == sizeof(uintptr_t), -EOVERFLOW);
            *((netstats_t **)val) = &netdev->stats;
            res = sizeof(uintptr_t);
            break;
#endif

        default:
            DEBUG("%s: %s not supported\n", __func__, netopt2str(opt));
            break;
    }
    return res;
}

static int _set(netdev_t *netdev, netopt_t opt, const void *val, size_t max_len)
{
    DEBUG("%s: %s %p %p %u\n", __func__, netopt2str(opt), netdev, val, max_len);

    CHECK_PARAM_RET(netdev != NULL, -ENODEV);
    CHECK_PARAM_RET(val != NULL, -EINVAL);

    esp_now_netdev_t *dev = (esp_now_netdev_t *) netdev;
    int res = -ENOTSUP;

    switch (opt) {

#ifdef MODULE_GNRC
        case NETOPT_PROTO:
            CHECK_PARAM_RET(max_len == sizeof(gnrc_nettype_t), -EOVERFLOW);
            dev->proto = *((gnrc_nettype_t *)val);
            res = sizeof(gnrc_nettype_t);
            break;
#endif

        case NETOPT_ADDRESS:
            CHECK_PARAM_RET(max_len >= sizeof(dev->addr), -EOVERFLOW);
            memcpy(dev->addr, val, sizeof(dev->addr));
            res = sizeof(dev->addr);
            break;

        default:
            DEBUG("%s: %s not supported\n", __func__, netopt2str(opt));
            break;
    }
    return res;
}

static void _isr(netdev_t *netdev)
{
    DEBUG("%s: %p\n", __func__, netdev);

    CHECK_PARAM(netdev != NULL);

    esp_now_netdev_t *dev = (esp_now_netdev_t*)netdev;

    if (dev->recv_event) {
        dev->recv_event = false;
        dev->netdev.event_callback(netdev, NETDEV_EVENT_RX_COMPLETE);
    }
    else if (dev->scan_event) {
        dev->scan_event = false;
        esp_now_scan_peers_start();
    }
    return;
}

static const netdev_driver_t _esp_now_driver =
{
    .send = _send,
    .recv = _recv,
    .init = _init,
    .isr = _isr,
    .get = _get,
    .set = _set,
};

/** @} */
