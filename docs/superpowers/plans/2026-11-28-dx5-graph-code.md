# DupliX 2 の v5 グラフ（値の線・Field・コード・3 つの表示）実装計画

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 設計書 v5 の P1.5a〜d を、利用者が AE（`dev_plugins\DupliX2.aex`）で確かめられるところまで作る: エディターの Code 表示でコードを書いて適用すると AE の絵が変わり、ノード表示・積み重ね表示で同じグラフを触れ、Int Macro が Effect Controls に出る。

**Architecture:** 新しい核を `src/dxg5`（名前空間 `dx5`、AE も Win32 も含まない）に作り、v2 エフェクト（`src/v2`）の参照を `dxg` から `dx5` に切り替える。古い核 `src/graph` は版 1 のグラフの読み込み（`dxg::Parse`）と、版 1 から来たノードの C++ の実装（12 節の差し替え）のためだけに残す。言語は「字句 → 構文木 → グラフ」と「グラフ → コード」の 2 方向で、どちらも `dx5` の中。

**Tech Stack:** C++17、MSVC v143（`tools/build.ps1`）、Win32 / GDI（エディター）、AE SDK（SmartFX）。試験は `tests/dxg5/dxg5_test.exe`（`tests/graph` と同じ自前の CHECK）と、AE の自動試験 `tests/ae/run_dx2_graph.ps1`。

**Spec:** `docs/design/複製プラグイン設計案_v5.md`（以下「v5」）。v4 を参照するところは `docs/design/複製プラグイン設計案_v4.md`。

## Global Constraints

- 値は時刻・要素・グラフ・マクロ・Seed・レイヤーだけで決まる。前のフレームの結果を使わない（v5 冒頭）。
- `src/dxg5` は AE・Win32 のヘッダーを含まない（Mac でも同じものを使う）。
- Effect Controls の既存のパラメータの番号・Disk ID は変えない。Int Macro は **69 ＝ グループ「Int Macros」の始まり、70〜101 ＝ Int Macro 1〜32（`DupliX2-0070`〜`DupliX2-0101`）、102 ＝ 終わり**（v5 4.4）。`P_GRAPH` は 68 のまま。
- 数値の文字化は `std::to_chars`（ロケールに依らない最短の形、v4 8.1）。
- 構造の入力の丸めは四捨五入（0.5 は 0 から遠い方）してから範囲に収める（v4 8.1）。要素の総数は 10 万で打ち切り、警告を残す。
- `/` は Float の割り算、`//` は床の割り算、`%` は床に合わせた余り（v5 3）。
- 一度付いた key は変わらない。要素の ID は key から作る（v5 11）。版 1 から来たノードは P1 と同じ ID（`dxg::MixId(ノードの番号, 要素の番号)`）。
- グラフデータは版 2 の行の形（v5 7）。版 1 も読める。
- エディターの書き込みは `ed::Commit`（Idle Hook、取り消し 1 回分、v4 7）。ドラッグ中は書かない。
- 利用者への文言（エディターの表示・誤り）は英語（今のエディターに合わせる。利用者の母語での表示は求められていない）。

## Review Focus

1. **AI や利用者が書いた、通らないコード**（知らない名前・型の違い・構造の入力に Field・輪）: 読み取りが行と列つきの誤りを返し、グラフは変わらない。→ Task 5 の `lower_rejects_*` の試験。
2. **P1 で作ったプロジェクト（版 1 のグラフ）を開く**: 同じ要素・同じ ID（Source の Random の絵が変わらない）。→ Task 2 と Task 7 の `v1_*` の試験。
3. **コードを適用し直しても乱数が変わらない**（変数名が同じ）: → Task 5 の `reapply_keeps_keys` の試験。
4. **巨大な Count（マクロに 1e9）**: 10 万で打ち切り、AE が固まらない。→ Task 3 の `points_cap` の試験。
5. **エディターのコード欄で打った文字が AE のショートカットにならない、Ctrl+Enter で適用・Ctrl+Z は AE の取り消し**: → Task 10 の AE 試験の手順（窓にキーを送る）と、利用者の試用の確認事項。

---

## ファイルの構成

| ファイル | 役割 |
|---|---|
| `src/dxg5/value.h/.cpp` | 型（`Type`）、値（`Value`）、回転の演算、`Num`（数の文字化） |
| `src/dxg5/geometry.h/.cpp` | 並び（`Geometry`: 属性の列の集まり）、組み込みの属性の名前 |
| `src/dxg5/nodes.h/.cpp` | ノードの種類の表（`NodeDef`: 入力・出力・既定値・構造の入力か）。基本のノードと組み込みのグループの両方 |
| `src/dxg5/graph.h/.cpp` | グラフ（ノード・線・マクロの設定）、版 2 の保存と読み込み、版 1 からの変換 |
| `src/dxg5/eval.h/.cpp` | 評価（ジオメトリのノード、Field の配列ごとの評価、ID）。結果は今の `dxg::Element` に直して返す |
| `src/dxg5/lang_lex.cpp`・`lang_ast.h`・`lang_parse.cpp` | 字句・構文木・構文解析 |
| `src/dxg5/lang_lower.cpp` | 構文木 → グラフ（名前の解決・型・key の対応） |
| `src/dxg5/lang_print.cpp` | グラフ → コード |
| `src/dxg5/lang.h` | 言語の公開の入口（`ParseCode`・`PrintCode`・`ParseExpression`） |
| `src/dxg5/groups.cpp` | 組み込みのグループの定義（コードの文字列）と C++ の差し替え |
| `src/dxg5/edit.h/.cpp` | エディターの操作・プリセット・試験の台本（今の `src/graph/edit` の v5 版） |
| `tests/dxg5/dxg5_test.cpp`・`dxg5_test.vcxproj` | 核の試験（`tools/build.ps1 -Target dxg5`） |
| `src/v2/*` | 参照を `dx5` に切り替え、Int Macro、Code / Stack 表示、ノード表示の作り直し |

---

