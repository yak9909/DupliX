# 高速モードの GPU 化と CPU 版の軽量化 実装計画

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 高速モードの描画を、AE の GPU 設定が CUDA のときは GPU で完結させ、それ以外のときの CPU 版も結果を変えずに軽くする。

**Architecture:** コピーの一覧（射影・不透明度・奥行き）と、画面を 16×16 px の区画に分けた「区画ごとのコピーの一覧」を CPU で作る（純粋関数、CPU 版と GPU 版で共通）。CPU 版はこの一覧で区画ごとに描き、手前から不透明になった画素の奥を省く。GPU 版は CUDA のカーネル（ビルド時に PTX にして埋め込み、`nvcuda.dll` のドライバー API で実行）が 1 画素 1 スレッドで同じ計算をする。

**Tech Stack:** C++17、MSVC v143、CUDA Toolkit v12.2 の `nvcc`（PTX の生成だけ。実行時はドライバー API）、AE SDK 25.6、PowerShell 7、ExtendScript、Python（`spike\.venv`）。

**Spec:** `docs/superpowers/specs/2026-11-25-fast-mode-gpu-design.md`（承認済み）。前提の実装は `docs/superpowers/plans/2026-11-25-fast-mode-plugin.md`、事実は `docs/experiments/2026-11-25-dev-log.md`。

## Global Constraints

- 見た目は今の CPU 版と同じ（CPU 版の軽量化は差の最大 1/255、GPU 版は差の最大 2/255・平均 0.1 以下が目安）。Furikake 型の Z バッファ（一番手前だけ）は使わない。
- GPU は CUDA だけ。AE の GPU 設定がソフトウェアのみ・OpenCL・DirectX のとき、CUDA の読み込みに失敗したときは、今の CPU 版で描く（落ちない、描かれないことがない）。
- 使う側の PC に CUDA Toolkit を求めない（PTX を埋め込み、`nvcuda.dll` から関数を実行時に読み込む）。PTX は v12.2 の `nvcc` で `compute_52` 向けに作る。
- 1 画素で並べるコピーの上限は 64。超えた分は遠いものから順に先に合成する（その画素だけ近似）。
- 通常モードは変えない（`run_ref.ps1`・`run_ui.ps1`・fx_remove が合格のまま）。
- 利用者の aep は必ずコピーを開く。AE を動かす試験の前に AfterFX・aerender が動いていないことを確かめる。ビルドの置き換えは利用者が AE を閉じているときだけ。
- 区切り（完了・確認・利用枠の止め）では `HANDOFF.md` を更新してクリアできる状態にする（`.claude/CLAUDE.md`）。
- AE を動かす試験は 1 回の呼び出しで 10 分を超えないように分けて流す。

## Review Focus

1. **MFR（複数フレームの同時描画）と GPU**: 同じ GPU の文脈で同時にカーネルを動かしても、モジュールの読み込みが 1 回で済み、結果が壊れない → Task 4 の `run_fast_mode.ps1 -Gpu CUDA` を MFR オン・オンで回す。
2. **縮小表示（1/2）と負の原点の入力（前にドロップシャドウ）を GPU で** → Task 4 の `fast_half`・`fast_grow` を GPU でも比べる。
3. **1 画素に 64 枚を超えて重なる** → Task 2 の `raster_overflow_64` と Task 3 の `gpu_overflow_64`（落ちない、近似が CPU と同じ規則）。
4. **コピーがとても多い（1 万枚）と GPU のメモリ** → Task 3 の `gpu_many_copies`（区画の一覧が大きくても失敗せず、失敗時は CPU 版に切り替わる）。
5. **AE の GPU 設定を途中で変える**（CUDA → ソフトウェアのみ）→ Task 4 の `fast_gpu_switch` ケース。

---

## ファイル構成

```
src/core/tiles.h, tiles.cpp            … 区画ごとのコピーの一覧（純粋関数、CPU・GPU 共通）
src/core/raster.cpp                    … 区画の一覧を使い、手前から不透明になった画素の奥を省く
src/core/raster_ref.h, raster_ref.cpp  … 今の CPU 版をそのまま残した比較用（単体テストだけが使う）
src/gpu/fast_raster.cu                 … CUDA のカーネル（ビルド時に PTX → src/gpu/fast_raster_ptx.h）
tools/build_ptx.ps1                    … nvcc で PTX を作り、C++ のヘッダーに埋め込む
src/effect/gpu_raster.h, gpu_raster.cpp… ドライバー API の読み込み、文脈ごとのモジュール、転送と起動
src/effect/duplix.cpp                 … 前処理で GPU を可能にし、GPU の描画で gpu_raster を呼ぶ
tests/unit/unit_tests.cpp              … tiles・raster の単体テスト
tests/gpu/gpu_test.cpp, gpu_test.vcxproj … 自前の CUDA の文脈でカーネルを動かし、CPU 版と比べる（AE なし）
tests/ae/run_fast_mode.ps1, fast_mode.jsx, check_gpu_cpu.py … GPU 設定での AE の試験
```

