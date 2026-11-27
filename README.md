# DupliX

After Effects 2025 用のエフェクトです。適用したレイヤーを、3D 空間にランダムに n 枚のコピーとして並べます。コピーは AE 自身のクラシック 3D レンダラーで本物の 3D レイヤーとして描画されます。そのため、ほかのレイヤーとの前後・交差、ライトの陰影と影、被写界深度、モーションブラー、上にある調整レイヤーの結果が、同じ位置に複製レイヤーを置いた場合と画素単位で同じになります。

現在は実験の段階で、製品のコードはまだありません。`spike/` は方式を確かめるための使い捨てのコードです。製品の設計は [docs/superpowers/specs/2026-11-24-duplix-sublayer-design.md](docs/superpowers/specs/2026-11-24-duplix-sublayer-design.md)、実装計画は [docs/superpowers/plans/2026-11-24-duplix-sublayer-mvp.md](docs/superpowers/plans/2026-11-24-duplix-sublayer-mvp.md) にあります。

AE の非公開の内部関数をフックして動かすため、対象は After Effects 2025（25.6）の Windows 版だけです。

## 仕組み

### サブレイヤー

文字単位 3D を有効にしたテキストレイヤーは、1 つのレイヤーのまま、文字ごとに別々の 3D の面として描画されます。AE の内部（`BEE.dll`）では、この文字ごとの面を「サブレイヤー」と呼び、レイヤーのクラス `BEE_AVLayer` の仮想関数でレンダラーに渡しています。

| 仮想関数 | レンダラーに返すもの |
|---|---|
| `SubLayersRenderSeparately` | サブレイヤーを別々の面として描画するか |
| `SubLayerRenderCount` | サブレイヤーの数 |
| `GetSubLayerRects` | サブレイヤーごとの矩形・2D 行列（`M_Matrix3`）・3D 行列（`M_Matrix4`） |

テキスト以外の AV レイヤーでは、これらの関数は「サブレイヤーなし」を返します。DupliX は、このエフェクトを持つレイヤーに対してだけ「サブレイヤーが n 枚あり、それぞれこの変形」と返させます。あとはレンダラーが n 枚の面として扱うので、前後判定・陰影・影・被写界深度・モーションブラーはすべて AE のレンダラーが通常どおり計算します。

### vtable の書き換え

`BEE.dll` は `BEE_AVLayer` の vtable（仮想関数テーブル）を `??_7BEE_AVLayer@@6B@` としてエクスポートしています。この vtable は全 AV レイヤーで共有されているので、該当するスロットを書き換え、対象でないレイヤーでは元の関数を呼びます。書き換えるスロットの番号はハードコードせず、エクスポートされている関数（`?SubLayerRenderCount@BEE_AVLayer@@...` など）のアドレスを vtable の中身と照合して実行時に求めます。AE 2025 では次の番号でした。

| スロット | 関数 | 書き換え後の動作 |
|---|---|---|
| 217 | `SubLayersRenderSeparately` | 真を返す |
| 219 | `SubLayerRenderCount` | n を返す（下の例外あり） |
| 233 | `GetSubLayerRects` | n 枚分の矩形・M3・M4 を書き込む |
| 79 | `IsLayerType` | 特定の呼び出し元にだけ偽を返す（後述） |

1 つでも見つからなければ、何も書き換えません。

### `GetSubLayerRects` の中身

引数は、時刻（`{int value; unsigned scale}`、レイヤー時間）、レイヤーの矩形、出力先の `std::vector<M_FloatRect>`（1 要素 32 バイト）・`std::vector<M_Matrix3>`（72 バイト）・`std::vector<M_Matrix4>`（128 バイト）です。

コピー i の画像は「元のレイヤーのうち、M3 で変換した矩形の範囲」で、その面を M4 で 3D 空間に配置します。テキストでは M3 が文字の位置、M4 が文字の 3D 変形に当たります。DupliX は次のように書き込みます。

- 矩形は、元のレイヤーの矩形のまま
- M3 は単位行列（レイヤー全体をコピーの画像にする）
- M4 は行ベクトル形式で、4 行目が平行移動、単位はレイヤー座標の px

