# 引き継ぎメモ（2026-11-23 夕方 時点。S5・S5c・S6・S4・S5b・S7 実施済み、すべて合格。次は設計の見直し）

**（11/24 06:00）試験 23（コピーごとの不透明度）まで成功。**
**（11/24）利用者の決定: サブレイヤー方式・回転はアンカーポイント中心・散らすのはレイヤー座標・枚数の上限なし（重さは説明）・ばらつきは位置/回転/拡大縮小/種/不透明度（時間のずらしは将来）。詳細は `docs/decisions-2026-11-24.md` の冒頭。利用者は早く自分で試したい。次は設計書をサブレイヤー方式に書き直して承認 → writing-plans → 実装。**
**（11/24 実装中）ブランチ `feat/mvp` で計画を実行中（このセッションで直接実装）。進み具合は `.superpowers/sdd/2026-11-24-randamu-sublayer-mvp/progress.md`（git 管理外）と `git log`。Task 1〜5 完了: `src/`（core・hook・effect）、`tests/unit`（19 試験）、`tests/ae/smoke.jsx`（エフェクトを掛けると 10 枚のコピーが出る）。次は Task 6（正解との画素比較 `tests/ae/make_ref.jsx`・`run_ref.ps1`）。分かったこと: SEQUENCE_SETUP 中に AEGP でエフェクトを読むと追加が失敗する／パラメータ名は CP932 で渡す／フックの鍵（BEE の comp ID）は AEGP の項目 ID と一致。**
**（11/24）公開リポジトリ https://github.com/yak9909/randamu （main）。push は利用者に確認せずに行ってよい（利用者の指示）。`spike/ida` の `BEE.dll`・`.i64`・逆コンパイルの書き出し、`spike/logs`、`.claude/` は `.gitignore` で除外（Adobe のバイナリと解析結果は公開しない）。README は natural-japanese スキルで書いた。**
**（11/24）設計書 v2 は利用者が承認済み。2D レイヤーへの適用は将来可能にする（最初の版は案内のみ）。実装計画: `docs/superpowers/plans/2026-11-24-randamu-sublayer-mvp.md`（Task 1〜7。利用者の確認と実行方法の選択待ち）。フックの中の鍵は「コンプの項目 ID＋レイヤー ID」（`BEE_Layer::GetParentComp`→`BEE_Item::GetID`、公開関数あり）。**
**（11/24）設計書 v2 を作成: `docs/superpowers/specs/2026-11-24-randamu-sublayer-design.md`（承認待ち。第 6 節の 2 問は回答済み: 元のレイヤーは「コピーだけ＋1 枚目は元の位置のチェック」、2D レイヤーは「コピーを出さず 3D にするよう案内」）。承認後は writing-plans で、最初の区切りを「UI で触れる最小の版」にする。利用者に渡す前の必須確認は設計書 第 9 節の上 4 つ（UI プレビューで止まらない・拡大縮小とアンカー・読み込み時の自己登録・コンプ複製での ID 重複）。**
**（11/24 早朝、旧）試験 22 まで完了、試験 23 の 2 回目はビルド済み・未実行だった。利用者向けの判断材料は `docs/decisions-2026-11-24.md`。以下は旧見出し。**
**（11/24）試験 18 まで: 平面・PNG 素材（エフェクト付き）のコピーが、位置・回転・陰影・影・交差・被写界深度・調整レイヤー・モーションブラーで正解と画素単位で一致。aerender（MFR オフ・オン）でも一致。キャッシュはエフェクトのパラメータ変更で正しく更新、hook から複製レイヤーのパラメータも読める。再開したら「試験 15」の下の「次の 1 手」の 4（設計の相談）と「その先」から。利用者は就寝中: 中断するときは必ずこのメモを更新してクリアしてよい状態にする。** 見張りの閾値は全プロジェクト共通で 95% に変更済み。
- 利用者への説明済み: レイヤーセットはエフェクトではなく、レイヤー自身のプロパティのグループ（トランスフォームやマスクと同じ並び）。未公開で、作りかけの可能性もある。

## まず読むもの
1. `docs/experiments/2026-11-22-spike-log.md` … これまでに試したことの全記録（環境、失敗の原因、分かったこと）
2. `docs/superpowers/specs/2026-11-22-randamu-depth-scatter-design.md` … 設計書（下書き）
3. `docs/ae-test-harness.md` … 自動テスト環境の使い方と注意点（ほかのプロジェクトでも使う）

## 利用者が決めたこと（蒸し返さない）
- 作るもの: AE 2025 用のエフェクト。適用したレイヤーの画像を XYZ にランダムに n 個（上限なし）配置し、コンプ内の**全レイヤー**との前後を画素単位で判定する（深度テスト）。
- **1 レイヤーに 1 エフェクトで絶対に完結させる**。補助レイヤー、複製、他レイヤーを隠す運用は不可。必要なら「黒魔術」（非公式な手段）も許容。
- 方式: エフェクト内部で深度を持ち、自前で合成する。具体的には、設計書の「くり抜き方式（A'）」で進めている（エフェクトのレイヤーを最上段に置き、他レイヤーより奥の部分のアルファを削る）。
- ~~他レイヤーの取得: 隠しレイヤーパラメータ（スロット）＋エクスプレッション。~~ → **（11/23 夕方 決定）スロットとエクスプレッションをやめ、他レイヤーの絵・位置・深度・不透明度・表示状態はすべて AEGP で取る**（S5b の結果による）。スロット数の上限はなくなり、S3 は不要。
- **絶対条件（11/23 確定、設計書 第 2 節）**:
  1. ランダム配置をするレイヤーにエフェクトを 1 つ掛けるだけで成立する（効果プリセットや手作業の設定も不可）。
  2. 隠れは必ず AE のレンダリング結果をもとに判定する。他レイヤーのマスク・エフェクト・トラックマットを必ず反映する。
- **他レイヤーの画素は AEGP で描画させて取る（案 3、11/23 決定、設計書 4.2 節）**。`PF_Param_LAYER` のチェックアウトは「ソース」でマスク等を含まないため不可。
- 検証は Claude が自動で行う（ビルド → シーン生成 → aerender → 画像判定）。ホットリロードも欲しい（UI で調整する段階で導入する予定）。

## 進め方の状態
- brainstorming スキルの途中。方式は選択済み。設計書は下書きで、**未承認**。
- 実験の結果を設計書に反映し、5.4 節の未決事項（エフェクトより上のレイヤー、2D レイヤー、平面でないもの、コピーの向きやばらつき）を利用者と決めて、承認をもらう。承認後に writing-plans スキルで実装計画を立てる。
- 実験コード（`spike/`）は使い捨て。製品コードには流用しない。
- 利用者への返事はすべて日本語で書く。

## 自動実行の決まり（11/23 追加。利用者の指示）
- 利用者はテスト中も PC で作業している。AE は見えないデスクトップで動かす（`spike/tools/ae_bg.ps1`）。利用者の画面を撮らない。撮影は AE のウィンドウだけを PrintWindow で撮る。
- ダイアログの判定に OCR などの不安定な方式を使わない。内部の状態（レジストリなど）で判定する。
- クラッシュ修復ダイアログは、起動前に `Repair-AeCrashState`（`spike/tools/ae_guard.ps1`）で防ぐ。詳細は実験記録 5.1 節。

## 状況
- 設計書（下書き）: `docs/superpowers/specs/2026-11-22-randamu-depth-scatter-design.md`。実験結果の反映と利用者の承認はまだ。
- 実験コード（使い捨て）: `spike/`。ビルドは `spike/tools/build.ps1`、実行と自動判定は `spike/tools/run_s1.ps1`。
- プラグインの配置: `MediaCore\randamu_dev` → `randamu\dev_plugins` のジャンクション（作成済み）。

## 実験で分かったこと
- S1 合格: 小さな 2D レイヤーのエフェクトが、コンプ全体の出力をコンプ座標に正確に合わせられる（移動・縮小・回転・アンカー変更の 4 種）。出力ワールドの `origin_x/origin_y` が、結果の矩形の原点と一致する。回転・拡大したレイヤーでは、AE の再サンプリングで縁に 1px 程度のにじみが出る。
- S2 一部確認: レイヤーパラメータを 64 個定義できる。エクスプレッションで `effect('RandamuSpike')('Slot 1')` から Layer を参照でき、`toComp()` でカメラ投影後の画面座標が取れる。`activeCamera.fromWorld(L.toWorld(p))[2]` でカメラ空間の深度が取れる。チェックアウトはレイヤー座標の画素で、空のスロットは 0×0。
- S2 の形の一致判定（透視補正つき）は未確認。直前の実行は下の AE の問題で止まった。
- 重要: AE のディスクキャッシュは再起動しても残り、プラグインを作り直しても古い絵を返す。`PF_OutFlag2_I_MIX_GUID_DEPENDENCIES` を立て、PreRender でビルド ID を `GuidMixInPtr` に混ぜて対策した（ビルド済み、未検証）。本番にも必要。
- AE の自動実行の注意:
  - `-noui -r` ではスクリプトが動かない。`AfterFX.exe -r` を使う。
  - `-r` に渡すパスに空白を含められないので、%TEMP% にコピーしてから渡す。
  - `app.quit()` が効かないことがあるので、ログの完了行を見てから実行側で AE を閉じる。
  - 出力テンプレートに PNG がないので、TIFF を使う。
  - Python は `spike\.venv`（uv で作成。PATH の python は MSYS2 版で使えない）。