---

### Task 1: 実験と計測（製品コードは書かない）

AE の GPU の絵の形式と、今の時間の内訳を確かめる。結果で Task 3・4 の細部が決まる。

**Files:**
- Create: `spike/gpu_probe/`（使い捨て）
- Modify: `docs/experiments/2026-11-25-dev-log.md`

- [ ] **Step 1: 時間の内訳** — 利用者のシーンのコピー（`run_user.ps1` の仕組み）で、高速モードのコピー 10・100・1000 枚を、CPU 設定の aerender（MFR オフ）とビューアー（`run_preview.ps1`）で測る。プラグイン内の計測（`DUPLIX_TIMING=1`）で、前処理・入力の取り込み・描画（`RasterizeCopies`）・それ以外に分ける。プロジェクトの GPU 設定を CUDA にした場合も同じく測る（今は CPU に戻されるので、往復の費用が分かる）。
- [ ] **Step 2: GPU の絵の形式** — 一時的な分岐（環境変数 `DUPLIX_PROBE_GPU=1`）で、高速モードの前処理に `GPU_RENDER_POSSIBLE` を立て、`SMART_RENDER_GPU` で入力の GPU の絵の数画素を `cuMemcpyDtoH` で読んでログに出す。入力は既知の半透明の絵（例: 色 (1, 0.5, 0.25)、アルファ 0.5 の平面）。確かめること: 画素の形式（`PF_PixelFormat_GPU_BGRA128` か）、チャンネルの並び、ストレートかプリマルチプライか、行の長さ（rowbytes）、入力・出力の原点（`origin_x/y`）と縮小表示での大きさ。
- [ ] **Step 3: PTX の読み込み** — 同じ分岐で、`nvcc -ptx`（v12.2、`-arch=compute_52`）で作った最小のカーネル（出力を一色で塗る）を `cuModuleLoadData`・`cuModuleGetFunction`・`cuLaunchKernel` で AE の文脈（`PF_GPUDeviceInfo::contextPV`）とストリーム（`command_queuePV`）で動かし、画面にその色が出ることを確かめる。
- [ ] **Step 4: 記録して分岐を消す** — 開発記録に事実と数値を書く。`git diff src/` が空に戻ること。
- [ ] **Step 5: Commit** — `git commit -m "docs: GPU world format and fast-mode timing breakdown"`

---

### Task 2: 区画の一覧と CPU 版の軽量化

**Files:**
- Create: `src/core/tiles.h`, `src/core/tiles.cpp`, `src/core/raster_ref.h`, `src/core/raster_ref.cpp`
- Modify: `src/core/raster.h`, `src/core/raster.cpp`
- Test: `tests/unit/unit_tests.cpp`

**Interfaces:**
- Produces:
  - `struct TileLists { int tileSize, tilesX, tilesY; std::vector<int> start; std::vector<int> index; };`（区画 t のコピーは `index[start[t] .. start[t+1])`、コピーは奥から順の並びの番号）
  - `void BuildTiles(const RasterImage& src, const RasterCopy* copies, int n, double dsX, double dsY, const RasterImage& dst, int tileSize, TileLists* out);`（`copies` は奥から順に並べ済み。外接矩形は今の `CopyBox` と同じ。`CopyBox` は `tiles.cpp` へ移して公開する）
  - `bool RasterizeCopiesReference(...)`（今の `RasterizeCopies` と同じ引数・同じ中身。比較用）
  - `RasterizeCopies` の引数は変えない。

