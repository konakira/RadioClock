/**
 * RadioClock for ESP32-C6 + esp-matter
 *
 * 時刻取得優先順位:
 *   1. Matter Time Synchronization Cluster (SetUTCTime from controller)
 *   2. NTP フォールバック (pool.ntp.org / ntp.nict.jp) — 120s タイムアウト後
 *   3. 90秒 Deep Sleep して再試行
 */

#include <esp_log.h>
#include <driver/gpio.h>
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_timer.h>
#include <esp_sleep.h>
#include <driver/ledc.h>
#include <esp_sntp.h>
#include <nvs_flash.h>
#include <platform/CHIPDeviceLayer.h>
#include <app/server/Server.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <iot_button.h>
#include <button_gpio.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "radio_clock";

// ===== JJY 設定 =====
#ifndef JJY_GPIO
#define JJY_GPIO GPIO_NUM_17
#endif
#ifndef JJY_FREQ
#define JJY_FREQ 40000              // 40kHz（東日本）/ 60000（西日本）
#endif
#define LEDC_TIMER_NUM  LEDC_TIMER_0
#define LEDC_CHAN_NUM   LEDC_CHANNEL_0

// ===== タイムゾーン・スケジュール =====
#define JST_OFFSET_S        (9 * 3600)
#define WORKING_TIME_S      (30 * 60)
#define NTP_TRIGGER_S       30          // WiFiイベント取りこぼし時のNTPフォールバック待機
#define NTP_WAIT_S          30          // NTP 同期待ち最大: 30秒
#define WIFI_RETRY_US       90000000ULL // 再試行まで: 90秒

#define HOUR_MIN(h, m)  ((uint32_t)((h) * 60 + (m)))
static const uint32_t k_schedule[] = {
    HOUR_MIN(2, 0), HOUR_MIN(4, 0), HOUR_MIN(5, 0)
};
static const size_t k_schedule_count = sizeof(k_schedule) / sizeof(k_schedule[0]);

static const time_t kMinValidTime = 1577836800L; // 2020-01-01 00:00:00 UTC

// ===== Matter =====
static uint16_t s_endpoint_id   = 0;
static bool     s_matter_started = false;

// ===== RTC メモリ（Deep Sleep 後も保持） =====
RTC_DATA_ATTR static time_t s_target_wakeup_time = 0;
RTC_DATA_ATTR static bool   s_by_timer = false;

// ===== LED =====
#define LED_GPIO GPIO_NUM_15  // XIAO ESP32C6 オンボードLED
static bool s_led_state = false;

// ===== ボタン =====
#define FACTORY_RESET_PRESS_MS 5000
static button_handle_t s_boot_btn = NULL;

static void factory_reset_cb(void *, void *)
{
    ESP_LOGW(TAG, "Boot button 5s long press — factory reset");
    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() {
        chip::Server::GetInstance().ScheduleFactoryReset();
    });
}

// ===== タイマー・状態変数 =====
static esp_timer_handle_t s_sec_timer     = NULL;
static esp_timer_handle_t s_bit_off_timer = NULL;
static int64_t  s_timeout_start_us = 0;
static int64_t  s_work_start_us    = 0;

static volatile bool     s_transmitting   = false;
static volatile bool     s_going_to_sleep = false;
static volatile uint64_t s_sleep_us       = 0;
static volatile bool     s_ntp_needed     = false; // NTP フォールバック要求

// ===== JJY 信号制御 =====

static void jjy_pin_set(bool on)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHAN_NUM, on ? 1 : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHAN_NUM);
}

static void bit_off_cb(void *)
{
    jjy_pin_set(false);
}

static void send_bit_high_ms(uint32_t ms)
{
    jjy_pin_set(true);
    esp_timer_stop(s_bit_off_timer);
    esp_timer_start_once(s_bit_off_timer, (uint64_t)ms * 1000);
}

static void sendBit(unsigned b)  { send_bit_high_ms(b ? 500 : 800); }
static void sendMarker()         { send_bit_high_ms(200); }

