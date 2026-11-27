# 通常モードの軽量化（群れをまとめる） 実装計画

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 通常モードに「軽量化（群れをまとめる）」を足し、条件を満たすコピーを群れごとに 1 枚の面として AE に描かせる。まとめないコピーとほかのレイヤーとの前後は今と同じに保つ。

**Architecture:** エフェクトの出力は変えない。フック（数・変形・不透明度）が「まとめないコピー＋群れの面」を答え、新しく差し替える AEGPDriver の `BEE_RenderTextureForArtisan` が、群れの面のコピーにだけ DupliX が描いた群れの絵（`AEGP_WorldSuite3` の絵。AE が `delete` する）を渡す。群れ分けはコンプと時刻ごとに 1 回、描画スレッドのフックの中で AEGP から読んだカメラ・ほかのレイヤーで求め、全フックで共有する。

**Tech Stack:** C++17（MSVC 2022）、AE 2025 SDK、BEE.dll/AEGPDriver.aex の内部（今のフックと同じ方式）、既存の CPU 描画（`src/core/raster.*`）、AE の自動試験（`tests/ae/`）。

**Spec:** `docs/superpowers/specs/2026-11-25-merge-optimization-design.md`（実験は `docs/experiments/2026-11-25-dev-log.md` 5b 節、`spike/atlas/texture_swap_spike.patch`）

## Global Constraints

- まとめないコピーの画素は、軽量化オフのときと 1 つも変わらない（差 0）。
- 群れの中身は縁などで 1〜2 段階の差を許す（パラメータの説明に書く）。
- ほかのレイヤーとの前後は、まとめない場合と入れ替わらない。
- 群れの面は、群れの絵の矩形の範囲で、ほかの面と交差しない奥行き d に置く（設計書 5.9 の 3。Task 4 で継ぎ目が出ないと分かれば、色がある画素だけの判定に緩めてよい）。
- 新しいパラメータは末尾に足す（ストリーム番号・ディスク ID を動かさない）。既定オフ。
- 1 レイヤーに 1 エフェクトだけで成立する（補助レイヤーなどを作らない）。クラシック 3D だけが対象。
- フックは AE のメイン スレッドで AEGP を呼ばない（今と同じ）。
- AE を動かす試験の前に AfterFX・aerender が動いていないことを確かめる。利用者の aep は必ずコピーを開く。ビルドの置き換えは利用者が AE を閉じているときだけ。
- Task 3 の速さが「通常モードの半分以下の時間」に届かなければ、Task 5 以降に進まず利用者に相談する（設計書 5.9 の 4）。

## Review Focus

1. **コピーの一部がカメラの後ろ・カメラの面にかかる**: そのコピーはまとめず 1 枚ずつ描く（投影が破綻しない）→ Task 5 の `merge_near_plane_individual`。
2. **ほかのレイヤーのエフェクトが範囲を広げている（ドロップシャドウなど）**: 範囲が分からないので、その面は無限に広い平面として扱う（前後を誤らない）→ Task 2 の `scene_effect_layer_unbounded`。
3. **軽量化オンの DupliX が同じコンプに 2 つ以上**: 群れ分けはコンプで 1 つ。別のレイヤーの群れの面どうしの前後が中身と合う → Task 6 の `merge_two_layers`（AE）。
4. **MFR オンで同じフレームを複数のスレッドが同時に描く**: 群れ分けは 1 回だけ求め、全スレッドで同じものを使う → Task 3 の `plan_cache_one_build`、Task 7 の MFR の試験。
5. **カメラのないコンプ**: 最初の版ではまとめない（状態に理由を出す）。今と同じ絵になる → Task 2 の `scene_no_camera_no_merge`。

---

## ファイル構成

