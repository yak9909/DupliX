# 高速モードの作り直し（普通のプラグイン＋ワールド変形）実装計画

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 高速モードを、フックを使わない普通のプラグインとして作り直す。レイヤーは 2D、エフェクトはコンプと同じ大きさの絵に全コピーを描く。群れの 3D の置き場所は、両モード共通の新しいパラメータ「ワールド変形」で決める。通常 → 高速の切り替えで、レイヤーの 3D の変形を現在フレームの値でワールド変形へ自動で写す。

**Architecture:** AE に依存しない計算（ワールド変形の行列、行列からワールド変形への分解、書き写しの式）を `src/core/world.*` に置いて単体テストで確かめる。通常モードはフックの M4 に `G`（ワールド変形の行列）を掛けるだけ。高速モードは前処理・描画で「仮想の 3D レイヤー（コンプ中央・初期状態）× G」から各コピーを画面に投影し、既存のラスタライザでレイヤー座標の絵に描く。11/24 版のフック経由の面（`fast_registry`・`PlaneM4`・フックの高速モードの分岐）は消す。

**Tech Stack:** C++17、MSVC v143（`pwsh tools/build.ps1`）、AE SDK 25.6、ExtendScript、PowerShell 7、Python（`spike\.venv`）。

**Spec:** `docs/superpowers/specs/2026-11-24-randamu-sublayer-design.md` の 12.13（11/25 の決定を本計画と同時に追記する）、12.11（高速モードで失われるもの）。前の計画 `docs/superpowers/plans/2026-11-24-fast-mode.md` と判断の一覧 `2026-11-24-fast-mode-rulings.md`（ストレートアルファなど）も読む。

## 利用者の決定（11/25。蒸し返さない）

- 高速モードはフックを使わない普通のプラグイン。レイヤーは **2D**。レイヤーの 2D の変形は、描いた 1 枚の絵に掛かる。
- **ワールド変形はどのモードにもある。** 群れの置き場所は「レイヤーの変形 × ワールド変形」（掛け合わせ）。群れはワールド変形のパラメータで動かす前提で作る。
- **通常 → 高速**: 現在フレームのレイヤーの 3D の変形（親を含む）をワールド変形に掛け合わせて写し、レイヤーを初期状態（2D、位置＝コンプの中央、拡大縮小 100%、回転 0、親なし）にする。見た目は変わらない。
- **高速 → 通常**: 変形は写さない。レイヤーを 3D に戻すだけ。
- 複数の複製対象は、この計画のあと（別の計画）。

## この計画で決めたこと（利用者に確認してもらう）

1. **ワールド変形のパラメータ**: 位置 X/Y/Z（px、既定 0、レイヤー座標でのずらし）、回転 X/Y/Z（角度、既定 0）、拡大縮小 X/Y/Z（%、既定 100）。グループ「ワールド変形」にまとめる。方向（Orientation）は持たない（書き写しでは方向と回転をまとめて回転 X/Y/Z にする）。
2. **行列の定義**: `G = T(−A) · S · Rx · Ry · Rz · T(A + P)`（行ベクトル、A はレイヤーのアンカー）。既定値で単位行列なので、既存のプロジェクトの見た目は変わらない。回転の順と符号は AE のレイヤーの回転と同じにする（Task 2 で AE と照合）。
3. **書き写しの式**: `G_new = G_old · L2W(t) · V⁻¹`。`V` は初期状態の 3D レイヤーのレイヤー→ワールド（`T(−A) · T(コンプの幅/2, 高さ/2, 0)`）。`G_new` を位置・回転・拡大縮小に分解して書く。せん断（親の不均一な拡大縮小と回転の組み合わせ）は表せないので近似になる（状態の欄で案内しない。quickstart に書く）。
4. **アニメーションのある変形**: 現在フレームの値だけを写す（利用者の決定）。レイヤーの位置・回転・方向・拡大縮小のキーフレームとエクスプレッションは消して初期値にする。親は外す。アンカーはそのまま。すべて 1 つの取り消しの単位にまとめる。
5. **書き写しが動く条件**: 「高速モード」のチェックが、利用者の操作でオフ → オンに変わった `PF_Cmd_USER_CHANGED_PARAM` だけ。読み込み・エフェクトの複製・プリセットの適用では動かない。「高速モード」は時間で変えられないようにする（`PF_ParamFlag_CANNOT_TIME_VARY`、変更の通知のため `PF_ParamFlag_SUPERVISE`）。
6. **高速モードの絵の置き方**: 画面の点 s（コンプ px）は、出力のレイヤー px `q = s − (幅/2, 高さ/2) + A` に描く。レイヤーが初期状態なら画面と 1 対 1 に重なり、あとでレイヤーの 2D の位置・回転・拡大縮小を変えると絵全体が動く。
7. **状態の欄**: 高速モードで 3D レイヤー → 「2D レイヤーにしてください」。通常モードで 2D → 今までどおり「3D レイヤーにしてください」。高速モードはフックがなくても動く（フックがない版でも「準備できています」）。11/24 版の「ライトを受けるをオフに」は消す（2D レイヤーにはライトが当たらない）。
8. **後回し**: Furikake のような画質の選択（多重サンプリングなど）、GPU（CUDA）での描画、複数の複製対象。