class RadioClockData {
public:
  void setTime(time_t t);
  void incrementMin();
  void sendData(time_t sec);
  void sendData2(time_t sec);

private:
  unsigned _year[2]; // BCDで記録。添え字が小さい方が下のケタ。
  unsigned _yday[3];
  unsigned _hour[2];
  unsigned _min[2];
  unsigned _dayOfWeek;
  unsigned _minParity, _hourParity;
  unsigned calcParity(unsigned d, unsigned b);
} rcd;

unsigned RadioClockData::calcParity(unsigned d, unsigned b)
{
  unsigned retval = 0;

  while (0 < b--) {
    retval ^= d;
    d >>= 1;
  }
  return retval & 1;
}

void storeBCD(unsigned d, unsigned *buf, unsigned digits)
{
  while (0 < digits--) {
    *buf++ = d % 10;
    d /= 10;
  }
}

// 掛け算や割り算など、時間がかかるかもしれない計算はここでやってしまう
void RadioClockData::setTime(time_t t)
{
  struct tm timeInfo;

  gmtime_r(&t, &timeInfo);
  storeBCD((unsigned)(timeInfo.tm_year % 100), _year, 2);

  storeBCD((unsigned)(timeInfo.tm_yday + 1), _yday, 3);

  storeBCD((unsigned)timeInfo.tm_hour, _hour, 2);
  storeBCD((unsigned)timeInfo.tm_min,  _min,  2);

  _minParity = (calcParity(_min[1], 3) ^ calcParity(_min[0], 4));
  _hourParity = (calcParity(_hour[1], 2) ^ calcParity(_hour[0], 4));

  _dayOfWeek = (unsigned)timeInfo.tm_wday;
}

void RadioClockData::incrementMin() {
  _min[0]++; // 1分インクリメント
  if (9 < _min[0]) { // 1ケタ目が10分以上になったらBCDの2ケタ目をインクリメント
    _min[0] = 0;
    _min[1]++;
    if (5 < _min[1]) { // 分の2ケタ目が6以上になったら時間をインクリメント
      _min[1] = 0;
      unsigned h = _hour[1] * 10 + _hour[0]; // 時間はBCDのケタごとに判定するのが面倒なので一度戻す
      h++;
      if (23 < h) { // 24時以上になったら日付をインクリメント
        _hour[0] = _hour[1] = 0;
        unsigned y = _yday[2] * 100 + _yday[1] * 10 + _yday[0]; // 日付もケタごとに計算するのは面倒なので一度戻す
        y++;
        if (365 < y) { // 365日を超えたらうるう年の計算が面倒なのでnow()拾ってセットしなおす
          setTime(time(NULL) + JST_OFFSET_S);
          return;
        }
        storeBCD(y, _yday, 3);

        if (6 < ++_dayOfWeek) { // 曜日を進める
          _dayOfWeek = 0;
        }
      }
      storeBCD(h, _hour, 2);
    }
  }
  _hourParity = (calcParity(_hour[1], 2) ^ calcParity(_hour[0], 4));
  _minParity = (calcParity(_min[1], 3) ^ calcParity(_min[0], 4));
}