- [ ] **Step 1: 比較用を作る** — 今の `raster.cpp` の中身を `raster_ref.cpp` に `RasterizeCopiesReference` として写す（名前空間 `rnd`、関数名だけ変える）。
- [ ] **Step 2: 失敗するテストを書く**
  - `tiles_cover_boxes`: 3 枚のコピー（重なり・画面外・画面全体）で、各コピーの外接矩形に掛かる区画にだけ、そのコピーの番号が奥から順に入る。
  - `raster_matches_reference`: 乱数で作った場面 20 種（コピー 1〜200 枚、斜め・半透明・不透明・画面外・カメラの後ろにかかるもの、8/16/32 bpc、縮小 1 と 0.5、`screenShift` あり）で、`RasterizeCopies` と `RasterizeCopiesReference` の差の最大が 1（8 bpc 換算）以下。
  - `raster_overflow_64`: 1 画素に 100 枚の半透明のコピーが重なる場面で落ちず、差の最大が 1 以下（参照も同じ規則にするため、`raster_ref.cpp` にも上限 64 と「超えたら遠いものから先に合成」を入れる。今の CPU 版は上限なしなので、この 1 点だけは参照を変える）。
  - 速さの目安を 1 行出す（1920×1080、577×680 を 10・100・1000 枚、参照と新しい版の ms）。
- [ ] **Step 3: 失敗を確認** — `pwsh tools/build.ps1 -Target unit` → `BuildTiles` が未定義でビルド失敗。
- [ ] **Step 4: 実装** — 行の帯ごとではなく区画ごとに仕事を配る（区画の中の画素だけ、その区画のコピーだけを見る）。各画素のかけらを奥行きで並べたあと、**手前から**合成し、不透明度が 1 − 1/1024 以上になったら奥の合成を省く（結果は奥から合成した場合と丸めの範囲で同じ）。1 画素のかけらは 64 まで持ち、65 個目からは、持っているうちで一番遠いものを先に合成した結果へ足していく。
- [ ] **Step 5: 合格を確認** — `build/unit/unit_tests.exe` → `ALL PASS`。速さの目安で、新しい版が参照より遅くない（1000 枚で速くなる）ことをログで確かめる。
- [ ] **Step 6: Commit** — `git commit -m "perf(core): tiled CPU rasterizer with front-to-back early out"`

---

### Task 3: CUDA のカーネルと実行の部品（AE なしで試験）

**Files:**
- Create: `src/gpu/fast_raster.cu`, `tools/build_ptx.ps1`, `src/effect/gpu_raster.h`, `src/effect/gpu_raster.cpp`, `tests/gpu/gpu_test.cpp`, `tests/gpu/gpu_test.vcxproj`
- Modify: `tools/build.ps1`（`-Target gpu`、`effect`・`gpu` の前に `build_ptx.ps1` を呼ぶ）、`src/effect/duplix.vcxproj`（生成した `src/gpu/fast_raster_ptx.h` をインクルードできるように）、`.gitignore`（生成物）

**Interfaces:**
- Consumes: Task 2 の `TileLists`・`BuildTiles`、`RasterCopy`。
- Produces:
  - `struct GpuImage { void* devPtr; int width, height; long rowBytes; int originX, originY; double screenShiftX, screenShiftY; };`（形式は Task 1 で確かめた AE の GPU の絵。BGRA float の想定）
  - `bool GpuRasterAvailable();`（`nvcuda.dll` と必要な関数がある）
  - `bool GpuRasterize(void* cuContext, void* cuStream, const GpuImage& src, const RasterCopy* copies, int n, const TileLists& tiles, double dsX, double dsY, GpuImage& dst);`（false なら呼び出し側は失敗として扱う。モジュールは文脈ごとに 1 回だけ読み込み、`std::mutex` で守る。コピーと区画の一覧は毎回 `cuMemAllocAsync` 相当で転送し、描画の完了を待ってから解放）
- カーネル: 1 画素 1 スレッド、16×16 のブロック＝1 区画。区画のコピーを奥から順に見て、画素の中心の射影（`float`、区画の左上を原点にずらして桁落ちを防ぐ）→ バイリニア → かけらを奥行きで並べる（上限 64、Task 2 と同じ規則）→ 手前から合成して早めに打ち切る → 出力の形式で書く。

- [ ] **Step 1: 失敗する試験を書く** — `tests/gpu/gpu_test.cpp`: 自前で `cuInit`・`cuCtxCreate` した文脈で、Task 2 の `raster_matches_reference` と同じ乱数の場面を float の BGRA に直して `GpuRasterize` し、CPU の `RasterizeCopies`（32 bpc）と比べる。`gpu_matches_cpu`（差の最大 2/255、平均 0.1/255 以下）、`gpu_overflow_64`、`gpu_many_copies`（1 万枚・1920×1080 で true が返り落ちない）、`gpu_no_copies`（0 枚で全面透明）。速さを 1 行出す（10・100・1000 枚の ms）。
- [ ] **Step 2: 失敗を確認** — `pwsh tools/build.ps1 -Target gpu` → `GpuRasterize` が未定義でビルド失敗。
- [ ] **Step 3: 実装** — `build_ptx.ps1` は `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.2\bin\nvcc.exe -ptx -arch=compute_52 -O3` で PTX を作り、`static const char kFastRasterPtx[] = R"PTX(...)PTX";` のヘッダーに書く（生成物は git 管理外）。ドライバー API の関数は `gpu_copy.cpp` と同じく `nvcuda.dll` から読み込む（共通の読み込みは `gpu_raster.cpp` に寄せ、`gpu_copy.cpp` はそれを使う）。
- [ ] **Step 4: 合格を確認** — `build/gpu/gpu_test.exe` → `GPU ALL PASS`。
- [ ] **Step 5: Commit** — `git commit -m "feat(gpu): CUDA fast-mode rasterizer loaded from embedded PTX"`

