# Colab での学習（接続ガイド）

GPUのない環境向けに、学習ステップ（`training/train.py`）だけを Google Colab の
GPU で実行するためのガイド。軌跡生成はローカル CPU、学習は Colab、書き出された
重み（Cヘッダ）と学習指標を持ち帰ってローカルで検証する、という分担が前提。

接続方法は2通りある。本ガイドの主対象は (B) の CLI で、2026-11-18 に T4 実機で
一連の流れ（認証→割当→実行→回収→解放）を実証済み。

## (A) ブラウザでノートブックを使う（従来型）

1. https://colab.research.google.com/ を開き、ランタイムを GPU（T4）に設定する。
2. リポジトリと軌跡データをアップロード（または Google Drive 経由で持ち込む）。
3. セルから `train.py` を実行し、出力（重みヘッダ・指標）をダウンロードして持ち帰る。

## (B) google-colab-cli で手元のターミナルから実行する

公式 CLI（`google-colab-cli`、Linux / macOS 対応。Windows は現時点で非対応）。

### インストール

```bash
uv tool install google-colab-cli   # 推奨
# または
pip install google-colab-cli
```

### 初回認証（1回だけ）

`colab run` などの初回実行時に認証 URL が表示される。ブラウザで開いて
Google アカウントで承認すると認証コードが表示されるので、ターミナルに貼る。
トークンは OS のキーリングに保存され、次回からはコード不要で自動更新される。

※ ヘッドレス Linux（キーリングがない環境）では保存に失敗し、毎回再認証を
求められることがある。その場合はファイル型キーリングを使う：

```bash
pip install keyrings.alt
export PYTHON_KEYRING_BACKEND=keyrings.alt.file.PlaintextKeyring
```

（保存先 `~/.local/share/python_keyring/`、ファイル権限 600。平文保存になる点に注意。）

### 学習の実行

一番簡単なのは使い捨て実行（割当→実行→解放を自動でやる）：

```bash
colab run --gpu T4 training/train.py -- --data <軌跡データ> --input <入力> --output <出力>
```

依存（numpy / torch）は Colab 側に torch が入っていることが多いが、足りなければ
セッション型の流れで入れる：

```bash
colab new -s trainer --gpu T4
colab install -s trainer -r training/requirements.txt
colab upload -s trainer <軌跡データ> <リモートパス>
colab exec -s trainer -f training/train.py
colab download -s trainer <重みヘッダ> ./weights.h
colab stop -s trainer
```

- `colab exec -f file.py` はファイル内容を自動で送るので、スクリプト自体の upload は不要。
- 大きなデータは `colab drivemount` で Google Drive 経由も使える。
- 残量確認は `colab usage`（コンピュートユニット残高と消費レート）。

### 注意：411 Length Required が出たら

CLI 0.8.0 では、ランタイム割当（assign / unassign）の POST が本文なしで送られ、
環境によって HTTP 411 で拒否されることがある（2026-11-18 に実測）。
上流に既知問題としての報告は見つかっておらず、公式に保証された回避策でもないが、
実測では CLI の `colab_cli/client.py` の該当 POST に JSON ボディ `"{}"` と
`Content-Type: application/json` を付けて送ると通った。
（この経路 `/tun/m/assign` は新 Colab API とは別のインターフェース。）

## (C) MCP / Colab API で直接つなぐ（上級者向け）

ホスト型 MCP エンドポイント `https://colaboratory.googleapis.com/mcp` が公開されて
いる（ツール：`create_runtime` / `create_session` / `execute_code` /
`get_subscription` / `list_runtime_specs` 等）。公式手順は次のとおり：

1. Google Cloud プロジェクトで Colab API（`colaboratory.googleapis.com`）を有効化する。
2. MCP リクエストに `Authorization: Bearer <OAuth 2.0 access token>`（スコープ
   `https://www.googleapis.com/auth/colaboratory`）と、設定例として
   `x-goog-user-project: <PROJECT_ID>` を付ける。
   クォータプロジェクトなしのトークンでは 403（quota project が必要）になることを実測。
3. `create_runtime` の前に `list_runtime_specs` で `eligible` な spec を確認する。
   作成は遅くクォータ制限があり、`FAILED_PRECONDITION`（空き不足・上限超過）や
   `RESOURCE_EXHAUSTED`（レート制限）が返ることがある。

通常の学習用途では (B) の CLI で足りる。出典：developers.google.com/colab/api、
docs.cloud.google.com/mcp。

## 無料枠・課金の注意

- 無料枠でも T4 は使えるが、可用性と上限はアカウントの状態と混雑に依存する。
  `eligible` は資格の表示であって、割当の保証ではない。
- コンピュートユニット残高がある場合はそちらから消費される。`colab usage` で
  実行前に残高と消費レートを確認すること。
- セッションは使い終わったら `colab stop` で解放する（`colab run` は自動解放）。

## 実証メモ（2026-11-18）

- google-colab-cli 0.8.0 / Linux（Python 3.12）で `colab run --gpu T4` が完走：
  Tesla T4、PyTorch 2.14.1+cu130、CUDA 計算 OK、終了時の解放まで正常。
- 認証はリモート・コピペ方式（ブラウザ承認→コード貼付）で1回のみ。以降は
  保存トークンで無人実行できた。