```
src/effect/duplix_params.h     … RP_MERGE / RD_MERGE を末尾に追加
src/effect/duplix.cpp          … パラメータの定義・説明、高速モードのとき灰色、状態の欄に理由
src/hook/registry.h, scatter.h  … ParamSnapshot / ScatterParams に merge を追加
src/core/merge_geom.h/.cpp      … 純粋な幾何（画面の四角形、重なり、平面のどちら側か、奥行きの範囲、群れの面の M3/M4）
src/core/merge_plan.h/.cpp      … 群れ分け（入力: カメラ・コピー・ほかの面。出力: サブレイヤーの一覧と群れ）
src/hook/merge_scene.h/.cpp     … 描画スレッドで AEGP から 1 フレームの入力を集める（カメラ・ほかのレイヤー・ライト・理由）
src/hook/merge_cache.h/.cpp     … コンプと時刻ごとの群れ分けの保存（1 回だけ作る）
src/hook/merge_texture.h/.cpp   … 群れの絵を描いて AEGP の絵にする
src/hook/bee_hooks.cpp          … Count/Rects/Opacity が群れ分けを使う。BEE_RenderTextureForArtisan(+WithReceipt) の差し替え
tests/unit/unit_tests.cpp       … merge_geom / merge_plan / merge_cache の単体試験
tests/ae/merge.jsx, run_merge.ps1, check_merge.py … AE の試験（置き方・前後・継ぎ目・速さ・経路）
docs/duplix-quickstart.md      … 使い方の節
```

---

### Task 1: パラメータ「軽量化（群れをまとめる）」

**Files:**
- Modify: `src/effect/duplix_params.h`（`RP_MERGE` を `RP_WORLD_TOPIC_END` の後、`RD_MERGE` を末尾に）
- Modify: `src/core/scatter.h`（`ScatterParams` に `bool merge = false;`）
- Modify: `src/effect/duplix.cpp`（`ParamsSetup` にチェック。`ParamsFromArray` で読む。`UpdateParamsUi` で高速モードのとき灰色）
- Modify: `src/hook/bee_hooks.cpp`（`ReadParamsFromEffect` で `RP_MERGE` を読む）
- Test: `tests/unit/unit_tests.cpp`

**Interfaces:**
- Produces: `RP_MERGE`、`RD_MERGE`、`ScatterParams::merge`（Task 3 以降のフックが `s.p.merge` を見る）

- [ ] **Step 1: 失敗する試験 `params_merge_last`**: `RP_MERGE == RP_WORLD_TOPIC_END + 1`、`RP_NUM_PARAMS == RP_MERGE + 1`、`RD_MERGE == RD_WORLD_TOPIC_END + 1`。`ParamsFromArray` に merge=1 のチェックを渡すと `p.merge == true`、渡さない（古いプロジェクト）と false。
- [ ] **Step 2: 実行して失敗を確認** — `pwsh tools/build.ps1 -Target unit` → コンパイルエラー（`RP_MERGE` がない）。
- [ ] **Step 3: 実装**。名前は「軽量化（群れをまとめる）」（CP932 で渡す。今の名前と同じ扱い）、説明は設計書 5.7 の文。
- [ ] **Step 4: 単体 ALL PASS。AE の回帰**: `pwsh tests/ae/run_ref.ps1`（REF ALL PASS）、`pwsh tests/ae/run_ui.ps1`（UI ALL PASS）、`pwsh tests/ae/run_transfer.ps1`。
- [ ] **Step 5: Commit** `feat(params): merge (lightweight) checkbox, off by default`

### Task 2: 1 フレームの入力を描画スレッドで集める（関門: 読めなければ止めて相談）

**Files:**
- Create: `src/hook/merge_scene.h/.cpp`
- Modify: `src/hook/bee_hooks.cpp`（診断用に、`DUPLIX_LOG` のとき Count のフックから 1 回呼んでログに出す）
- Test: `tests/unit/unit_tests.cpp`（理由の判定は純粋関数で）、AE の試験 `tests/ae/merge.jsx` の `scene_*` ケース