void RadioClockData::sendData(time_t sec)
{
  switch (sec) {
  case 0:
  case 9:
  case 19:
  case 29:
  case 39:
  case 49:
  case 59:
    sendMarker();
    break;

  case 4:
  case 10:
  case 11:
  case 14:
  case 20:
  case 21:
  case 24:
  case 34:
  case 35:
  case 38: // SU1、予備ビット
  case 40: // SU2
  case 53: // LS1、うるう秒
  case 54: // LS2
  case 55:
  case 56:
  case 57:
  case 58:
    sendBit(0);
    break;

  case 1:
    sendBit((_min[1] & 4) >> 2);
    break;

  case 2:
    sendBit((_min[1] & 2) >> 1);
    break;

  case 3:
    sendBit(_min[1] & 1);
    break;

  case 5:
    sendBit((_min[0] & 8) >> 3);
    break;

  case 6:
    sendBit((_min[0] & 4) >> 2);
    break;

  case 7:
    sendBit((_min[0] & 2) >> 1);
    break;

  case 8:
    sendBit(_min[0] & 1);
    break;

  case 12:
    sendBit((_hour[1] & 2) >> 1);
    break;

  case 13:
    sendBit(_hour[1] & 1);
    break;

  case 15:
    sendBit((_hour[0] & 8) >> 3);
    break;

  case 16:
    sendBit((_hour[0] & 4) >> 2);
    break;

  case 17:
    sendBit((_hour[0] & 2) >> 1);
    break;

  case 18:
    sendBit(_hour[0] & 1);
    break;

  case 22:
    sendBit((_yday[2] & 2) >> 1);
    break;

  case 23:
    sendBit(_yday[2] & 1);
    break;

  case 25:
    sendBit((_yday[1] & 8) >> 3);
    break;

  case 26:
    sendBit((_yday[1] & 4) >> 2);
    break;

  case 27:
    sendBit((_yday[1] & 2) >> 1);
    break;

  case 28:
    sendBit(_yday[1] & 1);
    break;

  case 30:
    sendBit((_yday[0] & 8) >> 3);
    break;

  case 31:
    sendBit((_yday[0] & 4) >> 2);
    break;

  case 32:
    sendBit((_yday[0] & 2) >> 1);
    break;

  case 33:
    sendBit(_yday[0] & 1);
    break;

  case 36:
    sendBit(_hourParity);
    break;

  case 37:
    sendBit(_minParity);
    break;

  case 41:
    sendBit((_year[1] & 8) >> 3);
    break;

  case 42:
    sendBit((_year[1] & 4) >> 2);
    break;

  case 43:
    sendBit((_year[1] & 2) >> 1);
    break;

  case 44:
    sendBit(_year[1] & 1);
    break;

  case 45:
    sendBit((_year[0] & 8) >> 3);
    break;

  case 46:
    sendBit((_year[0] & 4) >> 2);
    break;

  case 47:
    sendBit((_year[0] & 2) >> 1);
    break;

  case 48:
    sendBit(_year[0] & 1);
    break;

  case 50:
    sendBit((_dayOfWeek & 4) >> 2);
    break;

  case 51:
    sendBit((_dayOfWeek & 2) >> 1);
    break;

  case 52:
    sendBit(_dayOfWeek & 1);
    break;
  }
}

void RadioClockData::sendData2(time_t sec) {
  switch (sec) {
  case 40:
  case 41:
  case 42:
  case 43:
  case 44:
  case 45:
  case 46:
  case 47:
  case 48:
  case 49:
  case 50:
  case 51:
  case 52:
    sendBit(0);
    break;

  default:
    sendData(sec);
    break;
  }
}

// ===== スケジュール計算 =====

static uint32_t calcSleepMinutes(int jst_hour, int jst_min)
{
    uint32_t in_min = (uint32_t)(jst_hour * 60 + jst_min);
    for (size_t i = 0; i < k_schedule_count; i++) {
        if (in_min < k_schedule[i]) return k_schedule[i] - in_min;
    }
    return k_schedule[0] + 24 * 60 - in_min;
}

// ===== NTP フォールバック =====
// メインタスクから呼ぶ（ブロッキング、最大 NTP_WAIT_S 秒）

static bool try_ntp_sync()
{
    ESP_LOGI(TAG, "Matter Time Sync timeout. Trying NTP fallback...");

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "ntp.nict.jp");
    esp_sntp_init();

    for (int i = 0; i < NTP_WAIT_S; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (time(NULL) >= kMinValidTime) {
            time_t t = time(NULL);
            struct tm utc_tm;
            gmtime_r(&t, &utc_tm);
            ESP_LOGI(TAG, "NTP synced: UTC %04d-%02d-%02d %02d:%02d:%02d",
                     1900 + utc_tm.tm_year, 1 + utc_tm.tm_mon, utc_tm.tm_mday,
                     utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec);
            esp_sntp_stop();
            return true;
        }
    }

    esp_sntp_stop();
    ESP_LOGW(TAG, "NTP sync failed");
    return false;
}

// ===== Matter OnOff 属性更新 =====

