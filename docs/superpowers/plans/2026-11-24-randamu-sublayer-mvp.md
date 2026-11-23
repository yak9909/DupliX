# randamu 最初の版（UI で触れる最小の版）実装計画

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** レイヤーにエフェクト「randamu」を 1 つ掛けると、そのレイヤーが AE 自身の 3D 描画で n 枚のコピーとしてランダムに配置され、本物の複製レイヤーと画素単位で同じ絵になる。利用者が AE の画面で触って試せる状態で渡す。

**Architecture:** AE に依存しない「コア」（乱数とコピーの変形の計算）、BEE.dll／AEGPDriver.aex の関数を差し替える「フック層」、パラメータ定義と自分のレイヤーの登録を行う「エフェクト本体」の 3 つに分ける。フックは登録一覧の照合だけで対象を見分け、描画スレッドでは AEGP でパラメータを読み、メインスレッドでは AEGP を呼ばずに写しを使う。

**Tech Stack:** C++17、MSVC v143（VS 2022 Build Tools）、AE SDK 25.6（`C:\AfterEffectsSDK\ae25.6_61.64bit.AfterEffectsSDK`）、PowerShell 7、ExtendScript、Python（`spike\.venv`）。

**Spec:** `docs/superpowers/specs/2026-11-24-randamu-sublayer-design.md`（承認済み）。事実の出典は `HANDOFF.md` の試験 1〜23 と `spike/RandamuSpike/RandamuSpike.cpp`。

## Global Constraints

- 対象は After Effects 2025（25.6）、Windows x64、クラシック 3D レンダラーのみ。
- 1 レイヤー 1 エフェクトで成立する。補助レイヤー・複製・効果プリセット・手作業の設定を求めない。
- 見た目は「各コピーを本物の複製 3D レイヤーとして置いたコンプ」と 8 bpc で差の最大 1/255 以内。
- 実験コード（`spike/RandamuSpike/`）はコピーして使わない。事実（関数名・オフセット・命令の並び）だけを使い、書き直す。テスト用の道具（`spike/tools/ae_bg.ps1`・`ae_guard.ps1`）は `docs/ae-test-harness.md` のとおり再利用してよい。
- フックの中（BEE の仮想関数の差し替え先）で、メインスレッドから AEGP を呼ばない（試験 20 で AE が止まった）。
- 差し替え先が 1 つでも見つからなければ何も差し替えない。AE を落とさない。
- AE の自動実行は見えないデスクトップで行い、利用者の画面を撮らない。判定は OCR ではなくログと画素比較で行う。実行前に AfterFX／aerender が動いていないことを確かめる。
- 利用者への文章・UI の文言は日本語。

## Review Focus

1. **複数のコンプで同じレイヤー ID**: レイヤー ID はコンプごとの小さな番号。別のコンプにある同じ ID のレイヤー（エフェクトなし）にはコピーが出てはならない → Task 6 のケース `multi_comp`。
2. **保存して開き直したプロジェクト・aerender**: 最初の描画の問い合わせより前に、エフェクトが自分を登録していなければならない → Task 6 のケース `reopen` と aerender の全ケース。
3. **UI での操作中（スクラブ・パラメータ変更・プレビュー）**: AE が止まらず、変更がすぐ絵に反映される → Task 7。
4. **エフェクトを無効（fx スイッチ）・削除・取り消し**: コピーが消え、元のレイヤーに戻る → Task 6 のケース `fx_off`・Task 7。
5. **コピー数 0 と 1、範囲がすべて 0**: 0 枚ならレイヤーは描かれない、範囲 0 なら元のレイヤーと同じ絵 → Task 2 の単体テストと Task 6 のケース `count0`・`identity`。

---

## ファイル構成