### Task 1: 型・値・並び・ノードの表

**Files:**
- Create: `src/dxg5/value.h`, `src/dxg5/value.cpp`, `src/dxg5/geometry.h`, `src/dxg5/geometry.cpp`, `src/dxg5/nodes.h`, `src/dxg5/nodes.cpp`
- Create: `tests/dxg5/dxg5_test.cpp`, `tests/dxg5/dxg5_test.vcxproj`（`tests/graph/graph_test.vcxproj` を写し、`..\..\src\dxg5\*.cpp` と `..\..\src\graph\*.cpp` をコンパイル）
- Modify: `tools/build.ps1`（`-Target dxg5` → `build\dxg5\dxg5_test.exe`、`all` に含める）

**Interfaces:**
- Produces:
  - `enum class Type { Geometry, Float, Int, Bool, Vector, Rotation, Color }`
  - `struct Rot { double m[9]; }`（行優先、列＝コピーの x, y, z 軸。`dxg::Element::basis` と同じ並び）、`struct Vec3 { double x, y, z; }`
  - `struct Value { Type type; double f; int64_t i; bool b; Vec3 v; Rot r; }` と `Value::F(double)`・`I(int64_t)`・`B(bool)`・`V(Vec3)`・`R(Rot)`、`double AsFloat(const Value&)`、`int64_t AsInt(const Value&)`（四捨五入）、`bool Convert(const Value&, Type, Value*)`（v5 3 の暗黙の変換だけ）
  - `Rot RotXYZ(double rx, double ry, double rz)`（R ＝ Rz・Ry・Rx、度。`dxg::RotationXYZ` と同じ値）、`Rot operator*(Rot, Rot)`、`Vec3 operator*(Rot, Vec3)`、`Rot Inverse(Rot)`
  - `struct Column { Type type; std::vector<double> f; std::vector<int64_t> i; std::vector<Vec3> v; std::vector<Rot> r; }`（型に合う 1 つだけを使う。Bool は `i` に 0/1）
  - `struct Geometry { size_t count; std::vector<uint64_t> id; std::map<std::string, Column> attrs; }`。組み込みの属性は `attrs` の決まった名前: `position`（Vector）・`rotation`（Rotation）・`scale`（Vector）・`opacity`（Float）・`source`（Int、既定 −1）・`visible`（Bool、既定 1）・`weight`（Float、既定 1）・`q`（Float）・`s`（Int）。`Geometry MakePoints(size_t n)` は既定の値で埋める（`q`・`s` は Index の Order）。`index`・`count`・`id` は属性ではなく評価が作る。
  - `struct InputDef { std::string name; Type type; Value dflt; bool structural; bool field; double softMin, softMax; std::string choices; }`、`struct OutputDef { std::string name; Type type; bool field; }`、`struct NodeDef { std::string name; std::string category; std::vector<InputDef> inputs; std::vector<OutputDef> outputs; bool group; }`、`const NodeDef* FindDef(const std::string& name)`、`const std::vector<NodeDef>& AllDefs()`

- [ ] **Step 1: 試験を書く** `value_rot_matches_v1`（`RotXYZ(10, 20, 30)` の 9 要素が `dxg::RotationXYZ(10, 20, 30, …)` と 1e-12 で一致、`RotXYZ(0,0,90) * Vec3{1,0,0}` が (0,1,0)）、`value_convert`（Int→Float、Float→Int は 2.5→3・−2.5→−3、Bool→Float、Float→Vector、Vector→Float は false）、`points_defaults`（`MakePoints(3)` の `visible` が 1、`source` が −1、`q` が 0, 0.5, 1）、`defs_have_points_and_set_position`（`FindDef("points")` の入力 `count` が structural・Int、`FindDef("set_position")` の入力が `geometry`・`position`（field）・`where`（field、Bool、既定 true））
- [ ] **Step 2: 失敗を確かめる** Run: `pwsh tools/build.ps1 -Target dxg5`。Expected: コンパイル誤り（ファイルがない）
- [ ] **Step 3: 実装する** ノードの表（`nodes.cpp`）には、この計画で使う基本のノードをすべて入れる: `points`、`set_position`・`set_rotation`・`set_scale`・`set_opacity`・`set_source`・`set_visible`・`set_weight`（どれも入力 `geometry`、値、`where`）、`store`（`geometry`・`name`（文字列は `choices` を使わず、ノードの文字列の設定として持つ。Task 2 の `Node::text`）・`value`・`where`）、`capture`（出力 `geometry` と `value`）、`delete`、`output`（`geometry`・`copy_scale` 既定 100）、値: `position`・`index`・`id`・`count`・`time`・`seed`・`rotation`・`scale`・`opacity`・`attr`（文字列の設定）・`macro`（設定 `k`）・`int_macro`（設定 `k`）・`random`（設定 `salt`）・`expr`（式の文字列の設定）・`math_*`（`add sub mul div floordiv mod pow neg eq ne lt le gt ge and or not sin cos tan asin acos atan2 sqrt abs floor ceil round min max clamp mix smoothstep len normalize dot cross vec sep_x sep_y sep_z rot rot_mul rot_apply inverse switch`）・`at`・`accumulate`・`bounds`（出力 `min`・`max`・`center`）・`reduce`（設定 `op` ＝ sum/min/max/mean）・`layer_set`（入力 `k`、structural）。組み込みのグループ（`group=true`）は Task 7 で足す。
- [ ] **Step 4: 通ることを確かめる** Run: `pwsh tools/build.ps1 -Target dxg5; build\dxg5\dxg5_test.exe`。Expected: `ALL PASS`
- [ ] **Step 5: Commit**

### Task 2: グラフ・版 2 の保存・版 1 からの変換

**Files:**
- Create: `src/dxg5/graph.h`, `src/dxg5/graph.cpp`
- Test: `tests/dxg5/dxg5_test.cpp`