static void update_onoff(bool state)
{
    if (!s_matter_started) return;
    bool     b     = state;
    uint16_t ep_id = s_endpoint_id;
    chip::DeviceLayer::SystemLayer().ScheduleLambda([b, ep_id]() {
        esp_matter_attr_val_t val = esp_matter_bool(b);
        // report() はコールバックをスキップするため内部更新に使用
        esp_matter::attribute::report(
            ep_id,
            chip::app::Clusters::OnOff::Id,
            chip::app::Clusters::OnOff::Attributes::OnOff::Id,
            &val);
    });
}

// ===== 1秒タイマーコールバック =====

static bool s_time_initialized = false;
static int  s_jjy_sec = 0;
static int  s_jjy_min = 0;

static void one_second_tick(void *)
{
    if (s_going_to_sleep || s_ntp_needed) return;

    time_t t = time(NULL);

    // 未同期: WiFiイベントで即時トリガーされるが、取りこぼし時は30秒でフォールバック
    if (t < kMinValidTime) {
        int64_t elapsed = (esp_timer_get_time() - s_timeout_start_us) / 1000000LL;
        if (elapsed > NTP_TRIGGER_S && !s_ntp_needed) {
            ESP_LOGW(TAG, "NTP fallback triggered (%llds)", (long long)elapsed);
            s_ntp_needed = true;
        }
        return;
    }

    // 初回の有効時刻: 発振開始
    if (!s_time_initialized) {
        s_time_initialized = true;

        if (s_by_timer && s_target_wakeup_time > 0 && t + 60 < s_target_wakeup_time) {
            uint64_t remain_s = (uint64_t)(s_target_wakeup_time - t);
            ESP_LOGI(TAG, "Early wakeup, re-sleep %llu s", remain_s);
            s_sleep_us = remain_s * 1000000ULL;
            s_going_to_sleep = true;
            return;
        }

        time_t jst_t = t + JST_OFFSET_S;
        struct tm jst_tm;
        gmtime_r(&jst_t, &jst_tm);
        s_jjy_sec = jst_tm.tm_sec;
        s_jjy_min = jst_tm.tm_min;
        rcd.setTime(jst_t + 1);

        s_work_start_us = esp_timer_get_time();
        s_transmitting  = true;
        s_led_state     = false; // 点滅開始前に状態リセット（次tickから点滅）
        update_onoff(true);
        ESP_LOGI(TAG, "JJY started: JST %04d-%02d-%02d %02d:%02d:%02d",
                 1900 + jst_tm.tm_year, 1 + jst_tm.tm_mon, jst_tm.tm_mday,
                 jst_tm.tm_hour, s_jjy_min, s_jjy_sec);
        return;
    }

    if (!s_transmitting) return;

    // 30分経過 → スリープ
    int64_t elapsed_s = (esp_timer_get_time() - s_work_start_us) / 1000000LL;
    if (elapsed_s >= WORKING_TIME_S) {
        s_transmitting = false;
        jjy_pin_set(false);

        time_t jst_now = time(NULL) + JST_OFFSET_S;
        struct tm jst_tm;
        gmtime_r(&jst_now, &jst_tm);
        uint32_t sleep_min = calcSleepMinutes(jst_tm.tm_hour, jst_tm.tm_min);
        uint64_t sleep_s   = (uint64_t)sleep_min * 60 - (uint64_t)jst_tm.tm_sec;
        s_target_wakeup_time = time(NULL) + (time_t)sleep_s;
        s_sleep_us = sleep_s * 1000000ULL;
        ESP_LOGI(TAG, "Work done, next wake in %lu min", sleep_min);
        s_going_to_sleep = true;
        return;
    }

    // LED 点滅（発振中 1秒ごとにトグル）
    s_led_state = !s_led_state;
    gpio_set_level(LED_GPIO, s_led_state ? 0 : 1); // アクティブLOW: 0=点灯, 1=消灯

    // JJY ビット送信
    if (s_jjy_min == 15 || s_jjy_min == 45) {
        rcd.sendData2(s_jjy_sec);
    } else {
        rcd.sendData(s_jjy_sec);
    }

    if (++s_jjy_sec > 59) {
        s_jjy_sec = 0;
        rcd.incrementMin();
        if (++s_jjy_min > 59) s_jjy_min = 0;
    }
}

// ===== Matter コールバック =====

