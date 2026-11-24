# AE プラグインの自動テスト環境（Windows）

After Effects のエフェクトプラグインを、ビルドからレンダー結果の判定まで自動で確かめるための環境と、つまずいた点をまとめる。
randamu のために作ったが、ほかのプラグインのプロジェクトでも同じ方式を使う前提で書く。

確認した環境は Windows 10、After Effects 2025（25.6、日本語版）、AE SDK 25.6、Visual Studio 2022 Build Tools。
経緯の詳細は `docs/experiments/2026-11-22-spike-log.md` の 3〜5 節と 8 節にある。

## 1. 全体の流れ

```
build.ps1        MSBuild で .aex をビルド → AE が読む場所に出力
   ↓
run_*.ps1
 1. 前回の強制終了の印を消す        ae_guard.ps1  Repair-AeCrashState
 2. 見えないデスクトップで AE を起動  ae_bg.ps1     Start-AeHidden  (AfterFX.exe -r シーン生成.jsx)
 3. jsx がテスト用プロジェクトを作って保存し、ログに完了行を書く
 4. 完了行を見たら AE を正常に閉じる  ae_bg.ps1     Stop-AeHidden
 5. aerender でプロジェクトをレンダー（UI なし）
 6. プラグインのログと出力画像を Python で判定
```

利用者がテスト中も同じ PC で作業していることを前提にしている。AE の画面は利用者の画面に一度も出ず、フォーカスも奪わない。画面全体のスクリーンショットも撮らない。

## 2. 部品

| ファイル | 役割 | ほかのプロジェクトへの持ち出し |
|---|---|---|
| `spike/tools/ae_bg.ps1` | AE を見えないデスクトップで起動・ウィンドウ列挙・ウィンドウ単体の撮影・正常終了 | そのまま使える |
| `spike/tools/ae_guard.ps1` | 強制終了の印（レジストリ）を起動前に消す | そのまま使える |
| `spike/tools/build.ps1` | VS の開発者シェルに入って MSBuild | プロジェクト名と出力先を書き換える |
| `spike/tools/run_s1.ps1` | 上の流れをつなぐ実行スクリプト | 手本として写し、jsx と判定を差し替える |
| `spike/scenes/make_s1.jsx` | テスト用プロジェクトを作る ExtendScript | 手本 |
| `spike/tools/check_s1.py` | 画像の判定（NumPy + Pillow） | 手本 |

`ae_bg.ps1` と `ae_guard.ps1` は pwsh 7 から dot-source して使う。

```powershell
. .\ae_bg.ps1; . .\ae_guard.ps1
$null = Repair-AeCrashState
$p = Start-AeHidden -Exe "$sf\AfterFX.exe" -Arguments "-r $jsx"   # $jsx は空白なし・引用符なし
# … jsx のログに完了行が出るのを待つ …
if (-not (Stop-AeHidden -Process $p)) { $null = Repair-AeCrashState }   # false = 最後は強制終了した
```

## 3. 一度だけ行う準備

1. **プラグインの置き場所**: `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore` は管理者権限がないと書き込めない。管理者権限で一度だけジャンクションを作り、以後はビルドの出力先をジャンクションの先にする。
   ```
   mklink /J "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\<名前>_dev" "<プロジェクト>\dev_plugins"
   ```
   読み込まれたかどうかは `%APPDATA%\Adobe\After Effects\25.6\Plugin Loading.log` で確かめる。
2. **スクリプトのファイル書き込み許可**: AE の環境設定で「スクリプトによるファイルへの書き込みとネットワークへのアクセスを許可」をオンにする（`Pref_SCRIPTING_FILE_NETWORK_SECURITY = "1"`）。jsx がログを書くのに要る。
3. **Python**: PATH の python が使えないことがあるので、判定用に `uv` で venv を作る（`.venv\Scripts\python.exe` を直接呼ぶ）。

## 4. 気を付けること・つまずいたこと

### 4.1 AE をスクリプトで動かす