**Interfaces:**
- Consumes: Task 1 の `NodeDef`・`Value`
- Produces:
  - `struct Link { uint32_t from = 0; int output = 0; }`（`from == 0` は線なし）
  - `struct InputSlot { std::string name; Value value; Link link; }`
  - `struct Node { uint32_t id; std::string key; std::string type; double x, y; std::vector<InputSlot> inputs; std::string text; int setting = 0; uint64_t salt = 0; uint32_t zone = 0; uint32_t legacyId = 0; std::vector<std::string> unknownLines; }`（`text` は Expression の式・Store / Attr の名前・Each のローカル名。`setting` は Macro の番号など。`zone` は所属する Each ゾーンのノード番号、0 は外。`legacyId != 0` は版 1 から来たノード）
  - `struct MacroSetting { std::string name; double sliderMin = 0, sliderMax = 100; }`
  - `struct Graph { uint32_t revision = 0, nextId = 1; std::vector<Node> nodes; MacroSetting macros[32], intMacros[32]; std::vector<std::string> unknownLines; Node* Find(uint32_t); const Node* Find(uint32_t) const; const Node* FindKey(const std::string&) const; const Node* OutputNode() const; }`
  - `uint32_t AddNode(Graph&, const std::string& type, const std::string& key, double x, double y)`（入力を表の既定値で埋める。key が空なら `<type><番号>`。重なれば `_2`…）、`std::string Serialize(const Graph&)`、`bool Parse(const std::string&, Graph*, std::string* error)`（版 1 も受ける）、`std::string UniqueKey(const Graph&, const std::string& want)`
- 版 2 の形（v5 7。1 行 1 文、最初の行 `DupliX graph 2`）:
  - `revision <n>` / `nextid <n>` / `macro <k> <lo> <hi> [name]` / `imacro <k> <lo> <hi> [name]`
  - `node <id> <type> <x> <y> <key> [legacy=<id>] [zone=<id>] [salt=<hex>] [setting=<n>]`
  - `in <node> <input> const <値>`（Float `f:1.5`、Int `i:3`、Bool `b:1`、Vector `v:1,2,3`、Rotation は保存しない（定数の回転は `rot` ノード））
  - `in <node> <input> link <from-node> <output>`
  - `text <node> <文字列の残り全部>`
  - 知らない文はそのまま書き戻す。知らない種類のノードは型 `?<名前>` として保持し、入力は文字のまま書き戻す。
- **版 1 からの変換**（`dxg::Parse` で読んでから）: Grid / Circle / Line / Transform / Modify / Source / Output → 種類 `grid` / `circle` / `line` / `transform` / `modify` / `source` / `output`（Task 7 の組み込みのグループの名前）、key は `<種類><版 1 の番号>`、`legacyId` ＝ 版 1 の番号。入力の名前は版 1 のキー（`countX` → `cx` のように Task 7 の表で対応）。`base` だけなら定数。`base == 0`・リンク 1 つ・深さ 1 なら Macro ノード（key `m<k>`、1 つのマクロに 1 つのノードを共用）からの線。それ以外は Expression ノード（式 `base + depth*m1 + …`、key `<使う先の key>.<入力>`）。版 1 の線は `geometry` 入力への線。macro 文は `macros[]` へ。

- [ ] **Step 1: 試験を書く** `v2_roundtrip`（ノード 3・線・定数 4 種・text・知らない文 1 行を持つグラフを `Serialize` → `Parse` → `Serialize` で同じ文字）、`v2_unknown_node_kept`（`node 5 wobble …` と `in 5 amount const f:0.5` が書き戻される）、`v1_default_preset_converts`（`dxg::Serialize(dxg::DefaultPreset().graph)` を `Parse` すると `grid` と `output` と Macro ノード `m1`〜`m3`、`grid1` の `legacyId == 1`、`cx` の線が `m1` から）、`v1_expression_link`（版 1 の `param 1 spacingX 10 m2 2` → Expression ノードの `text == "10 + 2*m2"`）
- [ ] **Step 2: 失敗を確かめる**（コンパイル誤り）
- [ ] **Step 3: 実装する**
- [ ] **Step 4: 通ることを確かめる** Expected: `ALL PASS`
- [ ] **Step 5: Commit**

### Task 3: 評価（ジオメトリと Field）

**Files:**
- Create: `src/dxg5/eval.h`, `src/dxg5/eval.cpp`
- Test: `tests/dxg5/dxg5_test.cpp`

**Interfaces:**
- Consumes: Task 1・2
- Produces:
  - `struct EvalContext { double time = 0; int seed = 0; std::function<double(int k, double t)> macro; std::function<int64_t(int k, double t)> intMacro; uint32_t layersSet = 0; size_t maxElements = 100000; }`
  - `dxg::EvalResult Evaluate(const Graph&, const EvalContext&)`（結果の形は今の `dxg::EvalResult`・`dxg::Element` のまま。描画と dump を変えないため）
  - `std::string FormatElements(const dxg::EvalResult&)` は `dxg::FormatElements` をそのまま使う
- 決まり:
  - ジオメトリのノードは `Geometry` を返す。値のノードの評価は `Column EvalField(uint32_t node, int output, const Geometry& ctx)`（文脈の並びの要素数の列。単一の値は長さ 1 の列にして広げる）を、(ノード, 出力, 文脈) ごとに 1 回の評価の中で使い回す。
  - 文脈（v5 5 の表）: Set 系・Store・Capture・Delete はジオメトリの入力、`points` は作る要素（`index` 0〜n−1、`count` n）。
  - 構造の入力は文脈なしで評価する（Field が来たら 0 と警告。読み取りが拒むので通常は起きない）。
  - `id`: `points` の要素は `MixId(Hash(key), i)`（`Hash` は FNV-1a 64 の文字列ハッシュ）。グループの中の `points` は `MixId(Hash(呼んだノードの key + "/" + 中の key), i)`。`legacyId != 0` のノードは `dxg::MixId(legacyId, i)`。
  - `random`: `dxg::Rand01(id, seed, salt)`（salt が 0 なら `Hash(key)` の下位 32 ビット）。
  - Set 系は `where` が真の要素だけ書き換える。`set_rotation` は回転を置き換える（足すのは `rotation * rot(…)` と書く）。
  - `at(h, i)`: `i` を [0, count−1] に寄せる。`accumulate(v)`: 文脈の並びの番号順の前方の和。
  - `bounds(g, f)`・`reduce(g, f)`: 単一の値（空なら 0）。
  - Output: `copy_scale / 100` を `EvalResult::copyScale` に。並びを `Element` に直す（`index`、`q`・`s` は属性のまま、`basis` ＝ `rotation`、`source`・`opacity`・`visible`・`weight`）。描画は `visible` の偽を描かない（今の `Element::visible`）。
  - 要素の総数が `maxElements` を超えたら切り、`warnings` に `"cut to 100000 elements"`。

