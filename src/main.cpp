#include <TimeLib.h>

#include <WiFi.h>
#include <WiFiMulti.h>

//#include <esp_deep_sleep.h>
// #include <esp_sleep.h> It was said that esp_deep_sleep.h will be deprecated
// and esp_sleep.h should be instead, but it does not declare esp_deep_sleep_pd_config
#include <esp_sleep.h>

class LED {
public:
  LED(int p, bool initialOn = false) : pin(p), led(initialOn) {
    pinMode(pin, OUTPUT);
    if (initialOn) {
      on();
    }
    else {
      off();
    }
  }
  ~LED() {
    off();
    pinMode(pin, INPUT);
  }
  void on() {
    led = true;
    digitalWrite(pin, HIGH);
  }
  void off() {
    led = false;
    digitalWrite(pin, LOW);
  }
  bool flip() { // 桁上がりを返す
    if (led) {
      off();
      return true;
    }
    else {
      on();
      return false;
    }
  }
private:
  int pin;
  bool led;
};

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
// the above value is taken from:
// https://www.etechnophiles.com/esp32-blinking-led-tutorial-using-gpio-control-with-arduino-ide/

#ifdef BUILTIN_LED
LED blue(BUILTIN_LED);
#else
LED blue(LED_BUILTIN);
#endif

// WiFi AP information should be stored at auth.h
struct WifiCredential {
  const char* ssid;
  const char* pass;
};
#include "auth.h"
const int wifi_count = sizeof(wifi_list) / sizeof(wifi_list[0]);
WiFiMulti wifiMulti;


#include <Ticker.h>
Ticker timeout, interval;

void writelog(String mesg)
{
  Serial.println(mesg);
}

// WiFiに接続できない場合にDeep Sleepする

void gotoSleep(uint64_t);

const unsigned long WIFI_TIMEOUT = 60000000ul; // 60sec
hw_timer_t *hwTimer; // hardware timer for Wi-Fi timeout
// Timer is documented below:
// https://espressif-docs.readthedocs-hosted.com/projects/arduino-esp32/en/latest/api/timer.html

void IRAM_ATTR onWiFiTimeout() {
  const uint64_t WIFI_RETRY_INTERVAL = 90000000ul; // 90sec

  gotoSleep(WIFI_RETRY_INTERVAL);
}

void connectWiFi()
{
  const unsigned waitTime = 500, waitTimeOut = 60000;
  unsigned i;
  static bool firstTime = true;
  
  hwTimer = timerBegin(0, 80, true); // for 80MHz
  timerAttachInterrupt(hwTimer, &onWiFiTimeout, true);
  timerAlarmWrite(hwTimer, WIFI_TIMEOUT, false);
  timerAlarmEnable(hwTimer);

  wifiMulti.run();
  if (firstTime) {
    // start ticker with 0.5 because we start in AP mode and try to connect

    // indicating wifi trial would be good.
  }
  for (i = 0 ; WiFi.status() != WL_CONNECTED && i < waitTimeOut ; i += waitTime) {
    delay(waitTime);
  }
  if (firstTime) {
    // showing connection completion would be good.
  }
  firstTime = false;

  timerAlarmDisable(hwTimer);
  timerDetachInterrupt(hwTimer);
  timerEnd(hwTimer);
}

const int timePin = 25; // GPIO 25, for time output to 40kHz radio
const int radio = 0;

void setPin(int m)
{
  ledcWrite(radio, m ? 1 : 0);
  // digitalWrite(timePin, m ? HIGH: LOW);
}

void turnOffRadio()
{
  setPin(0);
}

void sendBit(unsigned b)
{
  uint32_t duration;

  setPin(1);

  switch (b) {
  case 0:
    duration = 800;
    break;
    
  default:
    duration = 500;
    break;
  }
  timeout.once_ms(duration, turnOffRadio);
}

void sendMarker()
{
  setPin(1);
  timeout.once_ms((uint32_t)200, turnOffRadio);
}

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

  storeBCD(year(t), _year, 2);

  gmtime_r(&t, &timeInfo);
  storeBCD(timeInfo.tm_yday + 1, _yday, 3);

  storeBCD(hour(t), _hour, 2);
  storeBCD(minute(t), _min, 2);

  _minParity = (calcParity(_min[0], 3) ^ calcParity(_min[1], 4));

  _dayOfWeek = weekday(t) - 1;
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
          time_t t = now();
          setTime(t);
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