**Interfaces:**
- Produces:
```cpp
namespace rnd {
enum MergeBlock : unsigned {  // whole-layer reasons not to merge (status / log)
    MB_NONE = 0, MB_NO_CAMERA = 1, MB_MOTION_BLUR = 2, MB_DOF = 4, MB_LIGHTS = 8, MB_SHADOWS = 16,
    MB_BLEND = 32, MB_OPACITY = 64, MB_MATTE = 128, MB_UNREADABLE = 256 };
struct ScenePlane {            // another surface: a layer's rectangle in world space (row vectors)
    double layerToWorld[16];
    double rect[4];            // left, top, right, bottom (layer px)
    bool unbounded;            // extent unknown (effects that may grow it): the whole infinite plane
};
struct SceneBox { double minW[3], maxW[3]; };  // non-planar (collapsed 3D precomp, per-character 3D text)
struct MergeLayerInput {        // one DupliX layer with merge on
    LayerKey key; ParamSnapshot snap; double layerToWorld[16]; double srcRect[4]; unsigned block;
};
struct MergeSceneInput {
    CameraView cam; bool haveCamera;
    double dsX, dsY;                       // filled by the texture hook (Task 3); 1 here
    std::vector<MergeLayerInput> layers;   // merge-on DupliX layers of the comp
    std::vector<ScenePlane> planes;        // other 3D layers, and every copy of DupliX layers with merge off
    std::vector<SceneBox> boxes;
};
// Reads the comp of `layer` at comp time t through AEGP (render threads only). False if anything required could
// not be read (then nothing merges: MB_UNREADABLE).
bool ReadMergeScene(const void* layer, const A_Time& compTime, MergeSceneInput* out);
// Pure: the whole-layer reasons from flags read by ReadMergeScene.
struct LayerFlagsForMerge { bool compMotionBlur, layerMotionBlur, dof, acceptsLights, anyLight, castsShadows,
                            anyShadowLight, normalBlend, fullOpacity, trackMatte, haveCamera; };
unsigned MergeBlockReasons(const LayerFlagsForMerge& f);
}
```
- 読むもの（AEGP）: 有効なカメラ（`AEGP_GetActiveCamera`、`AEGP_GetLayerToWorldXform`、ズーム＝`dist`）とコンプの大きさ。コンプの全レイヤー（`AEGP_GetCompNumLayers`/`AEGP_GetCompLayerByIndex`）: 3D か、`AEGP_IsVideoActive`、種類（カメラ・ライト・ヌル・調整レイヤーは面として数えない）、変形、ソースの大きさ、エフェクトの有無（あれば `unbounded`）、コラップスした 3D プリコンプ・文字単位 3D のテキストは `SceneBox`（大きさが分からなければ全体を覆う箱）。ライト（種類、影を落とすか）。このレイヤーのモーションブラー・描画モード・トラックマット・不透明度・ライトを受ける・影を落とす、コンプのモーションブラー、カメラの被写界深度。
- 判断（計画の決定）: 2D レイヤーは 3D の重なりを分けるが、最初の版は 3D レイヤーを全部「ほかの面」に入れる（安全側。まとめる量が減るだけ）。カメラがなければ `MB_NO_CAMERA`（設計書の範囲を狭める判断。カメラなしの既定のカメラを求める計算は後回し）。

- [ ] **Step 1: 失敗する試験** `merge_block_reasons`（各フラグで対応するビットが立つ。全部問題なければ `MB_NONE`）、`scene_no_camera_no_merge`（`haveCamera=false` → `MB_NO_CAMERA`）。
- [ ] **Step 2: 実行して失敗を確認**（コンパイルエラー）。
- [ ] **Step 3: 実装**。`ReadMergeScene` は Count のフックの診断から 1 回呼び、`merge: scene cam=%d layers=%zu planes=%zu boxes=%zu block=%x` をログに出す。
- [ ] **Step 4: AE の関門** — `tests/ae/merge.jsx` の `scene_basic`（カメラ、3D の壁 2 枚（1 枚はエフェクトつき）、ライトなし、DupliX 10 枚・軽量化オン）と `scene_light`（同じ＋ライト、レイヤーはライトを受ける）を作り、`pwsh tests/ae/run_merge.ps1 -Cases scene_basic,scene_light` で aerender（MFR オフ）。期待: ログに `cam=1 planes=2`、壁のエフェクトつきの方が `unbounded`（ログの詳細行）、`scene_light` は `block` に `MB_LIGHTS`。**読めない・止まる・落ちるなら、ここで止めて利用者に相談**（設計書 5.9 の 2）。
- [ ] **Step 5: Commit** `feat(merge): read the frame's camera, layers and lights on render threads`

### Task 3: 群れ 1 つで描く（絵の差し替え）と速さの関門

**Files:**
- Create: `src/core/merge_geom.h/.cpp`（この Task では群れの面の M4/M3 と画面の矩形だけ）
- Create: `src/hook/merge_cache.h/.cpp`、`src/hook/merge_texture.h/.cpp`
- Modify: `src/hook/bee_hooks.cpp`（Count/Rects/Opacity、`BEE_RenderTextureForArtisan`・`...WithReceipt` の IAT 差し替え）
- Test: `tests/unit/unit_tests.cpp`、`tests/ae/merge.jsx`・`check_merge.py`