## Global Constraints

- 対象は After Effects 2025（25.6）、Windows x64。通常モードはクラシック 3D のみ（今どおり）。
- 新しいパラメータは `RP_FAST`・`RD_FAST` の後ろに足す。既存のストリーム番号とディスク ID は動かさない。
- `ScatterParams`（96 バイト）と `SeqData`（112 バイト）の並びは変えない（プロジェクトに生のまま保存される。`static_assert` あり）。ワールド変形は別の構造体に持つ。
- ワールド変形が既定値のとき、通常モードの見た目は今と同じ（`run_ref.ps1` が REF ALL PASS のまま）。
- 前処理では必ず `GuidMixInPtr` を呼ぶ。`result_rect` は要求との共通部分、全体は `max_result_rect`。
- フックの中（BEE の仮想関数）から UI スレッドで AEGP を呼ばない。
- 利用者の aep（`tests/ae/user/`）は必ずコピーを開く。AE を動かす試験の前に、AfterFX・aerender が動いていないことを確かめる。ビルドの置き換えは利用者が AE を閉じているときだけ。
- 文言は今の方式（`UiName` で日本語／英語）。英語化は改名の作業で別途。
- AE のエフェクトの画素はストレートアルファ（ラスタライザは内部でプリマルチプライして合成する。今の `raster.cpp` のまま）。

## Review Focus

1. **書き写しの直前と直後で見た目が変わらない**（親付き・方向あり・軌道ヌルの Y 回転 26°、利用者のシーンの形） → Task 5 の `toggle_parent` ケース。
2. **書き写しを 1 回の取り消しで全部戻せる**（3D スイッチ・位置・親・キーフレーム・ワールド変形の値） → Task 5 の `toggle_undo` の確認。
3. **高速モードでカメラやワールド変形が動くとき、キャッシュが古い絵を返さない**（2D レイヤーはカメラに依存すると AE が知らない） → Task 4 の `fast_anim`。
4. **既存のプロジェクト（ワールド変形のパラメータがない頃に保存）を開いても、通常モードの見た目が変わらない** → Task 3 の `run_ref.ps1`（既定値で単位行列）と、Task 3 Step 6 の旧 aep を開く確認。
5. **フックが入らない版・設定でも高速モードは動く** → Task 4 の `fast_nohooks`（`RANDAMU_HOOKMASK=0`）。

---

## ファイル構成