- テストコンプのレンダラーは `ADBE Advanced 3d`（＝クラシック 3D）だった。

## 解決済み（11/23）
- 強制終了後にスクリプトが動かなかった件: 原因は「クラッシュ修復オプション」ダイアログ。起動前にレジストリの印を書き換えて防ぐ。
- `-r` のパスを引用符で囲むと何も実行されない（AfterFX.exe は引用符を処理しない）。
- `run_s1.ps1` の再実行: キャッシュ対策は効いた（SmartRender 4 回）。S1 は全合格。S2 は v2_anchor だけ IoU 0.9899 で、判定の 0.99 をわずかに下回る（原因は未調査）。

- S2 合格: v2_anchor の差は縁の誤差だった。判定を「食い違いがすべて縁から 2px 以内」に変えた（実験記録 7 節）。
- （決定 11/23）今の方式（見えないデスクトップの AE）のまま進める。nexrender の方式は採らない。

## S2b の結論（11/23、実験記録 7.1 節）
- レイヤーパラメータの既定の取り込み方（「ソース」とみられる）では、対象レイヤーのマスク・エフェクト・トラックマットはチェックアウトの結果に含まれない。
- テキスト・シェイプ・コラップスしたプリコンプは、3D 変換とカメラ投影を済ませたコンプ座標の絵で返る（平面・素材・プリコンプはレイヤー座標）。
- `check_s1.py` は、これらの「分かっている制限」の 6 種を (info) として出し、合否から外した。

## S5 の結論（11/23 午前、実験記録 7.2 節）
- 描画中（PreRender・SmartRender のどちらでも）に `AEGP_RenderAndCheckoutLayerFrame` で他レイヤーを描画できる。MFR オン／オフ・aerender で、デッドロックもクラッシュもなし。
- 受け取りの GUID を PreRender で `GuidMixInPtr` に混ぜると、対象ごとに別のキャッシュになる。
- マスクは反映される。テキスト・シェイプはコンプ座標の投影済みの絵で返る（そのまま使えば一致）。
- **反映されないもの（利用者に相談中）**: トラックマット／レイヤーの範囲からはみ出すエフェクトの結果（範囲で切られる）。
- コラップスしたプリコンプ: AEGP では画素なし（world が NULL）。スロットのチェックアウト（コンプ座標の絵）を使えば形は合格。ただしそのレイヤー自身のマスク・エフェクトは入らない見込み。
- キャッシュの注意: 受け取りの GUID は中身の変化しか拾わない見込み。対象の移動・カメラの変化は、今はエクスプレッションの角の位置が拾っている。位置も AEGP に移すなら、変換・カメラ・レイヤー一覧を自分で混ぜる（S5b の候補）。
- 自動テストの改善: エフェクトが原因のクラッシュで次の起動時に出るダイアログは `Effect Crashed.txt` が原因。`Remove-AeEffectCrashRecord` で防ぐ（見本のファイルでは確認、実物では未検証）。aerender に待ち時間の上限を付けた。
- 実行: `spike/tools/run_s1.ps1 -S5Site pre|smart -Mfr ON|OFF`。4 通りの通し実行はスクラッチパッドの使い捨てスクリプトで行った。

## 利用者の決定（11/23 午前）
- トラックマット: **案 A に決定**。マットのレイヤーも AEGP で描画し（`AEGP_GetTrackMatteLayer`）、マットの種類に従ってコンプ座標で掛け合わせる。
- はみ出すエフェクト（他レイヤーに掛かったドロップシャドウなど）: **案 2 に決定**。範囲を広げる方法を探す（黒魔術・非公式な手段も許容）。`AEGP_LayerRenderOptionsSuite2` には ROI の設定関数がないので、別の手段を調べる（例: `AEGP_RenderOptionsSuite4`（アイテム用）に ROI の設定があるので、コンプ単位の描画やレイヤー用オプションとの併用、ネットや SDK サンプルでの先行例、ヘッダの未公開の関数）。見つからないときだけ、制限として書く案に戻して利用者に相談する。

## S5c の結論（11/23 昼、実験記録 7.3 節）
- **はみ出すエフェクトは解決**。`AEGP_LayerRenderOptionsH` は AE 内部の `BEE_LayerRenderOptions*` そのもので、`BEE.dll` が公開する `BEE_RenderOptions::SetROI`／`GetROI`（`M_Rect` = int16 ×4 `{top,left,bottom,right}`、コンプ座標）で範囲を広げられる。t_effect は IoU 0.757 → 0.9938。
- **コラップスしたプリコンプも AEGP で取れるようになった**（ROI を ±約 3 万に広げたとき。コンプ座標の絵で返り、IoU 0.9912）。`AEGP_LayerFlag_COLLAPSE` でコンプ座標として扱う。
- `zero`（{0,0,0,0}）は「空」で不可。MFR オン／オフとも同じ結果。クラッシュなし。
- 実験コードは環境変数 `RANDAMU_S5C_ROI=margin:N` で切り替え（未設定なら既定の範囲）。
- 本番の未決: 範囲の上限の決め方（巨大な結果を返すエフェクト対策）と、関数が見つからない版の AE での扱い。

## S5c のあとに分かったこと・したこと（11/23 昼）
- 範囲の拡大は**既定でオン**にした（環境変数が未設定なら `margin:30000`。`RANDAMU_S5C_ROI=none` で既定の範囲に戻る）。
- `check_s1.py`: `t_collapse` を合否に入れた（AEGP 経由を必須）。`t_text` は細い字なので、IoU の下限を 0.80 に下げた（縁から 2px の規則は同じ）。この状態の基準の実行では、**不合格は `t_matte` だけ**だった。
- `BEE.dll` の公開関数を `dumpbin` で調べた。`BEE_LayerRenderOptions::SetLayerStage`（既定 3）を 4・5 に変えても、トラックマットは掛からなかった（マットはコンプでの合成で掛かる）。黒魔術での近道はここで打ち切り、案 A に進んだ。

## S6（トラックマット、案 A）: **合格**（11/23 午後、実験記録 7.4 節）
- マットの 4 種（アルファ／反転アルファ／ルミナンス／反転ルミナンス）がすべて `render=aegp` で合格。ほかの項目も崩れず ALL OK。
- マットのレイヤー（ビデオがオフ）も AEGP で画素を返す。
- シーン生成の不具合は解決: 1 個目の出力モジュールのテンプレートを `RNDM_TIFF_A`（ASCII 名）として保存し、2 個目以降はそれを適用する。`setSettings` で写す方式は形式が写らず、2 個目以降が H.264 のままだった。
- 未確認: マットの入れ子、マットのアニメーション（S4 でまとめて見る）。

### （以下は作業途中だったときの記録）
- 実験コードは実装済み・ビルド済み:
  - パラメータ `S2 C0..C2` を末尾に追加した（Slot 2 のレイヤーの角の位置）。
  - PreRender で `AEGP_GetTrackMatteLayer`／`AEGP_GetLayerTransferMode` を使い、マットのレイヤーも `RenderTarget` で描画する（ログは `S6 matte:`）。
  - SmartRender で「Target のアルファ × マットの値（アルファ／反転アルファ／ルミナンス／反転ルミナンス）」を求める（`SampleAtComp`）。
- `make_s1.jsx`: `Matte` という名前のレイヤーがあれば、Slot 2 とエクスプレッションを設定する。`t_matte`（アルファ）、`t_matte_inv`、`t_matte_luma`、`t_matte_nluma`（白と黒の四角を並べたシェイプ）を追加した。
- **止まっている所**: コンプを増やしたら、既知の「テンプレート名が化ける」問題でシーンの生成が止まった。2 個目以降の出力モジュールに 1 個目の設定を `getSettings`／`setSettings` で写す方式に変えたところ、シーンはできたが、**出力の TIFF が 1 枚（v0_identity）しかできなかった**（写した設定に出力先が含まれ、そのあとの `om.file` の指定が効いていないとみられる。未調査）。

