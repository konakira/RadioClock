# RadioClock — ESP-IDF + Matter 版

JJY 模擬電波（40kHz）を発振する ESP32-C6 向けファームウェアです。
Wi-Fi 接続・時刻同期を Matter プロトコルで行います。

![Running Image](https://github.com/konakira/RadioClock/blob/main/images/board-c6.png) "Radio generator with C")

Arduino 版のオリジナルコードは [RadioClock リポジトリ（main ブランチ）](https://github.com/konakira/RadioClock) にあります。

## 動作概要

- 毎日 2:00、6:00、14:00 JST に Deep Sleep から起床
- Wi-Fi に接続し、Matter コントローラ（Apple Home など）から時刻を取得
- 取得できない場合は NTP（pool.ntp.org / ntp.nict.jp）にフォールバック
- 取得成功後 30 分間 JJY 模擬電波を発振し、再び Deep Sleep へ
- Boot ボタン 5 秒長押しでファクトリーリセット

## ハードウェア

| 部品 | 備考 |
|------|------|
| Seeed Studio XIAO ESP32-C6 | メイン基板 |
| LED | GPIO15（オンボード LED） |
| JJY 発振出力 | GPIO17（40kHz キャリア） |

## ビルド環境

| ソフトウェア | バージョン |
|------------|----------|
| ESP-IDF | v5.2 |
| esp-matter | [espressif/esp-matter](https://github.com/espressif/esp-matter)（ESP-IDF v5.2 対応版） |

## セットアップ手順

### 1. ESP-IDF のインストール

[ESP-IDF 公式ドキュメント](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32c6/get-started/index.html) に従って ESP-IDF v5.2 をインストールし、環境を有効化します。

```bash
source ~/esp/esp-idf/export.sh
```

### 2. esp-matter のセットアップ

```bash
git clone --recursive https://github.com/espressif/esp-matter.git ~/esp/esp-matter
cd ~/esp/esp-matter
source export.sh
```

### 3. esp-matter へパッチを当てる

Boolean State クラスターの UnsupportedWrite 問題を修正するパッチです。

```bash
cd ~/esp/esp-matter
git apply /path/to/this/repo/patches/esp_matter_boolean_state.patch
```

### 4. このリポジトリをクローン

```bash
git clone -b esp-matter https://github.com/konakira/RadioClock.git
cd RadioClock
```

### 5. ビルド

```bash
source ~/esp/esp-idf/export.sh
source ~/esp/esp-matter/export.sh
idf.py set-target esp32c6
idf.py build
```

### 6. 書き込み

```bash
idf.py -p /dev/ttyUSBx flash
```

`/dev/ttyUSBx` はお使いの環境に合わせて変更してください（macOS では `/dev/cu.usbmodem*` など）。

## コミッショニング（初回設定）

書き込み後、Apple Home などの Matter 対応アプリからデバイスを追加します。

デバイス追加時に「セットアップコードを入力」を選び、以下のコードを入力してください。

| 項目 | 値 |
|------|----|
| セットアップコード（パスコード） | `20202021` |
| ディスクリミネーター | `3840`（`0xF00`） |

> これは esp-matter のデフォルトテスト用コードです。セキュリティが必要な場合は
> `sdkconfig` の `CONFIG_DEVICE_PASSCODE` などを変更してください。

## スケジュールの変更

`main/app_main.cpp` の以下の行を編集してください。

```c
static const uint32_t k_schedule[] = {
    HOUR_MIN(2, 0), HOUR_MIN(6, 0), HOUR_MIN(14, 0)
};
```

## ファクトリーリセット

Boot ボタンを 5 秒以上長押しするとファクトリーリセットされ、再コミッショニングが必要になります。
