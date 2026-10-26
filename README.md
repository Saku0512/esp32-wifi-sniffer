# XIAO ESP32C3 Wi-Fi beacon sniffer

ESP-IDF の Promiscuous Mode で、アクセスポイントに接続せずに Beacon フレームを観測します。SSID、BSSID、RSSI、受信チャンネルを USB シリアルに表示する最初の実験です。

## 実行

Seeed Studio XIAO ESP32C3 を USB-C で接続し、PlatformIO から書き込みとシリアル監視を行います。

```sh
pio run -t upload
pio device monitor -b 115200
```

シリアルポートが自動検出されない場合は、`pio device list` で確認して `--port` を指定してください。

出力例（架空の値）:

```text
[CH  1] BEACON RSSI:-45 BSSID:12:34:56:78:9A:BC SSID:MyWiFi
```

チャンネル 1〜11 を約 700 ms ごとに切り替えます。表示するチャンネルは Beacon 内の広告値ではなく、実際に受信したチャンネルです。SSID が非公開の場合は `<hidden>`、情報要素がない場合は `<missing>` と表示します。制御文字や非 ASCII の SSID バイトは `\xHH` 形式で表示します。

ESP32-C3 の Wi-Fi は 2.4 GHz 帯のみです。チャンネルを巡回するため、観測数はその帯域の全フレーム数を表しません。実測値を公開するときは、自分が管理する AP・端末を中心に観測し、BSSID などの識別子を匿名化してください。