```
randamu/
  src/
    core/scatter.h, scatter.cpp        … 乱数、コピー i の変形（M4）と不透明度。AE に依存しない
    hook/bee_abi.h                     … BEE の型の配置（RawVec、M4 の並び）、公開関数の装飾名
    hook/registry.h, registry.cpp      … 登録一覧（鍵＝コンプの項目 ID＋レイヤー ID）とパラメータの写し
    hook/bee_hooks.h, bee_hooks.cpp    … 差し替え先の探索、差し替えた関数、IAT の差し替え
    hook/flatten_site.h, flatten_site.cpp … IsLayerType の呼び元の命令の並びの照合（純粋関数）
    effect/randamu.h, randamu.cpp      … エフェクトの入口、パラメータ、コマンド処理
    effect/randamu_params.h            … パラメータの番号（コアとフックと共有）
    effect/RandamuPiPL.r
    effect/randamu.vcxproj
  tests/
    unit/unit_tests.cpp, unit_tests.vcxproj … コアと照合・登録一覧の単体テスト（枠組みなし、CHECK マクロ）
    tools/randamu_dump.cpp（unit_tests.vcxproj の 2 つ目の構成ではなく別の小さな vcxproj） … パラメータからコピーの変形を JSON で出す
    ae/make_ref.jsx                    … 正解比較用のシーン生成
    ae/run_ref.ps1                     … シーン生成 → aerender（MFR オフ・オン）→ 画素比較
    ae/ui_session.jsx, run_ui.ps1      … UI 操作の試験
    ae/cmp.py                          … 画素比較（差の最大・平均・閾値ごとの画素数）
  tools/build.ps1                      … 全部をビルドし、.aex を dev_plugins に置く
```

---

### Task 1: 土台（ビルドと、何もしないエフェクト）

**Files:**
- Create: `src/effect/randamu.h`, `src/effect/randamu.cpp`, `src/effect/RandamuPiPL.r`, `src/effect/randamu.vcxproj`, `src/effect/randamu_params.h`, `tools/build.ps1`, `.gitignore`
- Test: `tests/ae/smoke.jsx`, `tests/ae/run_smoke.ps1`

**Interfaces:**
- Produces: エフェクトのマッチ名 `RANDAMU Scatter`、表示名 `randamu`、カテゴリ `randamu`。`randamu_params.h` の列挙 `enum RandamuParam { RP_INPUT = 0, RP_STATUS, RP_COUNT, RP_SEED, RP_POS_X, RP_POS_Y, RP_POS_Z, RP_ROT_X, RP_ROT_Y, RP_ROT_Z, RP_SCALE_MIN, RP_SCALE_MAX, RP_OPACITY_MIN, RP_OPACITY_MAX, RP_KEEP_FIRST, RP_NUM_PARAMS };`（AEGP のストリーム番号もこの値）。ログ関数 `void RLog(const char* fmt, ...)`（環境変数 `RANDAMU_LOG` が出力先。未設定なら何もしない）を `src/effect/randamu.h` に宣言。

- [ ] **Step 1: git を始める**

`git init`、`.gitignore` に `spike/.venv/`、`spike/logs/`、`spike/ida/*.i64`、`spike/build/`、`build/`、`dev_plugins/`、`*.pdb`、`*.ilk` を書く。最初のコミットは既存の docs と spike（上の除外つき）。

- [ ] **Step 2: vcxproj と PiPL を作る**

`spike/RandamuSpike/RandamuSpike.vcxproj` の設定（SDK のインクルード、`AE_PLUGIN_BUILD_DIR` への出力、`.aex`、PiPLTool の手順＝作業フォルダに移ってファイル名だけ渡す、C++17、/MD）を同じ値で書き直す。ソースは `src/core/*.cpp`、`src/hook/*.cpp`、`src/effect/*.cpp`。出力名 `randamu.aex`。

- [ ] **Step 3: エフェクトの入口を書く**

GLOBAL_SETUP（`PF_OutFlag_DEEP_COLOR_AWARE`、`PF_OutFlag2_SUPPORTS_SMART_RENDER`、`PF_OutFlag2_SUPPORTS_THREADED_RENDERING`、`PF_OutFlag2_I_MIX_GUID_DEPENDENCIES`）、PARAMS_SETUP（Task 5 のパラメータを先に全部定義する。既定値は Task 5 の表）、SMART_RENDER は入力をそのまま出力にコピー。PRE_RENDER でビルド ID を `GuidMixInPtr` に混ぜる（AE のディスクキャッシュ対策、`HANDOFF.md`「重要: AE のディスクキャッシュ」）。

- [ ] **Step 4: `tools/build.ps1` を書いてビルドする**