- [ ] **Step 1: 試験を書く** `field_sin_index`（`points(5)` → `set_position(position + vec(0, sin(index*0.3)*20, 0))` → output を手でグラフに組み、要素 3 の y が `sin(0.9)*20`）、`field_context_differs`（同じ `index` の値のノードを 2 つの `set_position` につなぎ、並びの数 3 と 5 で別々に評価される）、`where_selects`（`where: index % 2 == 0` で偶数だけ動く）、`points_cap`（Count 1e9 で 100000 個と警告、計算は 1 秒以内）、`ids_from_key`（key `a` と `b` の `points` で ID が違い、key が同じなら同じ）、`legacy_ids`（`legacyId = 3` の `points` の ID が `dxg::MixId(3, i)`）、`accumulate_and_at`（`accumulate(1)` が 1,2,3…、`at(g, index-1).position` が端で寄る）、`capture_rides_along`（Capture の後に `delete where index == 0` しても、残った要素の写した値が元の要素のもの）
- [ ] **Step 2: 失敗を確かめる**
- [ ] **Step 3: 実装する**
- [ ] **Step 4: 通ることを確かめる**
- [ ] **Step 5: Commit**

### Task 4: 字句と構文解析（構文木）

**Files:**
- Create: `src/dxg5/lang_ast.h`, `src/dxg5/lang_lex.cpp`, `src/dxg5/lang_parse.cpp`, `src/dxg5/lang.h`
- Test: `tests/dxg5/dxg5_test.cpp`

**Interfaces:**
- Produces（`lang_ast.h`）:
  - `struct Pos { int line, col; }`、`struct Diag { Pos pos; std::string msg; }`
  - `struct Expr`（種類: `Num`・`Str`・`Name`・`Member`（`a.b`）・`Call`（関数名・位置の引数・名前の引数）・`Unary`・`Binary`・`Cond`（`c ? a : b`）・`Lambda`（`(e) => 式`）、`Pos pos`、子は `std::unique_ptr<Expr>`）
  - `struct Stmt`（種類: `Assign`（名前のリスト 1〜2 個 = 式。`g_b, b = capture(...)` のため）・`CompoundAssign`（`+= -= *= /=`、Each の中だけ）・`ExprStmt`・`Each`（出力名（なければ空）・入力の名前・本体）・`If`（条件・then・else）・`Group`（名前・引数（名前・型の注記・`structural`・既定値）・本体・`return` の式）・`Return`）
  - `struct Program { std::vector<Stmt> stmts; }`
  - `bool ParseProgram(const std::string& src, Program* out, Diag* err)`、`bool ParseExpr(const std::string& src, std::unique_ptr<Expr>* out, Diag* err)`
- 文法（演算子の強さは低い順）: `?:` < `||` < `&&` < `== !=` < `< <= > >=` < `+ -` < `* / // %` < 単項 `- !` < `**`（右結合）< 後置（呼び出し `()`・`.名前`・`.(式)`）。文は改行か `;` で区切る。注記は `//…` と `/*…*/`（`//` は行の頭か空白の後で注記、式の中の `a // b` は演算子: 字句は直前の字句が値で終わるなら演算子とみなす）。数は `1`、`1.5`、`1e3`、`0x3fa1`。文字列は `"…"`（`\"` と `\\` だけ）。
- [ ] **Step 1: 試験を書く** `parse_precedence`（`a + b * c ** 2 ** 3` の木、`-2 ** 2` は `-(2**2)`）、`parse_floordiv_vs_comment`（`x = a // b` は割り算、`x = 1 // note` は注記つきの 1、`// line` は注記）、`parse_each_if`（`g2 = each g { if t < 0.5 { scale *= 0.5 } else { opacity = 0 } }`）、`parse_lambda_member_call`（`p = (e) => e.position + e.parent.index`、`b = g.position`、`b = g.(index * 2)`、`g_b, b = capture(g, position)`）、`parse_group`（`group grid(cx: int structural = 5, sx = 100) { … return g }`）、`parse_errors`（`x = (1 +` は行 1 列 8 付近の誤り、`each { }` は誤り）
- [ ] **Step 2〜5:** 失敗を確かめる → 実装 → 通る → Commit

### Task 5: 構文木 → グラフ（lower）

**Files:**
- Create: `src/dxg5/lang_lower.cpp`
- Modify: `src/dxg5/lang.h`
- Test: `tests/dxg5/dxg5_test.cpp`

