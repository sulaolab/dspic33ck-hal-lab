# dsPIC33CK1024MP710 Touch-CAN-LIN port

この文書はボードの実装・操作・検証範囲をまとめたもの。開発セッションの引継ぎは公開対象外。

対象は dsPIC33C Touch-CAN-LIN Curiosity / dsPIC33CK1024MP710。
ユーザー指定の実機は EV97U97A。DS70005531A 本文の EV97A97U 表記と
回路図revision表記には不一致がある。実物revisionはまだ記録していない。

2026-10-13 に新ボードの音声を実装した。
新構成 `CK1024MP710_EV97U97A` のビルド・書き込み・起動を確認し、
ユーザーが AVAS の発音と POT による音程変化を確認した。

## GPIO / PPS / CN 実装

公開関数のシグネチャとNORA名前空間は維持した。

| 項目 | 対応 |
|---|---|
| GPIO Port F | ANSEL/TRIS/PORT/LAT/ODC/CNPU/CNPDのレジスタ行を条件付き追加 |
| RP80〜95 | RF0〜15へ変換。各RPのDFP aliasが存在する場合のみ対応 |
| RP96〜99 | RA5〜8へ変換。A0〜4・A9以降はRP変換しない |
| GPIO→RP | FとA5〜8を追加。逆変換で同じDFP可用性判定を利用 |
| PPS出力 | RP80〜99の書込みと物理ピン読出し表を追加 |
| PPS入力 | 追加範囲は出力表のDFP定義を確認してから設定。旧デバイスでは拒否 |
| PPS読出し幅 | DFPの `_RPOR0_RP32R_MASK` を使用。MP710の7bitコード68〜71を保持 |
| CN Port F | CNCONF/CNEN0F/CNEN1F/CNFFとCNFのIF/IE/IP aliasを追加 |
| CN割り込み | IF/IEはDFP bit aliasへ定数0/1を書込む。実際のISR vectorはアプリ所有 |

`NORA_GPIO_RP_MAX=99` はファミリの上限で、選択デバイスのpad数ではない。
従来B/C/DのRP変換は既存の算術動作を維持しており、変換成功だけでは
package pinの存在を保証しない。RP176〜181はPPS内部接続として維持し、
GPIO変換と物理出力ピン探索から除外する。Port Eはpin指定GPIOで使う。

既存のMC005読出し表にも修正を加えた。以前は隣接RPの存在からMC105と同じ
詰めた配置だと推測していたが、MC005はRP65/72/74/77が
RPOR16/20/21/22の低位フィールドにある。実際のDFPフィールド定義で判定する。
MC105のRPOR15/16の配置とMP508/MP710の配置は維持する。

## 資料との接点

DS70005531A（PDFページ基準）p.19〜21:

| ボード信号 | 新ピン | 今回の対応との関係 |
|---|---|
| mikroBUS SCK / CS | RA5/RP96、RA6/RP97 | 拡張RPとPPSが必要 |
| PKOB4 UART TX | RA8/RP99 | 拡張RPとPPSが必要 |
| S2 / S3 | RF3/RP83、RF4/RP84 | Port F GPIO/CNが必要 |
| LIN RX / SENT | RF0/RP80、RF1/RP81 | 今回はピンを扱う基盤のみ。プロトコル実装は後続 |

デバイス根拠はDS70005496DのGPIO/PPS章（PDF p.286〜401）と
Microchip dsPIC33CK-MP_DFP 1.16.521の `p33CK1024MP710.h`。
PPS出力フィールドは7bit、RP80はRPOR24低位、RP99はRPOR33高位。

## オフライン検証

再実行入口は `tools/gpio_check/verify_port.ps1`。
Python、XC-DSC 3.31.01、MPLAB X 6.30、host GCCを使用する。
ツールパス・pack rootの変更は `run_gpio_check.py` の引数で指定できる。
既存3ボードの意味のある構成設定とビルドの既定値は維持した。
新ボードのソースは既存構成から除外している。

