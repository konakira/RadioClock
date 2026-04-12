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
graph LR
    subgraph ESP32 [ESP32 Dev Board]
        PWM[GPIO 25 : PWM Out]
        GND[GND]
        VCC[3.3V or 5V]
    end

    subgraph MOSFET [2SK2232 : Nch-MOSFET]
        G[Gate]
        D[Drain]
        S[Source]
    end

    subgraph Antenna [JJY Antenna]
        Coil[バーアンテナ / コイル]
    end

    %% 配線
    PWM -- "1. 信号" --> G
    VCC -- "2. 電源" --> Coil
    Coil -- "3. 負荷" --> D
    S -- "4. 接地" --> GND

    %% スタイル設定（VS Codeでも確実に表示される書き方）
    style ESP32 fill:#e1f5fe,stroke:#03a9f4,stroke-width:2px
    style MOSFET fill:#fff3e0,stroke:#ff9800,stroke-width:2px
    style Antenna fill:#f9f9f9,stroke:#333,stroke-width:2px