**Interfaces:**
- Consumes: Task 2 の `ReadMergeScene`、`MergeSceneInput`。既存の `PlaneM4`、`CopyHomography`、`CopyDepth`、`RasterizeCopies`、`ComputeCopy`、`WorldM4`、`Mul4`。
- Produces:
```cpp
namespace rnd {
// One sub-layer AE draws for a merge-on layer.
struct MergeSub { int copy; int cluster; };   // copy >= 0: an individual copy; cluster >= 0: a group plane
struct MergeCluster {
    std::vector<int> copies;     // copy indices of this layer
    double depth;                // camera-space z of the plane (d)
    int rect[4];                 // screen buffer px (downsampled), left, top, right, bottom (exclusive)
};
struct MergeLayerPlan { std::vector<MergeSub> subs; std::vector<MergeCluster> clusters; unsigned block; };
struct MergePlan { std::map<long long, MergeLayerPlan> layers; CameraView cam; double dsX, dsY; };  // key: comp<<32|layer
// merge_geom: the plane of a group: layer px of the texture world AE clips to (texRect, layer px) shows screen
// buffer rect `rect` (downsampled by ds) at camera-space depth d.
bool GroupPlaneM4(const CameraView& cam, const double layerToWorld[16], double d, const double texRect[4],
                  const int rect[4], double dsX, double dsY, double outM4[16]);
void GroupTextureM3(const double texRect[4], int worldW, int worldH, double outM3[9]);  // world px -> layer px
// merge_cache: the plan of (comp, comp time); built once by the first caller (others wait), rebuilt when the
// registry generation or the inputs' hash change. Returns null when the layer does not merge this frame.
std::shared_ptr<const MergePlan> MergePlanFor(const void* layer, const A_Time& compTime);
// merge_texture: draws a group from the layer texture AE gave (AEGP world + its M3) into a new AEGP world.
AEGP_WorldH RenderGroupTexture(AEGP_WorldH layerTex, const double texM3[9], const MergePlan& plan,
                               const MergeLayerInput& layer, const MergeCluster& c, double outM3[9]);
}
```
- この Task の群れ分けは仮のもの（`MergePlanFor` の中で、まとめる条件に当てはまるコピーを全部 1 つの群れ、`d` = 群れの一番手前）。Task 5 で本物に置き換える。

- [ ] **Step 1: 失敗する単体試験**
  - `group_plane_maps_rect`: 適当なカメラ・`layerToWorld`・`d`・`texRect={0,0,120,100}`・`rect={300,200,500,350}`・ds 1 と 0.5 で、`GroupPlaneM4` の結果で `texRect` の 4 隅を投影すると、画面の `rect`（ds 0.5 なら 2 倍した全解像度の座標）の 4 隅に誤差 1e-6 以内で一致。
  - `group_texture_m3`: `GroupTextureM3(texRect, 200, 150)` で絵の (0,0)→(texRect 左上)、(200,150)→(右下)。
  - `plan_cache_one_build`: `MergePlanFor` の作る関数を差し替え可能にし（試験用のフック `SetMergePlanBuilderForTest`）、8 スレッドが同じ (comp, time) を同時に求めても作る関数が 1 回だけ呼ばれる。登録の世代が変われば作り直す。
- [ ] **Step 2: 実行して失敗を確認**（コンパイルエラー）。
- [ ] **Step 3: 実装**
  - フック: `s.p.merge` かつ `MergePlanFor` が群れ分けを返すときだけ、Count は `subs.size()`、Rects は `subs` の順に、コピーなら今と同じ M4、群れなら `GroupPlaneM4`（`texRect` は Rects の引数の矩形）。Opacity は、コピーならそのコピーの不透明度、群れなら AE の値のまま。返さないときは今と同じ（ここで今と一致しないと Global Constraints に反する）。
  - 絵: サブレイヤーが群れなら、元の関数で受け取った絵と M3 から `RenderGroupTexture` で群れの絵を作り、元の絵を `delete`（仮想デストラクタ、`(**(void(***)(void*,int))w)(w,1)`）、`*world` と `m3` を差し替える。失敗したら元の絵のまま返す（群れの面に元の絵が写るので、ログに `merge: texture failed` を出す。試験で 0 回であること）。
  - 群れの絵の描画: 画面の矩形（ds の縮小を反映した画素）の大きさで `AEGP_New`（受け取った絵と同じ型）。各コピーの `CopyHomography` に、受け取った絵の M3 の逆と ds を折り込んで `RasterizeCopies` の src の画素に合わせる。不透明度は `RasterCopy::opacity`。8/16/32 bpc。
  - ds（縮小表示）: テクスチャのフックの M3 と絵の大きさから求める（`texRect` に対する絵の幅の比）。