```
src/core/world.h, world.cpp        … ワールド変形の構造体、G の組み立て、行列の分解、書き写しの式（純粋関数）
src/core/raster.h, raster.cpp      … 出力の座標のずらし（画面 px ↔ レイヤー px）を足す
src/core/fast_plane.h, .cpp        … PlaneM4・AnchorDepth を消す（CopyHomography・CopyDepth は残す）
src/effect/randamu_params.h        … ワールド変形のパラメータを末尾に追加
src/effect/randamu.cpp             … パラメータ、通常モードの登録に G、高速モードの前処理・描画、書き写し、状態の欄
src/effect/transfer.h, .cpp        … 書き写し（AEGP でレイヤーを読む・書く、取り消しの単位）
src/hook/registry.h                … ParamSnapshot に WorldXform を足す
src/hook/bee_hooks.cpp             … ワールド変形を読み、M4 に G を掛ける。高速モードの分岐を消す
src/hook/fast_registry.*           … 消す
tests/unit/unit_tests.cpp          … world の単体テスト、raster のずらしのテスト。fast_registry のテストを消す
tests/ae/fast_mode.jsx, check_fast_mode.py, run_fast_mode.ps1 … 新しい高速モードの試験に書き直す
tests/ae/transfer.jsx, check_transfer.py, run_transfer.ps1    … 書き写しの試験（新規）
tests/ae/world_xform.jsx, check_world_xform.py                … 通常モードのワールド変形の試験（新規。run_jsx.ps1 で回す）
```

---

### Task 1: 実現性の確認（短い実験。製品コードは書かない）

AE の中でしか分からない 4 点を先に確かめる。1 つでも駄目なら、ここで止めて利用者に相談する（設計の決定が変わる）。

**Files:**
- Create: `spike/transfer_probe/`（使い捨て。実験コードは製品に流用しない。結果は開発記録に書く）
- Modify: `docs/experiments/2026-11-25-dev-log.md`（新規でよい。事実と、うまくいかなかったこと）

- [ ] **Step 1: 実験用のビルド** — 今のエフェクトに、環境変数 `RANDAMU_PROBE_TRANSFER=1` のときだけ動く分岐を一時的に足す（実験が済んだら消す）: 「高速モード」に `PF_ParamFlag_SUPERVISE` を付け、`USER_CHANGED_PARAM` で `RP_FAST` が変わったら、(a) `params[RP_POS_X]` の値を書き換えて `PF_ChangeFlag_CHANGED_VALUE` を立てる、(b) `AEGP_StartUndoGroup` → `AEGP_SetStreamValue` でレイヤーの位置を変える → `AEGP_SetLayerParent(L, nullptr)` → `AEGP_SetLayerFlag(L, AEGP_LayerFlag_LAYER_IS_3D, FALSE)` → `AEGP_EndUndoGroup`、をログ付きで行う。キーフレーム付きのストリームは `AEGP_DeleteKeyframe` で消してから値を設定、エクスプレッションは `AEGP_SetExpressionState(FALSE)` か `AEGP_SetExpression` で空にする。
- [ ] **Step 2: 確かめる** — 見えないデスクトップの AE（`run_jsx.ps1`）で、次をログで確かめる。
  1. スクリプトの `fx.property(15).setValue(1)` で `USER_CHANGED_PARAM` が来るか（来なければ、書き写しの自動試験は Task 5 で手動の確認に切り替える。その旨を記録）。
  2. (a)(b) がすべて成功し、AE が止まらず、`app.executeCommand(16)`（取り消し）1 回で全部戻るか。
  3. 2D レイヤーのエフェクトの前処理で `AEGP_GetEffectCameraMatrix` がコンプのカメラを返すか（カメラのないコンプでは既定のカメラ）。
  4. 2D レイヤーのエフェクトで、出力の `max_result_rect` を負の原点のコンプ大（例: (−200, −150, 200, 150)）にでき、AE が 2D の変形を掛けて正しい場所に出すか（実験 S1 と同じ。位置＝コンプの中央のとき出力のレイヤー px (q) が画面の q − A + 中央に出る）。
- [ ] **Step 3: 記録して実験の分岐を消す** — 結果を開発記録に書く。分岐を消してビルドが今の版と同じに戻ることを確かめる（`git diff src/` が空）。
- [ ] **Step 4: Commit** — `git commit -m "docs: transfer and 2D fast-mode feasibility probe"`

---

