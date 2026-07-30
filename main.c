#include <stdint.h>
#include <string.h>
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_sdh_soc.h"
#include "ble.h"
#include "ble_gap.h"
#include "app_error.h"
#include "app_uart.h"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "boards.h"

#define APP_BLE_CONN_CFG_TAG    1
#define SCAN_INTERVAL            0x00A0   // 100 ms
#define SCAN_WINDOW              0x0050   // 50 ms
#define SCAN_DURATION             0       // 0 = scan forever

#define UART_TX_BUF_SIZE 256
#define UART_RX_BUF_SIZE 256

// ---- TEST MODE: filter by UUID (your iPhone simulator's MAC rotates,
// so we match on the fixed UUID you set instead, for now) ----
// UUID: 13EA2EE5-F292-4B6A-B83C-14A3280B4EEC
static const uint8_t TEST_UUID[16] = {
    0x13, 0xEA, 0x2E, 0xE5, 0xF2, 0x92, 0x4B, 0x6A,
    0xB8, 0x3C, 0x14, 0xA3, 0x28, 0x0B, 0x4E, 0xEC
};

// Searches the raw advertisement bytes for our test UUID anywhere within it.
static bool contains_test_uuid(uint8_t const * data, uint16_t len)
{
    if (len < 16) return false;
    for (uint16_t i = 0; i <= len - 16; i++)
    {
        if (memcmp(&data[i], TEST_UUID, 16) == 0)
        {
            return true;
        }
    }
    return false;
}

static void uart_error_handle(app_uart_evt_t * p_event)
{
    // Minimal handler - just ignore UART errors for now
}

static void uart_init(void)
{
    uint32_t err_code;
    const app_uart_comm_params_t comm_params =
    {
        8,  // RX pin - not actually used since we only transmit, but must be a valid pin number
        9,  // TX pin - P0.09, confirmed as the board's actual labeled "UART TX" pin
        0, // RTS pin (unused)
        0, // CTS pin (unused)
        APP_UART_FLOW_CONTROL_DISABLED,
        false,
        UART_BAUDRATE_BAUDRATE_Baud115200
    };

    APP_UART_FIFO_INIT(&comm_params,
                        UART_RX_BUF_SIZE,
                        UART_TX_BUF_SIZE,
                        uart_error_handle,
                        APP_IRQ_PRIORITY_LOWEST,
                        err_code);
    APP_ERROR_CHECK(err_code);
}

// Sends one character out the physical UART pins, to the Mega
static void uart_put_char(char c)
{
    while (app_uart_put((uint8_t)c) != NRF_SUCCESS) {}
}

static void uart_print(const char * str)
{
    while (*str) { uart_put_char(*str++); }
}

static void uart_print_hex_byte(uint8_t b)
{
    const char hex[] = "0123456789ABCDEF";
    uart_put_char(hex[(b >> 4) & 0xF]);
    uart_put_char(hex[b & 0xF]);
}

static uint8_t m_scan_buffer_data[BLE_GAP_SCAN_BUFFER_MAX];
static ble_data_t m_scan_buffer =
{
    .p_data = m_scan_buffer_data,
    .len    = BLE_GAP_SCAN_BUFFER_MAX
};

static ble_gap_scan_params_t m_scan_params =
{
    .active        = 0,
    .interval      = SCAN_INTERVAL,
    .window        = SCAN_WINDOW,
    .timeout       = SCAN_DURATION,
    .scan_phys     = BLE_GAP_PHY_1MBPS,
};

static void log_init(void)
{
    ret_code_t err_code = NRF_LOG_INIT(NULL);
    APP_ERROR_CHECK(err_code);
    NRF_LOG_DEFAULT_BACKENDS_INIT();
}

// Sends ONLY matching (UUID-matching, for test purposes) results over
// the physical UART, to the Mega. Format: MAC,RAWHEXDATA\r\n
// ---- THROTTLING NOTE ----
// This throttle exists ONLY because the iPhone simulator broadcasts
// continuously and rapidly for testing purposes.
// The REAL alerTire sensors broadcast only on pressure/temperature
// CHANGE (value-change advertising, per their own documentation) -
// meaning real updates are already naturally rare and meaningful.
// When switching to real sensors, set this to 1 (forward every match)
// so you never miss a genuine, infrequent update.
#define FORWARD_EVERY_NTH_MATCH 10   // TEST MODE value - use 1 for real sensors
static uint32_t match_counter = 0;

static void uart_forward_if_known(ble_gap_evt_adv_report_t const * p_adv)
{
    if (!contains_test_uuid(p_adv->data.p_data, p_adv->data.len)) return;

    match_counter++;
    if (match_counter % FORWARD_EVERY_NTH_MATCH != 0) return;

    NRF_LOG_INFO("MATCH FOUND! Forwarding to UART.");
    NRF_LOG_FLUSH();

    for (int i = 5; i >= 0; i--)
    {
        uart_print_hex_byte(p_adv->peer_addr.addr[i]);
        if (i > 0) uart_put_char(':');
    }
    uart_put_char(',');

    for (uint16_t i = 0; i < p_adv->data.len; i++)
    {
        uart_print_hex_byte(p_adv->data.p_data[i]);
    }
    uart_put_char('\r');
    uart_put_char('\n');
}

static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context)
{
    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_ADV_REPORT:
        {
            uart_forward_if_known(&p_ble_evt->evt.gap_evt.params.adv_report);

            ret_code_t err_code = sd_ble_gap_scan_start(NULL, &m_scan_buffer);
            if (err_code != NRF_SUCCESS && err_code != NRF_ERROR_INVALID_STATE)
            {
                APP_ERROR_CHECK(err_code);
            }
            break;
        }
        default:
            break;
    }
}

NRF_SDH_BLE_OBSERVER(m_ble_observer, 3, ble_evt_handler, NULL);

static void ble_stack_init(void)
{
    ret_code_t err_code = nrf_sdh_enable_request();
    APP_ERROR_CHECK(err_code);

    uint32_t ram_start = 0;
    err_code = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start);
    APP_ERROR_CHECK(err_code);

    err_code = nrf_sdh_ble_enable(&ram_start);
    APP_ERROR_CHECK(err_code);
}

static void scan_start(void)
{
    ret_code_t err_code = sd_ble_gap_scan_start(&m_scan_params, &m_scan_buffer);
    APP_ERROR_CHECK(err_code);
    NRF_LOG_INFO("Scanning started...");
    NRF_LOG_FLUSH();
}

int main(void)
{
    log_init();
    NRF_LOG_INFO("Step 1: log_init OK");
    NRF_LOG_FLUSH();

    uart_init();
    uart_print("nRF52810 TPMS scanner booting...\r\n");

    ble_stack_init();
    NRF_LOG_INFO("Step 2: SoftDevice enabled OK");
    NRF_LOG_FLUSH();

    scan_start();
    NRF_LOG_INFO("Observer-only BLE scanner running.");
    NRF_LOG_FLUSH();

    for (;;)
    {
        NRF_LOG_FLUSH();
        __WFE();
    }
}