| 症状 | 原因 | 対処 |
|---|---|---|
| `AfterFX.exe -r "パス"` で何も起きない | AfterFX.exe は引数の引用符を処理しない。引用符で囲むと、空白のないパスでも何も実行されない | jsx を `%TEMP%` など空白のない場所にコピーし、引用符なしで渡す |
| 空白を含むパスの jsx が動かない | 同上（引用符で囲めない） | 同上 |
| `AfterFX.com -noui -r` でスクリプトが動かない | この PC では `-noui` だと `-r` が実行されなかった | UI ありの `AfterFX.exe -r` を見えないデスクトップで動かす |
| `app.quit()` で終わらないことがある | ホーム画面が残る | jsx はログに完了行を書くだけにし、終了は実行側（`Stop-AeHidden`）で行う |
| すでに AE が起動していると、自分の AE が使われない | `-r` は起動中の AE にスクリプトを渡す | 実行前に AfterFX の有無を確かめ、動いていたら止める（利用者の AE を巻き込まない） |
| jsx に値を渡したい | ― | 環境変数で渡し、jsx では `$.getenv()` で読む。見えないデスクトップで起動しても引き継がれる |
| 出力モジュールに PNG がない | 日本語版のテンプレートに PNG がない | 「TIFF シーケンス (アルファ付き)」を使う。テンプレート名は言語によって違う |
| jsx の書き方の誤りで何も起きない | エラーがどこにも出ない | 1 行目でログを書き、`try/catch` で例外もログに書く |
| レンダーキューの項目が増えると `applyTemplate` が「TIFF ????? は有効なテンプレート名ではありません」で失敗する | `om.templates` が返す名前が、途中から文字化けすることがある（15 個目で発生） | テンプレート名は最初に一度だけ取得して使い回す |
| aerender が止まったまま戻らない（描画中の AEGP 呼び出しのデッドロックなど） | 待ち時間の上限がない | `run_s1.ps1` は上限（既定 600 秒）を過ぎたら aerender を止める |
| 別のコンプなのにプラグインが呼ばれない | 同じビルドで、AE から見て入力が同じなら、コンプをまたいでキャッシュが使われる | 「呼ばれなかった」こと自体が、AE が入力を同じと見なした証拠になる。判定ではコンプごとの呼び出しを確かめる |

### 4.2 クラッシュ修復ダイアログ

- AE を強制終了すると、次の起動で「クラッシュ修復オプション」が出る。モーダルなので、`-r` のスクリプトはその後ろで止まる。
- AE はクラッシュを次の値で判定している（レジストリの差分を取って特定した）。
  - `HKCU\Software\Adobe\After Effects\AppStates\<セッションの GUID>` の `AppState`
  - 起動中は 3、正常に終了すると 1001 になる。強制終了すると 3 のまま残る。
  - 同じキーの `ProcessID` にそのセッションの pid が入っている。
- `Repair-AeCrashState` は、プロセスがもう存在しないのに `AppState≠1001` の記録を 1001 に書き換える。生きている AE の記録には触らない。
- 使わなかった方法:
  - ダイアログの文字を OCR で読んで「続行」を押す方式は、誤読が多く不安定なのでやめた。
  - `HKCU\Software\Adobe\After Effects\25.6` の `ForceQuitOccured` は、正常に終了したあとも 1 のままで、判定に使えない。
  - `Debug Database.txt` の `AE.DebugShowPreviousCrashWarning` は、利用者のふだんの AE の動作も変えてしまうので使っていない。
- いちばんの対策は、強制終了しないこと（`Stop-AeHidden` で正常に閉じる）。

### 4.2.1 エフェクトが原因のクラッシュのダイアログ