---

### Task 4: エフェクトへのつなぎ込みと AE での試験

**Files:**
- Modify: `src/effect/duplix.cpp`、`tests/ae/run_fast_mode.ps1`、`tests/ae/fast_mode.jsx`
- Create: `tests/ae/check_gpu_cpu.py`

**Interfaces:**
- Consumes: Task 3 の `GpuRasterAvailable`・`GpuRasterize`、Task 2 の `BuildTiles`。
- Produces: `std::vector<RasterCopy> BuildFastCopies(const FastPre& f, const double srcRect[4]);`（今の `FastSmartRender` のコピーの組み立てを切り出したもの。CPU・GPU で共通）

- [ ] **Step 1: 前処理** — 高速モードで、`extra->input->what_gpu == PF_GPU_Framework_CUDA` かつ `GpuRasterAvailable()` なら `PF_RenderOutputFlag_GPU_RENDER_POSSIBLE` を立てる。`GPU_DEVICE_SETUP` では CUDA のとき `GpuRasterAvailable()` を条件にする。
- [ ] **Step 2: GPU の描画** — `SMART_RENDER_GPU` で高速モードなら、入力・出力の GPU の絵（`GetGPUWorldData`）を `GpuImage` にし、`BuildFastCopies`・奥から順の並べ替え・`BuildTiles` のあと `GpuRasterize`。失敗したら `PF_Err_INTERNAL_STRUCT_DAMAGED` を返し、ログに理由を出す（AE が CPU で描き直すかは Task 1 の結果に従う。描き直さないなら、前処理で失敗の記録を見て GPU を可能にしない）。
- [ ] **Step 3: AE の試験** — `fast_mode.jsx` は `DUPLIX_GPU=CUDA` のときプロジェクトの GPU 設定を CUDA にする（`run_ref.ps1 -Gpu CUDA` と同じ方法）。`run_fast_mode.ps1 -Gpu CUDA` は出力を `build/test/fast_gpu` に分け、通常どおり fm と nm を比べたうえで、`check_gpu_cpu.py build/test/fast build/test/fast_gpu` で GPU と CPU の fm を比べる（差の最大 2、平均 0.1 以下）。プラグインのログに GPU の描画の行（`fast: gpu render`）が出たことも確かめる（GPU を本当に使った証拠）。ケース `fast_gpu_switch`: 同じセッションで GPU 設定を CUDA → ソフトウェアのみに変えて描いた絵が、CPU 版と差の最大 1。
- [ ] **Step 4: 合格を確認** — `pwsh tests/ae/run_fast_mode.ps1`（CPU）→ `FAST ALL PASS`、`pwsh tests/ae/run_fast_mode.ps1 -Gpu CUDA` → `FAST ALL PASS` と `GPU vs CPU ALL PASS`（MFR オフ・オン）。`run_ref.ps1`、`run_ui.ps1`、fx_remove、`run_transfer.ps1` → 合格。
- [ ] **Step 5: Commit** — `git commit -m "feat: fast mode renders on the GPU (CUDA)"`

---

### Task 5: 速さの比較と手引き

**Files:**
- Modify: `docs/duplix-quickstart.md`、`docs/experiments/2026-11-25-dev-log.md`、`HANDOFF.md`

- [ ] **Step 1: 測る** — Task 1 Step 1 と同じ条件（コピー 10・100・1000 枚、aerender とビューアー、CPU 設定と CUDA 設定）で測り、前後の表を開発記録に書く。
- [ ] **Step 2: 手引き** — 高速モードの節に「GPU（CUDA）の設定では GPU で描く。それ以外は CPU」「1 画素に 64 枚を超えて重なる所だけ近似」「目安の速さの表」を書く。
- [ ] **Step 3: Commit と引き継ぎ** — `git commit -m "docs: fast mode GPU timings"`、push。`HANDOFF.md` の冒頭を更新し、利用者に試してもらう。