**Interfaces:**
- Consumes: Task 2〜4
- Produces: `bool CodeToGraph(const std::string& code, const Graph& previous, Graph* out, std::vector<Diag>* errors, std::vector<Diag>* notes)`（`previous` の key が同じノードの位置・salt・legacyId を引き継ぐ。マクロの設定（名前・範囲）は `previous` のまま。`revision` は `previous` のまま、呼んだ側が上げる）
- 決まり（v5 の節）:
  - 名前の解き方（v5 4.2）: ① Each のローカル ② 文の変数 ③ グループの引数 ④ 組み込みの属性と値（`position index id count time seed rotation scale opacity source visible weight q s`、`m1`〜`m32`、`i1`〜`i32`）⑤ 文脈の並びにあると分かる名前付きの属性（上流の `store` と、組み込みのグループの「残す属性」の表）。なければ誤り `unknown name 'x'`。`attr("x")` は確かめない。
  - `p = position`（文の外で、裸の Field の名前だけ・または Field を含む式を `(e) =>` なしで変数に入れる）は誤り `a field in a variable needs (e) => ...; to copy values now write g.position`（v5 5.1）。
  - `b = g.position`: `g` の流れに Capture（key `b`）を入れ、以後の `g` は Capture の出力。`g.(式)` も同じ。
  - 関数の呼び出し: 名前が `FindDef` にあればそのノード。引数は位置か名前。足りない入力は既定値。型が合わなければ `type mismatch: <input> wants <Type>`。構造の入力に Field なら `<input> cannot vary per element`。
  - 演算子の式は、値のノードの木のうち演算子・数の関数・入力の値だけでできた最大の部分を **1 つの Expression ノード**（`text` に字句を整えた式）にする。`add(a, b)` の形は Math のノード。
  - key（v5 11）: 変数名。同じ名前への 2 回目以降の代入は `_2`、`_3`（`notes` に `renamed s to s_2: write distinct names to keep random values`）。式の中のジオメトリのノードは `<使う先の key>_<入力の名前>`。式の中の値のノードは `<使う先の key>.<入力の名前>`。Each の中は `<ゾーンの key>.<文の番号>`。Macro ノードは `m<k>` / `i<k>`（1 つを共用）。
  - Each（v5 9）: ゾーンのノード（種類 `each`、入力 `geometry`、出力 `geometry`）と、中の Set 系・Store・Capture（`zone` ＝ ゾーンの番号）。`+=` などは Set 系に直す（`position += v` → `set_position(position + v)`）。`if` は条件を名前のない Capture で写し、中の Set の `where` にそれ（と外の `where` の論理積）をつなぐ。ローカルは Capture。
  - Output は 1 つだけ（2 つめは誤り）。
- [ ] **Step 1: 試験を書く** `lower_basic`（v5 5 の例のコードが Task 3 の `field_sin_index` と同じ要素を出す）、`lower_rejects_unknown_name`（`ix` を持たない `points` で `ix`）、`lower_rejects_field_in_variable`（`p = position`）、`lower_rejects_field_to_count`（`points(index)`）、`lower_rejects_type`（`set_position(g, true)`）、`lower_rejects_loop`（`a = set_position(b, …); b = set_position(a, …)` → `unknown name 'b'` で先に止まる）、`reapply_keeps_keys`（`s = points(4); s = set_scale(s, 2); output(s)` を 2 回適用して ID が同じ、`notes` に改名の知らせ）、`capture_syntax`（`b = g.position; g = set_position(g, vec(0,0,0)); g = store(g, "old", b)` で `old` が元の位置）、`each_if_once`（`if scale.x < 1 { scale *= 2; position += vec(1,0,0) }` で scale 0.8 の要素も動く）
- [ ] **Step 2〜5:** 失敗 → 実装 → 通る → Commit

### Task 6: グラフ → コード（print）と往復

**Files:**
- Create: `src/dxg5/lang_print.cpp`
- Modify: `src/dxg5/lang.h`
- Test: `tests/dxg5/dxg5_test.cpp`

**Interfaces:**
- Produces: `std::string GraphToCode(const Graph&)`、`std::string ExprText(const Graph&, uint32_t node, int output)`（積み重ね表示の入力欄に出す式）、`bool SameGraph(const Graph& a, const Graph& b, std::string* why)`（v5 13 の「同じ」。つながった Expression ノードを 1 つにまとめる正規化をしてから比べる）
- 決まり（v5 8.2）: ジオメトリのノードは `key = 呼び出し` の独立の文（同じ名前に 2 回代入しない）。値のノードは使う先が 1 つなら埋め込み、2 つ以上なら `key = (e) => …`（Field）か `key = 式`（単一の値）。Expression は演算子の式、Math は `add(a, b)`。Capture は入力の出力がほかで使われていなければ `b = g.position`、使われていれば `g_b, b = capture(g, position)`。Each は `key = each in { … }`。名前付きのマクロは `m1 /*Spread*/`。名前付きの属性で ⑤ で分からないものは `attr("x")`。並びはつながりの順、同じ順位は (y, x) の小さい順。定数の文字は `Num`、Int はそのまま、Bool は `true`/`false`、Vector は `vec(…)`。
- [ ] **Step 1: 試験を書く** `print_parse_roundtrip`（Task 5 の試験のコード 6 本について、`GraphToCode(CodeToGraph(c))` を読み直して `SameGraph`、2 回目の文字は 1 回目と同じ）、`print_v1_default_preset`（版 1 の既定のプリセットが `grid1 = grid(cx: m1 /*Count*/, …)` を含み、読み直して同じ要素）、`expression_vs_math`（Expression ノードと `add` ノードの区別が往復で戻る）、`chained_expressions_normalized`（Expression → Expression は `SameGraph` で同じ）
- [ ] **Step 2〜5:** 失敗 → 実装 → 通る → Commit

### Task 7: 組み込みのグループと C++ の差し替え

**Files:**
- Create: `src/dxg5/groups.cpp`
- Modify: `src/dxg5/nodes.cpp`（グループの `NodeDef` を足す）、`src/dxg5/eval.cpp`
- Test: `tests/dxg5/dxg5_test.cpp`

**Interfaces:**
- Produces: `const std::string* GroupSource(const std::string& name)`（定義のコード）、`bool NativeGroup(const std::string& name, …)`（差し替えの関数。eval の内部）
- グループと入力（v4 8.1 の P1 の定義と同じ値。既定値も P1 と同じ）:
  - `grid(cx, cy, cz: Int structural = 5, 5, 1; sx, sy, sz = 100)` 残す属性 `ix iy iz u v w`（版 1 のキー `countX→cx` …）
  - `circle(count: Int structural = 12, radius = 300, start_angle = 0, sweep = 360, align: Int structural choices "Off|Outward" = 0)` 残す `angle t normal`
  - `line(count = 10, length = 800, angle = 0)` 残す `t normal`
  - `transform(geometry, position: Vector, rotation_x/y/z, scale = 100, pivot: choices "Center|Origin")`
  - `modify(geometry, position: Vector, rotation_x/y/z, scale = 0, opacity = 0)`
  - `source(geometry, first: Int structural = 1, layers: Int structural = 1, assign: choices "Sequential|Random")`