### Task 2: ワールド変形の計算（純粋関数）

**Files:**
- Create: `src/core/world.h`, `src/core/world.cpp`（`randamu.vcxproj`・`unit_tests.vcxproj` に追加）
- Test: `tests/unit/unit_tests.cpp`

**Interfaces:**
- Produces:
  - `struct WorldXform { double pos[3] = {0,0,0}; double rotDeg[3] = {0,0,0}; double scale[3] = {100,100,100}; };`（scale は %）
  - `void WorldM4(const WorldXform& w, double ax, double ay, double out[16]);` … `G = T(−A)·S·Rx·Ry·Rz·T(A+P)`（行ベクトル、A = (ax, ay, 0)）。回転の符号と順は AE のレイヤーの X/Y/Z 回転と同じ（`detail::MakeM4` の規約と揃え、違う場合はここに理由を書く）。
  - `bool DecomposeWorld(const double G[16], double ax, double ay, WorldXform* out);` … `WorldM4` の逆。3x3 部分の行の長さを拡大縮小、正規化した行から回転（ジンバルロックでは Z = 0 に固定）。行列式が負（鏡映）なら拡大縮小 Z を負にする。最後の列が (0,0,0,1) でなければ false。
  - `void InitialLayerToWorld(double ax, double ay, double compW, double compH, double out[16]);` … `V = T(−A)·T(compW/2, compH/2, 0)`
  - `bool TransferToWorld(const WorldXform& cur, const double layerToWorld[16], double ax, double ay, double compW, double compH, WorldXform* out);` … `G_new = G_old·L2W·V⁻¹` を分解したもの

- [ ] **Step 1: 失敗するテストを書く**
  - `world_default_identity`: 既定の `WorldXform` で `WorldM4` が単位行列（誤差 1e-12）。
  - `world_roundtrip`: 位置 (12, −30, 400)、回転 (25, −40, 70)、拡大縮小 (80, 120, 150)、アンカー (288.5, 340) で `DecomposeWorld(WorldM4(w))` の `WorldM4` が元と一致（1e-9）。回転 Y = 90°（ジンバルロック）でも行列が一致。
  - `world_anchor_fixed`: 回転と拡大縮小だけのとき、アンカーの点 (ax, ay, 0) が動かない。
  - `transfer_keeps_world_placement`: 任意の L2W（方向 (10, 20, 30)＋回転 X/Y/Z＋位置＋拡大縮小 (90, 110, 100)＋親の回転を掛けたもの）と任意の `cur` で、`WorldM4(out)·V` が `WorldM4(cur)·L2W` と一致（1e-6）。レイヤーの点 (0,0,0)・(577,680,0)・(100,50,0) を両方で写して比べる。
  - `transfer_shear_is_approximate`: 親の不均一な拡大縮小 (200, 100, 100) と子の Z 回転 30° を掛けた L2W（せん断を含む）でも `TransferToWorld` は true を返す（近似で写すことの確認。誤差の大きさはテストで縛らない）。
- [ ] **Step 2: 失敗を確認** — `pwsh tools/build.ps1 -Target unit` → 未定義でビルド失敗。
- [ ] **Step 3: 実装** — 上の Interfaces どおり。`Invert4`・`Mul4` は `fast_plane.h` のものを使う。
- [ ] **Step 4: 合格を確認** — `build/unit/unit_tests.exe` → `ALL PASS`。
- [ ] **Step 5: Commit** — `git commit -m "feat(core): world transform matrix, decomposition and transfer"`

---

### Task 3: ワールド変形のパラメータと通常モード

**Files:**
- Modify: `src/effect/randamu_params.h`、`src/effect/randamu.cpp`、`src/hook/registry.h`、`src/hook/bee_hooks.cpp`、`tests/ae/make_ref.jsx`（パラメータの番号が変わらないことの確認だけ）
- Create: `tests/ae/world_xform.jsx`, `tests/ae/check_world_xform.py`

