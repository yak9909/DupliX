# AE クラシック 3D のコピー 1 枚の描き方（解読メモ、2026-11-26）

目的: AE（Advanced3D.aex、AE 2025）がコピー（SY_TRLayer）1 枚を描く計算を、DupliX が GPU で同じ結果（ビット単位）で行う。
AE の順番のまま、コピーの面の描画だけを差し替える（`bee_hooks.cpp` の `HookA3dDraw`）。

## 計測（理由）

- 利用者のシーン: コピー 1 枚あたりの時間の 9 割以上が AE の画素の計算（小さい花で 1 枚 約 1.5 ms の CPU、大きい花で 約 14 ms）。
- コピーの描画を飛ばすと、残りは 1 枚 約 0.1 ms（`run_user_time.ps1 -Count 100`、Test.aep）。

## 呼び出しの流れ（8 bit、動きのぼかしの標本 1 つ、ライトなし）

1. `sub_180014470`（フレームの描画）→ 面ごとに `sub_180014800` → `SY_TRLayer::Draw`（vtable RVA 0x80438 の slot 1 = `sub_180012B30`）。
   引数 `(self, ctx, a3=&ctx[7|9], a4, a5=params の補助, a6, a7)`。`a7` → `{int kind, buffer*}`。kind 1 = 直接の描き込み。
2. Draw の中: `sub_18005D6C0`（描画の状態 v45 を作る）→ `sub_1800117A0`（凸包から行ごとの範囲）→ ctx+224 の反復（行を AE のスレッドで並列）
   → 行ごとに `sub_180016900`（d = {ctx, v45, a5=params, v15=mask, spans**, count, y0(+44), field(+48), a7(+56)}）。
3. `sub_180016900` → `sub_180050B40`（kind 1）→ `sub_18004CFD0`（bit 深度で分岐）→ 8 bit・標本 1 つ: `sub_1800549F0`（標本化の状態 S）と `sub_180054A90`（ライト用）→ `sub_180045210`（行の画素のループ）。

## データの形

- バッファ（描き込み先・テクスチャの両方。vtable 0x...298240 の C++ の物）: +32 data、+40 rowbytes、+44 width、+48 height。
  +8 から PF_LayerDef（data は +8+24）。画素は ARGB の 8 bit（バイト順 A,R,G,B）、ストレートアルファ。
- 状態 v45: +16 テクスチャ（バッファ*）、+24 SAT、+32 標本の配列（72 バイト＝double 9 個 m0..m8）、+40 その数、+48 使う標本の番号、
  +0x98 遠近の有無（byte）、+0xA0 double、+0xA8 画質（byte: 0 最近傍、1 バイリニア/範囲平均、2 バイキュービック）。
- params（d+16、r12）: vtable の関数 0 は `return byte [this+0x8D]`（ライトの補間の要否）、+0x18 合成の種類（7 bit、最上位 = 描き込み先のアルファを保つ）、
  +0x20 不透明度 double、+0x5B（2 なら色を 255 に）、+0x8C ライト、+0xB0 → [0] が 0 以外なら別の経路。
- 捕獲（利用者のシーン、Test.aep）: 合成 2（前に重ねる）、不透明度 1.0、ライト 0、+0x8D 0、+0x5B 1、マスクなし、合成の表（*(*ctx+480)）なし、
  画質 1、遠近 1、U_G_CPU_DataP+8 = 1（i7-6700）。

## 行の初期化 `sub_1800542C0(S, v45, x+0.5, y+0.5)`（x, y は double）

m = 標本 [v45+48] の 9 個。S の場:
- +0x30 = (float)m0, +0x38 = (float)m1, +0x40 = (float)m2, +0x34 = (float)m3, +0x3C = (float)m4, +0x44 = (float)m5
- +0x10 U = (y*m3 + x*m0) + m6、+0x18 V = (x*m1 + y*m4) + m7、+0x20 W = (x*m2 + y*m5) + m8（double）
- h = (float)m2 * 0.5f、k = (float)m5 * 0.5f、a = h + 0（=h）、b = 0 - h、
  p = (0 < m2f) ? a : b、q = (0 < m2f) ? b : a、
  m5f > 0 なら q' = q - k、p' = k + p、そうでなければ q' = k + q、p' = p - k。S+0 = q' + q'、S+4 = p' + p'（float）
- (double)m2f が 1e-12 より小さく、かつ qword_1800805B0（未確認）より大きければ S+0x40 = 0
- S+0x0C = v45[0x98]、S+0x28 = 遠近なら 1.0 / W、でなければ 1.0
- v45[0xA0] が 0 でなければ S+8 = (float)(1.0 / v45[0xA0])
- `sub_1800549F0`: S+0x48 = テクスチャ、+0x50 = SAT、+0x58 = bit 深度（テクスチャの vfunc+8）、+0x5A = 画質、+0x60..0xC0 = 0、
  +0x70 = テクスチャ+8（PF_LayerDef*）、+0x84 = -1、+0x88 = 0。S+0x60 から PF_SampPB（x_radius, y_radius, ..., src=+0x10）。

