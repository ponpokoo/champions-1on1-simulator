# 型スペックと CLI リファレンス

エンジン（[engine/engine.c](../engine/engine.c)）に渡す型スペックの文法と、コマンドラインの使い方をまとめます。探索の進め方は [skill/SKILL.md](../skill/SKILL.md) と [search/README.md](../search/README.md)、情報制約は [docs/observation_spec_v3.md](observation_spec_v3.md)（現行 v3；旧版 v1 / v2 は [docs/archive/](archive/)）を参照してください。

## 1. 型スペックの文法（v3・現行）

1 行が 1 つの型を表します。全種族共通の正規形は次のとおりです。

```
<種族>:<ポイント配分>|<技>|<方針>|<持ち物>|<メガ>|<特性>|<性格>|<性別>
```

例（ギャラドスの探索で決勝 1 位になった型）：

```
gyarados:14,7,0,7,6,32|fireblast,powerwhip,rest,roar|obsh4|gyaradosite|2|intimidate|4,1|f
```

### 種族とポイント配分

- 種族は [data/species_dataset.json](../data/species_dataset.json) のベース種族 ID（英語小文字）。対応は実装済みの全 341 種族。メガ形態そのものを種族に指定する書き方は正規形ではありません（メガは持ち物とメガ欄で表します）。
- ポイント配分は H, A, B, C, D, S の順に 6 つの数値（各 0〜32、合計ちょうど 66）。
  実数値への換算は、フォルムごとの基礎値（ポイント 0・個体値 31）にポイントを加え、最後に性格補正（上昇 ×1.1、下降 ×0.9、いずれも切り捨て）を掛けます。基礎値はデータセットと [engine/stage8/species_data.h](../engine/stage8/species_data.h) を参照してください。

### 技

- 4 つの異なる技を、英語小文字の技 ID（または生成済み短縮コード）で書きます（例：`earthquake`）。各種族の習得技はデータセットを参照してください。技の数値はチャンピオンズ仕様です。

### 方針（操縦役）

**誰が操縦するかで型の評価は変わる**ため、結果には必ず方針を併記してください。

| トークン | 内容 |
|---|---|
| `obsh4` | 観測 v3 のヒューリスティクス方針（全種族・現行の標準） |
| `obsh3` | 観測 v2 のヒューリスティクス方針（メタグロス 68 技用） |
| `learned` / `learned2` | 学習方針（観測 v1 / v2。68 技の `learned2` は対照超え未確認） |
| `obsh2` | 観測 v1 のヒューリスティクス方針（17 技用） |
| `obsdemo` / `obsdemo2` / `obsdemo3` | 観測インターフェース接続用の簡易デモ方針 |
| `mirror` / `noise` / `fast` / `stall` | 単純方針（検証マトリクス等） |
| `generic` | 全情報ベースライン（比較用と明示した場合のみ使用） |

### 持ち物・メガ・特性・性格・性別

- 持ち物は英語小文字の ID（例：`leftovers`, `lifeorb`、メガストーンは `gyaradosite` のような種族ごとの ID、なしは `none`）。実装済みは通常持ち物 85 種＋メガストーン 81 種です。
- メガ欄は `0`（変身しない）/ `1`（メガ形態で開始）/ `2`（通常形態で開始し、石を保持。メガシンカの宣言タイミングは方針が決める）。対応しない石やメガを持たない種族への `1` / `2` は不正として拒否されます。
  標準の評価前提は 1 ターン目メガであり、その根拠と種族別の検証手順は [search/README.md](../search/README.md) を参照してください。
- 特性は開始形態の特性候補のいずれか（英語小文字 ID）。実装済みは 217 種（初期 216＋戦闘中到達可能 1）です。メガ後の特性はフォルムデータで決まります。
- 性格は `上昇,下降` の 2 つの数値で、`1`=攻撃、`2`=防御、`3`=特攻、`4`=特防、`5`=素早さ、無補正は `0,0`。HP に性格補正は掛かりません。
- 性別は `m` / `f` / `n`（種族の性別比に従う合法値のみ）。めろめろ・ゆうわく・とうそうしん等が性別で変わるため型の一部として扱います。

### 旧形式（メタグロス専用・互換）

段階 6 までのメタグロス専用形式 `実数値6つ|技コード|方針|アイテム|メガ|特性[|性格]` も読み取り互換があります（例：`164,192,182,113,132,162|FS,IW,ST,EQ|obsh3|stone|1|cb`）。新規の型は v3 形式で書いてください。

## 2. ビルドと検証

```sh
# Linux / macOS
make verify
./engine/engine policy-version

# 単体ビルド（Windows MinGW 等）
gcc -O3 -march=native -o engine/engine engine/engine.c -lm
```

- Windows では `engine/engine.exe` が生成され、標準出力が CRLF（`\r\n`）になります。`matrix` の MD5 を確認するときは `\r` を除いて比較してください（`Makefile` の `verify` ターゲットおよび `search/search_controller.py` は `\r\n` を `\n` に正規化して照合します）。
- 検証前に環境変数 `ENGINE_SEQ`・`ENGINE_OBSERVER`・`ENGINE_OBS_COMPAT` が残っていないことを確認してください。

## 3. CLI リファレンス

以下、`<engine>` は `./engine/engine`（Windows は `engine/engine.exe`）と読み替えます。

