# ポケモンチャンピオンズ 1on1 型探索（1on1 シミュレーター）

本プロジェクトは、ポケモンチャンピオンズの同種ミラー 1on1 対戦を対象に、高速シミュレーションと情報制約つき評価・強化学習を用いて最適な型を探索するツールです。

実際のプレイヤーが得られる情報に制約した環境で多様な型との対戦結果を評価し、特定の相手に勝つためだけではなく、未知の相手に対しても高い勝率を期待できる型の発見を目指します。単一の最適な型が存在しない場合には、型同士の有利不利や三すくみ構造を分析します。

## 免責・出典

- 本プロジェクトは非公式のファンプロジェクトであり、任天堂・株式会社ポケモン・ゲームフリークとは関係がありません。ポケモンおよび名称・データの権利は各権利者に帰属します。
- 技・種族などのデータは Pokémon Showdown（MIT ライセンス）の champions mod、Serebii、PokeAPI を出典として突き合わせています。
- コード部分のライセンスは [LICENSE](LICENSE) を参照してください（データ部分は出典側の権利に従います）。

## リポジトリ構成

| パス | 役割・内容 |
|---|---|
| [engine/](engine/engine.c) | 単一 C ファイルの高速対戦エンジン（全 341 種族・511 技＋わるあがき・217 特性・通常持ち物 85 種＋メガストーン 81 種）。`stage3/`・`campaign3/` に学習済み重み、`stage8/` に生成済み種族・技・特性テーブルを収録 |
| [data/](data/species_dataset.json) | チャンピオンズ仕様のデータセット（[species_dataset.json](data/species_dataset.json)：341 形態・512 技・PP 上限 20 準拠、[type_chart.json](data/type_chart.json)：18×18 タイプ相性表） |
| [search/](search/README.md) | 任意の種族向けの規模選択・条件絞り込みつき型探索ツール（`obsh4` 操縦・[search_tiers.py](search/search_tiers.py) / [search_controller.py](search/search_controller.py)） |
| [training/](training/README.md) | PPO 強化学習パイプライン（**v1 / `MGO1`・17 技メタグロス専用**。Colab GPU ガイドは [training/colab/README.md](training/colab/README.md)） |
| [viewer/](viewer/index.html) | 全 341 種族対応 HTML ワークベンチ（[viewer/server.py](viewer/server.py) で自作型 1on1 テスト／絞り込み探索起動／`runs/*/finals_results.json` 分析・`file://` 静的閲覧対応） |
| [docs/](docs/spec_and_cli.md) | 型スペック文法・CLI（[spec_and_cli.md](docs/spec_and_cli.md)）、観測仕様 v3（[observation_spec_v3.md](docs/observation_spec_v3.md)）、既知仕様・評価規約（[known_issues.md](docs/known_issues.md)）。開発段階の仕様・記録は [docs/archive/](docs/archive/) に保管 |
| [skill/](skill/SKILL.md) | 1on1 型探索の汎用分析手順（AI エージェント向けスキル定義） |

## 仕組みの要点

### 1. 実機準拠の同時選択と完全な再現性
- 対戦エンジン [engine/engine.c](engine/engine.c) は外部ライブラリにほぼ依存しない（`-lm` のみ）C 実装です。
- **両者の技選択を「同時」に処理**します。片方の行動を見てから他方を決める逐次選択にすると先後・読み・勝率が実機と逆転するため、逐次選択を禁止しています。
- 乱数は試合インデックスからハッシュで決定的に生成し、検証マトリクス（17 技プール、3000 戦、seed=12345）の出力 MD5（`f3128c506954574a0c48bdfb79e2a917`）を固定しています。

### 2. 情報制約（部分観測）の徹底
実際のプレイヤーが対戦画面から得られる情報だけを操縦方針に渡します（相手 HP は整数パーセント表示、技は使用されてから判明、相手の実数値点値は渡さない等）。  
全種族対応の現行仕様 [docs/observation_spec_v3.md](docs/observation_spec_v3.md)（`MGO3`）では、公開情報・公開履歴から導ける「相手の特性・持ち物・技の可能性集合」と「能力値の実数値区間」を belief として保持し、標準方針 `obsh4` がこれを参照して行動を決定します。