`spike/tools/build.ps1` と同じ手順（VsDevShell → msbuild、`AE_PLUGIN_BUILD_DIR` は `dev_plugins`）で `src/effect/randamu.vcxproj` をビルド。実験の `RandamuSpike.aex` は `dev_plugins` から `spike/build/out` に移す（同時に読み込ませない）。
Run: `pwsh tools/build.ps1`
Expected: `dev_plugins\randamu.aex` ができる。

- [ ] **Step 5: 読み込みの試験**

`smoke.jsx`: 新しいコンプに 3D の平面を作り `addProperty("RANDAMU Scatter")` を実行、`saveFrameToPng` で 1 フレーム描き、ログに `SMOKE OK` を書く。`run_smoke.ps1` は `docs/ae-test-harness.md` の手順（`Repair-AeCrashState` → `Start-AeHidden` → 完了行を待つ → `Stop-AeHidden`）。
Run: `pwsh tests/ae/run_smoke.ps1`
Expected: `SMOKE OK`、PNG が平面そのままの絵。

- [ ] **Step 6: Commit**

`git add -A; git commit -m "feat: randamu effect scaffold (pass-through)"`

---

### Task 2: コア（乱数とコピーの変形）

**Files:**
- Create: `src/core/scatter.h`, `src/core/scatter.cpp`
- Test: `tests/unit/unit_tests.cpp`, `tests/unit/unit_tests.vcxproj`

**Interfaces:**
- Produces:
```cpp
namespace rnd {
struct ScatterParams {
    int count;               // >= 0
    int seed;
    double pos[3];           // ± 範囲（レイヤー座標の px）
    double rotDeg[3];        // ± 範囲（度）
    double scaleMin, scaleMax;     // %（100 = 等倍）
    double opacityMin, opacityMax; // %（0..100）
    bool keepFirst;          // true なら copy 0 は単位変形・不透明度 100%
};
struct CopyXform { double m4[16]; double opacity; };  // m4 は行ベクトル形式（p' = p·M、行 3 が平行移動）、opacity は 0..1
double Rand01(int seed, int copy, int channel);       // [0,1)、(seed,copy,channel) だけで決まる
void ComputeCopy(const ScatterParams& p, int i, double anchorX, double anchorY, CopyXform* out);
}
```
- channel の割り当て: 0..2 位置 XYZ、3..5 回転 XYZ、6 拡大縮小、7 不透明度。

- [ ] **Step 1: 失敗するテストを書く**

`unit_tests.cpp` に `CHECK(cond)`／`CHECK_NEAR(a,b,eps)` マクロと次のテスト（失敗数を終了コードに）:
- `rand_deterministic`: `Rand01(7,3,2)` を 2 回呼んで同じ値、`0 <= v < 1`。
- `rand_stable_when_count_grows`: count=5 と count=50 で `ComputeCopy(i=3)` の m4 が全要素一致（コピー数を増やしても既存のコピーは動かない）。
- `zero_ranges_identity`: pos=rot=0、scale 100–100、opacity 100–100 → m4 が単位行列（1e-12）、opacity 1.0。
- `translate_only`: rot=0・scale 100 で m4 の行 3 が `(tx,ty,tz,1)`、`|tx|<=pos[0]` など。
- `rotation_about_anchor`（行列は `detail::MakeM4(tx,ty,tz, rx,ry,rz, s, ax,ay)` を公開して直接試す）: `MakeM4(0,0,0, 0,0,90, 1, 50,50)` で点 (50,50,0) → (50,50,0)（アンカーは動かない）、点 (100,50,0) → (50,100,0)（AE の Z 回転 +90 は画面で時計回り＝y 下向きが正）。
- `rotation_order_zyx`: `MakeM4(0,0,0, 20,30,40, 1, 0,0)` が、テスト内で 4×4 の掛け算で作った `Rz·Ry·Rx`（行ベクトル。`HANDOFF.md` 試験 11 の確定形 `T(−A)·Rz·Ry·Rx·T(A+t)` で A=t=0）と 1e-12 で一致。
- `scale_about_anchor`: `MakeM4(0,0,0, 0,0,0, 2, 50,50)` で (60,50,0) → (70,50,0)、(0,0,10) → (−50,−50,20)（X・Y・Z すべてに s を掛ける。AE の 3D レイヤーの拡大縮小と同じ）。
- `keep_first`: keepFirst=true で copy 0 は単位行列・opacity 1.0、copy 1 は keepFirst=false の copy 1 と同じ。
- `opacity_range`: opacityMin=30・Max=30 で opacity 0.30。
- `count_zero`: count=0 でも `ComputeCopy` は呼べる（呼び出し側が 0 枚を扱う）。