#define HOUR_MIN(x, y) (x * 60 + y)

#ifndef WAKEUP_SCHEDULE
#define WAKEUP_SCHEDULE HOUR_MIN(2, 0), HOUR_MIN(6, 0), HOUR_MIN(14, 0)
#endif

static unsigned long schedule[] = {WAKEUP_SCHEDULE, HOUR_MIN(24, 99) /* means "end of array" */};
static const unsigned long minutesInADay = HOUR_MIN(24, 0);

unsigned long calcSleepMinutes()
{
  time_t t = now();
  unsigned inMinute = HOUR_MIN(hour(t), minute(t));

  for (unsigned long *nextWakeupTime = schedule ; *nextWakeupTime < minutesInADay ; nextWakeupTime++) {
    if (inMinute < *nextWakeupTime) {
      return *nextWakeupTime - inMinute;
    }
  }
  return schedule[0] + minutesInADay - inMinute;
}

void gotoSleep(uint64_t sleeplen)
{
  esp_sleep_enable_timer_wakeup(sleeplen);
  esp_deep_sleep_start();
}

void onTimer()
{
  static int sec = -1, min = -1;
  // It is safe to use digitalRead/Write here if you want to toggle an output

  blue.flip();
  
  if (sec < 0) {
    time_t t = now();
    sec = second(t);
    min = minute(t);
    rcd.setTime(t + 1); // 時計、1秒ぐらい遅れてる感じなので1秒進めておく。
  }
  if (min == 15 || min == 45) {
    rcd.sendData2(sec);
  }
  else {
    rcd.sendData(sec);
  }
  if (59 < ++sec) {
    // rcdの「分」をインクリメントする
    sec = 0;
    rcd.incrementMin(); // rcd.incrementMin() is performed in main loop in parallel.
    if (59 < ++min) {
      min = 0;
    }
  }
}

bool byTimer = false;
RTC_DATA_ATTR time_t targetWakeupTime = 0;
RTC_DATA_ATTR time_t bootTime = 0;
const time_t recentPastTime = 1500000000UL; // 2017/7/14 2:40:00 JST

class Timer {
public:
  Timer() { startTime = millis(); }
  unsigned long lapTime() { return millis() - startTime; }
private:
  unsigned long startTime;
} clockTimer;

void mySecTimerEvent()
{
  const unsigned long workingTime = (30UL * 60UL); // 30 minutes
  time_t t = now();

  if (bootTime < recentPastTime && recentPastTime < t) {
    bootTime = t;
  }
  
  if (recentPastTime < t) { // 時間がただしく設定されたら
    if (((t + 60 < targetWakeupTime) && byTimer) || // 60秒以上早く起きていたら
	workingTime < clockTimer.lapTime() / 1000) { // もしくは十分働いたら
      time_t sleepsec, sleepmin;

      // 稼働日数の記録
      // Blynk.virtualWrite(VUPTIME, (now() - bootTime) / 3600.0 / 24.0); // day

      if ((t + 60 < targetWakeupTime) && byTimer) {
	sleepsec = targetWakeupTime - t;
	// for debug
	char buf[256];

	sleepmin = sleepsec / 60;
	snprintf(buf, 256, "RadioClock wave generator will sleep for %ld min.", sleepmin);
	writelog(buf);
      }
      else { // 十分働いたら
	char buf[256];

	sleepmin = calcSleepMinutes();
	sleepsec = sleepmin * 60UL - second(t); // 指定時の正分に起きるため補正
	snprintf(buf, 256,
		 "RadioClock wave generator will sleep for %ld min.", sleepmin);
	writelog(buf);
	targetWakeupTime = t + sleepsec;
      }

      if (sleepmin < (24 * 60)) { // sleepminが24時間を超えてなければ
	gotoSleep((uint64_t)sleepsec * (uint64_t)1000000ul);
	// usec単位だとunsigned longは1時間11分で桁あふれしちゃう
      }
      else {
	targetWakeupTime = 0; // ここに来ることはほぼありえないと思う
	// なんらかの原因でtargetWakeupTimeの値が壊れた時とかぐらいかな？
	// ここにきたら1秒後に以下のelse節から続きが行われる
      }
    }
    else {
      static bool firstTime = true;
      if (firstTime) {
	firstTime = false;
	writelog("RadioClock wave generator started.");

	// 稼働日数の記録
	// Blynk.virtualWrite(VUPTIME, (now() - bootTime) / 3600.0 / 24.0); // day
      }
      onTimer();
    }
  }
}