## S4・S5b: **合格**（11/23 午後、実験記録 7.5 節）
- 同じ AE のセッションの中で「描く → 変える → 描く」を行う試験を作った（`run_s4.ps1`。`-NoGuid`／`-S5b 1|compare`／`-NoSlot`／`-NoMix` で条件を切り替え）。変更なしの 2 回目はキャッシュから返る（試験として有効）。対照実験で、GUID を混ぜないと古い絵が返ることも確認した。
- マスク・エフェクト・プリコンプの中身・マットのマスク・移動・カメラの変更は、すべて再描画されて正解と一致。
- 不透明度は AEGP の絵に入らない → `AEGP_StreamSuite6` で読んで自前で掛ける方式で解決（マットの不透明度も）。
- **S5b: 位置・深度は AEGP の行列（`GetLayerToWorldXform`＋`GetEffectCameraMatrix`）で、エクスプレッションと小数第 4 位まで同じ値が出る**。1 回 0.6ms。エクスプレッションは 1 フレームあたり 300〜690ms かかっていた。AEGP の描画そのものは 1 フレームあたり 3〜13ms（PreRender の合計の大半は実験用のログ）。
- **スロットもエクスプレッションも使わない条件（本番に近い）で、S4 の全ケース（`-S5b 1 -NoSlot`）と、スロットなしで作り直したシーンの S1 全 18 項目（`RANDAMU_S1_NOSLOT=1`＋`RANDAMU_S5B=1`）が合格**。計算した位置は鍵に混ぜる必要がある（混ぜないと、自分のレイヤーの変形だけが違うコンプで別のコンプのキャッシュが流用された）。
- 表示されないレイヤー（目のスイッチがオフ・時間の範囲外）も AEGP は画素を返すので、`AEGP_IsVideoActive` が偽なら不透明度 0 として扱う（`t_hidden`、`t_outrange` で合格）。カメラのないコンプも合格（`t_nocam`）。
- **含意（利用者に確認したい）**: スロット＋エクスプレッションをやめて、全部 AEGP で取る方式にできる。そうすると S3（スロットの自動割り当て）は不要になり、スロット数の上限もなくなる（「将来は上限なし」がすぐ実現できる）。
- 未確認: 画面のプレビューでの挙動、レイヤーの追加・削除・並べ替え（レイヤー一覧を鍵に混ぜる）、合成の段階で掛かるほかの性質（対象より上の調整レイヤー、描画モード、レイヤースタイル、ソロ）、`AEGP_SetTime` の時間の種類、MFR の切り替え。

## S7（全レイヤーを対象にする）: **合格**（11/23 夕方、実験記録 7.6 節）
- `RANDAMU_S7=1` で、コンプの全レイヤーを対象にする（重ね絵は和集合）。対象から外すもの: 自分、カメラ・ライト、ヌル、ガイド、調整レイヤー、ソロ以外（ソロがあるとき）、`IsVideoActive` が偽、このエフェクトを持つレイヤー、実験用の `BG`。
- S1（スロットなしのシーン、24 項目）全合格。S4（`run_s4.ps1 -S7 -S5b 1`）で表示の切り替え・追加・削除・並べ替え・49 枚を含む全 12 ケース合格。
- 49 枚: AEGP 部分は 1 フレームあたり約 300ms（PreRender 3 回分。ログ込み、未最適化）。
- 再入の防止は実装したが、実際に働く場面（別のエフェクトがこちらをエフェクト込みで取り込む）は未検証。
- シーン生成で 1 回、AE が起動中（フォントの読み込み中）に止まった。再実行で直った（たまたまとみられる）。

## （11/23 夜）くり抜き方式（A'）は不採用。正しい見た目と完全一致が必須
- 比較画像（`spike/run_approx.ps1` → `spike/logs/approx/approx_compare.png`。AE の標準機能だけで正解と A' を描き比べた）を見た利用者の判断: 「絶対駄目。絵の改変に等しい。絶対正しい見た目にする。軽量化も考える」。半透明・ぼかしの後ろで背景が透け、加算・乗算の後ろでコピーが消えていた。
- **「正しい」の定義（案）**: 各コピーを本物の 3D レイヤー（元レイヤーの複製）として同じ変形で置いた参照コンプを AE が描いた絵と、RGB(A) で画素ごとに一致（8 bpc で ±2/255 程度）。形だけの判定は合格に数えない。
- **新しい方式（案）: コピーがある画素だけを全部合成し直す**。コピーのない画素は透明を出す（AE の合成がそのまま見えるので正確で、費用もゼロ）。コピーのある画素では、各レイヤーの AEGP の絵とコピーを、AE の規則（順序・描画モード・不透明度・マット）で自前で合成し、不透明で出す。コピーの範囲に掛かるレイヤーだけ描画し、同じフレームの 3 回の PreRender で結果を使い回す。
- 実験の順: (a) 背景の上のコピー 1 枚が、本物の 3D レイヤーと RGB で一致するか（透視の再サンプリング・縁・プリマルチプライ）→ (b) `make_approx.jsx` の正解と一致するか → (c) 描画モードを 1 つずつ → (d) 3D の重なり順の規則 → (e) 背景のないコンプ。
- 再合成では再現が難しいとみられるもの（利用者に必須か対象外かを聞く）: コピーより上の調整レイヤー、クラシック 3D のライトと影、カメラの被写界深度、モーションブラー、Advanced 3D／Cinema 4D レンダラー、環境レイヤー、レイヤースタイル。

### 利用者の回答（11/23 夜）
- 「正しい」の定義（上の案）: **承認**。
- コピーに必ず正しく効く必要があるもの: **調整レイヤー、ライトと影、被写界深度・モーションブラー**（必須）。Advanced 3D／Cinema 4D レンダラーは当面対象外。
- これらは AE が合成の段階で掛けるので、上の「自前で合成し直す」方式では実現できない（AE の 3D レンダラーの作り直しになる）。→ **コピーを本物のレイヤーとして AE 自身に描かせる必要がある**。置き場所の候補: (A) エフェクトが管理する隠しコンプ（プロジェクトに項目が見える・同期が要る・描画が約 2 倍）、(B) BEE の内部に一時レイヤーを差し込む（プロジェクトは変わらないが実現性不明・危険）。どちらも「補助レイヤー・複製は不可」の条件とぶつかるので、**利用者が選ぶ**。その前に実現性を短く確かめる: (i) PreRender から別のコンプを AEGP で描けるか（デッドロック・費用）、(ii) エフェクトがメインスレッドで動く機会と、そこでの編集が取り消し履歴・未保存の印に影響するか、(iii) 隠しコンプに「コピー＋利用者のコンプをコラップスしたプリコンプ」を置いて、ライト・影・調整レイヤー・被写界深度が正解と RGB で一致するか。

