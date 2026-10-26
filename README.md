# XIAO ESP32C3 Wi-Fi beacon sniffer

ESP-IDF の Promiscuous Mode で、アクセスポイントに接続せずに 2.4 GHz 帯のフレームを観測します。Beacon の SSID・BSSID・RSSI・受信/広告チャンネルを表示し、Management / Control / Data の種類別件数、チャンネル別件数、RSSI 分布を集計します。

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

Beacon の個別行は、BSSID と受信チャンネルの組み合わせにつき最初の1件だけ、最大48行を表示します。Data フレームの送信元・宛先アドレスも `To DS` / `From DS` の組み合わせごとに最初の1件だけ表示します。ペイロードは保存・表示しません。

60秒ごとに累積 `SUMMARY` を表示します。`TYPE` は3種類の件数、`SUBTYPE` は主なサブタイプ、`CH` は受信チャンネル別の観測時間・件数・Beaconで見えたBSSID数、`RSSI` は全種類の受信フレームの分布です。`queue_dropped` は処理用キューからあふれた件数です。これが0以外なら集計に欠落があります。5分間の測定には `elapsed_s=300` のレポートを使います。

識別子を含む個別行を除外して集計だけをPCに表示する場合は、PlatformIO の Python 環境で次を実行します。

```sh
~/.platformio/penv/bin/python tools/capture_summary.py --port /dev/ttyACM0
```

接続ポートが異なる場合は `--port` を変更してください。ヘッダ解析のホスト側テストは `cc -std=c11 -Isrc test/frame_parser_test.c src/frame_parser.c -o /tmp/frame_parser_test` でビルドできます。

[2026-10-26の5分間検証結果](docs/verification-2026-10-26.md)には、識別子を含まない集計値と未検証項目をまとめています。

ESP32-C3 の Wi-Fi は 2.4 GHz 帯のみです。チャンネルを巡回するため、観測数はその帯域の全フレーム数を表しません。受信チャンネルが重なって同じAPのフレームを隣接チャンネルで数えることもあります。実測値を公開するときは、自分が管理する AP・端末を中心に観測し、SSIDやMACアドレスなどの識別子を匿名化してください。
