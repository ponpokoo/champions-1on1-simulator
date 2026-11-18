# 学習パイプライン（v1 / メタグロス 17 技）

自己対戦による方針／価値ネットワークの PPO 学習一式です。軌跡生成は CPU（C エンジン）、学習は GPU（PyTorch）を想定し、生成と学習を分離しています。

> **適用範囲**: 本パイプラインは観測 v1（`MGO1`・メタグロス 17 技・`learned` 方針）を対象としています。68 技（`learned2`）や Stage 8 の全 341 種族一般化向けの学習パイプラインは同梱していません（全種族の標準方針はヒューリスティクス `obsh4` です）。

## ファイル構成

- [campaign_local.py](campaign_local.py)：ビルド → 176 型母集団の準備 → `MGT3` 軌跡生成 → PPO 学習 → C/PyTorch パリティ検証 → 保留評価を一気通貫で回すローカル実行コントローラ。
- [loop.c](loop.c)：高速な `MGT3` 自己対戦軌跡生成および評価バイナリ（`engine/engine.c` を内包）。
- [model.py](model.py)：`MGO1` エンコーダ、`MWP1` モデル形式の読み書き、初期重み生成（`init`）、C ヘッダ書き出し（`export`）。
- [data.py](data.py)：`MGT3` 軌跡バイナリの検証・読込。
- [train.py](train.py)：ゲームバランス Monte Carlo PPO 学習本体（CUDA 前提、256 試合以下のみ `--cpu-smoke` 可）。
- [parity.c](parity.c) / [parity.py](parity.py)：PyTorch と C 推論のエンコーダ／ロジット／価値の一致検証。
- [sampling_test.c](sampling_test.c)：`loop.c` のサンプリング分離・マスク不変条件の単体テスト。
- [analyze.py](analyze.py)：`loop stage3 evaluate` 出力（`.bin` / `.csv`）の 95% 信頼区間・Markdown 表集計。
- [FORMAT.md](FORMAT.md)：`MGT3` 軌跡・`MWP1` モデル・評価バイナリの厳密仕様。
- [colab/README.md](colab/README.md)：ローカルに GPU がない場合に `train.py` のみを Google Colab GPU で実行するガイド（Modal 用スクリプトは未実装）。

## 依存

- C コンパイラ（`gcc`）と `libm`
- Python 3、`numpy`、`torch`（[requirements.txt](requirements.txt)）

## 使い方

```sh
# 1. ローカルで全自動実行（GPU 環境。CPU で動作確認だけする場合は --cpu-smoke）
python3 training/campaign_local.py --generations 1 --games-per-gen 128 --eval-games 50 --cpu-smoke

# 2. C 単体テスト（sampling_test）のみ実行する場合
gcc -O2 -Wall -Wextra -o training/sampling_test training/sampling_test.c -lm
./training/sampling_test
```

## 学習・評価の注意

- 学習の評価は必ず**未見の型（保留プール）**で行い、訓練母集団での結果と区別して報告してください（`campaign_local.py` で `--population` / `--eval-population` を省略すると重複なしの 176 型×2 を自動生成します）。
- 方針の事前分布（`obsh2` へのロジット加算 `prior`）を弱める過程では、決定論 argmax 評価が事前分布から実際に動いているかを世代ごとに確認してください。
