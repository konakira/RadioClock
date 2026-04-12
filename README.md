# RadioClock
Radio generator for RadioClock in Japan

ESP32を利用して、NTPサーバーから取得した正確な時刻をもとに日本の標準電波（JJY）を疑似的に生成・送信するプロジェクトです。電波が届きにくい室内でも、市販の電波時計を正確に合わせることができます。

## 必要なハードウェア (Hardware Requirements)
* ESP32 開発ボード
* Nch-MOSFET (使用例: 2SK2232)
* バーアンテナ / コイル

## 回路図 (Circuit Diagram)
以下の図は、JJY信号を送信するためのアンテナ駆動回路です。ESP32のPWM出力を利用してMOSFETをスイッチングし、コイルを駆動させます。

```mermaid
graph TD
    %% 全体のスタイル調整
    subgraph ESP32 ["📱 ESP32 Dev Board"]
        PWM["GPIO 25 (PWM)"]
        GND["GND"]
        VCC["3.3V / 5V"]
    end

    subgraph MOSFET ["🔌 MOSFET (2SK2232)"]
        G["Gate"]
        D["Drain"]
        S["Source"]
    end

    subgraph Antenna ["📡 JJY Antenna"]
        Coil["Bar Antenna"]
    end

    %% 配線 (少し太めの線にする)
    PWM ==> G
    VCC --- Coil
    Coil --- D
    S --- GND

    %% 見た目を整えるスタイル指定
    style ESP32 fill:#f0f7ff,stroke:#007bff,stroke-width:2px
    style MOSFET fill:#fff9f0,stroke:#ff9800,stroke-width:2px
    style Antenna fill:#f5f5f5,stroke:#333,stroke-width:2px


