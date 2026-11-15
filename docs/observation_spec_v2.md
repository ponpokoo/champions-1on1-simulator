# Observation specification v2 — DRAFT (stage 6c)

Status: approved by the user 2026-11-15 for implementation (rulings R1–R3 as written). Implementation brief: stage6/sol_task_6c.md.
Schema: `metagross_observation_v2` (new transport, working name `MGO2`). Scope: the full
68-move Champions Metagross mirror, post-stage-6b engine. This draft is a DELTA on
`OBSERVATION_SPEC.md` (v1): everything in v1 remains in force except where this document
explicitly amends it. v1 / MGO1 / g12 stay frozen as the 17-move stack; v2 pilots must
reject 17-move-profile battles and v1 pilots must keep rejecting 68-move battles (the
vocabulary rejection landed in stage 6b part 1).

## 1. Principles carried over, and the 2026-11-15 calculability rulings

The v1 core invariant is unchanged: identical own state plus identical actor-visible
public history implies identical observation and mask, regardless of hidden opponent
state. Opponent engine truth is never copied; every foe feature is `exact` (own),
`public-event`, or `belief` (a deterministic function of allowed evidence only).

Three user rulings (2026-11-15) extend the v1 boundary for the new state space. They
were given after Muse surfaced the ambiguous cases instead of deciding them alone;
that ask-first rule applies to any further ambiguity found during implementation.

- **R1 — Calculable means knowable.** A value that is not displayed but is uniquely
  determined by public information may be encoded as a belief. Canonical case: the
  badly-poisoned stage count — never shown, but a player can calculate it from the
  observed residual-damage progression (equivalently, ticks since the announced
  infliction). The same class covers remaining turns of fixed-duration field effects
  and the Future Sight impact turn.
- **R2 — Seen damage yes, exact inversion no.** A damage-calculating player forms
  archetype-level reads (HB-ish, AS-ish) from the damage they actually saw, not exact
  stat intervals. v2 therefore adds cumulative *seen-damage records* (section 3) built
  purely from public HP-bar endpoints, and continues v1's deferral of numerical
  damage-derived stat bounds: no [lo,hi] inversion over crit/item/roll hypotheses.
- **R3 — Random remaining durations: elapsed only.** Where the remaining duration is
  random and not calculable (freeze thaw, confusion length), encode the state plus
  elapsed turns since the publicly announced infliction — counting is public — and
  never the remaining count.

## 2. Field-level contract for post-v1 state

Classes and provenance rules are v1's. "Derived" below always means replayed from the
public event stream, never read from the foe's `Mon` or the battle `FIELD`.