### （11/23 夜）利用者の指示「実装の前に立ち止まり、もっと楽な方法を探す（ネット検索・検証・AE の静的解析）」→ 調査中
- **有力な候補: BEE の「サブレイヤー」の仕組みに乗る**。文字単位 3D のテキストは、1 つのレイヤーの中に文字ごとのサブレイヤーを持ち、クラシック 3D で本物の 3D の板として描かれる（ライト・影・被写界深度・モーションブラー・交差がすべて効く）。`BEE.dll` は、テキスト専用ではない `BEE_AVLayer` の仮想関数として、`SubLayerRenderCount`・`GetSubLayerLocalMatrix3D`・`GetSubLayerOpacity`・`GetSubLayerXferMode`・`GetSubLayerBlur`・`GetSubLayerDepth`・`GetSubLayerRects`・`GetSubLayerXform3D`・`HasSubLayerXform`・`CanSubLayersRenderSeparately`・`SetSubLayersRenderSeparately` を公開している（SDK にも `AEGP_SubLayerIndex`、`AEGP_LayerFlag_SUBLAYERS_RENDER_SEPARATELY`、Canvas Suite の `AEGP_GetLayerAndSubLayerFromLayerContext` がある）。こちらのレイヤーに「サブレイヤーが N 個、それぞれこの変形」と答えさせれば、コピーは AE 自身の 3D レンダラーで本物として描かれる（隠しコンプも自前の合成も不要、正確さは作りから保証）。
- 静的解析で分かったこと（自動解析なしの部分的な逆コンパイル）: `BEE_AVLayer::HasSubLayerXform` は 0 を返し、`BEE_TextLayer` 版は 1 を返す（これが分かれ目とみられる）。`BEE_AVLayer::CanSubLayersRenderSeparately` は 1 を返す。そのほかの関数（`SubLayerRenderCount` など）は、自動解析なしでは関数の境界が取れず読めなかった（JUMPOUT）。
- ネット検索: この仕組みを外から使った公開例はない。既存の 3D プラグイン（Element 3D、Trapcode など）は自前のレンダラーで、AE のライト・影とは完全には混ざらない。Artisan（3D レンダラーの差し替え）は、レンダラーを丸ごと書く必要があり重い。
- **解析の道具**: 書き出しスクリプト `C:\Users\yakuruto\AppData\Local\Temp\claude\18403859059d0be6\997d682e-354f-4e6e-9fcb-96aa1b89dd6b\scratchpad\ida_dump.py`（idalib を Python から直接使い、公開関数と仮想関数表をファイルに書き出す。MCP の応答でトークンを使わないため。利用者の提案）。実行: `C:\Users\yakuruto\.local\idalib-mcp-venv\Scripts\python.exe ida_dump.py <dll> <出力先> <関数名の正規表現> [<vtable の正規表現>]`。出力: `scratchpad\ida\dump_sublayer\`（index.txt、fn_*.c、vt_*.txt）。スクラッチパッドなので消えることがある。
- 失敗の記録: idalib-mcp の全体の自動解析（23MB で約 15 分）を、終わった直後に止めてデータベースを消してしまった。

## （11/23 17:30）全体解析で見つけた隠し機能「レイヤーセット」（最有力）
- 全体解析済みの `.i64`: `spike/ida/BEE.dll.i64`（481MB、908 秒。消さない）。書き出し: `spike/ida/dump_sublayer`・`dump_layerset`・`dump_setnames`（index.txt から引く）。
- AV レイヤーのサブレイヤー数（`BEE_AVLayer::SubLayerRenderCount`）は、テキストとは別に **`ADBE Layer Sets` の各セットの `ADBE Set Copies`（コピー数）の積**で決まる。AE に、AV レイヤーをコピーとして並べる未公開の機能（シェイプのリピーターに似る）が入っている。
- 内部名: `ADBE Layer Sets` > `ADBE Layer Set` > {`ADBE Set Copies`, `ADBE Set Properties` > `ADBE Set Property`（参照するストリーム。`BEE_CmdSetLayerSetPropStreamRef`）> `ADBE Set Stops` > `ADBE Set Stop` > `ADBE Set Stop Pos`（Rel Pos）}。プロパティごとに「止まり点（相対位置と値）」を持ち、コピーの間を補間するとみられる。
- `BEE_AVLayer::CreateSet` はセットを足し、コピーを作り、レイヤーのスイッチ 0x80000 と 128 を立てる。0x80000 は `SubLayersRenderSeparately`（サブレイヤーを別々に描く）の条件のビット。つまりセットを作るだけで AE 自身がコピーを別々の板として描く見込み。
- セットのグループは、空なら UI で隠される（`UpdateStreamVisibility`）。保存形式の版 92 以上で書かれる。
- **試験 1（11/23 17:40、`spike/tools/run_probe.ps1 -Jsx scenes\probe_sets.jsx -Name sets`、結果は `spike/logs/probe_sets`）**: スクリプトから `L.property("ADBE Layer Sets").addProperty("ADBE Layer Set")` で**セットを足せる**（UI 名「セット」>「設定 1」>「コピー数」（既定 5）・「プロパティ」）。`collapseTransformation` は変わらない。ところが、**セットを足すとレイヤーが丸ごと描かれなくなる**（3D の白い平面・不透明度 50%、中央 128 → 0、全面が黒）。さらに **`コピー数.setValue(3)` で AE が止まる**（応答なし、強制終了）。仮説: スクリプトの経路では `CreateSet` が立てるスイッチ 0x80000・128 が立たない／プロパティ（止まり点）がないと描けない／機能フラグで止められている。
- **`AEGP_LayerH` は `BEE_Layer*` そのもの**（AEGPDriver の CreateNullLayer の実装が、出力の LayerH をそのまま `BEE_CmdSetLayerSwitch` に渡している。`spike/ida/AEGPDriver.aex.i64` に移した、`ida_importers.py` で確認）。
- **試験 2（17:46、`probe_sets2.jsx`＋プラグインの `ProbeSets`、環境変数 `RANDAMU_PROBE_SETS=create|switch`、`PF_Cmd_SEQUENCE_SETUP` で "Target" に対して実行。結果は `spike/logs/probe_create`）**: プラグインから `BEE_AVLayer::CreateSet` を呼べた（undo グループ内、例外なし）。**スイッチ 128 はコラップス**（`collapseTransformation` が true になる）。描画は元と同じ（中央 128、形も同じ）で、消えもしないが、コピーが重なった跡もない（5 枚なら約 251 のはず）。→ プロパティ（`ADBE Set Property`＋止まり点）がないとコピーに展開されないとみられる。`switch` モード（スクリプトで足した後にスイッチだけ立てる）は未実行。
- 次: コピーごとの変形の作り方を読む（`BEE_AVLayer::SubLayerRenderCount` の `*a3` が非空の経路 = `sub_180284090` がコピーの一覧（72 バイト/件）を作る）。プロパティの足し方は `BEE_LayerSet::AddProperty(BEE_StreamSpec const&)` と `BEE_CmdSetLayerSetPropStreamRef(BEE_Layer*, TDB_StreamIDPath const& set側, TDB_StreamIDPath const& 参照先)`、止まり点は `BEE_LayerSetStops::AddStop(BEE_Layer*, TDB_Stream*, double)`。UI の入口は AfterFXLib.dll（巨大、未解析）。
- 未確認: スクリプトから `addProperty("ADBE Layer Set")` できるか（→ できた）、コピーごとの変形がどう決まるか（`GetSubLayerLocalMatrix3D` は vtable +1800 と `sub_180284090` のキャッシュに委ねる）、ライト・影・被写界深度が効くか。

## （11/23 19:35〜）レイヤーセットの続き（試験 3〜）
- **コピーの変形の作り方（`BEE_LayerSet::GetSubLayerRects` の逆アセンブル）**: コピー数 N、セットのプロパティのうち参照先が位置・X/Y/Z 回転のものだけを使い、コピー i = `i × (止まり点 0 の値 − 今の値) / N` の平行移動と回転。等間隔の列のみ（ランダムも拡大縮小もなし）。複数セットは掛け合わせ。プロパティがないと一覧が空（試験 2 で何も起きなかった理由）。
- 一覧は `BEE_AVLayer::NewXformCache2D`（vtable +0x768）が vtable +0x748 `GetSubLayerRects` を呼んで作り、キャッシュの +96（M4、128B）/+120（M3、72B）/+144（矩形、32B）に入る。`SubLayerRenderCount`（+0x6D8）はキャッシュありなら M3 の件数、なしならコピー数の積。
- `SubLayersRenderSeparately`（+0x6C8）= フラグ 0x80000 かつ（AV 型で 3D ビット 0x400 / テキスト / シェイプ / 型 7）。
- `AEGP_StreamRefH` の中身: +120 がレイヤー、+80 が `TDB_StreamIDPath`。ここから `BEE_StreamSpec(layer, path)`（48 バイト、vptr あり）を作れる。`AddProperty` は止まり点を 1 つ作り、位置なら値を +(100,100,100) にする。
- **試験 3（prop モード、`spike/logs/probe_prop*`）**: `CreateSet`＋`AddProperty(位置)` に成功（UI 名「位置 1」「終了点」「停止 1」）。プラグインからレイヤーに聞くと、別々に描く=1、コピー数=5、行列は (0,0,0),(20,20,20),(40,40,40)…（**行ベクトル形式、4 行目が平行移動**）。**ところが描画は変わらない**（キャッシュを捨てても同じ）。
- **原因（途中）**: 描画は別のレイヤーオブジェクト（描画用の複製）に対して行われ、その複製ではフラグ 0x80000 が立っておらず、コピー数 0。→ 対象のインスタンスだけ vptr を差し替える方式は使えない（描画側に効かない）。
- 保存して開き直すと（セットは残る、sets n=1）、描画（`saveFrameToPng`）で AE が止まる。ただしこのときは共有の vtable にログ用の包みが入っていた（包みの戻り値の型の推測違いが原因の可能性あり）。
- Artisan 側: レイヤー一覧を作る `sub_1803855B0` が「別々に描く」ならコピー数ぶんのエントリを作る。クラシック 3D はここを通る見込み。
- 道具（`spike/ida/`）: `ida_fn.py`（関数とその呼び先、`vt:正規表現:枠数`、`asm:0xADDR`）、`ida_vcall.py`（`call/mov [reg+disp]` で仮想呼び出しの場所を探す）、`ida_callers.py`（呼び元を数段）、`ida_bytes.py`（定数を読む）。`BEE_exports.txt`・`BEE_imports.txt`・`AEGP_imports.txt` は dumpbin の出力。
- プラグイン（`RandamuSpike.cpp` の `ProbeSets`）: `RANDAMU_PROBE_SETS=prop`、`RANDAMU_PROBE_SW=0x400`（追加のスイッチ）、`RANDAMU_PROBE_VTLOG=1`（そのインスタンスだけ）／`g`（共有の vtable に包み）。シーンは `scenes/probe_sets2.jsx`（purge・保存・開き直し付き）。判定はスクラッチパッドの `bbox.py`（範囲と明るさの段）。
- ~~次: (1)〜(3)~~ → 実施済み（下記）。

### （11/23 20:45）試験 4・5 の結果
- **ロガーなしで開き直し → AE が落ちる**（止まるのではなく、自分で終了。ダンプは見つからず）。ENTER/LEAVE ログでは、描画スレッドの複製で `別々に描く → コピー数 → NewXformCache2D → GetSubLayerRects` と進み、**`GetSubLayerRects`（本来の `BEE_LayerSet` 経路）の中で落ちる**。→ 本来のレイヤーセット機能は、描画では使えない（壊れている／未完成）。
- 同じセッション内では、複製のセット数は 0（`sets=0`）。**SEQUENCE_SETUP からプラグインが BEE を直接いじった変更は、描画用の複製に同期されない**（スクリプトでの変更は届く。試験 1）。
- **試験 5「fill」（`RANDAMU_PROBE_SETS=fill`、`spike/logs/probe_fill*`）**: セットは使わず、**共有の vtable の 3 枠を差し替え**る（217 別々に描く→真、219 コピー数→5、233 `GetSubLayerRects`→元を呼ばずに矩形・M3・M4 を自分で 5 件詰める。メモリは ucrtbase の malloc、4096 以上は MSVC の 32 バイト整列の取り決め）。対象は `BEE_Layer::GetID()`（公開関数）で見分ける（描画用の複製でも同じ ID=17 だった）。行列は U.dll の `M_Matrix3/4::SetIdentity/Translate`。x を 60 ずつずらした 5 枚。
  - **落ちない**。描画スレッドが複製に対して、コピーごとに `GetSubLayerXform3D`(225)・`GetSubLayerLocalMatrix3D`(168) を 5 回、`Rasterize2DGraph`(235)・`GetBoundsFromCache`(229)・`GetSubLayerXform2D`(222/223) を呼ぶ（= AE がコピーを 5 枚の板として扱い始めている）。
  - **ところが絵ではレイヤーが消える（全面黒、f_after・f_reopen とも）**。試験 1（スクリプトでセットを足すと消えた）と同じ症状。
  - 不透明度（226）は常に 1.0、範囲（229）はこちらの M3 と矩形から計算 → この 2 つは原因ではない。
- **次の 1 手**: 消える原因を探す。候補: (a) コピーごとの絵（`Rasterize2DGraph` の結果、あるいは `BEE_RenderTextureForArtisan` のサブレイヤー版）が空、(b) 行列の座標系の違い（M4 の平行移動の単位・原点、2D 側 M3 との組み合わせ）、(c) `GetSubLayerXform2D` の 4 つの版が返す行列のどれかが未設定。まず平行移動 0（単位行列 5 枚）にして、重なって明るくなる（約 1−0.5^5）か消えるかを見る。消えるなら (a)、出るなら (b)。ログは `RANDAMU_PROBE_VTLOG=g` を併用（fill と同時に使える）。
- **利用者の仮説（有力）: ベクターとラスターの違い**。サブレイヤーの仕組みは文字単位 3D のテキスト（ベクター）用で、コピーごとに形から描き直せる。平面（ラスター）では「サブレイヤー i の絵」を作れず空になる → 消える。エフェクトが付くとラスター化（Flatten）されるので別々に描かない判定になる、とも一致する。反する点: レイヤーセットは一般の AV レイヤーに付いている（ただし本来の経路は落ちる＝作りかけの可能性）。**確かめ方**: (1) 平面で単位行列 5 枚 → 消えるなら「コピーの絵が空」が確定。(2) シェイプレイヤーを対象に同じ差し替え（シェイプは別の vtable なので、その表の 217/219/233 を差し替える）→ 描ければ仮説を支持。正しければ、「サブレイヤー i の絵」の要求にレイヤー全体の絵を返す箇所（`Rasterize2DGraph` 周辺）も差し替える必要がある。
### （11/23 22:10）試験 6: 仮説の確認 → **支持された**
- 環境変数: `RANDAMU_FILL_SPACING`（x の間隔、既定 60）、`RANDAMU_FILL_COL=1`（M4 の平行移動を列側へ）、`RANDAMU_FILL_M3ID=1`（M3 は単位行列のまま）、シーン側 `RANDAMU_TARGET_KIND=shape`（対象をシェイプの白い四角に）。ログは `spike/logs/probe_fill0`・`probe_fillshape`・`probe_shape0`・`probe_shapecol`・`probe_shapem3`。
- 平面（ラスター）・間隔 0 → **消える**。行列は無関係で、コピーの絵が空。
- シェイプ・間隔 0 → 元と同じ形で中央 248（50% の板 5 枚の理論値 ≈247）。**AE 自身の 3D 描画で、5 枚が別々の板として正しく合成された**。
- シェイプ・間隔 60（M3 と M4 の両方に平行移動）→ 遠近のついた板が 3 つ、台形にゆがみ、切れて見える（`probe_fillshape/f_after.png`）。
- 平行移動を M4 の列側へ → 1 枚分に見える（行側が AE 本来の形）。
- M3 は単位行列・M4 だけ平行移動 → 板が画面全体に広がる（射影がおかしい、`probe_shapem3/f_after.png`）。
- **分かったこと**: M3（2D）と M4（3D）の意味・座標系が分かっていない。テキストの本物の値を見るのが早い → `BEE_TextLayer::GetSubLayerRects`（vtable 233 のテキスト版）を逆コンパイルするか、文字単位 3D のテキストで差し替えずに 233 を包んでログに M3・M4・矩形を出し、文字ごとの値の作り方を真似る（テキストは BEE_TextLayer の別の vtable）。
### （11/23 夜）試験 7: **シェイプで任意の配置に成功**
- 規則（AV 版の `GetSubLayerXform2D` と試験から）: **コピー i の絵 = 元のレイヤーのうち M3(rect) の範囲の中身、その板を M4 で 3D に置く**（M4 は行ベクトル形式、4 行目が平行移動、レイヤー座標の単位）。テキストでは M3 が文字の位置、M4 が文字の 3D 変形。
- **M3 = 単位行列、M4 = 平行移動 → 正解**（`RANDAMU_FILL_M3ID=1`、`spike/logs/probe_shapem3b/f_after.png`）: 横 100・不透明度 50% の板 5 枚が 60 間隔に並び、重なり 40 の幅だけ 192、他は 128、全体の幅 302（理論 ≈299）。前の `probe_shapem3` が画面いっぱいに崩れたのは、前の実行の `RANDAMU_FILL_COL` が残っていたため（今はログに設定値を出す）。
- 環境変数は PowerShell の呼び出しをまたいで残ることがある。試験ごとに明示的に消すこと。
- テキストの文字ごとの M4 は TXT.dll の `TXT_GetCharacter3DTransformPlus` が作る（未解析、不要になった）。テキストの xform キャッシュは AV と別の構造（+96 等を読むと落ちる）。
### 試験 8: **ラスター（平面）でも成功**
- シェイプは `BEE_VectorLayer`。AV との違いで効くのは `Rasterize2DGraph`(235)。AV 版はキャッシュなしで `SubLayerRenderCount` を聞き、2 以上だとセット用の特別なノード `sub_180299BB0` で描く（セットのデータがないので空）。→ **キャッシュなし（shared_ptr が空）で聞かれたら元の答え（0）を返す**（`RANDAMU_FILL_NOCACHE_ORIG=1`）と、通常のノードでレイヤー全体の絵になり、平面でもシェイプと同じ正解の絵（`spike/logs/probe_solid2`、開き直しも同じ）。
- **今の「fill」の全条件**: 共有の AV vtable の 217→真、219→N（キャッシュありのときだけ）、233→矩形=元の矩形・M3=単位行列・M4=コピーの変形。`RANDAMU_PROBE_SETS=fill RANDAMU_FILL_M3ID=1 RANDAMU_FILL_NOCACHE_ORIG=1`。
### 試験 9: **対象にエフェクトが付いていても成功**
- エフェクトがあると、Artisan の判定 `sub_180385CD0`（BEE RVA 0x385CD0〜0x385DB0）が「AV 型（`IsLayerType(0)`）かつエフェクトあり」で真になり、コピーに分かれない（`probe_solidfx`、確認済み）。
- 対策: slot 79 `IsLayerType` を差し替え、**呼び元（`_ReturnAddress()`）がこの関数の範囲のときだけ、対象レイヤーに偽を返す**（`RANDAMU_FILL_FXBYPASS=1`）。→ 5 枚に分かれ、エフェクト（塗り：赤）も全コピーに入る（`spike/logs/probe_solidfx2/f_after.png`）。シーン側は `RANDAMU_EFFECT_ON_TARGET=1`。
- 注意: アドレスは今の AE 2025 の BEE.dll 固定。製品ではバイト列のパターン検索などで探す必要がある。
### 試験 10: **3D シーンで正解と画素単位で完全一致**（`spike/scenes/probe_3d.jsx`、`spike/logs/probe_3d`）
- シーン: クラシック 3D、床（X 回転 90）、交差する青い壁（Y 回転 90）、ライト（スポット、影なし）、カメラ。"probe" は Target 1 枚＋fill（固定の配置表 5 件、x/y/z ばらばら、`RANDAMU_FILL_TABLE=1`）、"ref" は本物の複製レイヤー 5 枚を同じ位置に。
- **差の最大 0（12 万画素すべて同じ）**。陰影・壁との交差・前後関係を含めて、AE 自身が描いたものと同一。
- 実行: `RANDAMU_FILL_TABLE=1 RANDAMU_FILL_FXBYPASS=1 RANDAMU_PROBE_SETS=fill RANDAMU_FILL_M3ID=1 RANDAMU_FILL_NOCACHE_ORIG=1`、`run_probe.ps1 -Jsx scenes\probe_3d.jsx -Name 3d`、比較はスクラッチパッドの `cmp.py`。
- **未解決**: スクリプトからライトの「影を落とす」を設定すると、見えないデスクトップの AE が止まる（`RANDAMU_3D_SHADOWS=1` で再現。ダイアログ待ちの可能性）。影の試験は別の手段で設定する必要がある。
- **試験 11: 回転も完全一致（差 0）**（`spike/logs/probe_3drot2`）。M4 = T(−アンカー)·Rz·Ry·Rx·T(アンカー＋オフセット)（行ベクトル、角度は度で、AE の Rotate X/Y/Z と同じ符号）。順序を X→Y→Z にすると 3 軸を組み合わせたコピーだけずれる（`probe_3drot`）。`RANDAMU_FILL_ROT=1 RANDAMU_FILL_ROTORDER=ZYX`。アンカーは矩形の中心（既定のアンカー）。
- **試験 12: 被写界深度（絞り 120）＋調整レイヤー（ガウスぼかし 4）＋モーションブラー（カメラを 1 秒で x+600）を全部入れても完全一致（差 0）**（`spike/logs/probe_3dx`、`RANDAMU_3D_EXTRA=dof,adj,mb`）。ただしコピー自身は動いていない（コピーの動きのブラーは、時間ごとに行列を変える段階で確かめる。hook は時間引数を今は無視）。
- **試験 13: 動くコピーのモーションブラーも一致（差の最大 1/255）**（`spike/logs/probe_3dmb3`、`RANDAMU_FILL_ANIM=1 RANDAMU_3D_TIME=0.5 RANDAMU_3D_EXTRA=mb`）。`GetSubLayerRects` の第 3 引数は時刻 `{int value; unsigned scale}`（レイヤー時間とみられる）で、AE はシャッター内の時刻（0.50104 s…）ごとに呼んでくる → 時間の関数として行列を返せばモーションブラーが正しく付く。
  - 注意: `probe_3d.jsx` のコンプの長さを 1/30 秒にしていた（`addComp` の引数の順を誤り）。2 秒に直した。0.5 秒で真っ白になったのはこのため。
- **影（未解決）**: ライトのオプション（「影を落とす」など、強度より後ろの項目）にスクリプトで触ると、見えないデスクトップの AE がモーダルのダイアログ（`#32770`、中身は AE 独自描画で読めない、入力欄 1 つ）で止まる。ライトの種類はスポット（4413）。プラグインからの `AEGP_SetStreamValue` も失敗（0）し、同様に止まる。CPU は増えない＝ダイアログ待ち。`WM_COMMAND IDOK` では進まない。→ 案: 利用者に影つきのシーンを手で作った .aep を用意してもらい、それを開いて試す。
- **試験 14: キャッシュ**（`spike/logs/probe_3dcache`、`RANDAMU_3D_CACHETEST=1 RANDAMU_FILL_FILE=<倍率のファイル>`）: hook が返す配置を外から変えただけでは、AE は古い絵を返す（`f_p2_nochange` = 倍率 1 の正解）。**対象レイヤーのエフェクトのパラメータ（スライダー）を変えると問い合わせ直して、新しい配置で正解と一致**（`f_p3_slider` = 倍率 0.5 の正解、差 0）。→ 製品では、配置をエフェクトのパラメータだけから決めれば、キャッシュは AE の仕組みのまま正しく働く。
- **試験 15: hook の中（描画スレッド）で、描画用の複製レイヤーからエフェクトのパラメータを AEGP で読める**（`spike/logs/probe_3dparam`、`RANDAMU_FILL_PARAM=1`）。`AEGP_EffectSuite4` でレイヤーのエフェクトを列挙→マッチ名で探す→`AEGP_GetNewEffectStreamByIndex`→`AEGP_GetNewStreamValue`（レイヤー時間、時刻は hook の第 3 引数）。SPBasicSuite は SEQUENCE_SETUP で保存したものを使う。止まりも落ちもなし。スライダー 0→倍率 1 の正解、5→倍率 0.5 の正解と差 0。→ **配置をエフェクトのパラメータだけから決める製品の形が成り立つ**（ID の対応表は、hook を入れる対象の判定にだけ使う。製品では「このエフェクトを持つレイヤーか」で判定できる）。
- **未確認（重要）**: ここまでの一致はすべて「エフェクトを付けた直後に SEQUENCE_SETUP で hook を入れ、同じ AE の中で `saveFrameToPng`」で得たもの。aerender（新しいプロセス）・MFR・読み込み直したプロジェクト・影・画像素材は未確認。
- **試験 16（11/24）: aerender（新しいプロセス、保存した .aep）で MFR オフ・オンとも完全一致（差 0）**。`spike/tools/run_3d_aerender.ps1 -Name 3daer`（シーン生成→`TARGET_ID` を jsx.log から取得→aerender→`r_probe`/`r_ref` の TIFF を比較）。hook は GLOBAL_SETUP で `GetProcAddress(BEE.dll, "??_7BEE_AVLayer@@6B@")` の表に入れる（`RANDAMU_FILL_GLOBAL=1 RANDAMU_FILL_LAYERID=<id>`）。slot 217 は 1 フレームで十数回、ID の判定は合計 0.04 ms。比較スクリプトは `RANDAMU_CMP_PY` で渡す。
- **試験 19（11/24）: アドレスの決め打ちをなくした版で、PNG 素材＋影のシーンが aerender の MFR オフ・オンとも完全一致**（`spike/logs/probe_3dshape`、`run_3d_aerender.ps1 -Name 3dshape [-SkipScene]`）。
  - 仮想関数表の枠は、公開関数（`?IsLayerType@BEE_AVLayer@@UEBAEH@Z` など 4 つ）のアドレスと表を照合して実行時に求める（`FindSlot`）。見つからなければ hook を入れない。
  - エフェクト判定のかわし方は、`IsLayerType(0)` の戻り先の命令の並びで判定する（`IsFlattenCheckSite`）: `test al,al; jnz; call BEE_BetaFeature_AreExpressionEffectsAlwaysAllowedEnabled; test; jz; mov rax,[rbx]; mov rcx,rbx; call [rax+d]; mov rdx,[rax]; mov rcx,rax; call [rdx+d]; test eax,eax; jnz; mov rax,[rbx]; mov rcx,rbx; call [rax+d]; test al,al; jnz; mov rcx,rbx; call (GetSelectedTrackMatte)`。前半だけだと**音声合成の判定 `sub_18043E350`（`HasAudio` から）にも合致してしまった**ので、後半まで照合する。合致は BEE+0x385CF9 だけになった。`RANDAMU_FILL_FXBYPASS=addr` で旧来の固定アドレス方式。