**Interfaces:**
- Consumes: Task 2 の `WorldXform`・`WorldM4`。
- Produces:
  - `RP_WORLD_TOPIC, RP_WORLD_POS_X, RP_WORLD_POS_Y, RP_WORLD_POS_Z, RP_WORLD_ROT_X, RP_WORLD_ROT_Y, RP_WORLD_ROT_Z, RP_WORLD_SCALE_X, RP_WORLD_SCALE_Y, RP_WORLD_SCALE_Z, RP_WORLD_TOPIC_END`（`RP_FAST` の後ろ）と同名の `RD_*`。
  - `ParamSnapshot` に `WorldXform world;`
  - `bool ReadWorldFromEffect(AEGP_EffectRefH eff, const A_Time& t, WorldXform* out);`（`ReadParamsFromEffect` から呼ぶ）
  - `WorldXform WorldFromArray(PF_ParamDef* params[]);`

- [ ] **Step 1: パラメータを足す** — グループ「ワールド変形」（`PF_ADD_TOPIC`／`PF_END_TOPIC`）、位置 X/Y/Z（`PF_ADD_FLOAT_SLIDERX`、−100000〜100000、目安 −2000〜2000、既定 0）、回転 X/Y/Z（`PF_ADD_ANGLE`、既定 0）、拡大縮小 X/Y/Z（−10000〜10000 %、目安 0〜400、既定 100）。「高速モード」に `PF_ParamFlag_SUPERVISE | PF_ParamFlag_CANNOT_TIME_VARY` を付ける。
- [ ] **Step 2: 通常モードで G を掛ける** — フックでコピー i の M4 を求める 2 か所（`bee_hooks.cpp` の `ComputeCopy` の呼び出し）で、`M4 = copyM4 · WorldM4(s.world, s.anchorX, s.anchorY)` にする。`ReadParamsFromEffect`（描画スレッド）と `RegisterSelf` の `Array` の経路でワールド変形を読む。`SeqData` の経路（読み込み直後のメインスレッド）は、シーケンスデータの末尾に足した `WorldXform` を使う: `SeqData` のあとに `WorldXform world; int hasWorld;` を持つ `SeqDataV2` を作り、ハンドルの大きさ（`PF_GET_HANDLE_SIZE`）が `sizeof(SeqDataV2)` 以上のときだけ読む。古い大きさのハンドルは書くときに `PF_RESIZE_HANDLE` で広げる。
- [ ] **Step 3: 試験を書く** — `world_xform.jsx` は、同じ素材で 2 つのコンプを作る: `wx_param`（レイヤーは初期状態の 3D、ワールド変形 = 位置 (60, −40, 300)・回転 (20, 35, −15)・拡大縮小 (120, 120, 120)、コピー 10 枚）と `wx_layer`（ワールド変形は既定、レイヤー自身の変形を `G·L2W` と同じになるように置いたもの。アンカー中心なので位置 = 中央 + (60, −40, 300)、X/Y/Z 回転 = (20, 35, −15)、拡大縮小 120%）。aerender の MFR オフで描き、`check_world_xform.py` で差の最大 ≤ 1 を確かめる（`WORLD ALL PASS`）。回転の順が AE と違えばここで分かる → Task 2 の `WorldM4` を直す。
- [ ] **Step 4: 合格を確認** — `pwsh tools/build.ps1 -Target all; build/unit/unit_tests.exe; pwsh tests/ae/run_jsx.ps1 -Jsx tests/ae/world_xform.jsx -Name world -Check tests/ae/check_world_xform.py` → `ALL PASS`、`WORLD ALL PASS`。
- [ ] **Step 5: 既存の試験** — `pwsh tests/ae/run_ref.ps1` → `REF ALL PASS`、`pwsh tests/ae/run_ui.ps1` → `UI ALL PASS`、fx_remove → `FX REMOVE ALL PASS`。
- [ ] **Step 6: 古いプロジェクト** — 今の commit（963e092）の版で保存した `tests/ae/user/DupliX Test.aep` のコピーを `pwsh tests/ae/run_as_is.ps1` で描き、`AS-IS ALL PASS`（見た目が変わらない）。
- [ ] **Step 7: Commit** — `git commit -m "feat: world transform parameters (both modes)"`

