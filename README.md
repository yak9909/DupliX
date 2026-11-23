# randamu

After Effects 2025 用のエフェクトです。掛けたレイヤーを 3D 空間にランダムに n 枚のコピーとして並べます。コピーは AE 自身のクラシック 3D レンダラーで本物の 3D レイヤーとして描かれるので、ほかのレイヤーとの前後・交差、ライトの陰影と影、被写界深度、モーションブラー、上にある調整レイヤーが、同じ位置に複製レイヤーを置いた場合と画素単位で同じになります。

現在は実験の段階で、製品のコードはまだありません。`spike/` は方式を確かめるための使い捨てのコードです。製品の設計は [docs/superpowers/specs/2026-11-24-randamu-sublayer-design.md](docs/superpowers/specs/2026-11-24-randamu-sublayer-design.md)、実装計画は [docs/superpowers/plans/2026-11-24-randamu-sublayer-mvp.md](docs/superpowers/plans/2026-11-24-randamu-sublayer-mvp.md) にあります。

AE の非公開の内部関数を差し替えて動かすため、対象は After Effects 2025（25.6）の Windows 版だけです。

## 仕組み

### サブレイヤー

文字単位 3D を有効にしたテキストレイヤーは、1 つのレイヤーのまま、文字ごとに別々の 3D の板として描かれます。AE の内部（`BEE.dll`）では、この板を「サブレイヤー」と呼び、レイヤーのクラス `BEE_AVLayer` の仮想関数でレンダラーに伝えています。

| 仮想関数 | レンダラーに返すもの |
|---|---|
| `SubLayersRenderSeparately` | サブレイヤーを別々の板として描くか |
| `SubLayerRenderCount` | サブレイヤーの数 |
| `GetSubLayerRects` | サブレイヤーごとの矩形・2D 行列（`M_Matrix3`）・3D 行列（`M_Matrix4`） |

テキスト以外の AV レイヤーでは、これらは「サブレイヤーなし」を返します。randamu は、このエフェクトを持つレイヤーに対してだけ「n 枚あり、それぞれこの変形」と答えさせます。あとはレンダラーが n 枚の板として扱うので、前後判定・陰影・影・被写界深度・モーションブラーはすべて AE のレンダラーが通常どおり計算します。

### 仮想関数表の差し替え

`BEE.dll` は `BEE_AVLayer` の仮想関数表を `??_7BEE_AVLayer@@6B@` として公開しています。この表は全 AV レイヤーで共有されているので、表の枠を書き換え、対象でないレイヤーでは元の関数を呼びます。差し替える枠の番号は決め打ちにせず、公開されている関数（`?SubLayerRenderCount@BEE_AVLayer@@...` など）のアドレスを表と照合して実行時に求めます。AE 2025 では次の番号でした。

| 枠 | 関数 | 差し替え後の動作 |
|---|---|---|
| 217 | `SubLayersRenderSeparately` | 真 |
| 219 | `SubLayerRenderCount` | n（下の例外あり） |
| 233 | `GetSubLayerRects` | n 枚分の矩形・M3・M4 を書き込む |
| 79 | `IsLayerType` | 特定の呼び元にだけ偽（後述） |

1 つでも見つからなければ何も差し替えません。

### `GetSubLayerRects` の中身

引数は、時刻（`{int value; unsigned scale}`、レイヤー時間）、レイヤーの矩形、出力先の `std::vector<M_FloatRect>`（1 件 32 バイト）・`std::vector<M_Matrix3>`（72 バイト）・`std::vector<M_Matrix4>`（128 バイト）です。

コピー i の絵は「元のレイヤーのうち、M3 で写した矩形の範囲」で、その板を M4 で 3D に置きます。テキストでは M3 が文字の位置、M4 が文字の 3D 変形に当たります。randamu は次のように書き込みます。

- 矩形は、元のレイヤーの矩形のまま
- M3 は単位行列（レイヤー全体をコピーの絵にする）
- M4 は行ベクトル形式で、4 行目が平行移動、単位はレイヤー座標の px