行列の確定形（行ベクトル）: `M = T(−ax,−ay,0) · S(s,s,s) · Rz · Ry · Rx · T(ax+tx, ay+ty, tz)`。各回転は `spike/RandamuSpike/RandamuSpike.cpp:1908-1910` と同じ形（Rx = `{{1,0,0,0},{0,c,s,0},{0,-s,c,0},{0,0,0,1}}` など）。拡大縮小は `s = scale%/100`。

- [ ] **Step 2: テストを実行して失敗を確かめる**

Run: `pwsh tools/build.ps1 -Target unit; build\unit\unit_tests.exe`
Expected: リンクエラーまたは FAIL。

- [ ] **Step 3: 実装**

`Rand01` はカウンタ方式の整数ハッシュ（例: SplitMix64 に `seed*0x9E3779B97F4A7C15 ^ copy*0xBF58476D1CE4E5B9 ^ channel*0x94D049BB133111EB` を入れ、上位 53 ビットを [0,1) に）。範囲は `v = (2*Rand01−1) * range`、拡大縮小と不透明度は `min + Rand01*(max−min)`。`detail::MakeM4` を `scatter.h` に公開する。

- [ ] **Step 4: テストを実行して通ることを確かめる**

Expected: `ALL PASS`、終了コード 0。

- [ ] **Step 5: Commit**

`git commit -m "feat(core): deterministic scatter transforms"`

---

### Task 3: 正解比較用の書き出し道具

**Files:**
- Create: `tests/tools/randamu_dump.cpp`, `tests/tools/randamu_dump.vcxproj`
- Modify: `tools/build.ps1`（`-Target dump`）

**Interfaces:**
- Consumes: `rnd::ScatterParams`、`rnd::ComputeCopy`、`rnd::detail` の乱数（Task 2）。
- Produces: `randamu_dump.exe --count N --seed S --pos X Y Z --rot X Y Z --scale MIN MAX --opacity MIN MAX [--keep-first]` が標準出力に JSON `{"copies":[{"t":[tx,ty,tz],"r":[rx,ry,rz],"s":s,"o":o}, ...]}` を出す。`t`・`r`（度）・`s`（%）・`o`（%）は M4 を作る前の値（jsx が本物のレイヤーの位置・回転・拡大縮小・不透明度に入れる）。そのため `scatter.h` に `struct CopyParams { double t[3], r[3], s, o; }; CopyParams DrawCopy(const ScatterParams&, int i);` を足し、`ComputeCopy` は `DrawCopy`＋`MakeM4` で作る形にする（Task 2 のテストはそのまま通ること）。

- [ ] **Step 1: 単体テストを足す** — `drawcopy_matches_compute`: 10 枚について `MakeM4(DrawCopy(i)...)` と `ComputeCopy(i)` の m4 が一致、`DrawCopy(i).o/100 == ComputeCopy(i).opacity`。
- [ ] **Step 2: 失敗を確かめる**（未定義）。
- [ ] **Step 3: `DrawCopy` と `randamu_dump.cpp` を実装**。
- [ ] **Step 4: テストと道具を確かめる**
Run: `build\unit\unit_tests.exe; build\dump\randamu_dump.exe --count 3 --seed 1 --pos 100 100 100 --rot 0 0 0 --scale 100 100 --opacity 100 100`
Expected: `ALL PASS`、`copies` が 3 件の JSON。
- [ ] **Step 5: Commit** — `git commit -m "test: randamu_dump for reference scenes"`

---

### Task 4: フック層（差し替え先の探索・照合・登録一覧）

**Files:**
- Create: `src/hook/bee_abi.h`, `src/hook/flatten_site.h/.cpp`, `src/hook/registry.h/.cpp`, `src/hook/bee_hooks.h/.cpp`
- Test: `tests/unit/unit_tests.cpp`（照合と登録一覧のテストを追加）