---

### Task 4: 高速モードを 2D レイヤーの普通のプラグインに作り直す

**Files:**
- Modify: `src/effect/randamu.cpp`、`src/core/raster.h`、`src/core/raster.cpp`、`src/core/fast_plane.*`、`src/hook/bee_hooks.cpp`、`src/hook/bee_hooks.h`
- Delete: `src/hook/fast_registry.h`、`src/hook/fast_registry.cpp`（vcxproj からも）、単体テストの `fast_registry_*`・`fast_plane_maps_pixels_1to1`・`fast_plane_depth_is_anchor`
- Rewrite: `tests/ae/fast_mode.jsx`、`tests/ae/check_fast_mode.py`、`tests/ae/run_fast_mode.ps1`
- Test: `tests/unit/unit_tests.cpp`

**Interfaces:**
- Consumes: Task 2 の `WorldM4`・`InitialLayerToWorld`、Task 3 の `ParamSnapshot::world`・`WorldFromArray`。
- Produces:
  - `RasterImage` に `double screenShiftX = 0, screenShiftY = 0;`（dst だけが使う。dst のバッファの画素 (i, j) は、全解像度の画面 px `((i + originX + 0.5) / dsX + screenShiftX, (j + originY + 0.5) / dsY + screenShiftY)`）。高速モードでは `screenShift = (幅/2 − ax, 高さ/2 − ay)`。
  - `SelfState::Need2D`（「2D レイヤーにしてください」）。`SelfState::FastLights` は消す。

- [ ] **Step 1: 失敗するテストを書く** — `raster_screen_shift`: 単位の射影のコピー 1 枚、dst の `screenShift = (10, −5)` で、dst の (i, j) が src の (i + 10, j − 5) と一致（8 bpc で完全一致）。
- [ ] **Step 2: 失敗を確認** — ビルド失敗（フィールドなし）。
- [ ] **Step 3: ラスタライザ** — `CopyBox` と `DrawRows` の画面 px の計算に `screenShift` を足す。
- [ ] **Step 4: 合格を確認** — `build/unit/unit_tests.exe` → `ALL PASS`（既存の raster のテストも）。
- [ ] **Step 5: 前処理と描画** — 高速モードのとき:
  - 前処理: カメラ（`AEGP_GetEffectCameraMatrix`）、コンプの大きさ、アンカー、パラメータ（`CheckoutScatterParams` とワールド変形）を読む。`W = WorldM4(world, A) · InitialLayerToWorld(A, 幅, 高さ)` を `FastPre` に持つ（実際のレイヤーの `AEGP_GetLayerToWorldXform` は使わない）。`max_result_rect` = `(ceil((ax − 幅/2)·dsX), ceil((ay − 高さ/2)·dsY))` からコンプの大きさ × ds、`result_rect` は要求との共通部分。`GuidMixInPtr` に、ビルド ID・カメラ行列・dist・`W`・コンプの大きさ・ds・時刻・アンカー・パラメータ全部を混ぜる。フックの登録（`FastSetPlane`）はしない。
  - 描画: 今の `FastSmartRender` のまま、`f->W` が上の `W`、dst の `screenShift` を設定する。
  - 高速モードはフックの有無に関係なく動く。`RegisterSelf` は、高速モードのとき `snap.valid = false` で登録する（フックはそのレイヤーで元の関数を呼ぶ）。