- [ ] **Step 4: 単体 ALL PASS**。
- [ ] **Step 5: AE の試験（置き方）** — `merge.jsx` の `single`（コピー 1 枚、壁なし、軽量化オン＝群れ 1 枚にコピー 1 枚）と同じシーンの軽量化オフを描き、`check_merge.py` で比べる。期待: 平均差 ≤ 0.5、2 段階を超える画素はコピーの縁（縁から 2 画素以内）だけ。`ds_half`（1/2 解像度）も同じ基準。`scene_basic` を軽量化オフで描いた絵が、Task 1 の前と差 0（まとめないときに何も変わらない）。
- [ ] **Step 6: 速さの関門** — 利用者のシーン（`tests/ae/user/DupliX Test.aep` のコピーを開く。`run_user.ps1` に `-Merge` を足し、軽量化オンのコピーで測る）で、コピー 100 枚の 1 フレームの時間（MFR オフ、書き出し）を、軽量化オフ・オン・高速モード（CPU）で測る。**オンがオフの半分以下でなければ、ここで止めて利用者に報告・相談**。結果は開発記録に書く。
- [ ] **Step 7: Commit** `feat(merge): group plane drawn from a swapped texture (single group)`

### Task 4: 透明な部分での交差の継ぎ目を確かめる

**Files:**
- Modify: `tests/ae/merge.jsx`・`check_merge.py`（ケース `seam`）、開発記録

**Interfaces:**
- Consumes: Task 3（試験用に、群れの d を環境変数 `DUPLIX_MERGE_FORCE_D`（カメラ空間の z）で固定できるようにする。製品では使わない診断）

- [ ] **Step 1: 試験を書く** — `seam`: 不透明な 3D の壁を Y 軸 60° に回し、群れ（コピー 2 枚を壁の左右の空き場所に置き、壁とは画面上で重ならない）の面が、絵の透明な部分で壁と交差する d を `DUPLIX_MERGE_FORCE_D` で与える。比べる: 同じシーンの軽量化オフ。判定: 群れのコピーの縁から 2 画素より外の画素が差 0。
- [ ] **Step 2: 実行** `pwsh tests/ae/run_merge.ps1 -Cases seam`。
- [ ] **Step 3: 結果を開発記録に書き、決める** — 差 0 なら「色がある画素だけで判定」（設計書 5.5 の緩い条件）を Task 5 で使う。差があれば「群れの絵の矩形の範囲で交差しない d」（Global Constraints の原則）を使う。ledger に Ruling として残す。
- [ ] **Step 4: Commit** `test(merge): seam check where a group plane crosses a wall in its transparent part`

### Task 5: 群れ分けの本体（純粋な関数）

**Files:**
- Modify: `src/core/merge_geom.h/.cpp`（重なり・平面のどちら側か・奥行きの範囲）
- Create: `src/core/merge_plan.h/.cpp`
- Test: `tests/unit/unit_tests.cpp`

