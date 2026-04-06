# Organic Assembler — Differential Programming, Debugging, and Development

This document explores three interlocking meanings of "differential" in the Organic
Assembler project: (1) differential/incremental computation patterns in the codebase,
(2) differential debugging approaches used during development, and (3) how the concept
of differentiation — rates of change, deltas, accumulation — applies to instruments
as a domain.

## Part I: Differential Computation in the Codebase

### 1. Three-Level Dirty Cascade

The graph editing system tracks changes at the finest granularity and propagates them
upward. This is a differential approach: instead of re-evaluating the entire graph,
only the changed parts trigger work.

```
FlowArg2 mutation
    │ mark_dirty()
    ▼
FlowNodeBuilder dirty flag
    │ notify node editor
    ▼
GraphBuilder dirty flag
    │ notify graph editor
    ▼
Inference / rendering invalidation
```

Key implementation details:

- **Arg-level**: Any setter (`ArgNet2::net_id()`, `ArgNumber2::value()`) calls `mark_dirty()`
- **Node-level**: Two independent dirty flags — semantic dirty (triggers re-inference)
  and layout dirty (triggers re-render only, not re-inference)
- **Graph-level**: Dirty propagation is deferred inside `edit_start()`/`edit_commit()` batches

The layout/semantic split is critical: dragging a node changes its position (layout dirty)
but not its type signature (no semantic dirty). Without this split, every mouse drag
during node movement would trigger a full type inference pass.

### 2. Mutation Deduplication

Within a single edit batch, each item's observer callback fires at most once:

```cpp
void GraphBuilder::add_mutation_call(void* ptr, std::function<void()>&& fn) {
    if (mutation_items_.count(ptr)) return;  // Already queued
    mutation_items_.insert(ptr);
    mutations_.push_back(std::move(fn));
}
```

This is differential in the set-theoretic sense: the delta between "state before edit"
and "state after edit" is computed as a deduplicated set of changed items, not a log
of every micro-mutation. If an arg is changed three times within one batch, observers
see one notification, not three.

### 3. Fixed-Point Type Inference

The inference engine is the purest example of differential computation in the project.
It iterates until the delta between iterations is empty:

```cpp
for (int iter = 0; iter < 10; iter++) {
    bool changed = false;
    changed |= propagate_connections(graph);
    changed |= infer_expr_nodes(graph);
    if (changed) idx.rebuild(graph);
    changed |= resolve_lambdas(graph);
    if (!changed) break;  // Fixed point: delta is zero
}
```

Each sub-phase returns `true` only if it actually modified a type. The phases are:

1. **propagate_connections**: Forward-propagate types through wires. Returns `changed = true`
   only if a pin's type was refined (never widened — types only become more specific).
2. **infer_expr_nodes**: Evaluate expression ASTs and resolve output types. Returns `changed`
   if any expression resolved to a new type.
3. **resolve_lambdas**: Determine lambda boundaries and parameter types. Returns `changed`
   if any lambda's signature changed.

**Monotonicity guarantee**: Types move from generic → specific, never the reverse. This
guarantees convergence. The 10-iteration cap is a safety net; most graphs converge in 2-3
iterations.

**Differential index rebuild**: `GraphIndex` (fast pin/node lookup) is rebuilt only when
pins were added or removed (`if (changed) idx.rebuild()`), not every iteration.

### 4. Wire Cache Invalidation

The editor maintains a precomputed wire list for rendering and hit-testing. Rather than
rebuilding it every frame, it uses a dirty flag:

```cpp
bool wires_dirty_ = true;

void rebuild_wires(ImVec2 canvas_origin) {
    if (!wires_dirty_) return;  // No delta since last rebuild
    cached_wires_.clear();
    // ... rebuild from current graph state ...
    wires_dirty_ = false;
}
```

Observer callbacks set `wires_dirty_ = true` when the graph changes. Frames where nothing
changed skip the rebuild entirely. This is differential rendering: only re-derive visual
state when the underlying model has a non-zero delta.

### 5. Shadow Node Differential Expansion

Shadow nodes (internal expression nodes for inline args) can be updated per-node rather
than regenerating the entire graph:

```
User edits node args string
    │
    ▼
update_shadows_for_node(node)
    ├── Remove old shadow nodes for this node
    ├── Parse new args
    └── Generate new shadow nodes + links
```

This is a localized delta: only the shadow nodes belonging to the edited node are
regenerated. All other shadow nodes remain untouched.

### 6. Version Migration as Format Delta

Each version migration function encodes the *difference* between two format versions:

```
nanoprog@0 ──Δ1──> nanoprog@1 ──Δ2──> attoprog@0 ──Δ3──> attoprog@1 ──Δ4──> instrument@atto:0
```

Each Δ is a transformation function:
- **Δ1**: Shadow node generation (v0 has inline args, v1 has shadow nodes)
- **Δ2**: Rename nano→atto in node types and paths
- **Δ3**: Strip `$` from variable refs, convert `@N` to `$N`
- **Δ4**: Add explicit GUIDs, structured net entries

The deserializer composes deltas: loading a `nanoprog@0` file applies Δ1∘Δ2∘Δ3∘Δ4.
Each delta is idempotent on files already at its target version.

---

## Part II: Differential Debugging During Development

### Git Commit Pattern Analysis

The commit history reveals a distinctive differential debugging approach. Looking at
158 commits across 5 days of development:

#### The "towards X" Pattern

Progress commits that acknowledge incomplete work:

```
817a1bd  towards lambdas
ed06b67  towards reusable lambas
4120050  towards lambda link rendering
bf4d01f  towards editor2
57e6805  towards evaluated decls
1812167  towards first class pins
```

These are differential checkpoints: "the system is closer to X than it was before, but
X is not yet complete." They preserve the delta so far, allowing rollback if the next
step breaks something. This is differential debugging in the version control sense —
each commit captures a known-good (or at least known-better) intermediate state.

#### The "more X work" Pattern

Incremental progress without claiming completion:

```
022c4a4  more graphbuilder work
4772975  more graphbuilder work
d0c9eb1  work on nets and nodes
28bcd4c  more net work
8dd992d  more shadow node work
974a5fa  more progress on literals
095d426  progress in literals?
da506db  more progress on literals?
6c9c0cc  more literal work
ac639d5  more literal work
079fc8b  more literals
```

The question marks (`progress in literals?`) reveal uncertainty — the developer isn't
sure if the delta is positive. Committing anyway preserves the ability to diff against
the previous state and evaluate whether the change helped.

#### The Emotional Honesty Pattern

```
3b7e936  this feels dodgy
6325002  questionable progress
fb3d96d  not perfect, but progress is progress
489adb7  klavier is now a clusterfuck
```

These commits are explicit about the quality of the delta. "This feels dodgy" is a
signal to future-self: the diff from this commit should be examined carefully. It's
a differential annotation — metadata about the *quality* of the change, not just its
content.

#### The Phased Refactor Pattern

```
3b1f9df  refactor: phase 1
37d881c  refactor: phase 2
bf0424b  refactor: phase3
1656fac  refactor: phase 3b
0b30130  refactor: make pin ptrs stable
6bcdad6  refactor: phase 4a
b3bfee2  refactor: phase 4b
0f5ab4e  refactor: Phase 5
```

Large refactors are decomposed into numbered phases, each a separate commit. This is
explicit differential decomposition: the total change (Δtotal) is split into ordered
sub-deltas (Δ1, Δ2, ..., Δ5) that can be reviewed and reverted independently. Phase 3
even has a sub-phase (3b), showing adaptive decomposition when a phase turned out to
be larger than expected.

#### The Naming Migration Trail

```
d480da9  graphbuilder -> graph_builder
bf8c9cd  graph rename
d55cf36  wire -> net in grahp_builder
753c051  as_Node/Net -> as_node/net, dead code deletion
f296f08  editor->window.cpp/h
722a591  move editor1 to legacy folder
```

Renames are pure-delta commits: the behavior doesn't change, only the names. These are
isolated into their own commits so the diff is unambiguous — every change is a rename,
no logic changes mixed in. This makes the diff self-documenting.

#### The Fix-After-Commit Pattern

```
3e9d3b1  alias fix from last commit
4423514  fixes
70e7621  fixes in shadow
```