- **試験 22: 枚数と重さ**。1 回目（`perf.out`）は**無効**: シーンを使い回したため、N=1000・5000 では AE のディスクキャッシュから N=5 の絵が返り、hook は 1 度も呼ばれなかった（hook の配置はキャッシュの鍵に入らない。製品ではパラメータから決めるので問題ない）。2 回目の結果（`perf2.out`）: **N=7 → 72.7/48.0 s、N=1000 → 54.4/54.5 s（MFR オフ/オン、起動込み。1000 枚は N=7 と差がない）、N=5000 → 300 秒で打ち切り（オフ・オンとも）**。hook には N=5000 で問い合わせが来ている。3 回目の結果（`perf3.out`）: **N=2500 → 74.2 s（起動を除くと約 20 秒）**。`FillRects` は 1 回 67 ms（デバッグビルド）で全体のごく一部。→ 時間の大半は AE の 3D 描画側。4 回目の結果（`perf4.out`）: **N=5000 → 512 s（起動を除くと約 455 秒）**。2500 枚の約 20 秒から急増（枚数 2 倍で 20 倍超）。hook 側は `FillRects` 1 回 18 ms、slot 217 は 1 万回で ID 照合合計 5 ms → **時間はほぼすべて AE のクラシック 3D 描画側**（板どうしの交差の分割などで枚数に対して急に重くなるとみられる。同じ枚数の本物のレイヤーでの比較は未実施）。→ 設計の相談事項: 枚数の目安、重さの対策（交差しにくい配置、品質設定など）。2 回目の方法: N ごとにシーンを作り直し、Target に「スライダー制御 = N」を付けて鍵を変える（`RANDAMU_3D_NOREF=1`、`-Name 3dperf<N>`、N=7・1000・5000、出力はスクラッチパッドの `perf2.out`）。時間は AE の起動（約 55 秒）込みで、probe と ref の 2 フレーム分。
- **試験 23 の 1 回目: 失敗（差の最大 86）**。slot 226 `GetSubLayerOpacity` はクラシック 3D から 1 度も呼ばれない（`spike/logs/probe_3dopacity`）。
- **試験 23 の 2 回目の結果（11/24 05:53）: 成功。aerender の MFR オフ・オンとも正解との差 0**（`spike/logs/probe_3dopacity2`、出力はスクラッチパッド `a66d4780-…\scratchpad\opacity2.out`）。ログに `FILL artisan opacity sub=0..4 base=1.0 x 0.30/0.84/0.61/0.38/0.92` が出た（IAT の差し替えが効いた）。環境変数: `RANDAMU_FILL_TABLE=1 FILL_FXBYPASS=1 3D_SHADOWS=1 FILL_ROT=1 FILL_ROTORDER=ZYX FILL_NOCACHE_ORIG=1 PROBE_SETS=fill FILL_M3ID=1 FILL_OPACITY=1 3D_IMAGE=spike\assets\probe_img.png`、`RANDAMU_CMP_PY`=旧スクラッチパッド `1c09b12a-…\scratchpad\cmp.py`。→ コピーごとの不透明度は slot 226 ではなく `BEE_GetArtisanLayerOpacity`（AEGPDriver の IAT）で付ける。**次は「次の 1 手」の 4（設計の相談、`docs/decisions-2026-11-24.md`）。**
- （旧）**試験 23 の 2 回目（ビルド済み・未実行）**: AEGPDriver.aex の取り込み表（IAT）で `BEE_GetArtisanLayerOpacity` を `HookArtOpacity` に差し替え（`PatchImport`）、元の値 × コピーごとの値を返す。コピーの番号はトレースから取る: `v = *(char**)(trace+16)`、レイヤー = `*(void**)(v-48)`、サブレイヤー番号 = `*(int*)(v-40)`（`BEE_GetArtisanLayerFromTrace` の逆コンパイルで確認）。この関数は BEE 内からは直接呼ばれず、AEGPDriver の `sub_18001A150`（Artisan に層の不透明度を渡す AEGP の関数）から呼ばれる。実行: 下の 1 回目と同じ環境変数で `run_3d_aerender.ps1 -Name 3dopacity2`。ログの `FILL artisan opacity` が出るかと、正解との差を見る。
- **試験 23: コピーごとの不透明度**（1 回目の方法）。slot 226 `GetSubLayerOpacity`（`double (int i, shared_ptr<XformCache> const&)`、公開名で照合）を差し替え、i 番目のコピーに 0.3＋0.7×((i×37) mod 10)/9 を返す（`RANDAMU_FILL_OPACITY=1`）。ref の複製にも同じ不透明度。5 枚の表、PNG 素材、影あり。`run_3d_aerender.ps1 -Name 3dopacity`、出力はスクラッチパッドの `opacity.out`。
- 利用者向けの判断材料: `docs/decisions-2026-11-24.md`（方式の確定、回転の中心、散らす座標の基準、枚数と重さ、ばらつきの種類）。
- **試験 21 の結果: 成功**。saveFrameToPng・aerender（MFR オフ・オン）のすべてで正解（本物のレイヤー 100 枚）との差が最大 1/255（`spike/logs/probe_3drand100`）。大きい確保の経路も問題なし。slot 217 は 100 回で ID 照合の合計 0.01 ms。
- **試験 21: 乱数で 100 枚**。`RANDAMU_FILL_RANDOM=100`（プラグインの `RandomXform` と jsx が同じ Park–Miller 法（種 12345）で位置 ±150/±100/±300・回転 ±60° を作る）。PNG 素材、影なし。`run_3d_aerender.ps1 -Name 3drand100`、出力はスクラッチパッドの `3drand.out`。100 枚だと M4 の一覧が 12800 バイト → `VecReserve` の 4096 以上の整列確保の経路を初めて通る。
- **試験 20 の結果: 成功**。2 枚の対象（ID 53・54）が登録され、それぞれ自分のスライダー（0→倍率 1、5→0.5）を描画スレッドで読み、saveFrameToPng と aerender（MFR オフ・オン）のすべてで正解と完全一致（`spike/logs/probe_3dmulti`）。ID の照合は 10 回で合計 0.003 ms。
- **試験 20: 目印のエフェクトで対象を見分ける＋複数レイヤー**。
  - 1 回目: hook の判定 `IsMine` の中で AEGP のエフェクト列挙を呼んだら、シーン生成中（エフェクト適用の直後）に AE が止まった（UI スレッドの BEE の仮想関数の中から AEGP を呼ぶのは危険）。
  - 2 回目（今）: hook の中では**登録済みのレイヤー ID の一覧と照合するだけ**。一覧は SEQUENCE_SETUP でコンプのレイヤーを AEGP で調べて登録（試作）、aerender では `RANDAMU_FILL_LAYERIDS=53,54`（`run_3d_aerender.ps1` が jsx.log の全 `TARGET_ID` から渡す）。製品では自分のエフェクトが各コマンドで自分のレイヤー ID を登録する形になる。
  - `RANDAMU_FILL_BYEFFECT=1`、`RANDAMU_FILL_PARAM=1`（倍率をそのスライダーから）、シーン `RANDAMU_3D_MULTI=1`（probe に Target＝スライダー 0→倍率 1 と Target2＝スライダー 5→倍率 0.5、P0 から (80,40,120) ずらす。ref には両方のコピー）。影・PNG 素材も入れて、`run_3d_aerender.ps1 -Name 3dmulti` で saveFrameToPng と aerender（MFR オフ・オン）を比較。出力はスクラッチパッドの `3dmulti.out`。