**Interfaces:**
- Consumes: `rnd::ComputeCopy`、`RandamuParam`（Task 1・2）。
- Produces:
```cpp
// registry.h
struct LayerKey { int compItemId; int layerId; };
struct ParamSnapshot { rnd::ScatterParams p; bool valid; };
void RegRegister(LayerKey k, const ParamSnapshot& snap);   // 追加 or 写しの更新
void RegUnregister(LayerKey k);
bool RegLookup(LayerKey k, ParamSnapshot* out);            // ロックなしで読める（MFR で同時に呼ばれる）
// flatten_site.h
bool MatchFlattenCheckSite(const unsigned char* p, uintptr_t betaFn);  // p = 戻り先、betaFn = BEE_BetaFeature_AreExpressionEffectsAlwaysAllowedEnabled
// bee_hooks.h
enum class HookStatus { NotInstalled, Installed, MissingSymbols };
HookStatus InstallHooks(SPBasicSuite* pica, AEGP_PluginID pid);  // GLOBAL_SETUP から 1 回。呼んだスレッドをメインスレッドとして控える
HookStatus GetHookStatus();
```
- 対象レイヤーの鍵は、フックの中では `BEE_Layer::GetParentComp() const`（`?GetParentComp@BEE_Layer@@QEBAQEBVBEE_CompItem@@XZ`）→ `BEE_Item::GetID() const`（`?GetID@BEE_Item@@QEBAAEBHXZ`、`const int&` を返す）と `?GetID@BEE_Layer@@QEBA?BHXZ` で作る。エフェクト側は `AEGP_GetLayerParentComp`→`AEGP_GetItemFromComp`→`AEGP_GetItemID` と `AEGP_GetLayerID` で作る。

- [ ] **Step 1: 失敗するテストを書く**
- `flatten_site_match`: `spike/RandamuSpike/RandamuSpike.cpp:2058-2088` の条件をそのまま満たすバイト列（`84 C0 75 xx E8 <rel32 → betaFn> 84 C0 74 xx 48 8B xx 48 8B xx FF 90 ...` から 61 バイト目まで）を配列で作り `true`。
- `flatten_site_audio_rejected`: 同じ前半で、25 バイト目以降が違う列（音声合成の判定）→ `false`。
- `registry_two_comps_same_layer_id`: `{10,48}` だけ登録 → `RegLookup({11,48})` は `false`、`{10,48}` は `true`。
- `registry_unregister`: 登録 → 解除 → `false`。
- `registry_update_snapshot`: 同じ鍵で count 5 → 9 に更新し、読むと 9。

- [ ] **Step 2: 失敗を確かめる**。