Bugs discovered immediately after a commit are fixed in a separate commit rather than
amending. This preserves the delta between "broken state" and "fixed state" in the
history, which is useful for understanding *what* broke and *why*.

#### Commit Velocity as a Signal

```
Day 1 (Mar 23): 16 commits  — Foundation (new project, broad strokes)
Day 2 (Mar 24): 28 commits  — Deep work (lambdas, expressions, refactoring)
Day 3 (Mar 25): 36 commits  — Peak velocity (type system, graphbuilder, editor2)
Day 4 (Mar 26): 72 commits  — Highest output (editor2 features, 72 commits in one day)
Day 5 (Mar 27):  6 commits  — Integration (wire connections, delete, nets)
```

The velocity curve shows a ramp-up phase (days 1-2), peak throughput (days 3-4), and
a consolidation phase (day 5, fewer but more substantial commits). This is characteristic
of differential development: early commits are exploratory (small deltas, uncertain
direction), middle commits are confident (large deltas, clear direction), and late
commits are integrative (connecting previously separate deltas).

#### Commit Message Style Taxonomy

The messages fall into distinct categories revealing the nature of each delta:

| Style | Example | Meaning |
|-------|---------|---------|
| `Add X` | `Add scroll pan speed to Editor2Style` | New feature, additive delta |
| `fix: X` | `fix: shorten node ids` | Bug fix, corrective delta |
| `refactor: X` | `refactor: phase 4a` | Structure change, zero-semantic delta |
| `towards X` | `towards lambdas` | Incomplete progress, partial delta |
| `more X work` | `more graphbuilder work` | Continuation, incremental delta |
| `X -> Y` | `wire -> net in graph_builder` | Rename, identity delta |
| `X!` | `Editor2Pane!` | Milestone, significant delta |
| `cosmetics` | `cosmetics` | Visual-only, zero-functional delta |
| `X concept` | `nets_editor concept` | Proof-of-concept, exploratory delta |
| bare noun | `Folding` | Feature name as commit, self-evident delta |

---

## Part III: Differentiation and Instruments

### The Mathematical Connection

Differentiation in calculus is about **rates of change**. This is not an abstract analogy
for instruments — it is literally what audio synthesis computes.

#### Phase Accumulation (Integration)

An oscillator generates a waveform by accumulating a phase delta each sample:

```
phase += 2π * frequency / sample_rate    // Integration: Δphase per sample
output = sin(phase)                       // Evaluate at current phase
```

In the klavier instrument, this appears as:

```toml
[[node]]
type = "expr"
args = ["2*pi/$0"]    # Compute phase step (Δphase) from frequency

[[node]]
type = "store!"
args = ["$0.p"]       # Accumulate: phase = phase + Δphase
```

The phase step (`2π/frequency`) is the **derivative** of the waveform's position with
respect to time. The store operation performs **numerical integration** — accumulating
the derivative to produce the signal.

#### Envelope Generators (Piecewise Differentiation)

An ADSR envelope (Attack, Decay, Sustain, Release) is defined by four rates of change:

```
Attack:  amplitude += attack_rate * dt     (positive derivative)
Decay:   amplitude -= decay_rate * dt      (negative derivative)
Sustain: amplitude = sustain_level         (zero derivative)
Release: amplitude -= release_rate * dt    (negative derivative)
```

Each segment is a piecewise-constant derivative. The envelope itself is the integral
of these piecewise derivatives. In attolang, this is expressed as a state machine
where each state stores the current rate (derivative) and the `store!` node
integrates it per sample.

#### Frequency as Derivative of Phase

The fundamental relationship in audio synthesis:

```
frequency = d(phase) / dt
```

Frequency IS the derivative of phase. When an instrument changes pitch (e.g., a
glissando or vibrato), it's modifying the derivative. The `store!` node that
accumulates phase is performing Euler integration of this derivative.

#### Audio Mixing as Superposition (Linearity of Differentiation)

The runtime's `output_mix!` node accumulates contributions from multiple oscillators:

```cpp
inline f32 _atto_mix_accum = 0.0f;

inline void output_mix(f32 value) {
    _atto_mix_accum += value;  // Sum of individual contributions
}

inline f32 atto_consume_mix() {
    f32 v = _atto_mix_accum;
    _atto_mix_accum = 0.0f;    // Reset for next sample
    return v;
}
```

This exploits the **linearity of differentiation**: if each oscillator independently
computes its output via phase accumulation (integration), their sum is the integral of
the sum of their derivatives. You can mix signals by adding them because integration
is a linear operator.

#### Wire Inspection as Sampling (Discrete Differentiation)

The planned `wire<T>` inspection system samples wire values at discrete points in time:

```
wire value at frame N:     v[N]
wire value at frame N-1:   v[N-1]
discrete derivative:       Δv = v[N] - v[N-1]
```

An oscilloscope view of a wire IS the discrete derivative visualization. The inspector
can show:
- **Raw value**: v[N] (the signal itself)
- **Rate of change**: v[N] - v[N-1] (the discrete derivative)
- **Accumulation**: Σ v[i] (the discrete integral)

This makes `wire<T>` a **differential probe**: it doesn't just show the value, it
enables computing derivatives and integrals of any signal in the instrument.

### Differential Dataflow

The broader connection between differential programming and visual dataflow:

#### Forward Mode (How Instruments Compute)

In forward-mode automatic differentiation, you compute the derivative alongside the
value as data flows through the computation graph. An instrument's dataflow graph
does exactly this when tracking rates of change:

```
[frequency] ──> [phase_step = 2π*freq/sr] ──> [phase += step] ──> [sin(phase)]
   value           derivative                   integral            output
```

Each node transforms both the value and its derivative. The `expr` node computes
the phase step (derivative), the `store!` node integrates it.

#### Reverse Mode (How Type Inference Works)

In reverse-mode automatic differentiation (backpropagation), gradients flow backward
through the graph from outputs to inputs. attolang's type inference does the same
thing with types:

```
Forward: producer type ──> wire ──> consumer input type
Reverse: consumer expected type ──> wire ──> producer output type (backprop)
```

The bidirectional inference engine is structurally identical to forward+reverse mode
automatic differentiation, with types playing the role of values/gradients.

#### Incremental Computation (How the Editor Stays Fast)

Incremental computation frameworks (like Adapton or differential dataflow) track
which outputs depend on which inputs and only recompute what changed. The Organic
Assembler's dirty tracking system is a manual implementation of this:

```
Input changed (arg mutation)
    │
    ▼
Which nodes depend on this input? (dirty propagation)
    │
    ▼
Recompute only those nodes (selective re-inference)
    │
    ▼
Which visual elements changed? (wire cache invalidation)
    │
    ▼
Re-render only those elements (selective re-draw)
```

The three-level dirty cascade (arg → node → graph) is an approximation of
true incremental computation. It's coarser than full dependency tracking
(it invalidates at the node level, not the pin level) but much simpler to
implement and reason about.

### The Differential Development Cycle

Putting it all together, the Organic Assembler project exhibits a fractal
differential structure at every level:

| Level | Delta Unit | Accumulation | Convergence Check |
|-------|-----------|--------------|-------------------|
| Audio sample | Phase step | `store!` integration | Waveform continuity |
| Type inference | Type refinement | Fixed-point iteration | `!changed` |
| Graph editing | Arg mutation | `edit_commit()` batch | Observer notification |
| Wire rendering | Layout change | Cache rebuild | `!wires_dirty_` |
| Git commit | Code change | Branch history | Tests pass / builds green |
| Version format | Migration delta | Δ1∘Δ2∘...∘Δn chain | Latest format |
| Project phase | Feature set | Daily development | Phase completion |

At every level, the pattern is the same:
1. **Compute a small delta** (one sample, one type refinement, one arg change, one commit)
2. **Accumulate it** (integrate the phase, propagate the type, batch the mutations, push the branch)
3. **Check convergence** (is the waveform smooth? did types stabilize? is the build green?)
4. **Repeat or stop** (next sample, next iteration, next edit, next feature)

This is differentiation in the deepest sense: the system evolves through small, tracked
changes, and the quality of the whole is determined by the quality of each increment.
