# XIAO ESP32C3 Wi-Fi beacon sniffer

ESP-IDF の Promiscuous Mode で、アクセスポイントに接続せずに Beacon フレームを観測します。SSID、BSSID、RSSI、受信チャンネル、AP が広告するチャンネルを USB シリアルに表示する最初の実験です。

## 実行

Seeed Studio XIAO ESP32C3 を USB-C で接続し、PlatformIO から書き込みとシリアル監視を行います。

```sh
pio run -t upload
pio device monitor -b 115200
```

シリアルポートが自動検出されない場合は、`pio device list` で確認して `--port` を指定してください。

出力例（[実測ログの匿名化抜粋](examples/serial-output.txt)。SSID と BSSID を置き換え、重複行を省略）:

```text
[RX CH  5] [AP CH  6] BEACON RSSI:-72 BSSID:02:00:00:00:00:01 SSID:ExampleAP-6
```

チャンネル 1〜11 を約 700 ms ごとに切り替えます。`RX CH` は実際に受信したチャンネル、`AP CH` は Beacon の DS Parameters（なければ HT Operation）に記載されたプライマリチャンネルです。どちらの情報要素もない場合、`AP CH` は `-` と表示します。隣接するチャンネルで同じ AP を受信することもあります。

SSID が非公開の場合は `<hidden>`、情報要素がない場合は `<missing>` と表示します。制御文字や非 ASCII の SSID バイトは `\xHH` 形式で表示します。

ESP32-C3 の Wi-Fi は 2.4 GHz 帯のみです。チャンネルを巡回するため、観測数はその帯域の全フレーム数を表しません。実測値を公開するときは、自分が管理する AP・端末を中心に観測し、BSSID などの識別子を匿名化してください。