M4 は、アンカーポイントを A、平行移動を t、拡大縮小を S、回転を Rx・Ry・Rz として `T(−A)·S·Rz·Ry·Rx·T(A+t)` です。回転の順序と符号は AE の X/Y/Z 回転と同じです。順序を X→Y→Z にすると、3 軸を組み合わせたコピーだけがずれました。

vector のメモリは AE 側が解放するので、AE と同じ `ucrtbase.dll` の `malloc` で確保します。4096 バイト以上は、MSVC の STL の実装どおり 32 バイト境界にアラインし、直前の 8 バイトに元のポインタを置きます。

レンダラーはモーションブラーのシャッターが開いている間の時刻ごとに `GetSubLayerRects` を呼びます。時刻に応じた行列を返せば、動くコピーのモーションブラーも正しく描画されます。

### ラスターのレイヤーでコピーが消える問題

最初は、平面や画像のレイヤーではコピーが全部消えました。シェイプやテキストのようなベクターのレイヤーでは描画できていました。

原因は、レイヤーの画像を作る `Rasterize2DGraph`（スロット 235）です。この関数はキャッシュを渡さずに `SubLayerRenderCount` を呼び、2 以上ならサブレイヤー用の別の描画ノードに切り替えます。そのノードは、ラスターのレイヤーでは空の画像を返します。そこで、`SubLayerRenderCount` は、キャッシュの `shared_ptr` が空の呼び出しには元の戻り値（0）を返すようにしました。これで画像は通常のノードでレイヤー全体として作られます。キャッシュを渡す呼び出しには n を返すので、レンダラーはその画像を n 枚の面として描画します。

### エフェクト付きのレイヤー

Artisan（3D レンダラー）には、「エフェクトが付いた AV レイヤーは 1 枚にフラット化して描画する」という判定があり、ここで真になるとサブレイヤーに分かれません。DupliX 自身がエフェクトなので、この判定を偽にする必要があります。

判定の中では `IsLayerType(0)` が呼ばれます。書き換えた `IsLayerType` は、リターンアドレスが指す命令列をパターンと照合し、この判定からの呼び出しのときだけ、対象レイヤーに偽を返します。照合するパターンは次のとおりです。

```
test al,al; jnz; call BEE_BetaFeature_AreExpressionEffectsAlwaysAllowedEnabled; test al,al; jz;
mov rax,[rbx]; mov rcx,rbx; call [rax+d]; mov rdx,[rax]; mov rcx,rax; call [rdx+d]; test eax,eax; jnz;
mov rax,[rbx]; mov rcx,rbx; call [rax+d]; test al,al; jnz; mov rcx,rbx; call BEE_GetSelectedTrackMatte
```

前半だけで照合すると、音声の合成の判定にも一致しました。そのため後半まで照合しています。

### コピーごとの不透明度

`BEE_AVLayer::GetSubLayerOpacity`（スロット 226）を書き換えても、クラシック 3D はこの関数を 1 度も呼びませんでした。レンダラーがレイヤーの不透明度を受け取るのは、`AEGPDriver.aex` がインポートしている `BEE.dll` の関数 `BEE_GetArtisanLayerOpacity` 経由です。

そこで `AEGPDriver.aex` のインポートアドレステーブル（IAT）のこのエントリを書き換え、元の値にコピーごとの値を掛けて返します。どのレイヤーの何番目のコピーかは、引数の `BEE_RenderTrace` から次のように読みます。

```
v     = *(char**)(trace + 16)
layer = *(void**)(v - 48)   // BEE_AVLayer*
index = *(int*)(v - 40)     // サブレイヤーの番号
```

### 対象レイヤーの見分け方

`AEGP_LayerH` は `BEE_Layer*` そのものです。描画はレイヤーの複製オブジェクトに対して行われますが、`BEE_Layer::GetID()` の値は複製でも同じでした。フックした関数の中では、エフェクトが登録した ID の一覧と照合するだけで、AEGP は呼びません。UI スレッドで呼ばれた BEE の仮想関数の中から AEGP を呼ぶと、AE が止まったためです。レイヤー ID はコンプごとの番号なので、製品ではコンプのアイテム ID（`BEE_Layer::GetParentComp()` → `BEE_Item::GetID()`）と組にして照合します。