M4 は、アンカーポイントを A、平行移動を t、拡大縮小を S、回転を Rx・Ry・Rz として `T(−A)·S·Rz·Ry·Rx·T(A+t)` です。回転の順序と符号は AE の X/Y/Z 回転と同じで、順序を X→Y→Z にすると 3 軸を組み合わせたコピーだけがずれました。

vector の領域は AE 側が解放するので、AE と同じ `ucrtbase.dll` の `malloc` で確保します。4096 バイト以上は、MSVC の STL の取り決めどおり 32 バイト境界にそろえ、直前の 8 バイトに元のポインタを置きます。

レンダラーはモーションブラーのシャッター内の時刻ごとに `GetSubLayerRects` を呼ぶので、時刻に応じた行列を返せば、動くコピーのモーションブラーも正しく描かれます。

### ラスターのレイヤーで絵が消える問題

最初は、平面や画像のレイヤーではコピーが全部消えました。シェイプやテキストのようなベクターのレイヤーでは描けていました。

原因は、レイヤーの絵を作る `Rasterize2DGraph`（枠 235）です。この関数はキャッシュなしで `SubLayerRenderCount` を呼び、2 以上ならサブレイヤー用の別の描画ノードに切り替えます。そのノードはラスターのレイヤーでは空の絵を返します。そこで、`SubLayerRenderCount` はキャッシュの `shared_ptr` が空の問い合わせには元の答え（0）を返すようにしました。絵は通常のノードでレイヤー全体として作られます。キャッシュを渡す問い合わせには n を返すので、レンダラーはその絵を n 枚の板として描きます。

### エフェクト付きのレイヤー

Artisan（3D レンダラー）には、「エフェクトが付いた AV レイヤーは 1 枚に平らにして描く」という判定があり、ここで真になるとサブレイヤーに分かれません。randamu 自身がエフェクトなので、この判定を偽にする必要があります。

判定の中では `IsLayerType(0)` が呼ばれます。差し替えた `IsLayerType` は、戻り先の命令の並びを照合し、この判定からの呼び出しのときだけ対象レイヤーに偽を返します。照合する並びは次のとおりです。

```
test al,al; jnz; call BEE_BetaFeature_AreExpressionEffectsAlwaysAllowedEnabled; test al,al; jz;
mov rax,[rbx]; mov rcx,rbx; call [rax+d]; mov rdx,[rax]; mov rcx,rax; call [rdx+d]; test eax,eax; jnz;
mov rax,[rbx]; mov rcx,rbx; call [rax+d]; test al,al; jnz; mov rcx,rbx; call BEE_GetSelectedTrackMatte
```

前半だけで照合すると、音声の合成の判定にも一致しました。そのため後半まで照合しています。

### コピーごとの不透明度

`BEE_AVLayer::GetSubLayerOpacity`（枠 226）を差し替えても、クラシック 3D はこれを 1 度も呼びませんでした。レンダラーがレイヤーの不透明度を受け取る経路は、`AEGPDriver.aex` が取り込んでいる `BEE.dll` の関数 `BEE_GetArtisanLayerOpacity` です。

そこで `AEGPDriver.aex` の取り込み表（IAT）のこの項目を差し替え、元の値にコピーごとの値を掛けて返します。どのレイヤーの何番目のコピーかは、引数の `BEE_RenderTrace` から次のように読みます。

```
v     = *(char**)(trace + 16)
layer = *(void**)(v - 48)   // BEE_AVLayer*
index = *(int*)(v - 40)     // サブレイヤーの番号
```

### 対象レイヤーの見分け方

`AEGP_LayerH` は `BEE_Layer*` そのものです。描画はレイヤーの複製オブジェクトに対して行われますが、`BEE_Layer::GetID()` の値は複製でも同じでした。フックの中では、エフェクトが登録した ID の一覧と照合するだけで、AEGP を呼びません。UI スレッドで呼ばれた BEE の仮想関数の中から AEGP を呼ぶと、AE が止まったためです。レイヤー ID はコンプごとの番号なので、製品ではコンプの項目 ID（`BEE_Layer::GetParentComp()` → `BEE_Item::GetID()`）と組にして照合します。