| コマンド | 引数 | 出力 |
|---|---|---|
| `policy-version` | なし | 学習方針ごとの観測スキーマ・エンコーダ・重み SHA-256（1 方針 1 行） |
| `matrix` | `[試合数] [シード]` | 検証マトリクス（既定 `3000` / `12345`）。MD5 が `f3128c506954574a0c48bdfb79e2a917` と一致することが導入セルフチェック |
| `battle` | `<型A> <型B> <試合数> <シード>` | 「A 勝ち数 - B 勝ち数（引分数）平均ターン数」 |
| `screen` | `<候補ファイル> <プールファイル> <試合数> <シード>` | 候補ごとに「番号 勝 敗 分」を 1 行ずつ（プール全員との合計） |
| `screenx` | `<候補ファイル> <プールファイル> <試合数> <シード>` | 候補ごとに「番号」に続けてプール 1 人ごとの「勝 敗 分」を 1 行で。実行中は進捗が標準エラーに出力 |
| `trace` | `<型A> <型B> <シード>` | 1 試合の詳細ログ（全情報方針同士のみ。観測系方針では不可） |
| `diagnose-obsh4` | `<型A> <型B> <試合数> <シード> <A\|B> [ターン]` | 観測 v3 の同種ミラーで、対象席の合法技を 1 手ずつ差し替えた比較 CSV。ターンの省略時は 1〜6 |
| `compare-obsh4` | `<型A> <型B> <試合数> <シード> <A\|B> [stat-drop\|substitute\|setup\|setup-tempo\|opening]` | 相手を候補ごとの比較元に固定し、対象席だけ候補方針に切り替えた比較 CSV。省略時は `stat-drop` |
| `--help` / `-h` | なし | コマンド一覧の表示 |

### 使い方の注意

- スコア規約は **（勝 + 0.5 × 引分）/（勝 + 敗 + 引分）**（引き分けが出ない環境では `勝 / 試合数`）。結果には常に対戦母集団・試合数・操縦方針・シードを併記してください。
- `screen` / `screenx` の `<プールファイル>` は最大 512 型、1 行 511 文字までです（超過時はエラー終了します）。
- `screen` / `screenx` では候補は常に **A 席** に座ります。席による差をならす必要がある評価（決勝など）では、候補とプールを入れ替えた実行も行って両席を併せてください（[search/search_controller.py](../search/search_controller.py) の `finals` では同型リスト同士の `screenx` 結果から A 席基準の `ranking` と両席合算・自己ミラー除外の `ranking_both_seats` の両方を出力します）。
- 平均勝率だけでなく **最悪相性**（プール内で最も低い相手別スコア）も必ず確認してください。相手別の内訳は `screenx` で得られます。
- シードは試合ごとにハッシュで展開される決定的な規約で、同じ引数なら結果は再現します。

### 行動診断

2 つの型を `obsh4`、メガ欄を `0` にして実行します。差し替え直前の両席の観測、合法マスク、選択、乱数状態を照合します。相手の選択はそのターンも固定し、次のターンから通常の方針に戻します。

`diagnose-obsh4` の `alternative` が空の行は通常戦です。`baseline_attempt` と `alternate_attempt` には対戦処理が実行に進んだ技が入り、行動不能などで実行に進まなかった場合は空になります。命中や追加効果の発生は勝敗とは別に扱い、この欄では判定しません。勝敗は対象席から見た `win`、`loss`、`draw` です。

固定母集団によるスクリーニング、別シードでの確認、候補方針の保留評価には Node.js を使います。エンジンと出力先には Windows でも利用できるパスを渡してください。集計用 JSON には試合数・シード・型・エンジンの SHA-256 が入ります。保留評価は調整を終えてから実行します。

```sh
node search/diagnose_obsh4.cjs screen ./engine/engine runs/obsh4-diagnostics
node search/diagnose_obsh4.cjs confirm ./engine/engine runs/obsh4-diagnostics
node search/diagnose_obsh4.cjs holdout ./engine/engine runs/obsh4-diagnostics
make verify-obsh4
```

標準の `obsh4` は、みがわりが公開済みの攻撃を受け止める見込みと HP コストを比較します。相手のたべのこしと「まもる」の公開履歴・合法候補に応じて、攻撃を見送る価値も見積もります。シャワーズ戦の再現と、別の対面での保留評価には次を使います。

```sh
node search/diagnose_obsh4.cjs substitute-check ./engine/engine runs/obsh4-substitute
node search/diagnose_obsh4.cjs substitute-holdout ./engine/engine runs/obsh4-substitute
```

`setup` は、みがわり改善後の方針を比較元に、瞑想・剣の舞・竜の舞など6技の判断を評価します。相手の攻撃が公開された後、今攻撃する経路と、1ターン積んでから攻撃する経路を最大5ターン比較します。与被ダメージ、行動順、たべのこしの回復を近似し、積んだ後の攻撃技は選び直します。`setup-tempo` は両経路とも勝つ見込みのとき、先に倒せる方を優先します。

```sh
node search/diagnose_obsh4.cjs setup-check ./engine/engine runs/obsh4-setup
node search/diagnose_obsh4.cjs setup-holdout ./engine/engine runs/obsh4-setup
node search/diagnose_obsh4.cjs setup-tempo-check ./engine/engine runs/obsh4-setup
node search/diagnose_obsh4.cjs setup-tempo-holdout ./engine/engine runs/obsh4-setup
```

ニンフィアの標準 `obsh4` では、初手に特殊攻撃を選ぶ局面で、相手の公開情報から想定した一致技・その他の特殊技・物理技・めいそうを比較します。最大 5 ターンの近似で、どの想定でも評価を下げず、平均評価が上がるときにめいそうへ切り替えます。こだわり持ちは切り替えの対象外です。`compare-obsh4 ... opening` では、みがわり改善後の方針を比較元に、対象席だけこの初手判断を有効にできます。