描画スレッドからの呼び出しでは、そのレイヤーの DupliX のパラメータを、AEGP（`AEGP_GetNewEffectStreamByIndex`・`AEGP_GetNewStreamValue`）で、呼び出し時の時刻の値として読めます。配置はパラメータだけから決まるので、パラメータを変えれば AE のキャッシュは通常どおり破棄されます。逆に、フックが返す配置をパラメータ以外の理由で変えると、AE はキャッシュに残った古い画像を返します。

### 見つけた経緯

`BEE_AVLayer::SubLayerRenderCount` は、テキストとは別に `ADBE Layer Sets` というプロパティから枚数を求めていました。AV レイヤーをコピーとして並べる、未公開の機能です。スクリプトから `ADBE Layer Set` を追加できましたが、描画するとレイヤーが消えるか、`BEE_LayerSet::GetSubLayerRects` の中で AE が異常終了しました。そのためこの機能は使わず、同じ仮想関数の戻り値を書き換える方式にしました。

## 検証

各コピーの位置に本物の複製 3D レイヤーを置いた参照コンプを作り、AE が描画した画像と画素ごとに比べています。12 万画素の差の最大は 0 か 1/255 でした。

- 条件: 位置、回転（3 軸）、不透明度、乱数で 100 枚、複数のレイヤー
- シーン: 交差する壁、スポットライトの陰影と影、被写界深度、ガウスぼかしの調整レイヤー、カメラとコピーのモーションブラー
- 素材: 平面、半透明の PNG、エフェクト付きのレイヤー
- レンダリングの方法: `saveFrameToPng`、aerender（MFR オフ・オン）

拡大縮小、中心からずらしたアンカーポイント、対象レイヤー自身に変形や親があるとき、UI のプレビューは、まだ確かめていません。

処理時間は、aerender の 1 フレームで比べると、1000 枚でも 7 枚のときとほぼ変わりません。2500 枚で 1 フレーム約 20 秒、5000 枚で約 450 秒でした。時間のほとんどは AE の 3D 描画で、フック側の処理は 1 フレーム数十 ms です。

試験ごとの記録は [HANDOFF.md](HANDOFF.md)（試験 1〜23）と [docs/experiments/2026-11-22-spike-log.md](docs/experiments/2026-11-22-spike-log.md) にあります。

## 採らなかった方式

- エフェクトの出力で、コピーより奥にある部分を透明にする方式は、半透明やぼかしのレイヤーの後ろで背景が透け、加算・乗算のレイヤーの後ろでコピーが消えました。
- コピーのある画素だけを自前で合成し直す方式では、調整レイヤー・ライトと影・被写界深度・モーションブラーを再現できません。

## フォルダ

| パス | 中身 |
|---|---|
| `spike/DupliXSpike/` | 実験用のエフェクト。環境変数で動作を切り替える |
| `spike/scenes/` | 試験のシーンを作る ExtendScript（`probe_3d.jsx` が参照コンプとの比較） |
| `spike/tools/` | ビルド、AE を非表示のデスクトップで動かすスクリプト、aerender での比較 |
| `spike/ida/` | `BEE.dll` を idalib で解析するのに使ったスクリプト（DLL と解析結果は含めていない） |
| `docs/` | 設計書、実装計画、決定事項、実験記録、自動テストの使い方 |

## ビルドと試験（実験コード）

必要なもの: Visual Studio 2022 Build Tools、AE SDK 25.6（`C:\AfterEffectsSDK\ae25.6_61.64bit.AfterEffectsSDK`）、After Effects 2025、PowerShell 7。

```powershell
pwsh spike/tools/build.ps1
```

出力先は `dev_plugins/` です。AE に読み込ませるには、`C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\duplix_dev` からこのフォルダへのジャンクションを作ります。

参照コンプとの比較は `spike/tools/run_3d_aerender.ps1 -Name <名前>` で行います。条件は環境変数 `DUPLIX_FILL_*`・`DUPLIX_3D_*` で指定します。組み合わせの例は HANDOFF.md の各試験にあります。自動テストの環境は [docs/ae-test-harness.md](docs/ae-test-harness.md) にまとめてあります。