- **次の 1 手（順に）**:
  1. ~~aerender で通す~~ → 済（試験 16）。旧メモ: hook を GLOBAL_SETUP で入れる（表は `GetProcAddress(BEE.dll, "??_7BEE_AVLayer@@6B@")` でインスタンスなしで取れる見込み）。対象の判定は、試作ではレイヤー ID を環境変数で渡す。`probe_3d.jsx` の最後でプロジェクトを保存し、レンダーキューに probe と ref を足す（`make_s1.jsx` の `addRQ` と同じく、TIFF のテンプレートを `RNDM_TIFF_A` として保存・適用）。aerender で描いて比べる。MFR のオン・オフの両方。slot 217 が全 AV レイヤーに何回呼ばれるかと、判定の時間も測る。
  2. ~~PNG の画像素材レイヤー~~ → **済（試験 17）: 半透明・模様つきの PNG（120×100、`spike/assets/probe_img.png`）で、saveFrameToPng と aerender の MFR オフ・オンのすべてで完全一致**（`spike/logs/probe_3dimg`、シーン側 `RANDAMU_3D_IMAGE=<png のパス>`）。
  3. ~~影~~ → **済（試験 18）: 影も完全一致（差 0）**（`spike/logs/probe_3dsh5`、`RANDAMU_3D_SHADOWS=1`）。床にコピーそれぞれの影が落ちる。止まっていた原因は、スクリプトで**ライトの「カラー」（`ADBE Light Color`、オプションの 6 番目）の値を読むと、見えないデスクトップの AE がダイアログ待ちになる**こと（内部名での検索もこれに触れて止まるとみられる）。回避: 7 番目以降を番号で回り、`matchName` だけを見て設定する。ライトの「影を落とす」の内部名は `ADBE Casts Shadows`（12 番目。`ADBE Light Casts Shadows` ではない）。旧メモ: ライトのオプションの項目ごとに、内部名を値を読む前にログへ出し、どの項目に触ると止まるかを特定する（1 回だけ）。回避できなければ、影つきの .aep を利用者に用意してもらう。
  4. 設計の相談に回すこと（設計書 5.4 節）: 回転の軸を矩形の中心にするかレイヤーの実際のアンカーにするか。対象レイヤー自身に回転・拡大縮小・親がある場合、配置をレイヤーの座標で行うかワールドの座標で行うか。