`run_gpio_check.py` は4種類の実DFPに対してGPIO/PPS/CN/tableをコンパイルする。
host側ではDFPのbitfield、alias、mask、コードからSFRモデルを生成し、実HALを実行する。
全物理RPについて、GPIO往復変換、PPSレジスタ配置、入力/出力/clearを確認する。
MP710ではCLC5〜8、FのGPIO設定、CNの立上り/立下りcallback、enable状態、
無関係なCNAフラグの保持を確認する。旧デバイスでは拡張RPとFの拒否も確認する。
MC105の詰めた配置とMC005の疎な配置はDFP由来の期待値に照合する。

hostモデルは割り込み競合の実時間や電気特性を再現しない。
IF/IEの書込み規則は既存 `irq_atomicity_source_lint.py` でも検証する。

コンパイルは `-Wall -Wextra -Werror -mno-eds-warn -O1`。
MP710では既存GPIOのローカル変数アドレスにもEDSの助言警告が発生したため、
既存ビルドと同じ `-mno-eds-warn` で抑制する。
新ボードの全体リンク時にはスタックを低位RAMへ配置し、mapで確認する必要がある。
この警告の抑制をEDSを含む全RAM利用の検証済みとは扱わない。

2026-10-06の最終スクリプト実行はexit 0（733.6秒）。4デバイスのHAL検証と
IRQ lint（152 files、0 violations）を通過し、既存3構成も全体ビルドを通過した。

| 既存構成 | program bytes / 使用率 | data bytes / 使用率 |
|---|---|---|
| CK64MC105_EV88G73A | 66063 / 99% | 5878 / 71% |
| CK256MC005_EV08P02A | 73731 / 27% | 5938 / 36% |
| CK256MP508_DM330030 | 72396 / 26% | 6272 / 25% |

MP710のtarget objectではIFS0/IEC0の9箇所の書込みが
`bclr.b` / `bset.b` であることも逆アセンブルで確認した。
7bit maskを意図的に6bitへ戻した作業用コピーはCLC5〜8の探索テストでexit 1になり、
このテストが読出し幅の不具合を検出することを確認した。
上記は2026-10-06のオフライン検証結果。2026-10-13の実機結果は以下。

## EV97U97A 音声ポート（2026-10-13）

WM8904 は mikroBUS A / J9、ジャンパは XTAL → MCLK。
WM8904 を master、dsPIC を slave とする 48 kHz / TDM8 / 32-bit slot。
通常経路は TDMin → AVAS → Gain → TDMout。既存の WM8904 レジスタ処理と
AVAS DSP 本体を維持し、board port と HAL のデバイス差分を追加した。
LIN/CAN/Touch/SENT/SAFE LED は今回の確認範囲外。

| 用途 | EV97U97A 設定 |
|---|---|
| BCLK / FS / SDO / SDI | RA5/RP96 / RA6/RP97 / RC11/RP59 / RC5/RP53 |
| I2C3 SCL / SDA | RD3/RP67 / RD4/RP68、ALTI2C3=ON、400 kHz |
| PKoB4 UART | TX RA8/RP99、RX RB13/RP45、230400 baud |
| MCP2221A UART（未実機検証） | EV97U97A_CONSOLE_PKOB4=0、TX RP46 / RX RP47 |
| POT | RA0/AN0、専用 ADC core 0、12-bit。J8 POT enable が必要 |
| S1 / S2 / S3 | RD13 / RF3 / RF4、active low |
| RGB | red RB12/PWM2H、green RB10/PWM3H、blue RD1/PWM4H |
| LED1 / LED2 | RE6 / RE7 |
| MCU clock | FRC → PLL、Fcy 100 MHz。codec の XTAL は MCU clock と独立 |
| config | PGD3、JTAG OFF、ALTI2C3 ON、SMB3EN=SMBUS2 |

HAL ADC の MP710 対応は AN0 の専用 core 0 に限定する。他チャネル、校正、
専用 core 1以降の利用を合格扱いにしない。DMA は既存 ch0..3 / 16-bit 低位RAMの
契約を維持し、MP710 の上位アドレスレジスタを明示的にゼロにする。
音声バッファは 0x10f4 と 0x18f4、各0x800 bytes。
linker は --local-stack / 最低1024 bytesを指定し、map の stack は0x2882..0x8000。
8ch DMA / EDS buffer / EDS stack の対応ではない。