- AE（aerender のホストを含む）がエフェクトの中で落ちると、`%APPDATA%\Adobe\After Effects\<版>\Effect Crashed.txt` が書かれる。UTF-16 ビッグエンディアン（BOM なし）で、エフェクトの照合名と表示名が入っている。
- 次の UI の起動で AE がこれを読んで消し、「『<エフェクト名>』が原因で After Effects が終了した可能性があります」（タイトルが空の `#32770`、492×295）を出す。モーダルなので `-r` のスクリプトが止まる。
- 記録は 1 回出すと消える。出たあとで探しても見つからないので、場所はクラッシュ直後の状態との差分で特定した。
- `Remove-AeEffectCrashRecord`（`Repair-AeCrashState` から呼ぶ）が、起動前に**自分のエフェクトの名前を含む記録だけ**を消す。ほかのプラグインの記録は利用者のものなので残す。照合する名前は `$script:AeOwnEffectMatchNames` に書く。手で作った見本のファイルでは動作を確かめたが、実物のクラッシュ記録ではまだ試していない（未検証）。
- aerender は、ホストの AE が落ちても終了コード 0 を返すことがある。出力の「Unable to receive」を見て判定する。

### 4.3 見えないデスクトップ

- 仕組み: `CreateDesktop("randamu_bg")` で作ったデスクトップを、`CreateProcess` の `STARTUPINFO.lpDesktop = "WinSta0\randamu_bg"` で指定して AE を起動する。デスクトップのハンドルは、起動した側のプロセスが生きている間は保たれる。
- ウィンドウの列挙は `EnumWindows` では見えない。`EnumDesktopWindows` を使う。
- 撮影は `PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT)` で、AE のウィンドウだけを撮る。画面からコピーする方式（`CopyFromScreen`）は利用者の画面を写すので使わない。
- `PostMessage` でキーやメッセージを送れる（デスクトップが違っても届く）。
- AE を閉じるとき:
  - ホーム画面（大きな `DroverLord - Window Class` のウィンドウ）が出ていると、`WM_CLOSE` が無視される。先にホーム画面へ Escape を送る。
  - 読み込み中も `WM_CLOSE` が無視される。効くまで繰り返す。
  - メインウィンドウのクラス名は `AE_CApplication_25.6`（バージョンで変わる）。`Process.MainWindowHandle` は 0 になり、`CloseMainWindow()` は効かない。
- AE のダイアログはタイトルが空の `#32770` で、中身は独自描画（DroverLord）。UI Automation では文字もボタンも取れない。起動画面（スプラッシュ）も、タイトルが空の `#32770` が 2 枚（766×516 と 296×450）なので、ダイアログと取り違えないこと。
- 起動からメインウィンドウまで約 70〜90 秒かかる。

### 4.4 キャッシュ（プラグインを作り直したのに結果が変わらない）

- AE と aerender のディスクキャッシュは、プロセスをまたいで残る。パラメータが同じなら、プラグインのバイナリを変えても古い結果を返し、プラグインの描画処理が一度も呼ばれない。
- 対策: `PF_OutFlag2_I_MIX_GUID_DEPENDENCIES` を立て（PiPL の OutFlags_2 も合わせる）、PreRender でビルドごとの識別子（`__DATE__ " " __TIME__` など）を `GuidMixInPtr` に混ぜる。
- 実行スクリプトでは、プラグインのログに描画処理の呼び出し行が 1 回以上あるかを必ず確かめる。0 回ならキャッシュの結果を見ている。

### 4.5 ビルド

| 症状 | 対処 |
|---|---|
| PiPL の変換（`PiPLTool`）が失敗する | 空白を含むパスを扱えない。作業フォルダに `pushd` してファイル名だけを渡す |
| `std::min` などがエラー | `NOMINMAX` を定義する |
| C4819 警告 | `/utf-8` を指定する |
| `GlobalSetup` のフラグの不一致の警告 | `out_flags`/`out_flags2` と PiPL の値を一致させる |
| PATH の cmake が別物 | 使わない。VS の開発者シェル（`Launch-VsDevShell.ps1`）に入って MSBuild を使う |

### 4.6 判定

- 色の値はプロジェクトのカラーマネジメントと線形合成の影響を受けるので、直接比べない。
- 「エフェクトを無効にした同じコンプ」（＝ AE 自身が描いた正解）を一緒にレンダーし、形（シルエットの IoU）や位置で比べる。
- 回転・拡大したレイヤーでは、AE の再サンプリングで縁が 1px ほどにじむ。判定に ±1px の幅を持たせる。
- aerender の終了コードだけに頼らない。ログの完了行と出力ファイルを見る。

