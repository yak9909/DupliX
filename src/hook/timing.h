// Per-frame timing (DUPLIX_TIMING=1 and DUPLIX_LOG set): for each (comp, frame time) the span from the
// first to the last event of this effect's work in that frame (effect pre-render/render, sub-layer
// queries, per-copy opacity = drawing of each copy), and the time spent inside the plugin. One log line
// per frame ("timing: ..."), written once the frame has been quiet for a while.
#pragma once

namespace rnd {

enum TimingKind { TK_HOOK = 0, TK_PRERENDER, TK_RENDER, TK_COPY };

bool TimingEnabled();
long long TimingNow();  // QPC ticks
// One event of `kind` for comp `compItemId` at frame time `tSec`, that ran from t0 to t1 (QPC ticks).
void TimingMark(int compItemId, double tSec, TimingKind kind, long long t0, long long t1);
void TimingFlushAll();

}  // namespace rnd