static esp_err_t app_attribute_update_cb(
    esp_matter::attribute::callback_type_t type,
    uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
    esp_matter_attr_val_t *val, void *)
{
    if (type == esp_matter::attribute::PRE_UPDATE &&
        endpoint_id == s_endpoint_id &&
        cluster_id  == chip::app::Clusters::OnOff::Id &&
        attribute_id == chip::app::Clusters::OnOff::Attributes::OnOff::Id) {
        // OnOff は内部制御のみ。コントローラからの書き込みはリジェクト
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Matter commissioning complete");
        break;
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "WiFi connected");
        if (time(NULL) < kMinValidTime) {
            s_ntp_needed = true; // WiFi接続直後にNTPを試みる
        }
        break;
    default:
        break;
    }
}

// ===== カスタム DeviceInstanceInfoProvider =====
// VendorName / ProductName だけ上書きし、それ以外は元のプロバイダに委譲する

class RadioClockDeviceInfoProvider : public chip::DeviceLayer::DeviceInstanceInfoProvider
{
public:
    void setDelegate(chip::DeviceLayer::DeviceInstanceInfoProvider * d) { mDelegate = d; }

    CHIP_ERROR GetVendorName(char * buf, size_t bufSize) override
    {
        snprintf(buf, bufSize, "Akira Kon");
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetProductName(char * buf, size_t bufSize) override
    {
        snprintf(buf, bufSize, "Radio Clock");
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetVendorId(uint16_t & v) override               { return mDelegate->GetVendorId(v); }
    CHIP_ERROR GetProductId(uint16_t & v) override              { return mDelegate->GetProductId(v); }
    CHIP_ERROR GetPartNumber(char * b, size_t s) override       { return mDelegate->GetPartNumber(b, s); }
    CHIP_ERROR GetProductURL(char * b, size_t s) override       { return mDelegate->GetProductURL(b, s); }
    CHIP_ERROR GetProductLabel(char * b, size_t s) override     { return mDelegate->GetProductLabel(b, s); }
    CHIP_ERROR GetSerialNumber(char * b, size_t s) override     { return mDelegate->GetSerialNumber(b, s); }
    CHIP_ERROR GetManufacturingDate(uint16_t & y, uint8_t & mo, uint8_t & d) override
        { return mDelegate->GetManufacturingDate(y, mo, d); }
    CHIP_ERROR GetHardwareVersion(uint16_t & v) override        { return mDelegate->GetHardwareVersion(v); }
    CHIP_ERROR GetHardwareVersionString(char * b, size_t s) override
        { return mDelegate->GetHardwareVersionString(b, s); }
    CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan & span) override
        { return mDelegate->GetRotatingDeviceIdUniqueId(span); }

private:
    chip::DeviceLayer::DeviceInstanceInfoProvider * mDelegate = nullptr;
};

static RadioClockDeviceInfoProvider s_device_info_provider;

// ===== app_main =====

extern "C" void app_main()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    s_by_timer         = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
    s_timeout_start_us = esp_timer_get_time();

    // Deep Sleepからの復帰でない場合はRTCの古い時刻をクリア
    // （ファクトリーリセット後などに前回の時刻が残っていてJJYが誤起動するのを防ぐ）
    if (!s_by_timer) {
        struct timeval zero = {0, 0};
        settimeofday(&zero, NULL);
    }