void ShowDateTime(struct tm *pti)
{

  Serial.print(pti->tm_year + 1900);
  Serial.print("/");
  Serial.print(pti->tm_mon + 1);
  Serial.print("/");
  Serial.print(pti->tm_mday);
  Serial.print(" ");
  
  Serial.print(pti->tm_hour);
  if (pti->tm_min < 10) {
    Serial.print(":0");
  }
  else {
    Serial.print(":");
  }
  Serial.print(pti->tm_min);
  if (pti->tm_sec < 10) {
    Serial.print(":0");
  }
  else {
    Serial.print(":");
  }
  Serial.println(pti->tm_sec);
}

// reset time to 1970/1/1 or something.
void resetTime()
{
  struct tm ti;

  ti.tm_year = 99;
  ti.tm_mon = 11;
  ti.tm_mday = 31;
  ti.tm_hour = 0;
  ti.tm_min = 0;
  ti.tm_sec = 0;
  ti.tm_isdst = 0;
  ti.tm_wday = ti.tm_yday = 0; // mktime() ignores them.

  time_t t = mktime(&ti);

  struct timeval tv;
  tv.tv_sec = t;
  tv.tv_usec = 0;
  settimeofday(&tv, NULL);
}

bool NTPSync() // return true if nsynchronized.
{
  bool retval = false;

  resetTime();
  delay(1000); // wait for 1 sec, just in case

  {
    time_t t;
    struct tm ti;
    t = time(NULL);
    localtime_r(&t, &ti);
    ShowDateTime(&ti);
  }

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  for (int i = 0 ; i < 30 && WiFi.status() != WL_CONNECTED ; i++) {
    delay(1000);
  }
  if (WiFi.status() == WL_CONNECTED) {
    // check NTP
    time_t t;
    struct tm ti;

    configTime(9 * 3600, 0, "ntp.nict.jp", "time.google.com", "ntp.jst.mfeed.ad.jp");

    for (t = 0 ; t < recentPastTime ; t = time(NULL)); // wait for NTP to synchronize
    retval = true;

    localtime_r(&t, &ti);
    setTime(ti.tm_hour, ti.tm_min, ti.tm_sec, ti.tm_mday, 1 + ti.tm_mon, 1900 + ti.tm_year);

    ShowDateTime(&ti);

    writelog("NTP Synchronized.");
  }
  return retval;
}

void setup()
{
  Serial.begin(115200);
  Serial.println("");

  configTime(9 * 3600, 0, nullptr); // set time zone as JST-9
  // The above is performed without network connection.

  blue.on(); // to indicate ESP turns on

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  if (wakeup_reason == 3) {
    // 1: caused by external signal using RTC_IO
    // 2: caused by external signal using RTC_CNTL
    // 3: caused by timer
    byTimer = true;
  }

#ifdef BUILTIN_LED
  Serial.print("BUILTIN_LED = ");
  Serial.println(BUILTIN_LED);
#endif

#ifndef JJY_FREQ
#define JJY_FREQ 40000
#endif
  
  // prepare for the PWM to work in 40kHz
  ledcSetup(radio, JJY_FREQ, 1); // chan 0, freq = 40000, bitlength = 1(means duty rate = 50%
  ledcAttachPin(timePin, radio); // attach the channel to LED chan 0, set above
  ledcWrite(radio, 0);

  for (int i = 0; i < wifi_count; i++) {
    wifiMulti.addAP(wifi_list[i].ssid, wifi_list[i].pass);
  }  
  wifiMulti.run();
  
  if (!NTPSync()) { // if not synchronized, then go sleep for 90 sec.
    onWiFiTimeout();
  }

  interval.attach_ms(1000, mySecTimerEvent); // timer should be called every second
}

void loop() 
{
}
