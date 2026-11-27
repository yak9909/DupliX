# DupliX 2 のエフェクトコントロール（ハリボテ、2026-11-27）

- 元にした設計: `docs/design/複製プラグイン設計案_v2.md`（採用）。その下敷きの ChatGPT 案は `docs/design/AE_Procedural_Instance_System_Prototype.md`（参考として保存）。
- 実物: エフェクト「DupliX 2 (Prototype)」（マッチ名 `DupliX2`、`src/v2/`、ビルドは `pwsh tools/build.ps1 -Target v2` → `dev_plugins\DupliX2.aex`）。今の DupliX（`src/effect/`）とは別のエフェクトで、互いに影響しない。
- 方針（利用者、11/27）: 入口はシンプルに、奥で細かく設定できる。Custom UI で分かりやすくする。複数の画像（コンプのレイヤー）を読み込めるようにする。まずは中身を作らず、エフェクトコントロールのハリボテを作る。

## 1. 今動くもの・動かないもの

| | 状態 |
|---|---|
| パラメータ一式（下の 3 節） | 全部ある。キーフレーム・エクスプレッションも普通に使える。ただし描画に効くのは下の 4 つだけ |
| Sources（Source 1〜8、Assign） | **実装済み。** 選んだレイヤーを読み込み、コピーごとに Assign（Sequential / Random / By A / By B）で割り当てて描く。Source が 1 つもなければ自分のレイヤーを使う。空きの番号は飛ばす。8 / 16 / 32 bpc |
| Count A・Count B・Grid Size X/Y・Copy Scale・Random Seed | 格子の仮の描画に使う（Count A × Count B の格子をレイヤーの中央に置く。Grid Size は位置だけを決め、コピーの大きさは素材の元の大きさ × Copy Scale）。Random Seed は Assign = Random の並びに効く |
| Custom UI（Overview・Modifier List・Source List） | 実装済み（2 節） |
| 表示の切り替え | Shape に合わない設定、Emission がオフのときの設定、使っていない Modifier の枠、Modifier の Type・Mask に関係のない行、使っていない Source の行を隠す（Effect Controls とタイムラインの両方。`AEGP_SetDynamicStreamFlag` の HIDDEN） |
| それ以外（Shape の Polar・Sphere・Path、Count C、Sampling、Flow、Morph、Emission、Modifier の効果、Source の時間、Render） | まだ何もしない（値を持つだけ） |

格子の仮の描画は 2D で、レイヤーの範囲の中に描く（AE の 3D やカメラには乗らない）。Particular や Form と同じく、コンプと同じ大きさの平面に掛けて使う想定。

## 2. Custom UI

いずれも値を持たない（`PF_Param_NO_DATA`）パラメータに `PF_PUI_CONTROL` で描く。値はすべて AE の普通のパラメータにあり、Custom UI は「見せ方」と「行の表示・非表示」だけを受け持つ（設計 v2 の 7.1 節「案 1」）。

- **Overview**（先頭）: 左に配置の図（Shape ごとに格子・同心円・球・曲線。点 1 つがコピー 1 つで、色はそのコピーが使う Source の色。最大 16 × 16 まで描く）。右に名前、「Grid 5 × 5 × 1 = 25 copies」、そのShape での A・B・C の意味（Grid なら Columns・Rows・Layers、Polar なら Around・Rings・Levels など）、Source の数と割り当て、使っている Modifier の数。
  - Count のパラメータ名は Shape で変えられない（AE の制限）ので、「Count A / B / C」の意味をここで示す。Cinema 4D の Cloner の Count、Form の Particles in X/Y/Z、Motion の Replicator の Columns / Rows に当たる。
- **Modifier List**（Modifiers の先頭）: 使っている枠を「1 Wave」のようなチップで並べる（左の色は Type）。チップの × で外す（Type を None にする → 枠が隠れる）。「+ Add Modifier」で次の空き枠を出して Type = Wave にする。
- **Source List**（Sources の先頭）: 選んだレイヤーを「色・番号・レイヤー名」のカードで 2 列に並べる。カードの × で外す（後ろの Source が 1 つずつ詰まる。取り消し 1 回で戻る）。「+ Add Source」で次の Source の行を出す（レイヤーを選ぶと固定される。何も選ばずに保存すると、開き直したとき空の行は閉じる）。

## 3. パラメータの並び

番号（＝Disk ID・AEGP のストリーム番号）は `src/v2/dx2_params.h`。エクスプレッションは名前で参照するので、名前はエフェクトの中で重複させない（Modifier の中は「M1 Type」のように枠の番号を付ける）。