### 4.7 速さの測り方（2026-11-24 に分かったこと）

| 誤り・症状 | 原因 | 正しい測り方 |
|---|---|---|
| aerender の「合計時間 ÷ フレーム数」が、利用者がタイムラインの「レンダリング時間」で見る値の 10 分の 1 程度になった | aerender は既定でマルチフレームレンダリング（MFR）をし、複数のフレームを同時に描く。割り算は 1 フレームの時間にならない | **`-mfr OFF`** で描き、aerender のログの「合計経過時間」（起動を除く）÷ フレーム数を使う（`tests/ae/run_user.ps1 -Mfr OFF`）。ビューアー経路の計測（下）と一致することを確かめた（142 ms と 141 ms） |
| aerender の起動の時間が計測の大半になる | AE の起動に 60〜190 秒かかり、ばらつく | 壁時計ではなく、ログの「合計経過時間」を読む。ログは CP932（日本語）。読み取りは `FileShare.ReadWrite` で開く（aerender の子プロセスがまだ掴んでいることがある） |
| aerender が「No render settings template was found」で 1 フレームも描かない | `-RStemplate "Best Settings"` は英語名。日本語版では名前が違う | テンプレートを指定しない（既定の設定） |
| `saveFrameToPng` で時間を測ると、本物のレイヤー 10 枚が 0.4 ms などの不自然な値になる | `saveFrameToPng` は書き出しの完了を待たずに戻る | 壁時計で測らない。プラグインの中で測る（下） |
| 時間を 1 フレームずつ進めても、2 フレーム目以降が描かれない | 動きのないシーンでは、AE は同じ内容のフレームを見分けて最初の結果を使い回す | 比較用のコンプのカメラに `value + [time * 40, 0, 0]` のエクスプレッションを付けて、フレームごとに内容を変える。素材のキャッシュは残るので、実際の使い方に近い。`app.purge(ALL_CACHES)` は素材の作り直しまで含んで大きく出るので使わない |
| 条件を変えたのに描かれない（例: サンプリングをバイリニアに） | その条件で絵が変わらないと、ほかのコンプの結果が使い回される | 「描かれなかった」こと自体を結果として読む。必要なら別の試験（`tests/ae/sampling.jsx`）で確かめる |

**プラグインの中での計測**（`RANDAMU_TIMING=1` と `RANDAMU_LOG`、`src/hook/timing.*`）: コンプと時刻ごとに、そのフレームでの DupliX の処理の最初から最後のコピーの描画（不透明度のフック）までの時間と、プラグインの処理の時間をログに 1 行ずつ出す（`timing: comp ... span=... in-plugin=...`）。コピーの描画が 0 回の行は、AE がコンプの全フレームについて前処理だけを呼んだもの（コンプを開いたときに約 1600 回）なので、集計から外す。

**起動したままの AE にスクリプトを送る**（`tests/ae/run_preview.ps1`）:
- AE を見えないデスクトップで普通に起動しておき（aep を引数に渡す）、あとから 2 つ目の `AfterFX.exe -r <jsx>` を、これも見えないデスクトップで起動すると、スクリプトは起動中の AE で実行され、AE は終わらない（`-r` で起動した AE は、スクリプトの終了とともに閉じる）。
- **起動が終わる前に送ると、2 つの起動が干渉して AE が起動画面（「MediaCore を初期化しています」）のまま止まる**。メインウィンドウ（クラス `AE_CApplication_25.6`、タイトルあり）が見えるまで待ってから送る。
- 送ったスクリプトの中で `app.scheduleTask` を使えば、AE が操作待ちの間に処理を続けられる（ビューアーの描画と同じ経路で 1 フレームずつ進める）。
- 試行で残った AE を片付けるときは、`Get-CimInstance Win32_Process` で起動の時刻と引数を見て、自分が起動したものだけを止める。

### 4.8 利用者のファイルを壊さない