ボードの新実装は `src/boards/ev97u97a/`。
元の DM330030 実装の license を維持し、コンソール診断は既存 EV88G73A の
`t` / `c` handler と同じ呼出しを追加した。
`build.ps1` は変化する build stamp の利用object（main/general_console）を
再生成する。生成makefileはheader依存更新が無効のため、これが無いと
インクリメンタルビルド後の ?gv が古いIDを返すことがある。

### 操作

| 操作 | 機能 |
|---|---|
| SW1 | DSP mute切替（ランプあり）。書き込み前のanalog mute確認には使わない |
| SW2 / `a` | AVAS Type_TY 起動／停止 |
| SW3 / `A` | AVAS Type_LB 起動／停止 |
| POT | ベース音程 0..+200 cent（2半音）、EMA/deadbandあり |
| `?gv` / `?gh` | build ID / console hello |
| `?tp` | 現在のpath、AVAS状態、入力slotのsampled peak |
| `*tp0002` / `*tp0004` | 1 kHz tone / 通常chain |
| `?ti` / `?to` / `?tc` | PRE gain / POST gain / POTとtrimの音程 |
| `?tq` | block count、miss、sampled DSP時間、UART health |
| `*ts` / `?ts` / `*tr` | analog mute＋停止 / 状態 / 再開 |
| `?lr` / `*lrRR GG BB`（空白なしhex） | RGB状態 / 手動RGB百分率 |

AVAS起動はfade-in、停止は約3秒fade-out。同じボタンで止め、fade-out完了後に
別voiceを起動する。POTとRGBの動作は旧DM330030のUIを継承している。
RGB発光・SW1 mute・外部アナログ入力のpassthrough音質は個別の人間確認がまだない。

### 複数 PKoB4 の識別

`buildtools/identify-pkob4.ps1` は Windows PnP の USB composite parent と
CDC child を照合する。COMを直接openしない。COM番号は再接続で変わり得るため、
記録済みシリアル番号を起点に毎回照会する。

```powershell
# [ran] 書き込み・reset・COM openなしの一覧
.\buildtools\identify-pkob4.ps1 -Json

# [ran] 指定シリアルのMCU照合。program/eraseはしないが接続でreset/haltし得る。
# 音声稼働中は先に下記 *ts 手順、または外部muteを行う。
.\buildtools\identify-pkob4.ps1 -Probe -Serial $targetSerial
```

2026-10-13の実機照合:

| 対象 | MCU確認 |
|---|---|
| EV97U97A | dsPIC33CK1024MP710、Device ID 0xa0650000、revision1 |
| 別の接続キット | CK1024照合不成立。実際のMCUは未確定、書き込まない |

`$targetSerial` は一覧から選び、対象ボードと照合したserialを指定する。
Probe成功は期待deviceのfound行と非ゼロDevice IDの両方を必要とする。
実ログの受理、Device ID=0の拒否、別MCUの拒否を検証した。
外部セッションのIDE/IPE/Javaを一括停止しない。

### 書き換え前の停止と再開（POP対策）

**EV97U97Aでも書き込み・reset・MDB probe前に音声を停止する。**
AVASだけの停止、SW1のDSP mute、path muteはこの代わりにはならない。
`*ts` は WM8904 のanalog muteをI2Cでreadback確認し、TDM/DMAを停止する。

1. `serial-monitor` の設定を解決してURLを取得する。`/status` のprofileがck、
   COMが対象PKoB4 serialに属することを確認する。COMを別プロセスからopenしない。
2. `*ts` を送信し、新しい応答
   `WM8904 audio: stopped (analog mute verified, TDM/DMA halted)` を確認する。
   `analog mute NOT verified` やtimeoutでは通常の書き込みへ進まない。
3. 確認後1.5秒待ってからflash/resetする。
4. boot後に `?gv` のROMイメージIDと `?ts` のrunningを確認する。
   flash/resetは自動起動、手動停止だけの場合は `*tr` で再開する。
   AVAS音は必要なボタンで改めて起動する。