- 定義は v5 の基本のノードで書く（Grid は v5 6.1 の例、Transform の Center は `bounds`、回転は `rot` と `rot_mul`、Source は `layer_set`）。
- 差し替え: グループのノードの非構造の入力がすべて単一の値（Field が来ていない）なら、`dxg` の P1 の C++（`src/graph/eval.cpp` の Grid / Circle / Line / Transform / Modify / Source と同じ計算）で計算する。ID は Task 3 の決まり（`legacyId` があれば `dxg::MixId(legacyId, i)`、なければ `MixId(Hash(key + "/pts"), i)`。定義の中の `points` の key は `pts`）。
- [ ] **Step 1: 試験を書く** `group_matches_native`（6 つのグループそれぞれ、入力 3 組で、定義のまま評価した要素と差し替えの要素が位置・回転・大きさ・不透明度・ID で 1e-9 以内）、`v1_graphs_same_elements`（`tests/graph` の既存の試験が使う版 1 のグラフ 4 つ（既定・円・Transform つき・Source Random）を v5 で評価し、`dxg::Evaluate` の要素と `FormatElements` が同じ文字）、`group_field_input`（`circle(count: 36, radius: 100 + index * 5)` で半径が要素ごとに違う＝差し替えを使わない）
- [ ] **Step 2〜5:** 失敗 → 実装 → 通る → Commit

### Task 8: 編集の操作・プリセット・試験の台本

**Files:**
- Create: `src/dxg5/edit.h`, `src/dxg5/edit.cpp`
- Test: `tests/dxg5/dxg5_test.cpp`

**Interfaces:**
- Produces（今の `dxg` の edit.h に対応する v5 版）:
  - `struct MacroWrite { bool isInt; int k; double value; }`
  - `uint32_t AddNodeAt(Graph&, const std::string& type, double x, double y)`、`bool RemoveNode(Graph&, uint32_t)`（線も外す）、`bool MoveNode(...)`、`bool Connect(Graph&, uint32_t from, int output, uint32_t to, const std::string& input, std::string* why)`（型・構造の入力・輪を確かめる）、`bool Disconnect(Graph&, uint32_t to, const std::string& input)`、`bool SetConst(Graph&, uint32_t, const std::string& input, double v)`（構造の入力は丸め、選択肢は範囲）、`bool SetExpression(Graph&, uint32_t, const std::string& input, const std::string& text, std::string* why)`（定数だけなら値に、そうでなければ Expression ノードをつなぐ。v5 8.3）、`bool LinkMacro(Graph&, uint32_t, const std::string& input, bool isInt, int k)`（Macro ノードを共用してつなぐ）、`bool Expose(Graph&, uint32_t, const std::string& input, MacroWrite*)`（入力が Int なら Int Macro に。空いているマクロ ＝ 線がつながっておらず名前もないもの）、`bool RenameMacro/SetMacroRange(Graph&, bool isInt, int k, …)`、`bool ExpandExpression(Graph&, uint32_t node)`、`bool CollapseToExpression(Graph&, const std::vector<uint32_t>& nodes)`、`bool ApplyCode(Graph&, const std::string& code, std::vector<Diag>* errors, std::vector<Diag>* notes)`、`uint32_t UsedMacros(const Graph&)`・`UsedIntMacros`・`UsedLayers`（Source の構造の入力が定数なら範囲、そうでなければ全部）
  - プリセット: `struct Preset { Graph graph; std::vector<MacroWrite> macros; }`、`Preset DefaultPreset()`（コード `g = grid(cx: i1, cy: i1, sx: m2, sy: m2)` / `output(g, copy_scale: m3)`、`i1` 名前 Count 範囲 1〜50、`m2` Spacing 0〜400、`m3` Copy Scale 0〜200、値 i1 = 5・m2 = 100・m3 = 100）、`Preset CirclePreset()`（`circle(count: i1, radius: m2)`、i1 = 12・m2 = 300・m3 = 100）、`Presets()`
  - **Effect Controls の既定値:** Macro 1〜3 の既定（5, 100, 100）は版 1 のプロジェクトのため変えない。新しいプリセットは Int Macro 1 と Macro 2・3 を使うので、**Int Macro 1 の既定 = 5**、Macro 2 = 100、Macro 3 = 100 で、プリセットの値と一致させる（書かずに描ける）。`double MacroDefault(int k)`・`int64_t IntMacroDefault(int k)`（i1 = 5、ほか 0）。
  - 台本 `bool RunScript(Graph&, const std::string&, std::vector<MacroWrite>*, std::string* error)`: 今の命令（`clear preset add remove move wire unwire set link unlink expose name range value`）を v5 の操作で受ける（`add Circle 10 20 as c` の種類名は大文字小文字を問わない。`wire a b [port]` は `geometry` 入力へ。`link n key m<k>|i<k> depth` は深さ 1 なら Macro の線、それ以外は Expression）に加え、`code` 行から `end` 行までを `ApplyCode` する命令。
- [ ] **Step 1: 試験を書く** `default_preset_5x5`（25 要素、`i1` の名前 Count、`IntMacroDefault(0) == 5`）、`expose_int_for_count`（`circle` の `count` を Expose → `i<k>`、`radius` → `m<k>`）、`set_expression`（`360/12` は定数 30、`m1*2+index` は Expression ノード）、`expand_collapse`（Expression `a*2+1` を展開すると Math 2 つ、まとめると 1 つで同じ値）、`connect_rejects`（Field を `points.count` に、ループ、型違い）、`script_code_block`（`code`…`end` で円を作る台本）
- [ ] **Step 2〜5:** 失敗 → 実装 → 通る → Commit