## 行のループ `sub_180045210`（今回の経路: マスクなし、[params+0xB0][0]==0、ライトなし、合成の最上位ビット 0）

- 不透明度 op8 = trunc(params+0x20 * 255.0 + 0.5) の下位 8 bit
- 各画素: 標本 s = 画質が 0 以外なら `sub_180055540(S, &q, &s)`、0 なら `sub_180056980`（最近傍）。
  2 画素ずつの組でも 1 画素ずつでも、順は「標本 → 歩進」。歩進: U += (double)S+0x30、V += (double)S+0x38、
  S+0x40 が 0 でなければ W += (double)S+0x40、遠近なら invW = 1.0 / W。
- params+0x5B == 2 なら s の R,G,B = 255。
- 組の 2 画素がどちらもアルファ 0 なら合成しない。そうでなければ `PF_Get_Xfer1_Opacity_table8(mode)(&s, tables, op8, dst)`（アルファ 0 の画素はその中で何もしない）。

## 標本化 `sub_180055540`（CPU の旗 1 のとき）

uf=(float)U, vf=(float)V, iw=(float)invW, Wf=(float)W。
- 遠近あり: Wf + S[0] > 0 でなければ透明（0）。u = uf*iw、v = vf*iw、
  dux = dUx*iw - (dWx*iw)*u、dvx = dVx*iw - (dWx*iw)*v、duy = dUy*iw - (dWy*iw)*u、dvy = dVy*iw - (dWy*iw)*v（各積は float、順は機械語どおり）、
  su = max(|dux|, |duy|) * 0.5f、sv = max(|dvx|, |dvy|) * 0.5f（vmaxss: a > b ? a : b）
- 遠近なし: u = uf、v = vf、su = max(|dUx|, |dUy|) * 0.5f、sv = max(|dVx|, |dUx|) * 0.5f（AE のまま）
- → `sub_18005D1C0(S, q, u, v, su, sv, out)`

## `sub_18005D1C0`

fx(t) = trunc((t < 0 ? -0.5f : 0.5f) + t * 65536.0f)。
x = fx(u)、y = fx(v)、rx = 100.0f > su ? fx(su) : 0x640000、ry = 同様（S+0x60, S+0x64 に保存）。
rx > 0x8000 か ry > 0x8000 なら範囲平均（q 2: `PF_AreaSample_CPlusPlus<PF_Pixel8,2>`、それ以外: `PF_AreaSample_Straight`）、
でなければ部分画素（q 2: `PF_SubpixelBicubic8`、それ以外: `PF_SubpixelSample_Straight`）。引数 (0, x, y, &S[0x60], out)。

## PF.dll（AE 2025、`spike/ida/versions/2025/PF.dll.i64`）

- `PF_SubpixelSample_Straight` 0x180089330: 機械語を読んで訳す（下の「訳」）。定数 dword_1803AB6E0、dword_1803A8A00、xmmword_1803AB640、xmmword_1803AB680 は未読。
- `PF_AreaSample_Straight` → 関数ポインター qword_180425A28（CPU で選ばれる。この PC では 0x18008A560）。その中で qword_180425A18 も呼ぶ（未確認）。
  定数 xmmword_1803AB620/630/650/660/670、dword_1803AB6E4 は未読。
- 合成 2（前に重ねる）: `PFp_G_Xfer1_Opacity_table8[2]` = 0x18020E5D0 → 表あり `sub_180254660`、表なし `sub_180254DE0`。
  表なし・CPU の旗 1 の経路は整数＋`vrcpps` 1 回。`vrcpps` の入力は合成後のアルファ（1..255 の整数を float にした値）だけなので、
  実行中の PC で 255 個の結果を表にして使えば完全に一致する。
- PF.dll 全体の `vrcpps` は 17 関数（`spike/ida/ida_scan_approx.py`）。Advanced3D 全体には近似命令も FMA もない。

## 捕獲（`RANDAMU_A3D_DRAW=3`）

`RANDAMU_A3D_CAPSUB`（コピー番号）、`RANDAMU_A3D_CAPW`（描き込み先の幅、例 1920。隠れた AE は小さな縮小表示も描くため）、
`RANDAMU_A3D_CAPDIR`。書き出すもの: surface.bin（232）、ctx.bin、d.bin、state.bin（v45 から 1024）、tex_hdr.bin・tex.bin、samples.bin、
params.bin・params_b0.bin、target_hdr.bin、cpu.bin、rows.bin（行ごとに int32 row, y, x0, x1, nbytes、描く前のバイト列、後のバイト列）。
例: `RANDAMU_A3D_DRAW=3 RANDAMU_A3D_CAPSUB=0 RANDAMU_A3D_CAPW=1920 RANDAMU_A3D_CAPDIR=<dir> pwsh tests/ae/run_user_time.ps1 -Aep "tests/ae/user/DupliX Test.aep" -Frames 1 -Count 3 -Variants off`