**Interfaces:**
- Consumes: Task 2 の `MergeSceneInput`、Task 3 の `MergeLayerPlan`・`MergeCluster`・`MergeSub`、Task 4 の Ruling（判定の範囲が「矩形」か「色がある画素（コピーの四角形）」か）。
- Produces:
```cpp
namespace rnd {
// merge_geom
struct ScreenQuad { double x[4], y[4]; double zMin, zMax; bool valid; };  // full-res comp px, camera-space z range
ScreenQuad ProjectRect(const CameraView& cam, const double toWorld[16], const double rect[4]);  // invalid if z <= 1
bool QuadsOverlap(const ScreenQuad& a, const ScreenQuad& b, double marginPx);
int SideOfPlane(const double planeToWorld[16], const double camPos[3], const double cornersWorld[4][3]);
    // +1: all corners on the camera's side, -1: all on the far side, 0: straddles
void DepthRangeOver(const CameraView& cam, const double planeToWorld[16], const double planeRect[4],
                    const ScreenQuad& over, double* zMin, double* zMax);  // the plane's z where it overlaps `over`
// merge_plan
struct MergeOptions { double marginPx = 3; bool strictRect = true; double maxAreaScreens = 2; };
MergePlan BuildMergePlan(const MergeSceneInput& in, const MergeOptions& opt);
}
```
- アルゴリズム（設計書 5.5・5.9 の条件。細部は実装者が決めてよい）:
  1. 各 merge オンのレイヤーで、`block != 0` ならそのレイヤーは全部 1 枚ずつ（群れなし）。
  2. 各コピーを `ProjectRect`。invalid（カメラの面にかかる）なら 1 枚ずつ。
  3. 「ほかの面」= `planes`＋`boxes`＋1 枚ずつのコピー（増えるたびにやり直す）。コピーと画面で重なる面ごとに `SideOfPlane`。0 なら 1 枚ずつ。
  4. 残りを、重なる面に対する側（+1/−1）の組で群れに分ける（同じ組＝同じ群れの候補）。同じレイヤーのコピーだけ。
  5. 各群れの d: 重なる各面の `DepthRangeOver`（strictRect なら群れの矩形の範囲、そうでなければコピーの四角形の範囲）で、手前側の面より奥・奥側の面より手前の区間を求め、区間の中で群れの一番手前（手前側の群れ）または一番奥に近い値。区間が空なら、群れを奥行きで 2 つに分けてやり直し、1 枚になったら 1 枚ずつ。
  6. 画面で重なる群れの組は、中身の前後（片方の全コピーがもう片方の全コピーより手前の側にある）と d の順を合わせる。合わなければ分ける。
  7. 群れの絵の合計面積が `maxAreaScreens` 枚分を超えたら、面積の大きい群れから 1 枚ずつに戻す。
  8. 群れのコピーが 1 枚だけなら 1 枚ずつ（面を減らせないので）。
- `subs` の並び: 元のコピーの番号順に、1 枚ずつのコピーはその位置、群れは最初のコピーの位置に 1 つ。

- [ ] **Step 1: 失敗する単体試験**（すべて `BuildMergePlan` の出力で判定）
  - `merge_free_one_group`: ほかの面なし、コピー 50 枚 → 群れ 1 つ、`subs.size()==1`。
  - `merge_wall_splits`: 正面向きの壁（z=500）、コピー 20 枚が z 300〜450、20 枚が z 550〜700、全部壁と重なる → 群れ 2 つ、手前の群れの `depth` < 500 < 奥の群れの `depth`。
  - `merge_tilted_wall_sides`: Y 60° の壁。壁のカメラ側のコピー、反対側のコピー、壁を突き抜けるコピー 3 枚 → 群れ 2 つ＋1 枚ずつ 3 枚。
  - `merge_side_free`: 壁と同じ奥行きで、画面上で壁の左右の空き場所にあるコピー 10 枚 → 群れ 1 つ（壁と重ならないので）。
  - `merge_near_plane_individual`: 1 枚がカメラの面にかかる → それは 1 枚ずつ、残りは群れ。
  - `merge_blocked_layer`: `block = MB_LIGHTS` → 群れなし、`subs` はコピーの数と同じで順に 0..n-1。
  - `merge_group_depth_order`: 画面で重なる 2 つの群れ（壁で分かれる）で、手前の中身の群れの `depth` が小さい。
  - `merge_area_cap`: 群れの面積が上限を超える設定 → 群れが減る（1 枚ずつが増える）。
  - `merge_plan_speed`: コピー 5000 枚・面 20 枚で 1 回 20 ms 以内（Debug ビルドの単体試験。最適化ありの `merge_plan.cpp`）。
- [ ] **Step 2: 実行して失敗を確認**（コンパイルエラー）。
- [ ] **Step 3: 実装**（`merge_plan.cpp` は `raster.cpp` と同じく、どの構成でも最適化してビルドする設定を vcxproj に足す）。
- [ ] **Step 4: 単体 ALL PASS**。
- [ ] **Step 5: Commit** `feat(merge): grouping by planes' sides with a plane depth that keeps every overlap order`