- [ ] **Step 6: フックの高速モードの分岐と `fast_registry` を消す** — `bee_hooks.cpp` の `s.p.fast` の 3 か所（Count = 1、Rects = 面、不透明度の早期 return）を消す。`fast_plane.*` から `PlaneM4`・`AnchorDepth` を消す。
- [ ] **Step 7: 状態の欄** — 高速モードで 3D → `Need2D`。高速モードで 2D → `Ready`（フックがなくても）。通常モードは今どおり。
- [ ] **Step 8: 試験を書き直す** — `fast_mode.jsx` の各ケースは、`nm_<case>`（通常モード、3D レイヤーを初期状態に置き、ワールド変形で配置）と `fm_<case>`（同じパラメータで高速モード、レイヤーは 2D の初期状態）を作る。ケースと合格の条件:

  | ケース | 内容 | 合格の条件 |
  |---|---|---|
  | `fast_identity` | コピー 1 枚・範囲 0、ワールド変形 = 回転 (25, −30, 10)、カメラを動かす | 差の最大 ≤ 8、平均 ≤ 0.5 |
  | `fast_scatter` | コピー 10 枚、位置・回転・拡大縮小の範囲、ワールド変形あり | 差の平均 ≤ 2、差 > 32 の画素 ≤ 1% |
  | `fast_anim` | カメラとワールド変形にキーフレーム、30 フレーム | 各フレームが、単独で描き直したものと差の最大 ≤ 1 |
  | `fast_half` | `fast_scatter` を 1/2 の解像度 | 1/2 の nm と上と同じ条件 |
  | `fast_nocam` | カメラなし | `fast_scatter` と同じ |
  | `fast_count0` | コピー 0 枚 | fm が全面透明 |
  | `fast_grow` | 前にドロップシャドウ（入力の原点が負） | `fast_scatter` と同じ |
  | `fast_move2d` | `fast_scatter` の fm で、レイヤーの 2D の位置を (+50, +20) | fm の絵が fm_scatter を (50, 20) ずらしたものと差の最大 ≤ 1 |
  | `fast_nohooks` | `RANDAMU_HOOKMASK=0` で aerender、`fast_scatter` の fm | `fast_scatter` の fm と差の最大 ≤ 1 |
  | `fast_remove` | 高速モードのままエフェクトを削除（同じセッション） | 素のレイヤーと一致（今の試験と同じ） |

  `fast_toggle` は Task 5 の書き写しの試験に移すので消す。
- [ ] **Step 9: 合格を確認** — `pwsh tools/build.ps1 -Target all; build/unit/unit_tests.exe; pwsh tests/ae/run_fast_mode.ps1` → `ALL PASS`、`FAST ALL PASS`（MFR オフ・オン）。比較画像 `build/test/fast/compare_<case>.png` を書き出す。
- [ ] **Step 10: 既存の試験** — `run_ref.ps1`、`run_ui.ps1`、fx_remove、`run_orbit_order.ps1`（高速モードの前後の試験。2D で作り直す必要があれば、同じ確認内容で直す）→ すべて合格。
- [ ] **Step 11: Commit** — `git commit -m "feat: fast mode as an ordinary plugin on a 2D layer"`

---

### Task 5: 通常 → 高速の書き写し・高速 → 通常の切り替え

**Files:**
- Create: `src/effect/transfer.h`, `src/effect/transfer.cpp`（vcxproj に追加）
- Modify: `src/effect/randamu.cpp`（`USER_CHANGED_PARAM` から呼ぶ）
- Create: `tests/ae/transfer.jsx`, `tests/ae/check_transfer.py`, `tests/ae/run_transfer.ps1`
- Modify: `docs/randamu-quickstart.md`

**Interfaces:**
- Consumes: Task 2 の `TransferToWorld`、Task 3 の `WorldFromArray` と `RP_WORLD_*`。
- Produces:
  - `enum class TransferResult { Done, DoneAnimationDropped, Failed };`
  - `TransferResult TransferLayerToWorld(PF_InData* in_data, PF_ParamDef* params[]);` … 通常 → 高速。メインスレッドの `USER_CHANGED_PARAM` の中だけで呼ぶ。
  - `bool SetLayer3D(PF_InData* in_data, bool on);` … 高速 → 通常（3D に戻すだけ）。