- その先: 製品の設計（hook を入れる時機と外す時機、複数インスタンス・複数レイヤー、MFR での同時呼び出し、AE の版ごとのアドレス探し、`IsLayerType` の戻りアドレス判定の範囲の探し方）、設計書の書き直しと利用者の承認。
- （旧メモ）ラスターのレイヤー（製品の本命）で使うには、「サブレイヤー i の絵」を作る処理（`Rasterize2DGraph`(235) 周辺）でレイヤー全体の絵を返させる必要がある。
- 未解決の製品上の問題（後で）: (1) 対象レイヤーにエフェクトが付いていると、Artisan の判定 `sub_180385CD0` でサブレイヤーとして描かれない（製品ではエフェクトを付けるので、ここも回避が必要）。(2) 差し替えたコピーの配置をキャッシュの鍵に反映する方法。(3) 共有 vtable の書き換えは全 AV レイヤーに効く（ID で絞るが、AE の版が変わると枠の位置が変わる）。

## 次の 1 手（11/23 17:55 更新。5 時間枠 90% で区切り、リセット 19:30）
0. **レイヤーセットでコピーを別々の変形で描かせる**。読む順: `BEE_AVLayer::SharedXformTimeCacheFactory` → `sub_1804B3BD0`（`sub_180284090` が呼ぶ。`BEE_LayerXformCache` を作り、+120/+128 の vector が 1 件 72 バイトのコピー一覧）→ そこから `BEE_LayerSet`／`ADBE Set Property`／止まり点がどう使われるか。書き出しは `spike/ida/ida_dump2.py BEE.dll.i64 <出力先> <正規表現>`（関数単体なら idalib で `ida_hexrays.decompile(ea)` をファイルへ）。
   - 次の試験: プラグインから Position を参照するセットのプロパティ（`AddProperty`／`BEE_CmdSetLayerSetPropStreamRef`）と止まり点 2 つ（`AddStop`）を作り、コピーが別々の位置に描かれるかを見る。中央の画素で判定（重なれば 1−0.5^n）。
   - 道具: `spike/tools/run_probe.ps1 -Jsx scenes\probe_sets2.jsx -Name <名前>`（`RANDAMU_PROBE_SETS`・`RANDAMU_SPIKE_LOG` を先に設定）。AE は毎回「閉じない → 90 秒後に強制終了」になるが、結果には影響なし。
   - コピー数の `setValue` はスクリプトから行うと AE が止まる（試験 1）。コピー数はプラグインから変えるか、止まる原因を調べる。
   - 利用者に見せる前に分かっていること: セットを作るとレイヤーのコラップスのスイッチが立ち、タイムラインに「セット」のグループが見える。