- **AE は、試験の後片付けで閉じるとき、変更のあるプロジェクトを確認なしで保存する**（ダイアログを抑止しているため）。2026-11-24 に、利用者の aep を直接開いて変更したため上書きしてしまい、変更を逆にたどって復元した（`tests/ae/restore_user.jsx`）。
- 利用者のファイルは必ず `build/` の下にコピーしてから開く（`run_user.ps1`・`run_preview.ps1` はそうしている）。保存は別名（`app.project.save(new File(...))`）。最初に取ったコピーは、作業が終わるまで消さない。
- 利用者のファイルは `tests/ae/user/`（Git の管理外。素材のパスなど私的な情報を含む）。

### 4.9 プラグインの試験で踏んだ AE の内部確認

| 症状 | 原因 | 対処 |
|---|---|---|
| 前処理は呼ばれるのに描画が始まらず、PNG も TIFF もできない。aerender のログに「内部確認に失敗しました。{I_MIX_GUID_DEPENDENCIES effect missing call to GuidMixInPtr during SMART_PRE_RENDER}」 | `PF_OutFlag2_I_MIX_GUID_DEPENDENCIES` を宣言したエフェクトが、前処理の別の分岐で `GuidMixInPtr` を呼ばなかった | 前処理のすべての分岐で `GuidMixInPtr` を呼ぶ |
| 同じく描画が始まらない。「結果の長方形は、PF_Cmd_SMART_PRE_RENDER の要求の長方形を越えることはできません」 | 前処理で `result_rect` を要求より大きくした | `result_rect` は要求との共通部分、全体の大きさは `max_result_rect` に入れる。AE は `max_result_rect` を見て、次から必要な範囲を要求してくる |
| UI の `saveFrameToPng` では何も起きず、ログにも出ない | 上の内部確認の失敗は UI では表に出ない | aerender で同じシーンを描いて、ログの WARNING 行を見る |
| GPU の経路が一度も使われない（`PF_Cmd_SMART_RENDER_GPU` が来ない） | `out_flags2` に GPU のビットを立てても、PiPL の `AE_Effect_Global_OutFlags_2` に入っていなかった | PiPL にも `SUPPORTS_GPU_RENDER_F32`（1<<25）と `SUPPORTS_DIRECTX_RENDERING`（1<<29）を入れる |
| パラメータの表示が 255 倍になる（位置の範囲 200 が 51000 と表示） | `PF_ValueDisplayFlag_PIXEL` は色の値（0〜1 を 0〜255 で見せる）用 | 位置などには `PF_ValueDisplayFlag_NONE` |

## 5. ほかのプロジェクトで使うときの手順

1. `ae_bg.ps1` と `ae_guard.ps1` をコピーする。`AeBg.DesktopName`（`randamu_bg`）はプロジェクトごとに変えてもよい。
2. 3 節の準備（ジャンクション、スクリプトの書き込み許可、Python）を行う。
3. プラグインに、ログを書く仕組みとキャッシュ対策（4.4 節）を最初から入れる。
4. `run_s1.ps1` を手本に、シーン生成の jsx・aerender・判定をつなぐ。
5. AE のバージョンが変わったら、メインウィンドウのクラス名（`AE_CApplication_<版>`）、レジストリのパス、テンプレート名を確かめる。

## 6. 参考にした外部の情報

- nexrender（オープンソースの AE 自動レンダリング）は、AE 付属の `commandLineRenderer.jsx` を書き換え、aerender でスクリプトを動かしている。UI の AE が要らなくなるが、Program Files の書き換え（管理者権限）が必要。AE 2025 の aerender では `-r` が使えないという報告もある。今回は採用していない。
  https://github.com/inlife/nexrender
- クラッシュ修復ダイアログを `-noerr` で抑えられるという報告が 1 件ある（未検証）。
  https://community.adobe.com/questions-529/how-to-supress-crash-repair-options-becuase-i-am-automating-the-rendering-process-via-script-59819
- AfterFX.exe が引用符を処理しないという報告。
  https://creativecow.net/forums/thread/run-jsx-without-afterfxcom-ae/