- [ ] **Step 3: 実装**
- `bee_abi.h`: `struct RawVec { char* first; char* last; char* end; };`、`VecReserve`（`spike/RandamuSpike/RandamuSpike.cpp:1641-1672` の規則: ucrtbase の malloc/free、4096 バイト以上は 32 バイト整列で `p[-1]` に元のポインタ）、矩形 32 バイト・M3 72 バイト・M4 128 バイト、装飾名の定数（`IsLayerType`・`SubLayersRenderSeparately`・`SubLayerRenderCount`・`GetSubLayerRects` は `RandamuSpike.cpp:2204-2207`、`BEE_GetArtisanLayerOpacity` は `:2227`、vtable は `??_7BEE_AVLayer@@6B@`）。
- `registry.cpp`: 固定長の配列（例 4096 件）＋原子的な件数、各要素は `std::atomic` の鍵と、写しを指すポインタ（更新は新しい写しを確保して差し替え、古いものは解放しない＝読み手とぶつからない。数 KB の漏れは許容し、コメントに書く）。
- `bee_hooks.cpp`: 差し替えた関数 5 つ（仕様書 第 3 節の表）。それぞれ、対象でなければ元の関数を呼ぶ。
  - `IsMine(layer, LayerKey*)`: 上の 3 つの公開関数で鍵を作り `RegLookup`。
  - `SubLayerRenderCount`（引数: this, 時刻, shared_ptr<XformTimeCache> const&, ParamBag）: キャッシュの shared_ptr が空（第 3 引数を `p` として `*(void**)p == nullptr`）なら元の答え、そうでなければ count。count が 0 のときは 0 を返す（試験は Task 6 の `count0`）。
  - `GetSubLayerRects`（引数: this, 戻り値の矩形, 時刻 `{int value; unsigned scale}`（レイヤー時間）, 入力の矩形, vector<矩形>, vector<M3>, vector<M4>, ParamBag）: 矩形＝入力の矩形、M3＝単位行列（`?SetIdentity@M_Matrix3@@QEAAXXZ`、U.dll）、M4＝`ComputeCopy(p, i, ax, ay)`。アンカー (ax, ay) はレイヤーのアンカーポイント（パラメータの写しに入れる。Task 5）。パラメータは、今のスレッドがメインスレッドなら写し、そうでなければ AEGP（`AEGP_EffectSuite4` で自分のマッチ名のエフェクトを探し、`AEGP_GetNewEffectStreamByIndex(RandamuParam)`→`AEGP_GetNewStreamValue(LayerTime, 引数の時刻)`。アンカーは `AEGP_GetNewLayerStream(AEGP_LayerStream_ANCHORPOINT)`）。AEGP の読み取りに失敗したら写しを使う。
  - `SubLayersRenderSeparately`: 対象なら真。
  - `IsLayerType`: 引数 0、対象、`MatchFlattenCheckSite(_ReturnAddress())` が真のときだけ偽。
  - IAT: AEGPDriver.aex の取り込み表で `BEE_GetArtisanLayerOpacity` を差し替え、`v = *(char**)(trace+16)`、レイヤー `*(void**)(v−48)`、番号 `*(int*)(v−40)` から、対象なら元の値×`ComputeCopy(...).opacity`。
  - 探索は `RandamuSpike.cpp:2104-2111` の `FindSlot` と同じ考え方。全部見つかってから一度に差し替える。1 つでも欠けたら `MissingSymbols`。
  - `RLog` で、差し替えた枠の番号、初回の呼び出しのスレッド ID と「メインかどうか」、鍵（comp, layer）を出す。

- [ ] **Step 4: テストが通ることを確かめる** — `ALL PASS`。
- [ ] **Step 5: Commit** — `git commit -m "feat(hook): sub-layer hooks with comp+layer keyed registry"`

---

### Task 5: エフェクト本体（パラメータ・登録・案内）

**Files:**
- Modify: `src/effect/randamu.cpp`, `src/effect/randamu_params.h`

**Interfaces:**
- Consumes: `InstallHooks`・`GetHookStatus`・`RegRegister`・`RegUnregister`（Task 4）、`rnd::ScatterParams`（Task 2）。

パラメータ（UI 名は日本語、既定値はここで決める。利用者が試したあとに変えてよい）:

| 番号 | UI 名 | 種類 | 既定 | 範囲（スライダー） |
|---|---|---|---|---|
| RP_COUNT | コピー数 | 整数スライダー | 10 | 0〜100000（1〜1000） |
| RP_SEED | 乱数の種 | 整数スライダー | 0 | 0〜1000000（0〜1000） |
| RP_POS_X/Y/Z | 位置の範囲 X/Y/Z | 浮動小数 | 200/200/200 | 0〜100000（0〜2000） |
| RP_ROT_X/Y/Z | 回転の範囲 X/Y/Z | 角度 | 0/0/0 | 0〜180 |
| RP_SCALE_MIN/MAX | 拡大縮小の最小/最大 (%) | 浮動小数 | 100/100 | 0〜10000（0〜400） |
| RP_OPACITY_MIN/MAX | 不透明度の最小/最大 (%) | 浮動小数 | 100/100 | 0〜100 |
| RP_KEEP_FIRST | 1 枚目は元の位置 | チェック | オフ | — |