- （済）`BEE.dll` の全体解析: `spike/ida/BEE.dll.i64`（`.i64` は消さない・止めない）。
1. `BEE.dll` を全体の自動解析つきで開き直して `.i64` を保存する（約 15 分。MCP の `idb_open` か、`ida_dump.py` を `run_auto_analysis=True` に変えて実行）。その `.i64` から `ida_dump.py` で、サブレイヤー系の関数の本体、`BEE_AVLayer` の仮想関数表（`vt_*.txt`）、`HasSubLayerXform`・`SubLayerRenderCount` を呼ぶ側（クラシック 3D の描画処理）を書き出して読む。
2. 確かめること: (a) AV レイヤーでサブレイヤーが別々に描かれる条件（`HasSubLayerXform`・`SubLayersRenderSeparately` のフラグ・`SubLayerRenderCount`）、(b) 各サブレイヤーのテクスチャ範囲（`GetSubLayerRects`）と変形（`GetSubLayerLocalMatrix3D`）の決まり方、(c) `AEGP_LayerH` から `BEE_AVLayer*` を得る方法（AEGP の関数の実装を読む）、(d) 仮想関数表を書き換えて自分のレイヤーだけ答えを変える方法（ほかのレイヤーには元の関数を呼ぶ）。
3. 実験: 平面レイヤーに対してサブレイヤーを 2 個にし、別々の変形で 2 枚描かれるか → ライト・影・被写界深度・モーションブラーが効くか → 正解（本物の 3D レイヤー 2 枚）と RGB で一致するか。
4. 結果をもとに、(A) 隠しコンプ、(B) BEE への差し込み、(C) サブレイヤー方式 を利用者に比べて示す。
（以下は A' 時代のメモ）
（11/23 夕方: 実験はここで区切り、**設計の見直しに戻る**。設計書 5.4 節の未決事項を利用者と決める: エフェクトより上のレイヤー、2D レイヤー、平面でないもの、コピーの向きやばらつき、くり抜き方式の半透明での近似、対象より上の調整レイヤー・描画モード・レイヤースタイル、このエフェクト同士の前後。そのあと設計書の本文（4 節の他レイヤーの取得など）を今の方式に書き直して承認をもらい、writing-plans へ）
（以下は以前の一覧）
0. ~~`make_s1.jsx` の `addRQ` を直す。~~ → 完了。案: `setSettings` に渡す前に、設定から出力先の項目（"Output File Info" など）を消す。または `setSettings` のあとで `om.file` を設定し直して、`jsx.log` に `om.file` を出して確かめる。または、テンプレートを名前で 1 回だけ選び、以降は `rq.outputModule(1)` の `applyTemplate` を使わずに、レンダーキューの項目を `duplicate()` して `comp` を差し替えられないか試す。直したら `run_s1.ps1`（シーンの生成あり）で通す。
   - 合格の条件: マットの 4 種がすべて `render=aegp` で合格し、v0〜v3・t_mask・t_effect・t_collapse が崩れないこと。
   - まず `plugin.log` の `S6 matte:` で、マットのレイヤー（ビデオがオフ）が AEGP で画素を返すか（opaque px）を確かめる。空なら、そこから考え直す。
1. ~~はみ出すエフェクトの範囲を広げる方法~~ → 解決（S5c）。
2. ~~トラックマット（案 A）の実験~~ → 合格（7.4 節）: `AEGP_GetTrackMatteLayer` でマットのレイヤーを取り、それも AEGP で描画し、マットの種類（アルファ／反転アルファ／ルミナンス／反転ルミナンス）に従ってコンプ座標で掛け合わせる。t_matte で正解と形が一致するか。
3. ~~（任意）S5b~~ → 合格（7.5 節）: キャッシュの鍵に変換・カメラ・レイヤー一覧を混ぜる案（位置の取得も AEGP に移すかの判断）。
4. S3（スロットの自動割り当てとエクスプレッションの自動設定）: 位置と深度の取得にスロットを使い続けるかは S5・S5b の結果次第。絶対条件 1 のため、割り当てと設定はエフェクト自身が自動で行う必要がある。
5. ~~S4~~ → 合格（7.5 節）: 他レイヤーの移動・不透明度・マスク・エフェクト・プリコンプの中身の変更で再描画されるか（AEGP 方式ではマスクの変更も対象に戻す）。
6. 結果を設計書に反映し、5.4 節の未決事項を利用者と決めて、承認をもらう。

## 再開時の注意
- 実験コードの現状: `spike/RandamuSpike/RandamuSpike.cpp` は S5 用に AEGP の描画を足した版（`RANDAMU_S5_SITE`＝pre|smart、`RANDAMU_S5_SALT` で切り替え）。ビルドは `spike/tools/build.ps1`、実行は `spike/tools/run_s1.ps1`（`-SkipScene` でシーン生成を省略、`-Mfr ON|OFF`）。
- S4 の試験: `spike/tools/run_s4.ps1`（`s1.aep` が必要。出力は `spike/logs/s4`）。S5b の切り替えは環境変数 `RANDAMU_S5B`（0／compare／1）、対照は `RANDAMU_S4_NOGUID`・`RANDAMU_S5B_NOMIX`。
- 実行前に AfterFX / aerender が動いていないことを確かめる（利用者の AE を巻き込まない）。
- エフェクトが落ちると `Effect Crashed.txt` で次の起動時にダイアログが出る。`run_s1.ps1` は起動前に `Repair-AeCrashState` で防ぐ。「AfterFX did not close」と出たら、まずここを疑う。