| Field / engine source | Perspective | Class | Encoding and notes |
| --- | --- | --- | --- |
| Major status kind (`status`: paralysis / freeze / poison / badly poisoned / burn; sleep stays on its v1 fields) | foe | public-event | Announced at infliction and cure (Lum, Rest, Fire-hit thaw, expiry). One categorical field; badly poisoned is distinct from poison. |
| Major status kind and counters | self | exact | Own-state allowance as in v1 (own sleep counter precedent): own status kind, own freeze timer, own confusion counter, own toxic stage are exact. |
| Toxic stage (`toxic_stage`) | foe | belief (R1) | Ticks elapsed since the announced badly-poisoned infliction, capped at 15. Reset on cure / re-infliction, per event replay. Never copied from `foe->toxic_stage`; the two agree only because the process is deterministic and public. |
| Freeze elapsed (`freeze_turns`) | foe | belief (R3) | Elapsed turns since the announced freeze. Remaining is forbidden: the engine timer decrements deterministically but the 1/4 early-thaw roll makes the true remaining unknowable. |
| Confusion (`confusion`) | foe | public-event + belief (R3) | Confused flag is public (announced; self-hit events are public). Elapsed turns since infliction encoded; remaining (random 2–5) forbidden. |
| Weather (`FIELD.weather`, `weather_turns`) | both | public-event + derived | Kind announced at start/end. Remaining turns derived by counting from the announcement (fixed 5-turn duration). The per-Mon compatibility `weather` projection is not a source. |
| Terrain (`FIELD.terrain`) | both | public-event + derived | Kind + derived remaining, as weather. No setter exists in the 68; the field is specified now so stage 8 does not need a schema change. |
| Reflect / Light Screen (`reflect`, `light_screen`) | each side | public-event + derived | Set/expiry announced per side; remaining derived (fixed 5). Screen *removal* by Brick Break / Psychic Fangs is an announced event and replays to zero. |
| Stealth Rock (`stealth_rock`) | each side | public-event | Present flag per side. No duration. (State only in 1v1 — no switching — but it is public state and belongs in the observation.) |
| Gravity (`FIELD.gravity`) | both | public-event + derived | Active flag + derived remaining (fixed 5). |
| Magnet Rise (`magnet_rise`) | foe | public-event + derived | Announced at use and at expiry; remaining derived (fixed 5). Gravity deleting it is an announced transition. |
| Groundedness | foe | belief | Derived summary only: grounded / floating / unknown, from public facts — Gravity active, announced Air Balloon (intact/popped), announced Magnet Rise, Iron Ball *only if revealed* (e.g. by Trick). An unrevealed Iron Ball keeps the value at its public default; never read the item or a levitation flag from `foe`. |
| Future Sight reservation (`future_turns`, on the target slot) | both | public-event + derived (R1) | Scheduling is announced. Encode, per seat perspective: a pending reservation against me (flag + turns until impact, deterministic: scheduled T lands at end of T+2) and my own pending reservation (same shape). Nothing about impact damage is encoded — it depends on live stats at impact and is not public in advance. A second reservation on the same slot fails publicly, so at most one pending per slot. |
| Charging (`charging`: Meteor Beam charge) | foe | public-event | The charge is announced; at a decision boundary the foe is visibly mid-move. Encode in-progress flag + the announced move ID. |
| Recharge (`recharge`: Giga Impact / Hyper Beam) | foe | public-event | Must-recharge is visible at the boundary. Encode flag + the announced move that caused it. |
| Focusing (`focusing`, `focus_lost`) | — | omitted | Intra-turn only: preparation is announced only after both choices are committed, and the state never survives to a decision boundary. Explicitly not a feature; the *outcome* (moved / lost focus) enters through the ordinary move_used / action_opportunity events. |
| Round chain (`round_chained`) | — | omitted | Intra-turn bookkeeping. The previous turn's Round use is already covered by the reveal records. |
| Endure (`endured_now`) | — | omitted as state | Intra-turn flag, cleared at the boundary. The public fact "the foe endured at 1 HP" is already carried by hp_change endpoints and the hit_outcome event. |
| Previous attempt result (`attempt_prev`) | foe | public-event | Stomping Tantrum's input is public history: encode the foe's last-turn attempt category as publicly seen (success / fail / not-fail / forced-skip classes, per the engine `ATTEMPT_*` taxonomy collapsed to what an observer can actually distinguish). Own `attempt_prev` is exact. This turn's `attempt_this` is resolution scratch — not a boundary feature. |
| `state_features` bitset | — | forbidden as a copy | Engine-internal aggregation. The implementation must map every bit it needs to its public provenance; copying the bitset (or any field) from the foe is a leak by construction. |
| Flinch, heal block, Substitute, Protect streak, stages, PP, items, abilities, order evidence | both | — | v1 contract unchanged. New item interactions from G4/G5 (e.g. Power Herb consumption) join the section-3 allowlist by the same rule: named announcement only where the engine branch actually announces/consumes publicly. |

Vocabulary: move wire IDs are 0–67 plus Struggle = 68 (stage 6b part 1). Revealed-move
records keep v1's structure (first-revelation order, announced-use counts split by
PP-spending / called, PP ranges); initial-PP metadata now comes from the versioned
Champions usable-PP table (base PP capped at 20), not raw dataset PP.

## 3. Seen-damage records (new in v2, per R2)

For each side's revealed damaging moves, the observer maintains a bounded cumulative
summary built only from `hp_change` endpoints attributed to that move's direct hits:

- observed bar-% loss per hit: minimum and maximum seen (foe HP: integer percents;
  damage to self: percent of own max HP, from own exact endpoints);
- hit count, and a separate critical-hit count (crit damage is not comparable to
  non-crit; the record keeps them distinguishable rather than merging silently);
- Substitute hits are recorded separately as count + broke flag only — the
  substitute's HP is unknown, so no % loss exists to record.

Residual damage, recoil, recoil-like costs, healing and weather damage are never
attributed to a move record; they remain in the per-turn HP totals v1 already carries.
Multi-hit moves do not exist in Metagross's 68; if stage 8 adds one, per-hit vs
per-use attribution needs a schema note then, not now. These records are the
sanctioned form of the damage-calc player's memory: the pilot sees what a player
saw, and any archetype read (HB/AS and the like) is the policy's own inference.

## 4. Announcement vocabulary additions