### Task 9: エフェクトを v5 の核に切り替え、Int Macro を足す

**Files:**
- Modify: `src/v2/dx2_params.h`（`P_INT_TOPIC = 69`、`P_INT = 70`（1〜32）、`P_INT_TOPIC_END = 102`、`P_NUM = 103`。static_assert を足す）、`src/v2/dx2.cpp`（パラメータの登録: Int Macro は `PF_ADD_SLIDER` の整数 −1,000,000〜1,000,000、スライダーの範囲 0〜100、既定は `IntMacroDefault`、グループ「Int Macros」は開いた状態）、`src/v2/dx2.h`（`dxg::Graph` → `dx5::Graph` など）、`src/v2/dx2_arb.cpp`（`GraphOf` は `dx5::Parse`。既定のグラフは `dx5::DefaultPreset`）、`src/v2/dx2_aegp.cpp`（`UpdateParamsUI` で Int Macro の行も隠す・範囲を付ける。使っているのは `UsedIntMacros`）、`src/v2/dx2_render.cpp`（PreRender の評価を `dx5::Evaluate`、`intMacro` は `PF_CHECKOUT_PARAM` の値。dump は同じ `FormatElements`）、`src/v2/dx2_ui.cpp`（Overview のマクロの一覧に Int Macro も）、`src/v2/dx2_editor.cpp`（`Commit` の `MacroWrite` の `isInt` で書き込むストリームを分ける）、`src/v2/duplix2.vcxproj`（`..\dxg5\*.cpp` を足す）
- Modify: `tests/ae/check_dx2_graph.py` と `tests/ae/dx2_graph.jsx`（下）、`tests/graph` は残す（版 1 の核の試験）

**Interfaces:**
- Consumes: Task 3 の `Evaluate`、Task 8 の `Preset`・`MacroWrite`・`UsedIntMacros`・`IntMacroDefault`
- [ ] **Step 1: AE 試験を v5 に合わせる** `dx2_graph.jsx` の `MACRO1 = 5` は版 1 の Count。新しいインスタンスの Count は Int Macro 1（`dx2(70)`）。A のコンプのキーは `dx2(70)` に 2 → 4、Macro 2 は今のまま。`check_dx2_graph.py` が `graph_test.exe eval` で比べているところを `dxg5_test.exe eval`（同じ引数に `i<k>=<v>` を足す）に替える。
- [ ] **Step 2: ビルドして AE 試験が落ちることを確かめる** Run: `pwsh tools/build.ps1 -Target v2; pwsh tests/ae/run_dx2_graph.ps1`。Expected: 切り替え前なので A のコンプの数が合わず FAIL
- [ ] **Step 3: 実装する**（上の Files のとおり）。`dxg5_test.exe eval <graph> <t> [m<k>=v] [i<k>=v] [seed=] [layers=]` を足す。
- [ ] **Step 4: 通ることを確かめる** Run: `pwsh tests/ae/run_dx2_graph.ps1 -Bpc 8`（16・32 も）。Expected: `DX2 GRAPH PASS`。あわせて `tests/ae/dx2_old_project.jsx` が今までどおり（版 1 のエフェクトのプロジェクトは開かないまま。新しい版 1 のグラフを持つ v2 のプロジェクトは開く）
- [ ] **Step 5: Commit**

### Task 10: エディター — 表示の切り替えと Code 表示

**Files:**
- Modify: `src/v2/dx2_editor_win.cpp`（大きいので分ける: 下の 3 つ）
- Create: `src/v2/dx2_editor_code.cpp`（Code 表示）、`src/v2/dx2_editor_view.h`（3 つの表示が共有する `Win` の宣言・描画の道具）
- Test: `tests/ae/run_dx2_graph.ps1` に手順を足す

**Interfaces:**
- 窓の上に 3 つのタブ「Nodes | Stack | Code」（高さ 26 px）。Code 表示は複数行の EDIT（`ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL`、等幅 Consolas 14 px）と、下に「Apply (Ctrl+Enter)」ボタンと誤りの行（赤）。表示を Code に替えたとき `GraphToCode` で中身を入れる。Apply で `ApplyCode` → 通れば `Commit("apply code")`、通らなければ誤りの行に `line L col C: msg` を出し、EDIT の該当行を選ぶ。Tab キーは 4 つの空白を入れる。Ctrl+Z は EDIT の中では EDIT の取り消し（AE の取り消しはエディターの他の表示で）。AE で取り消されたら（`GraphReloaded`）、編集中でなければ書き直す。編集中なら上に「The graph changed in After Effects. Reload」のボタンを出す。
- [ ] **Step 1: AE 試験の手順を足す** `run_dx2_graph.ps1`: コンプ D の窓に、Code タブのクリック → EDIT に `WM_SETTEXT` で円のコード（`c = circle(count: 6, radius: 80)` / `output(c, copy_scale: 50)`）→ Ctrl+Enter（`WM_KEYDOWN`）を送り、`d_code.png` で 6 個の円が描かれる（中心から 80 px の 6 点の画素が不透明、中心は透明）。誤りのコード `x = nope` を送ると誤りの行が出て（窓の子の STATIC の文字を読む）、グラフの revision が変わらない。
- [ ] **Step 2: 落ちることを確かめる**
- [ ] **Step 3: 実装する**
- [ ] **Step 4: 通ることを確かめる** Expected: `DX2 GRAPH PASS`
- [ ] **Step 5: Commit**

### Task 11: エディター — ノード表示の作り直し（値の線・縮尺・スナップ・マクロのドラッグ）

**Files:**
- Modify: `src/v2/dx2_editor_win.cpp`（ノード表示だけを残す）
- Test: `tests/ae/run_dx2_graph.ps1`（コンプ D の既存のマウス操作を新しい形に合わせる）