### Task 6: 群れ分けをつなぎ、前後を AE で確かめる

**Files:**
- Modify: `src/hook/merge_cache.cpp`（仮の群れ分けを `BuildMergePlan` に置き換え）
- Modify: `tests/ae/merge.jsx`・`check_merge.py`

**Interfaces:**
- Consumes: Task 5 の `BuildMergePlan`、Task 3 の差し替え。

- [ ] **Step 1: AE の試験を書く**（すべて軽量化オンとオフを同じシーンで比べる。判定 = 2 段階を超える画素が、群れのコピーの縁から 2 画素以内に限られ、その数が全体の 0.5% 以下。前後が入れ替われば広い範囲で大きな差が出る）
  - `wall_front_back`: 不透明な色の壁（正面）を挟んで手前と奥にコピー各 20 枚。
  - `wall_tilted`: Y 60° の壁を貫く配置（手前・奥・突き抜け）。
  - `wall_side`: 壁の左右の空き場所のコピー。
  - `two_layers` (`merge_two_layers`): 軽量化オンの DupliX のレイヤー 2 つ（色違い）が奥行きで入り組む。
  - `merge_off_identical`: 同じシーンで軽量化オフの絵が、この計画の前（Task 1 の前の commit のビルド）と差 0（`run_merge.ps1 -Baseline` で前のビルドの絵を保存して比べる）。
  - 状態: ログで、各ケースの群れの数が 1 以上（まとめが実際に起きている）、`merge: texture failed` が 0 回。
- [ ] **Step 2: 実行して失敗を確認**（仮の群れ分けでは `wall_front_back` などで前後が入れ替わる）。
- [ ] **Step 3: 置き換えて実行** `pwsh tests/ae/run_merge.ps1` → MERGE ALL PASS。
- [ ] **Step 4: 回帰** `run_ref.ps1`（軽量化オフ）・`run_ui.ps1`・`run_fast_mode.ps1`。
- [ ] **Step 5: Commit** `feat(merge): real grouping in the hooks; ordering tests against walls and two layers`

### Task 7: 経路・色の深さ・メモリ

**Files:**
- Modify: `tests/ae/merge.jsx`・`run_merge.ps1`・`check_merge.py`、必要なら `src/hook/merge_texture.cpp`

- [ ] **Step 1: 試験を足す** — `wall_front_back` を MFR オン、GPU 設定 CUDA（`-Gpu CUDA`）、16 bpc、32 bpc、1/2 解像度で（判定は Task 6 と同じ）。画面の経路: `run_ui.ps1` と同じ方式で、軽量化オンのシーンをビューアーで描いた絵が aerender と一致。`...WithReceipt` が呼ばれたかをログで数え、呼ばれたら差し替えが働いたこと。
- [ ] **Step 2: メモリ** — `wall_front_back` を 300 フレーム書き出し、AE のプロセスの作業メモリが、最初の 50 フレーム後と最後で 10% 以上増えない（`run_merge.ps1 -Long`）。
- [ ] **Step 3: 実行し、失敗があれば直す**（直したものは単体か AE の試験で RED→GREEN）。
- [ ] **Step 4: Commit** `test(merge): MFR, GPU setting, 16/32 bpc, half resolution, viewer, long render`

### Task 8: 状態の表示・手引き・計測

**Files:**
- Modify: `src/effect/duplix.cpp`（`UpdateParamsUi`: 軽量化オンで、まとめない理由（`MergeBlockReasons` と同じ判定をメインスレッドの AEGP で）を状態の欄に）
- Modify: `docs/duplix-quickstart.md`（「軽量化」の節: 使い方、まとめない条件、差が出る所、速さの表）
- Modify: 開発記録

- [ ] **Step 1: 試験** — `run_ui.ps1` に、ライトのあるシーンで軽量化をオンにすると状態の欄の文字に「ライト」が入ることを足す（UI の試験の読み取り方式に従う）。
- [ ] **Step 2: 実装して実行** → UI ALL PASS。
- [ ] **Step 3: 計測** — 利用者のシーン（コピー 10・100・1000 枚）の通常・軽量化・高速（CPU・CUDA）を `run_user.ps1` で測り、手引きの表に入れる。
- [ ] **Step 4: Commit** `docs(merge): status reasons, quickstart section and timings`