- [ ] **Step 1: 書き写しを実装** — `USER_CHANGED_PARAM` で `RP_FAST` が変わったとき（`PF_UserChangedParamExtra::param_index`）、オンになったなら `TransferLayerToWorld`、オフになったなら `SetLayer3D(true)`。`TransferLayerToWorld` の手順（Task 1 で確かめた方法で）:
  1. 現在のコンプ時刻で `AEGP_GetLayerToWorldXform`、アンカー、コンプの大きさを読む。
  2. `TransferToWorld(WorldFromArray(params), L2W, ...)` の値を `params[RP_WORLD_*]` に書き、`PF_ChangeFlag_CHANGED_VALUE` を立てる（ワールド変形のパラメータがキーフレーム付きなら、Task 1 の結果に従う。現在時刻のキーフレームの値を変えるのが既定）。
  3. `AEGP_StartUndoGroup("高速モードに切り替え")` → 位置・方向・X/Y/Z 回転・拡大縮小のキーフレームを消し、エクスプレッションを外す → 位置 (幅/2, 高さ/2, 0)、方向 0、回転 0、拡大縮小 100% → 親を外す → 3D をオフ → `AEGP_EndUndoGroup`。アンカーは変えない。
  4. キーフレームかエクスプレッションを消したなら `DoneAnimationDropped`（状態の欄に「レイヤーのアニメーションは写していません（取り消しで戻せます）」を、次の状態の更新まで出す）。
  - 書き写しのあとは `out_data->out_flags |= PF_OutFlag_REFRESH_UI` で UI を更新する。
- [ ] **Step 2: 試験を書く** — `transfer.jsx` のケース（スクリプトでチェックを切り替える。Task 1 で `setValue` から `USER_CHANGED_PARAM` が来ないと分かった場合は、下のケースを手動の確認の手順として `HANDOFF.md` に書き、利用者の確認に回す）:
  - `toggle_plain`: 3D レイヤー（位置・方向・回転・拡大縮小 110%、ワールド変形あり）、通常モードで描く（before）→ 高速をオン → 描く（after）。before と after が Task 4 の `fast_scatter` と同じ条件で近い。レイヤーが 2D・位置 = 中央・親なし。
  - `toggle_parent`: 利用者のシーンの形（軌道ヌルの子、ヌルの Y 回転 26°、レイヤーの位置 Z あり）。同じ条件。
  - `toggle_anim`: 位置にキーフレーム。after がその時刻の before と近い。キーフレームが消えている。
  - `toggle_undo`: `toggle_parent` のあと `app.executeCommand(16)`（取り消し）1 回で、3D・位置・親・ワールド変形の値が全部元どおり。
  - `toggle_back`: `toggle_plain` のあと高速をオフ → レイヤーが 3D、描いた絵が before と差の最大 ≤ 1（初期状態の 3D レイヤー × 写したワールド変形 = 元の配置）。
- [ ] **Step 3: 合格を確認** — `pwsh tests/ae/run_transfer.ps1` → `TRANSFER ALL PASS`。
- [ ] **Step 4: 利用者のシーン** — `tests/ae/user/DupliX Test.aep` のコピーで、花のレイヤーの高速モードをスクリプトでオンにし、前後の絵を `build/test/transfer/user_before.png`・`user_after.png` に書き出す（目視用）。
- [ ] **Step 5: quickstart** — 「高速モード」の項を書き直す: ワールド変形で群れを動かす、オンにした瞬間にレイヤーの変形が写る（現在フレームの値だけ、アニメーションと親は消える、取り消しで戻る、せん断は近似）、オフに戻すと 3D になるだけ、レイヤーの 2D の変形は絵に掛かる、高速モードで失われるもの（12.11 のうち、ほかのレイヤーとの前後はレイヤーの重ね順で決まる、に置き換え）。
- [ ] **Step 6: 全部の試験** — 単体・`run_ref`・`run_ui`・fx_remove・`run_fast_mode`・`run_transfer`・`run_as_is` → すべて合格。
- [ ] **Step 7: Commit と引き継ぎ** — `git commit -m "feat: transfer the layer transform to the world transform when fast mode turns on"`、push。`HANDOFF.md` の冒頭に再開の手順を書き、利用者に AE で試してもらう（ビルドの置き換えは AE を閉じているときだけ）。