- [ ] **Step 1: GLOBAL_SETUP で `InstallHooks` を呼ぶ。** 状態が `MissingSymbols` なら、効果コントロールの中で常に見える場所に「この AE の版ではコピーを作れません」と出す（Step 3 の案内と同じ場所）。場所は、パラメータの先頭に置く状態表示用のチェックボックス（操作は無効、横の文字で状態を見せ、`PF_Cmd_UPDATE_PARAMS_UI` と `PF_UpdateParamUI` で文字を更新、`PF_OutFlag_SEND_UPDATE_PARAMS_UI`）を第一候補にする。文字が更新できなければ実装者が別の常時表示の方法を選び、計画の表に追記する。この表示用のパラメータが `RP_STATUS`（Task 1 の列挙）。
- [ ] **Step 2: 登録。** `SEQUENCE_SETUP`・`SEQUENCE_RESETUP`・`SEQUENCE_FLATTEN` 後・`USER_CHANGED_PARAM`・`UPDATE_PARAMS_UI`・`PRE_RENDER` で、`AEGP_PFInterfaceSuite1::AEGP_GetEffectLayer` から自分のレイヤーを取り、鍵を作って `RegRegister`（写しは `in_data->current_time` のパラメータとアンカー）。`SEQUENCE_SETDOWN` で `RegUnregister`。これらのコマンドはエフェクト自身のコマンドなので AEGP を呼んでよい。
- [ ] **Step 3: 3D でないときの案内。** `AEGP_GetLayerFlags` で `AEGP_LayerFlag_LAYER_IS_3D` を見て、偽なら写しの `count` を 0 にして登録し、Step 1 と同じ場所に「3D レイヤーにしてください」と出す。3D にしたら（UPDATE_PARAMS_UI か PRE_RENDER で）元に戻す。
- [ ] **Step 4: エフェクトの無効化。** `AEGP_GetEffectFlags` の `AEGP_EffectFlags_ACTIVE` が偽なら count 0 として登録する。fx スイッチの切り替えでコマンドが来るかは Task 6 の `fx_off` で確かめる（来ない場合はフックの描画スレッド側で同じフラグを読む）。
- [ ] **Step 5: ビルドとスモーク** — `pwsh tools/build.ps1; pwsh tests/ae/run_smoke.ps1`。Expected: `SMOKE OK`、ログに `hooks installed`、鍵 `(comp, layer)` の登録、PNG にコピー 10 枚が写る（白の範囲が元より広い）。
- [ ] **Step 6: Commit** — `git commit -m "feat(effect): params, registration, 3D/version notices"`

---

### Task 6: 正解との画素比較（aerender、MFR オフ・オン）

**Files:**
- Create: `tests/ae/make_ref.jsx`, `tests/ae/run_ref.ps1`, `tests/ae/cmp.py`

**Interfaces:**
- Consumes: `randamu_dump.exe`（Task 3）、`randamu.aex`（Task 5）。

`make_ref.jsx` はケースごとに 2 つのコンプを作る: `probe_<case>`（対象レイヤー＋randamu）と `ref_<case>`（同じシーンで、対象レイヤーの代わりに `randamu_dump.exe` の出力どおりの本物の複製レイヤー）。共通のシーンは `spike/scenes/probe_3d.jsx` と同じ（クラシック 3D、床、交差する壁、スポットライト、カメラ。影は `ADBE Casts Shadows` を番号で探して設定し、ライトの「カラー」の値は読まない＝`HANDOFF.md` 試験 18 の注意）。ref の複製は、アンカー＝対象のアンカー、位置＝アンカー＋t、回転＝r、拡大縮小＝s、不透明度＝対象の不透明度×o/100。対象自身に変形・親があるケースでは、ref の複製を「対象と同じアンカー・位置・回転・拡大縮小・親を持つヌル」の子にする（ヌルのアンカーを対象のアンカーに合わせると、ヌルの座標＝対象のレイヤー座標になる）。最後にプロジェクトを保存し、レンダーキューに全コンプを TIFF で入れる（`RNDM_TIFF_A` の手順、`HANDOFF.md` S6）。

`run_ref.ps1` は `spike/tools/run_3d_aerender.ps1` と同じ流れで、環境変数で ID を渡さない（エフェクトの自己登録だけで動くこと）。`cmp.py` は `差の最大・平均・>2・>8・>32 の画素数` を出す。合格は差の最大 ≤ 1。

ケース（`-Cases` で選べる）:

| ケース | 内容 |
|---|---|
| `baseline` | 平面、count 5、位置のみ |
| `rotate` | 回転の範囲 60/60/60 |
| `scale` | 拡大縮小 50〜200 |
| `anchor` | 対象のアンカーを中心から (30, −20) ずらす＋回転 |
| `layer_xform` | 対象自身に回転 (10,20,30)・拡大縮小 150%・親のヌル |
| `opacity` | 不透明度 30〜100、対象の不透明度 80% |
| `keep_first` | 1 枚目は元の位置 |
| `png_fx` | PNG 素材（`spike/assets/probe_img.png`）＋対象に「塗り」 |
| `extras` | 影・被写界深度・調整レイヤー（ガウスぼかし）・モーションブラー（カメラが動く） |
| `count100` | count 100、全範囲 |
| `count0` | count 0（ref は対象なし） |
| `identity` | 範囲すべて 0・count 1（ref は元のレイヤー 1 枚） |
| `multi_comp` | 別のコンプに同じレイヤー ID のエフェクトなしレイヤー（ref は元のまま） |
| `fx_off` | エフェクトを無効にした対象（ref は元のレイヤー 1 枚） |
| `reopen` | 保存したプロジェクトを開き直した AE（`saveFrameToPng`）で `baseline` |

- [ ] **Step 1: `make_ref.jsx`・`run_ref.ps1`・`cmp.py` を書き、`baseline` だけ通す**
Run: `pwsh tests/ae/run_ref.ps1 -Cases baseline`
Expected: MFR オフ・オンとも `max=0` か `max=1`。
- [ ] **Step 2: 全ケースを通す**
Run: `pwsh tests/ae/run_ref.ps1`
Expected: 全ケース・MFR オフ・オンで `max<=1`。`multi_comp` でエフェクトのないレイヤーが増えていないこと。
- [ ] **Step 3: 失敗したケースの扱い**: `scale`・`anchor`・`layer_xform` が合わない場合は、行列の形（Task 2 の `MakeM4`）を直して単体テストも直す。`scale` が行列では合わない（コピーの絵が等倍で作られて引き伸ばされる）と分かった場合は、そこで止めて利用者に報告する（仕様の変更が要る）。フックの中で作った鍵が登録と合わない（描画用の複製でコンプの ID が違う）場合も止めて報告する。
- [ ] **Step 4: Commit** — `git commit -m "test: pixel-exact reference comparison suite"`

---

### Task 7: UI での操作の試験と、利用者への引き渡し

**Files:**
- Create: `tests/ae/ui_session.jsx`, `tests/ae/run_ui.ps1`, `docs/randamu-quickstart.md`

- [ ] **Step 1: `ui_session.jsx`**: 見えないデスクトップの AE（UI あり）で、コンプをビューアーで開き（`comp.openInViewer()`）、`comp.time` を 30 回動かし、そのたびに `saveFrameToPng`。途中でコピー数・種・回転の範囲を変え、エフェクトを無効→有効、取り消し（`app.executeCommand(16)`＝取り消し）を行う。各操作の後にログへ `STEP n OK`。
- [ ] **Step 2: `run_ui.ps1`**: 120 秒で終わらなければ「止まった」と判定して強制終了し、`RANDAMU_LOG` のログの最後の `STEP` を出す。ログの「フックの呼び出し元のスレッド（メインかどうか）」の集計も出す。
Run: `pwsh tests/ae/run_ui.ps1`
Expected: 全 STEP が OK、止まらない。変更後の PNG がそれぞれ変わる（種を変えたら配置が変わる）。メインスレッドからの問い合わせがあった場合、その回数と、写しを使ったことがログに出る。
- [ ] **Step 3: メインスレッドで写しを使った場合の正しさ**: キーフレームで動かした回転の範囲で、UI の `saveFrameToPng` の絵が aerender の同じフレームと一致するかを比べる。一致しない（写しの時刻がずれる）なら、利用者への説明に「プレビューでキーフレームの途中の値がずれることがある、書き出しは正しい」と書き、報告する。
- [ ] **Step 4: `docs/randamu-quickstart.md`**: 入れ方（`dev_plugins` のジャンクションは作成済み、AE の再起動）、使い方（3D レイヤーに「randamu」を掛ける、各パラメータ）、分かっている制限（2D は未対応、時間のずらしは未対応、多いと重い＝2500 枚で 1 フレーム約 20 秒、AE 2025 以外では動かない）、困ったとき（ログの出し方 `RANDAMU_LOG`）。
- [ ] **Step 5: Commit** — `git commit -m "test: UI session; docs: quickstart"`
- [ ] **Step 6: HANDOFF.md を更新し、利用者に試してもらう**（AE を起動したままの利用者の環境で、自分の AE を巻き込まないよう、ビルドの置き換えは利用者が AE を閉じているときだけ行う）。
