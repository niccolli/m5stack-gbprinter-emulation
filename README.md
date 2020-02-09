# M5stack "Gameboy Printer" Emulation

M5Stackをポケットプリンタに見立てて、ゲームボーイのポケットカメラから印刷ができるようにする仕組みです。

## Demo

![](https://raw.githubusercontent.com/niccolli/m5stack-gbprinter-emulation/master/assets/gb_printer_demo.jpg)

[https://twitter.com/niccolli/status/1225983387761987584](https://twitter.com/niccolli/status/1225983387761987584)

## Require

* M5Stack
* [esp32-idf toolchain setup](https://docs.espressif.com/projects/esp-idf/en/stable/get-started/index.html#setup-toolchain)
* I/F Board

```
$ xtensa-esp32-elf-gcc -v
gcc version 5.2.0 (crosstool-NG crosstool-ng-1.22.0-80-g6c4433a)
```

### I/F Board

M5Stackと初期型ゲームボーイを通信ケーブルで接続する際、下記の電圧降下回路を入れてください。

![](https://raw.githubusercontent.com/niccolli/m5stack-gbprinter-emulation/master/assets/schematic.png)

電池2本の本体では要らないかもしれません。

## Build

```
git clone --recursive https://github.com/niccolli/m5stack-gbprinter-emulation.git
cd m5stack-gbprinter-emulation
# This repository includes eps-idf v3.2.3
export IDF_PATH=$(pwd)/esp-idf
make
```

## 謝辞

このESP-IDFとM5Stack Arduinoの混合環境は、[m5stack-synth-emulation](https://github.com/h1romas4/m5stack-synth-emulation)を利用しています。

M5Stackでポケットカメラの信号を受信する箇所、およびビットマップに変換する箇所は、[Dhole氏の解析結果](https://dhole.github.io/post/gameboy_serial_2/)を利用しています。