描画スレッドからの問い合わせでは、そのレイヤーの randamu のパラメータを AEGP（`AEGP_GetNewEffectStreamByIndex`・`AEGP_GetNewStreamValue`）で、問い合わせの時刻の値として読めます。配置はパラメータだけから決まるので、パラメータを変えれば AE のキャッシュは通常どおり捨てられます。逆に、フックが返す配置をパラメータ以外の理由で変えると、AE は古い絵を返します。

### 見つけた経緯

`BEE_AVLayer::SubLayerRenderCount` は、テキストとは別に `ADBE Layer Sets` というプロパティから枚数を求めていました。AV レイヤーをコピーとして並べる、未公開の機能です。スクリプトから `ADBE Layer Set` を追加できましたが、描画するとレイヤーが消えるか、`BEE_LayerSet::GetSubLayerRects` の中で AE が異常終了しました。そのためこの機能は使わず、同じ仮想関数の答えを差し替える方式にしました。

## 検証

各コピーの位置に本物の複製 3D レイヤーを置いた参照コンプを作り、AE が描いた絵と画素ごとに比べています。12 万画素の差の最大は 0 か 1/255 でした。

- 条件: 位置、回転（3 軸）、不透明度、乱数で 100 枚、複数のレイヤー
- シーン: 交差する壁、スポットライトの陰影と影、被写界深度、ガウスぼかしの調整レイヤー、カメラとコピーのモーションブラー
- 素材: 平面、半透明の PNG、エフェクト付きのレイヤー
- 描き方: `saveFrameToPng`、aerender（MFR オフ・オン）

拡大縮小、中心からずらしたアンカーポイント、対象レイヤー自身に変形や親があるとき、UI のプレビューは、まだ確かめていません。

重さは、aerender の 1 フレームで比べると、1000 枚でも 7 枚のときとほぼ変わりません。2500 枚で 1 フレーム約 20 秒、5000 枚で約 450 秒でした。時間のほとんどは AE の 3D 描画で、フック側の処理は 1 フレーム数十 ms です。

試験ごとの記録は [HANDOFF.md](HANDOFF.md)（試験 1〜23）と [docs/experiments/2026-11-22-spike-log.md](docs/experiments/2026-11-22-spike-log.md) にあります。

## 採らなかった方式

- エフェクトの出力でコピーより奥の部分を削る方式は、半透明やぼかしのレイヤーの後ろで背景が透け、加算・乗算のレイヤーの後ろでコピーが消えました。
- コピーの画素だけを自前で合成し直す方式では、調整レイヤー・ライトと影・被写界深度・モーションブラーを再現できません。

## フォルダ

| パス | 中身 |
|---|---|
| `spike/RandamuSpike/` | 実験用のエフェクト。環境変数で動作を切り替える |
| `spike/scenes/` | 試験のシーンを作る ExtendScript（`probe_3d.jsx` が参照コンプとの比較） |
| `spike/tools/` | ビルド、AE を見えないデスクトップで動かす道具、aerender での比較 |
| `spike/ida/` | `BEE.dll` を idalib で解析するのに使ったスクリプト（DLL と解析結果は含めていない） |
| `docs/` | 設計書、実装計画、決定事項、実験記録、自動テストの使い方 |

## ビルドと試験（実験コード）

必要なもの: Visual Studio 2022 Build Tools、AE SDK 25.6（`C:\AfterEffectsSDK\ae25.6_61.64bit.AfterEffectsSDK`）、After Effects 2025、PowerShell 7。

```powershell
pwsh spike/tools/build.ps1
```

出力先は `dev_plugins/` です。AE に読ませるには、`C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\randamu_dev` からこのフォルダへのジャンクションを作ります。

参照コンプとの比較は `spike/tools/run_3d_aerender.ps1 -Name <名前>` で行います。条件は環境変数 `RANDAMU_FILL_*`・`RANDAMU_3D_*` で指定します。組み合わせの例は HANDOFF.md の各試験にあります。自動テストの環境は [docs/ae-test-harness.md](docs/ae-test-harness.md) にまとめてあります。