    // LED 初期化（GPIO15 単純GPIO）
    gpio_config_t led_io = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_io);
    gpio_set_level(LED_GPIO, 1); // アクティブLOW: HIGH=消灯

    // LEDC: 40kHz 1-bit PWM（duty=1 → 50% キャリア波）
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num       = LEDC_TIMER_NUM,
        .freq_hz         = JJY_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = JJY_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHAN_NUM,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_NUM,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch_cfg);

    // ビットオフ用ワンショットタイマー
    esp_timer_create_args_t bit_off_args = {
        .callback        = bit_off_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "bit_off",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&bit_off_args, &s_bit_off_timer);

    // Matter ノード作成
    esp_matter::node::config_t node_config;
    esp_matter::node_t *node = esp_matter::node::create(
        &node_config, app_attribute_update_cb, NULL);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    // EP0 に Time Synchronization クラスター追加
    esp_matter::endpoint_t *root_ep = esp_matter::endpoint::get(node, 0);
    if (root_ep) {
        esp_matter::cluster::time_synchronization::config_t ts_cfg;
        esp_matter::cluster::time_synchronization::create(
            root_ep, &ts_cfg, esp_matter::CLUSTER_FLAG_SERVER);
        ESP_LOGI(TAG, "Time Synchronization cluster added to EP0");
    } else {
        ESP_LOGE(TAG, "Cannot get root endpoint (EP0)");
    }

    // EP1: on_off_light_switch + OnOff クラスター
    esp_matter::endpoint::on_off_light_switch::config_t ls_cfg;
    esp_matter::endpoint_t *ep1 = esp_matter::endpoint::on_off_light_switch::create(
        node, &ls_cfg, esp_matter::ENDPOINT_FLAG_NONE, NULL);
    if (!ep1) {
        ESP_LOGE(TAG, "Failed to create on_off_light_switch endpoint");
        return;
    }
    esp_matter::cluster::on_off::config_t on_off_cfg;
    esp_matter::cluster::on_off::create(ep1, &on_off_cfg, esp_matter::CLUSTER_FLAG_SERVER);
    s_endpoint_id = esp_matter::endpoint::get_id(ep1);

    // Matter 起動
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }
    s_matter_started = true;
    gpio_set_level(LED_GPIO, 0); // 動作状態：点灯（アクティブLOW）

    // VendorName / ProductName を上書きするプロバイダを登録
    s_device_info_provider.setDelegate(chip::DeviceLayer::GetDeviceInstanceInfoProvider());
    chip::DeviceLayer::SetDeviceInstanceInfoProvider(&s_device_info_provider);

    esp_matter::console::init();

    // Boot button 長押し5秒でファクトリーリセット
    {
        button_config_t btn_cfg = {
            .long_press_time  = FACTORY_RESET_PRESS_MS,
            .short_press_time = 50,
        };
        button_gpio_config_t gpio_cfg = {
            .gpio_num     = GPIO_NUM_9,
            .active_level = 0,
        };
        iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &s_boot_btn);
        iot_button_register_cb(s_boot_btn, BUTTON_LONG_PRESS_START, NULL, factory_reset_cb, NULL);
        ESP_LOGI(TAG, "Boot button: hold %ds for factory reset", FACTORY_RESET_PRESS_MS / 1000);
    }

    // 1秒タイマー開始
    esp_timer_create_args_t sec_args = {
        .callback        = one_second_tick,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "jjy_sec",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&sec_args, &s_sec_timer);
    esp_timer_start_periodic(s_sec_timer, 1000000);

    ESP_LOGI(TAG, "RadioClock ready | ep=%d gpio=%d freq=%dHz by_timer=%d",
             s_endpoint_id, JJY_GPIO, JJY_FREQ, (int)s_by_timer);

    // メインループ
    while (!s_going_to_sleep) {
        if (s_ntp_needed) {
            // 1秒タイマーを止めて NTP を試みる（ブロッキング）
            esp_timer_stop(s_sec_timer);
            bool ok = try_ntp_sync();
            if (ok) {
                // 同期成功: タイムアウト基点をリセットしてタイマー再開
                // one_second_tick が次のティックで正しい時刻を見つけて発振を開始する
                s_timeout_start_us = esp_timer_get_time();
                s_ntp_needed = false;
                esp_timer_start_periodic(s_sec_timer, 1000000);
            } else {
                // NTP も失敗: 90秒スリープして再試行
                s_sleep_us = WIFI_RETRY_US;
                s_going_to_sleep = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Deep Sleep 前処理
    esp_timer_stop(s_sec_timer);
    esp_timer_stop(s_bit_off_timer);
    jjy_pin_set(false);
    gpio_set_level(LED_GPIO, 1); // アクティブLOW: HIGH=消灯

    update_onoff(false);
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "Entering deep sleep: %llu us", s_sleep_us);
    esp_sleep_enable_timer_wakeup(s_sleep_us);
    esp_deep_sleep_start();
}