| 場所 | パラメータ | 参考にした定番 |
|---|---|---|
| 入口 | Overview（Custom UI）、Shape（Grid / Polar / Sphere / Path）、Count A / B / C、Sampling（Regular / Scatter）、Random Seed、Copy Scale | C4D Cloner の Mode と Count、Form の Base Form と Particles in X/Y/Z、Motion Replicator の Shape と Columns / Rows、Form の Random Seed |
| Shape Settings（開いた状態） | Grid: Grid Size X/Y/Z、Brick Offset ／ Polar: Inner Radius、Outer Radius、Angle Range、Sides、Twist、Height ／ Sphere: Sphere Radius、Shell Spacing、Latitude Range ／ Path: Path（マスク）、Row Spacing、Even Spacing | C4D Cloner の Grid Array の Size・Radial の Radius / Start / End Angle、Cavalry Duplicator の Circle の Travel |
| Flow（閉じた状態） | Offset A、Speed A、Speed Variance A、Step A、同じく B、Boundary（Wrap / Ping Pong / Clamp）、Edge Fade | 設計 v2 の 3 節 |
| Morph（閉） | Morph Amount、Target Shape、Target Size | 設計 v2 の 4 節（Shape B の細かい設定は後で） |
| Emission（閉） | Emission（オン・オフ）、Emit Start、Emit Duration、Birth Rate、Birth Jitter、Life、Life Random、Loop Births、Velocity、Velocity Random、Direction、Spread、Gravity、Wind X/Y/Z、Air Resistance、Turbulence Amount / Scale / Complexity / Evolution / Flow Speed / Fade In | Particular の Emitter（Particles/sec、Velocity、Velocity Random）、Particle（Life、Life Random）、Physics（Gravity、Air Resistance、Wind、Turbulence） |
| Modifiers（開） | Modifier List（Custom UI）、Modifier 1〜8。枠の中: Type、ドライバー（Wave: Along・Frequency・Phase・Speed ／ Noise: Speed・Noise Scale・Evolution ／ Random: Seed ／ Stagger: Progress・Order・Softness ／ Map: Map Layer・Sample At・Channel）、Mask（All / Index Range / Every N / Field / Age Range とその設定）、量（Position T/N/B、Shift A/B、Rotation、Scale、Opacity、Tint、Tint Amount、Time Offset、Source Shift、Morph）、Strength | C4D MoGraph の Effector（Plain・Random・Shader・Delay）と Field、設計 v2 の 5 節 |
| Sources（開） | Source List（Custom UI）、Source 1〜8（レイヤー）、Assign（Sequential / Random / By A / By B）、Time Mode（Normal / Hold / Loop）、Source Time Offset、Time Quantize | C4D Cloner の Clones（Iterate / Random / Blend / Sort）、Particular の Sprite の Layer と Time Sampling、Cavalry の Shape Time Offset |
| Render（閉） | Billboard、Depth Sort、Depth Fade、Global Position、Global Rotation X/Y/Z、Global Scale | Form・Particular の Transform と Rendering |

- 入口に出るのは Overview と 7 つのパラメータだけ。Flow・Morph・Emission・Render は閉じた状態で始まる。
- Modifier の枠は 8 個（1 枠 39 パラメータ）。全体で 409 パラメータ。設計 v2 は 16 枠を目安にし、Effect Controls の重さを実測して決めるとしている。8 枠で重さを測ってから増やすか決める（増やすのは後ろに足す形でしかできない）。
- 表示・非表示は AE に毎回問い合わせず、インスタンスごとに「最後に設定した状態」を覚えて、変わる行だけ設定する（AE の `AEGP_GetDynamicStreamFlags` は HIDDEN を正しく返さないという報告があるため）。

## 4. 調べたもの

- Cinema 4D Cloner（Mode: Linear / Radial / Grid Array / Honeycomb、Clones の割り当て）: https://help.maxon.net/c4d/en-us/Content/html/OMOGRAPH_CLONER-ID_OBJECTPROPERTIES.html
- Cavalry Duplicator と Distribution: https://cavalry.studio/docs/nodes/shapes/duplicator/ 、https://docs.cavalry.scenegroup.co/nodes/general/distribution-types
- Motion の Replicator（Shape・Arrangement・Origin・Build Style・Columns / Rows）: https://support.apple.com/guide/motion/motn15320179/mac
- Trapcode Form（Base Form、Particles in X/Y/Z、Layer Maps、Designer のボタン）: https://help.maxon.net/rg/en-us/Content/html/04-Form-summary-of-ui.html 、https://help.maxon.net/rg/en-us/Content/html/16-Form-base-form.html
- Trapcode Particular の Sprite（Layer、Time Sampling、Random Seed）: https://help.maxon.net/rg/en-us/Content/html/24-Trapcode-Particular-particle-sprite-controls.html
- Effect Controls の上に看板（Custom UI）を置く方法（値のない NULL パラメータに描く）: https://community.adobe.com/t5/after-effects-discussions/in-an-ae-plugin-how-to-create-a-quot-banner-quot-at-the-top-of-the-ecw/td-p/14691867
- パラメータを隠す（`AEGP_SetDynamicStreamFlag`、隠した行の値はプラグインから変えられない、HIDDEN は読み出せない）: https://community.adobe.com/bug-reports-528/plug-ins-unable-to-set-value-of-parameters-whose-aegp-dynstreamflag-hidden-flag-is-set-1215452 、https://community.adobe.com/questions-529/aegp-getdynamicstreamflags-cannot-be-used-to-query-current-satus-of-aegp-dynstreamflag-hidden-57516

## 5. 試験

- `pwsh tests/ae/run_jsx.ps1 -Jsx tests/ae/v2_sources.jsx -Name v2_sources -Expect "^done" -Check tests/ae/check_v2_sources.py`
  （`$env:DUPLIX_V2_BPC='32'` で 32 bpc）。赤・緑・青の平面を Source にして、升目の中央の色と升目の間が空であることを確かめる（Sequential、By A で逆順、Source 2 を None にして詰まること、Source なしで自分のレイヤー）。
- Custom UI の見た目とクリックは書き出しでは確かめられない。AE の画面で確かめる。