`flashauto.ps1 -Configuration CK1024MP710_EV97U97A` はこの停止確認と待機を
flash前／単独reset前に自動実行する。URLはserial-monitorのresolverから取得し、
`/status` のCOMをPnP parentでflash対象serialと照合してから送信する。
`/command_wait` が応答待ちを送信前にarmするため、古いログは確認を通さない。
`-DryRun` はUART送信も停止も行わない。

```powershell
# [ran] URL解決、対象照合、*ts確認、待機（補助関数単体の実機検証）
. .\buildtools\pkob-audio-gate.ps1
$monitorUrl = Resolve-PkobMonitorUrl -Root (Get-Location).Path
Invoke-PkobVerifiedAudioStop -MonitorUrl $monitorUrl -Serial $targetSerial
```

monitorが応答しないROMの復旧時は、先にヘッドホン／スピーカーを外すか外部でmuteし、
明示的に `-StopAudioCommand none` を付ける。これは通常手順ではない。
初回停止なしflashでユーザーから大きなPOPが報告された。停止確認後の再flashでは
ユーザーが「POPは小さくなった」と確認した。再開時のアナログPOPが完全に消えることや、
POP量の定量比較は未検証。

### ビルドと書き込み

```powershell
# [ran] 新ボード全体build
.\buildtools\build.ps1 -Full -Configuration CK1024MP710_EV97U97A

# [ran] ROM書き込み。helper executablesを配置したfolderを明示する。
# この最終版は前節の *ts とanalog mute確認を必須にする。
.\buildtools\flashauto.ps1 -Configuration CK1024MP710_EV97U97A `
    -Serial $targetSerial -ToolsDir $flashResetToolsDir
```

`$flashResetToolsDir` は `flash_pkob4.exe` と `reset_pkob4.exe` のあるfolderを指定する。
別repoのscriptへ切り替えず、HAL-labのscriptを使う。EV97構成は明示serial必須、
DeviceはdsPIC33CK1024MP710に固定。build.ps1 -Fullは他構成のbuild/distも消すため、
実機用イメージを保持するなら先に別の証拠folderへコピーする。

### 今回の確認

- [ran] 新構成全体build（XC-DSC3.31.01 / MPLAB X6.30 / MP_DFP1.16.521）。
  program78762 bytes、data6274 bytes。通常再buildは時刻・build IDが変わる。
- [ran] 新ボードflash/reset、Fcy=100000000、on_target=1、clock diag=0、UART/pins/IO=1。
- [ran] WM8904初期化と音声DMA開始。初回観測block291903、miss0、
  sampled max約43.4us（AVAS停止時）。これは電気波形／音質評価ではない。
- [ran] ユーザー実聴: AVASが聞こえ、POTで音程が変化した。
- [ran] 停止gateを通した最終flash/resetが成功。?gvは
  `dd37ac0-dirty-20261013T122150679-dedd540935` と一致。
  SHA256: `7aca66b72e871487d936059b8c03a399ec279604bd3dbec898b13b2fb58b4f72`。
- [ran] ?gh/?ti/?to/?tc/?tp/?tq、tone/chain切替、*cy00、*ts/?ts/*trの実機応答。
  AVAS稼働時block202262、miss0、sampled max約373us/667us、余裕約294us。
- [ran] 停止gateのoffline検証: `pwsh -NoProfile -File tools/pkob_flash_gate_check.ps1`。
  正常応答、別serial/profile、未確認mute、timeout、ゼロDevice ID/別MCUを確認。
- [ran] 停止gate付き書き換え後、ユーザーがPOP縮小を確認。
- [ran] GPIO/PPS/CNの4デバイスtarget compile＋DFP由来host test。
- [ran] IRQ atomicity lint: 176 files、0 violations。
- [ran] 既存3構成の意味のあるXML設定を機械比較（新source除外を除いて同一）。
- [ran] 既存3構成全体build: EV88G73A program/data66279/5878、
  EV08P02A73944/5938、DM33003072738/6274 bytes。実機には書き込んでいない。
- [ran] git diff --check。

ビルド／実機の原ログとイメージはローカル生成物。Git管理のソースとこの結論を
恒久記録とし、個人PC上の生成物の絶対パスを文書の恒久リンクにしない。