**Interfaces:**
- ノードの箱: 題名、出力のソケット（右、出力ごとに 1 行）、入力の行（左にソケット、名前、値の欄）。ソケットの色: Geometry 緑 `RGB(90,200,140)`、Float 灰 `RGB(160,160,160)`、Int 濃い緑 `RGB(60,140,90)`、Bool 桃 `RGB(200,120,160)`、Vector 紫 `RGB(120,110,220)`、Rotation 青紫 `RGB(150,120,230)`。Field を運ぶ線は破線。線のつながった入力は値の欄を出さない（Expression と Macro の名前を薄く出す）。Expression ノードは本体に式の文字（クリックで編集）。グループのノードは題名の右に「▸」（開くのは後の段階。今は押すと Code 表示のその行へ）。ゾーン（Each）は枠で囲む。
- 縮尺: ホイールでカーソルの位置を中心に ×1.1 / ÷1.1（0.3〜3）。フォントは縮尺ごとに作り直す。ミドルドラッグで移動（今のまま）。Home で全体が入る縮尺。
- スナップ: 線をドラッグ中、カーソルから 24 px（画面）以内でいちばん近い、型の合う入力に吸い付く（線の先をそのソケットに描く）。離すとつなぐ。入力から逆にドラッグすると既存の線を外して持ち上げる（Blender と同じ）。
- 右の Macros の欄: Macro と Int Macro を「M」「I」の見出しで分けて一覧。説明の文は欄の幅で折り返し、欄の下端からはみ出す分は出さない（見切れの修正、v4 1.3）。マクロの行を値の欄へドラッグすると `LinkMacro`（型が Int の入力に Float のマクロをつないでもよい。丸める）。
- 追加のメニュー: 右クリックで、分類ごとのサブメニュー（Generate / Set / Geometry / Input / Math / Utility / Group）と、一番上に検索の欄（打つと絞った一覧）。
- 値の欄: クリックで入力欄（式を受け付ける、v5 8.3）、左右ドラッグで値、右クリックで Expose / Link to Macro（M / I のサブメニュー）/ Unlink / Reset / Expand Expression。
- [ ] **Step 1: AE 試験の手順を合わせる**（コンプ D: 出力から入力へのドラッグ、値のクリックと入力、ホイール 3 回で縮尺が変わる（窓の状態を `DUPLIX2_IDLE_LOG` に出す `editor zoom` の行で確かめる）、スナップ（ソケットから 15 px ずれた所で離してもつながる））
- [ ] **Step 2〜5:** 落ちる → 実装 → 通る（8 bpc）→ Commit

### Task 12: エディター — 積み重ね表示

**Files:**
- Create: `src/v2/dx2_editor_stack.cpp`
- Test: `tests/dxg5/dxg5_test.cpp`（並べ方）、`tests/ae/run_dx2_graph.ps1`

**Interfaces:**
- Produces（`src/dxg5/edit.h` に、AE なしで試験できる部分として）: `struct StackRow { uint32_t node; int depth; bool collapsedGraph; std::vector<uint32_t> branches; }`、`std::vector<StackRow> StackRows(const Graph&)`（v5 14: Output からジオメトリの入力をさかのぼった木を、**上が上流**の順に並べる。1 つの出力を 2 か所で使うノードから上は `collapsedGraph` の 1 行にまとめる）
- 行: 題名（▸ / ▾ で入力を開く）、開くと入力ごとの行（定数はスライダーと数、マクロはチップ `I1 Count`、それ以外は `ExprText` の文字。クリックで入力欄、式を受け付ける）。Each は 1 行で本体のコードの文字（クリックで Code 表示）。`collapsedGraph` の行は「(node graph — open in Nodes)」でクリックすると Nodes 表示。下の「+ Add」で、Output の直前にジオメトリの入力 1 つのノード（Set 系・transform・modify・source・delete）を足す。行の右クリックで Remove（上下をつなぎ直す）。
- [ ] **Step 1: 試験を書く** `stack_rows_default`（既定のプリセットが `grid1`・`out` の 2 行、grid が上）、`stack_rows_shared`（1 つの出力を 2 か所で使うと `collapsedGraph`）
- [ ] **Step 2: AE 試験の手順を足す**（Stack タブで grid の行を開き、Spacing の欄に `150` を入れて Enter → d の絵の間隔が 150）
- [ ] **Step 3〜6:** 落ちる → 実装 → 通る → Commit

### Task 13: 仕上げ — 全部の試験、文書、利用者への引き渡し

**Files:**
- Modify: `HANDOFF.md`、`docs/design/複製プラグイン設計案_v5.md`（17 節の P1.5 の結果を 1 段落）、`docs/experiments/2026-11-25-dev-log.md`（分かったこと）
- [ ] **Step 1:** Run: `pwsh tools/build.ps1 -Target all`、`build\graph\graph_test.exe`、`build\dxg5\dxg5_test.exe`、`pwsh tests/ae/run_dx2_graph.ps1 -Bpc 8` / `16` / `32`、`pwsh tests/ae/run_jsx.ps1 -Jsx tests/ae/dx2_perf.jsx -Name dx2_perf`。Expected: すべて PASS、描画の時間が e8ebe59 の値（71 ms）から大きく増えていない（評価は PreRender で、5×5 なら 1 ms 以下）
- [ ] **Step 2:** 引き継ぎメモに、利用者に試してもらう手順（Code 表示で例のコードを適用・Stack 表示で値を変える・ノード表示でホイール・スナップ・マクロのドラッグ・Int Macro にキー）と、自動の試験で見られない確認事項（コード欄で打った文字が AE のショートカットにならない、Ctrl+Enter、Ctrl+Z）を書く
- [ ] **Step 3: Commit と push**

---

## 後の段階に回すもの（この計画に入れない。v5 17 節）

Repeat ゾーン・Merge・Instance（P2）、グループを開いて編集・自分用にコピー・Ctrl+G（P1.5c の残り。定義はコードで見られる）、Motion・Map・Noise・LFO などの値のグループ（P3〜P5）、Set Order と Order の付け替え（P3）、積み重ね表示の並べ替えのドラッグ。