v1 section 3's contract extends with these kinds, same payload discipline (sequence,
turn, relative seat, kind, allowed IDs, visible outcome, permitted values only):
`status_start` / `status_end` extended to the new kinds (paralysis, freeze, poison,
badly poisoned, burn, confusion as a volatile with its own start/end), `weather_start`
/ `weather_end`, `terrain_start` / `terrain_end`, `screen_start` / `screen_end`
(per side, per screen), `stealth_rock_set`, `gravity_start` / `gravity_end`,
`magnet_rise_start` / `magnet_rise_end`, `future_sight_scheduled` /
`future_sight_impact`, `charge_start`, `recharge_start`. As in v1, events originate
at the resolving branch, never from a raw-state diff, and consume no RNG.

## 5. Transport, heuristic successor, and the 68-move baseline

- **Transport**: new versioned fixed-width format (`MGO2`); exact widths are an
  implementation detail to freeze in the brief, following v1's rules (explicit
  categorical tags and validity bits; unknown ≠ zero ≠ absent). MGO1 is untouched.
- **Heuristic successor** (working name `obsh3`): pilots 68-move battles through the
  v2 interface only; its internal damage estimates must range over public hypotheses
  (it may use the seen-damage records) and never call the privileged estimators with
  hidden state. Its role mirrors stage 2 → 3: the reference opponent and the
  campaign-3 prior/bootstrap. Detailed estimator design belongs to the implementation
  brief, not this draft.
- **68-move regression baseline** (implements the D4/D6 promise): (a) the 17-move
  validation matrix MD5 `f3128c506954574a0c48bdfb79e2a917` must still reproduce —
  engine inertness for the old space is not renegotiated; (b) a new frozen 68-move
  panel (fixed builds × fixed seeds, composition set in the implementation brief)
  whose result hash becomes the v2-era baseline; (c) the stage-6 fixture suites
  (verify_g0…g5) carry forward as part of the acceptance run.

## 6. Validation additions (for the implementation's acceptance plan)

- Equivalence pairs: hidden toxic-stage / freeze-timer / confusion-counter variants
  behind identical public histories produce byte-identical observations.
- Future Sight: pre-impact observations carry the reservation and countdown only;
  varying the (hidden) impact-time stats changes nothing before impact.
- Seen-damage records reconstruct from the sanitized event stream alone; a hidden
  defense change that leaves all bar endpoints unchanged changes no record.
- Groundedness: an unrevealed Iron Ball foe and a no-item foe with identical public
  histories produce identical observations.
- The v1 acceptance battery (hidden-state sweep, simultaneous isolation, reset and
  perspective, terminal/cap) re-runs against the v2 encoder, plus observer on/off
  matrix identity at the current baseline MD5.

## 7. Decisions log and open items

Decided (user, 2026-11-15): R1 (calculable = knowable; toxic stage is the canonical
case), R2 (seen-damage records in; exact stat inversion stays deferred), R3
(elapsed-only for random durations). The ask-first rule for ambiguous visibility
points is standing for the rest of stage 6c.

Open, none blocking this draft: exact MGO2 widths/layout (implementation brief);
the 68-move baseline panel composition (implementation brief); the `obsh3`
estimator design (implementation brief); whether campaign 3 (stage 6d) trains from
scratch on v2 or warm-starts any v1 weights (a 6d protocol question — the v2 feature
space differs, so from-scratch is the default assumption).

## Amendment A1 (2026-11-15, post-approval — from the Gemini prior-art discussion)

Section 3's seen-damage records gain one refinement, still inside R2 (no inversion):
alongside the all-hits min/max, keep a second min/max restricted to CLEAN hits —
non-critical hits observed while the publicly known damage context was unmodified
(the relevant stat stages at 0 on both sides, no screen up on the target's side, no
burn on the attacker, no damage-modifying weather). If no clean hit has been
observed for a move, the clean record is invalid (valid=0), never back-filled. The
predicate is computed from public state only and must be defined exactly in the
implementation report. Reason: raw min/max mixes boosted/reduced hits into the same
range and erases the unmodified damage band a damage-calculating player would
actually remember. All other section-3 rules are unchanged.

## 追補： 裁定R4・R5（ユーザー、2026-11-15）
6c実装が保守的に実装して報告した2件について、ユーザーが裁定した。
- R4: じゅうりょくが でんじふゆう の発動を止めた場合、技名は公開される（使用が
  アナウンスされ、試行は公開上の失敗として扱う）。
- R5: ねごと で呼べる技がなかった場合、その試行は公開上の失敗として扱う
  （不明のままにしない）。
