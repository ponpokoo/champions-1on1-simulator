# AGENTS.md

配布リポジトリ直下で実行する。

## ビルド・検証

```sh
make verify
./engine/engine policy-version
gcc -O3 -march=native -o engine/engine engine/engine.c -lm
```

- 最初に `make verify`（Windows の非 UNIX シェル環境では上記 `gcc` でビルドし、`\r` を除いた出力で `matrix 3000 12345` の MD5 を照合）を通す。次に `policy-version` で重みハッシュを確認する。
- 型スペックの文法とコマンドの使い方は `docs/spec_and_cli.md` を参照する。
- `./engine/engine matrix 3000 12345` の MD5 は `f3128c506954574a0c48bdfb79e2a917`。観測 OFF／互換観測 ON（`ENGINE_OBSERVER=1 ENGINE_OBS_COMPAT=1`）とも一致させる。検証前に `ENGINE_SEQ`・`ENGINE_OBSERVER`・`ENGINE_OBS_COMPAT` を解除する。

## 厳守

- 技は両者同時に選択させる。逐次選択にしない。
- `docs/observation_spec_v3.md`（旧版は `docs/archive/`）の許可情報だけを方針に渡す。全情報方針は「全情報ベースライン」と明示した比較に限る。
- RNG シードは試合インデックスからハッシュで決定的に導く。乱数消費順の変更は MD5 を変えるため注意する。
- 全結果に対戦母集団・試合数・操縦方針・エンジン／重みハッシュを付ける。スクリーニング・決勝・訓練・未見型の保留評価を区別する。母集団なしに「最強」と主張しない。
- てっていこうせん等の両倒れは使用者の負けとする。`docs/known_issues.md` の規約を勝手に変えない。
- 挙動変更後は `make verify` と関連検証（観測変更は仕様の受け入れ条件）を通す。

## 探索・配置

- 型探索は `skill/SKILL.md` に従う。数学的に同一な型だけ畳み、役割で候補を切らず、平均勝率と最悪相性を評価する。
- 種族別の探索は `search/` の規模選択ツールを使う（使い方は `search/README.md`）。規模は所要時間の予算から選ぶこと（軽量→標準の順に上げるのが基本。精密は数時間級）。
- メガシンカのタイミング検証は `search/README.md` の手順（机上チェック→オラクル上限→+3 ポイントの線引き）に従い、必要な種族だけ別途行う。標準の前提は 1 ターン目メガ。
- エンジン本体：`engine/engine.c`（全種族データは `engine/stage8/` のヘッダ群）。方針ヘッダ・重み：`engine/stage3/`（観測 v1・学習方針 g12 の `weights.h`）と `engine/campaign3/`（観測 v2 側のヘッダ）。現行の標準操縦役は、全種族では観測 v3 の `obsh4`、メタグロス 68 技では `obsh3`（学習方針 `learned2` は対照超えが確認できていない）。
- 学習：`training/`（対象は v1 / `MGO1` 17 技メタグロス）。軌跡生成は CPU、学習は GPU。GPU がなければ `training/colab/README.md` に従い学習のみ外部 GPU で行う（Modal 用スクリプトは未同梱）。学習後は未見型で保留評価する。
- 結果：`viewer/index.html`・`viewer/server.py`（全 341 種族 Stage 8 ワークベンチ：自作型テスト・絞り込み探索・`runs/*/finals_results.json` 分析、メタデータ生成は `viewer/build_metadata.py`）。データ更新時は結果メタデータも更新する。
