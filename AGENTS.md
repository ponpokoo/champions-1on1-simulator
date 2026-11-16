# AGENTS.md

配布リポジトリ直下で実行する。

## ビルド・検証

```sh
make verify
./engine/engine policy-version
gcc -O3 -march=native -o engine/engine engine/engine.c -lm
```

- 最初に `make verify` を通す。次に `policy-version` で重みハッシュを確認する。上のgccは単体ビルド用。
- 型スペックの文法とコマンドの使い方は `docs/spec_and_cli.md` を参照する。
- `./engine/engine matrix 3000 12345` のMD5は `f3128c506954574a0c48bdfb79e2a917`。観測OFF／互換観測ON（`ENGINE_OBSERVER=1 ENGINE_OBS_COMPAT=1`）とも一致させる。検証前に `ENGINE_SEQ`・`ENGINE_OBSERVER`・`ENGINE_OBS_COMPAT` を解除する。

## 厳守

- 技は両者同時に選択させる。逐次選択にしない。
- `docs/observation_spec*.md` の許可情報だけを方針に渡す。全情報方針は「全情報ベースライン」と明示した比較に限る。
- RNGシードは試合インデックスからハッシュで決定的に導く。乱数消費順の変更はMD5を変えるため注意する。
- 全結果に対戦母集団・試合数・操縦方針・エンジン／重みハッシュを付ける。スクリーニング・決勝・訓練・未見型の保留評価を区別する。母集団なしに「最強」と主張しない。
- てっていこうせん等の両倒れは使用者の負けとする。`docs/known_issues.md` の規約を勝手に変えない。
- 挙動変更後は `make verify` と関連検証（観測変更は仕様の受け入れ条件）を通す。

## 探索・配置

- 型探索は `skill/SKILL.md` に従う。数学的に同一な型だけ畳み、役割で候補を切らず、平均勝率と最悪相性を評価する。
- エンジン本体：`engine/engine.c`。方針ヘッダ・重み：`engine/stage3/`（観測v1・学習方針 g12 の `weights.h`）と `engine/campaign3/`（観測v2側のヘッダ。68技の現行操縦役はヒューリスティクス方針 `obsh3` で、学習方針 learned2 は対照超えが確認できていない）。
- 学習：`training/`。軌跡生成はCPU、学習はGPU。GPUがなければ `training/colab/`・`training/modal/` に従い学習のみ外部GPUで行う。学習後は未見型で保留評価する。
- 結果：`viewer/index.html`・`viewer/data/`。データ更新時は上記の結果メタデータも更新する。