### 3. 役割で候補を切らない探索
「物理アタッカー」「耐久型」といった先入観で候補を足切りせず、数学的に同一・劣位な型（性格↓能力への無駄ポイント、全能力 16 以下の均等型）のみを除外したうえで、段階的スクリーニングと決勝総当たりにより「平均勝率」と「最悪相性」を評価します（[skill/SKILL.md](skill/SKILL.md)、[search/README.md](search/README.md)）。

## クイックスタート

### エンジンのビルドと動作確認

```sh
# Linux / macOS
make verify
./engine/engine policy-version

# 単体ビルド（Windows MinGW 等）
gcc -O3 -march=native -o engine/engine engine/engine.c -lm
```

検証マトリクス（`matrix 3000 12345`）の MD5 が観測 OFF / 互換観測 ON（`ENGINE_OBSERVER=1 ENGINE_OBS_COMPAT=1`）ともに `f3128c506954574a0c48bdfb79e2a917` と一致すれば導入セルフチェックは正常です（Windows では改行 `\r` を除いて照合します。詳細は [docs/spec_and_cli.md](docs/spec_and_cli.md)）。

### 任意の種族で型探索を実行

```sh
python3 search/search_tiers.py gen gyarados light runs/gyarados-light
python3 search/search_controller.py runs/gyarados-light
```

規模ごとの候補数・試合数・所要時間の目安・絞り込みオプションは [search/README.md](search/README.md) を参照してください。1 時間程度で終えたい場合は `standard` までが目安です。

### HTML ワークベンチの起動

```sh
python3 viewer/server.py
```

標準ライブラリのみでローカルサーバ（既定 `http://127.0.0.1:8765/`）が起動し、全 341 種族の自作型テスト（`battle` / `generic` 比較ログ `trace` / 探索済み決勝プールへの `screenx` 検証）、条件絞り込み付き探索の起動・進捗監視、`runs/*/finals_results.json` の分析をブラウザ上で操作できます。[viewer/index.html](viewer/index.html) をブラウザへ直接開いた場合（`file://`）でも、種族・技・Lv.50 実数値・定義 A/B 確認とローカルの `finals_results.json` 読み込み・分析が利用できます。

## 動作要件

- **エンジン / 探索 / HTML ワークベンチ**：C コンパイラ（`gcc`）と `libm`（Linux / macOS / Windows MinGW で動作確認済み）＋ Python 3（標準ライブラリのみ、外部 Web フレームワーク不要）。スクリーニング・総当たり・軌跡生成は CPU 並列でスケールします（`obsh4` 同士で Core i5-12400・10 並列時に約 8,000〜10,000 試合/秒）。
- **学習**：Python 3 + `numpy` + `torch`（[training/requirements.txt](training/requirements.txt)。GPU 推奨、ない場合は [training/colab/README.md](training/colab/README.md) を参照）。

## 主な知見・現在の結果

- **単一の最強の型は存在しない**：メタグロス 17 技（`run1` / `run2`）・68 技（`run3`）・ギャラドス探索のいずれでも三すくみ構造が確認され、想定する相手分布によって 1 位が入れ替わります（探索出力 `runs/<名前>/finals_results.json` を [viewer/index.html](viewer/index.html) の結果分析タブに読み込むと分布スライダーで確認可能）。
- **17 技空間の PPO 学習（`g12`）**：情報制約つきのまま保留プール 176 型で対 `obsh2` 73.7%・対全情報 `generic` 74.6% を達成（詳細は [docs/archive/findings.md](docs/archive/findings.md)）。
- **68 技・全種族対応**：68 技空間の学習方針（`learned2`）は未学習の `obsh3` 超えが確認できていないため、68 技の現行標準は `obsh3`、全 341 種族の現行標準は観測 v3 の `obsh4` です。
- **メガシンカのタイミング**：メタグロスとギャラドスでは「1 ターン目メガ」が実測で正当と確認済みです。種族別の検証手順は [search/README.md](search/README.md) に記載しています。
- **既知の規約**：てっていこうせん等の自爆系両倒れは「使用者の負け」と扱います。詳細は [docs/known_issues.md](docs/known_issues.md) を参照してください。

## AI エージェント向け

AI エージェントに操作させる場合は [AGENTS.md](AGENTS.md) を参照してください。
