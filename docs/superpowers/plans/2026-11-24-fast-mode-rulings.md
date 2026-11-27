# 高速モードの実装で行った判断（実行台帳の写し、2026-11-24）

実行中の台帳（英語）をそのまま残したもの。`Ruling:` は計画からの逸脱や計画が決めていなかった点の判断、`minor (deferred)` は最終レビューの軽微な指摘で今回は直していないもの。

```
# SDD ledger — plan: /a/MyProjects/After Effects/Plugin Project/DupliX/docs/superpowers/plans/2026-11-24-fast-mode.md
Spec: docs/superpowers/specs/2026-11-24-duplix-sublayer-design.md (12.10-12.12)
Pre-flight: Task1->Task2: CopyHomography screenToSrc[9] (row-vector, homogeneous) consumed by RasterCopy.screenToSrc — consistent. Task1 needs the source rect for the behind-camera check; Interfaces omit it -> Ruling below.
Pre-flight: Task2 RasterizeCopies(ds) vs AE: in AE the input world is downsampled too -> Ruling below.
Ruling: CopyHomography/CopyDepth take the source rect (layer px, left/top/right/bottom) — the "any corner z<=1 -> false" check needs the corners — cost if wrong: signature churn only.
Ruling: dsX/dsY apply to BOTH src and dst RasterImage (AE hands downsampled input and output worlds); raster_downsample_half is rewritten accordingly (identity H at ds=0.5 copies the half-res src; H=x2 samples src index 2x+0.5) — cost if wrong: copies at 1/2 res drawn at wrong scale (caught by fast_half).
Task 1: complete (commits 8f3a005..6c45da4, tests: build/unit/unit_tests.exe → ALL PASS (26 tests, 804 checks))
Task 2: note: raster speed (Release, i7-6700 4C/8T) 10 copies 577x680 on 1920x1080: 1 thread 95 ms, 8 threads 20.5 ms, clear only 3 ms (Debug build ~4x slower)
Task 2: complete (commits 6c45da4..2f39e56, tests: build/unit/unit_tests.exe → ALL PASS (36 tests, 1308 checks))
Task 3: Ruling: Rects hook in fast mode never calls the original (plan Step 4 said "fall back to the original"): always one entry, plane from FastGetPlane or identity M4 — original returns an empty list for AV layers (count/vector mismatch) and its layer-set path crashed AE (spike test 4) — cost if wrong: a frame with no registered plane shows the drawing at the wrong place (logged as fast: plane lookups none=...).
Task 3: Ruling: plane table keyed by layer time (in_data->current_time/time_scale, same as the hook's T_Time), compared as fractions; AEGP camera/layer reads use comp time (header: comp_timeP) — cost if wrong: stale plane (fast_anim with layer startTime -0.4 checks it).
Task 3: Ruling: raster.cpp compiled with /O2 (BasicRuntimeChecks Default) in every configuration of both vcxprojs — AE loads the Debug build; Debug raster was ~4x slower — cost if wrong: none functional.
Task 3: Ruling: SeqData path forces fast=false (fast lives in ScatterParams padding; old projects may hold any byte); static_assert on sizes 96/112 — cost if wrong: after reopening, main-thread queries answer normal mode until the next parameter change/UI update; render threads read the real value.
Task 3: Ruling: plane depth = max(anchor camera z, 2.0) (anchor at/behind camera) — 1:1 mapping holds at any depth — cost if wrong: plane sorts in front of everything in that edge case.
Task 3: Ruling: fast-mode input checkout requests rect +-30000 (whole source), not output_request — output request is a screen region — cost if wrong: a huge source would be rendered whole.
Task 3: Ruling: FastSetup failure (camera/transform unreadable) -> empty result rect (nothing shown) rather than pass-through — cost if wrong: layer disappears instead of showing undistorted.
Task 3: Ruling: fast mode copies' draw-order depth = camera z of the source rect centre (plan: "コピーの中心").
Task 3: existing AE tests with fast mode off: run_ref REF ALL PASS, fx_remove FX REMOVE ALL PASS, run_ui UI ALL PASS (logs t3_*.log)
Task 3: complete (commits 2f39e56..95cd36b, tests: build/unit/unit_tests.exe → ALL PASS (39 tests, 1318 checks))
Task 4: Ruling: fast_identity criterion changed from raw max<=8 to raw mean<=0.5 and 3x3-averaged max<=24 — AE's resampling of fine detail differs from bilinear (raw max 58 on checker edges, correct placement); averaged max 15.7 correct vs >=72 for a 1 px shift — cost if wrong: a sub-pixel placement error under ~0.3 px could pass.
Task 4: Ruling (plan defect, found by fast_nocam/fast_scatter): AE effect worlds are STRAIGHT alpha, the plan/Task 2 assumed premultiplied -> transparent corners' colours leaked. Raster now premultiplies each tap, composites premultiplied, un-premultiplies on store; tests raster_opacity_half/raster_downsample_half updated to straight semantics, raster_straight_alpha_edge added (RED->GREEN) — cost if wrong: none (AE SDK: PF_Pixel is straight alpha).
Task 4: note: run_fast_mode.ps1 FAST ALL PASS with MFR OFF and ON (after the straight-alpha fix); plane lookups: no other-time/none fallbacks logged.
Task 4: note: timing runs were served from AE's disk cache (same build + same content -> no render); added DUPLIX_CACHE_SALT (mixed into the GUID, set per run by run_user.ps1).
Task 4: note: user scene, plugin span per frame (CPU ~60% busy with other user processes): v_asis ~110 ms, v_fast10 ~47 ms (raster ~31), v_fast100 ~200 ms (raster ~180).
Task 4: Ruling: plan's speed expectation "v_fast1000 <= 3x v_fast10" is not met on CPU — the raster cost grows with the pixels the copies cover (the user's copies are large on screen), the plan assumed the plane cost dominates; v_fast10 <= half of v_asis is met by span. Not optimised further here: GPU (CUDA) is the spec's next step (12.11 step 3) — cost if wrong: 1000 large copies stay ~1-2 s/frame on CPU.
Task 4: note: user scene aerender totals, 120 frames MFR OFF (CPU busy with other user processes): v_asis 150 ms/frame, v_fast10 117, v_fast100 267, v_fast1000 2.2 s (normal mode ~14 ms per copy -> ~1.4 s for 100, ~14 s for 1000). Plan target v_fast10 <= half of v_asis NOT met by aerender totals (met only by the plugin span 47 vs 110 ms: AE's own work on the full-screen plane and output is outside the span).
Final review: fresh reviewer (opus), verdict "With fixes", no Critical.
Final: fixed plane ring 8 -> 64 and nearest-time fallback (MFR on many cores, motion-blur sub-frames) — fast_registry_keeps_recent_64, fast_registry_fallback_nearest RED->GREEN, unit suite 42/42
Final: fixed fast render not cancellable — raster abort callback polled on the render thread, SmartRender returns PF_Interrupt_CANCEL — raster_abort_stops RED (compile) -> GREEN, unit suite 42/42
Final: coverage added: fast_grow (Drop Shadow before DupliX, negative input origin) and fast_remove (remove the effect in-session while fast is on) in run_fast_mode.ps1 (new coverage, not reproductions)
Final: Ruling: multi-view (two viewers, different cameras, same frame) can overwrite the (layer, time) plane — the hook does not know which view it serves; documented in quickstart as a limitation — cost if wrong: one viewer shows the copies misplaced in 2/4-view layouts (render output is unaffected).
Final: Ruling: declined-to-judge items (non-square pixels, collapsed/continuously rasterized layers, time stretch/remap on the fast layer) are left as open questions for the user's trial — cost if wrong: misplaced drawing in those setups.
Final: minor (deferred): an exception in the raster's calling-thread worker unwinds joinable threads -> std::terminate (bad_alloc only)
Final: minor (deferred): thread oversubscription (hardware_concurrency threads per SmartRender under MFR) and per-frame thread creation
Final: minor (deferred): GUID key omits parameter values (AE hashes checked-out params itself)
Final: minor (deferred): FastClear never called outside tests (stale entries after effect removal)
Final: minor (deferred): +-30000 input request renders large precomps/expanding effects whole
Final: minor (deferred): Fast Mode checkbox can be keyframed (consider CANNOT_TIME_VARY)
Final: minor (deferred): Count and Rects not answered atomically across a parameter change
Final: minor (deferred): anchor at/behind the camera -> plane at depth 2 might be near-clipped by classic 3D (untested)
Final: AE suite after fixes: run_fast_mode FAST ALL PASS (MFR OFF/ON, 9 cases), run_ref REF ALL PASS
Task 4: complete (commits 95cd36b..2483a06, tests: build/unit/unit_tests.exe → ALL PASS (42 tests, 1328 checks))
```
