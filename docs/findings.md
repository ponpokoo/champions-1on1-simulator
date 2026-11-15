# Stage 3 findings — learned-policy loop

Status: GPU campaign completed; actual results follow.

The PC probe and trainer metadata below record the executed path; historical SSH failures are retained in ssh_attempt.log.

## Design and information boundary

Collection/evaluation is `stage3/loop.c` around `engine.c`; training is `train.py` on the PC; `campaign.py` orchestrates generate → transfer → train → export → C/PyTorch parity → evaluate. `POL_LEARNED` uses the same frozen-observation interface as obsh2. `learned` is available in the main engine CLI and `policy-version` reports the compiled schema/encoder/hash. No mechanics or old policy changes were made.

The small residual policy/value network has 415 encoded inputs, ReLU layers 64/32, five masked logits and a tanh value head (28,902 learned parameters). A fixed +2 logit preference for the observation-only obsh2 action supplies a competent starting policy; neural residuals may override it. Deployment takes legal argmax with stable lowest-slot tie breaking. The initial zero policy/value heads make the bootstrap exactly obsh2 in deployment; this is **not a learned model**.

Both actor and critic take only MGO1 plus the legal mask. The fixed preference also calls only obsh2(MGO1, mask). Collection samples from these logits using a domain-separated per-game/per-actor stream; battle RNG is never passed to the callback. Frozen inputs are created before either choice. Snapshot opponents use the existing observation callback with separately frozen weights. Opponents may use the privileged generic policy; learner data and inference may not.

The encoder includes normalized existing numeric fields, validity-gated move/item one-hots, and expanded public belief bitsets. It adds no hidden-field estimates. Metadata (seed, seat, population/opponent IDs and final turns) is isolated from both heads. Terminal outcome is a target only. See `FORMAT.md` for the precise 64-byte file header, 390-byte decision records, MWP1 float order and lossless reconstruction of all 256 MGO1 words.

Optimization is game-balanced Monte Carlo PPO: final return minus frozen value, clipped policy objective, value MSE and entropy. Entire games, not individual decisions, are held out. There is no privileged action-label distillation, search or hidden-state critic. Training behavior probabilities/value estimates must agree with PyTorch before its first update; exported C logits/value/argmax must then agree with PyTorch.

## Frozen budget and actual work

Budget: **3 × 12,000 training games; up to 6 epochs and 540 seconds per generation; stop starting generations after 30 minutes**. Development evaluation 500 games/cell; final 3,000 games per population/opponent/seat, 36,000 total including a self-play diagnostic. A negative CUDA probe permits only a 128-game, one-epoch, 30-second CPU smoke. Full specification, seeds, learning rate and stopping rules are in `protocol.md`.

| Generation | Data | Training loss / value accuracy | Evaluation |
|---|---|---|---|
| 1 | 12,000 games / 74,174 decisions | 6 epochs; loss 0.34822073036974127; held-out value accuracy 0.7331897531803608 | results/g01.md |
| 2 | 12,000 games / 71,948 decisions | 6 epochs; loss 0.30815127305686474; held-out value accuracy 0.7582036976460694 | results/g02.md |
| 3 | 12,000 games / 73,737 decisions | 6 epochs; loss 0.30004870530330774; held-out value accuracy 0.7524207176988942 | results/g03.md |

Prepared generation 1: 74,174 decisions, 28,927,924 raw bytes; 2831715 compressed bytes. Learner W/L/D during stochastic collection: 5,028 / 6,947 / 25; average 6.18 turns. Opponent groups are exactly 3,000 games each. This collection score is not a head-to-head estimate. All collection fallbacks: **0**. A separate 128-game, 812-decision collection/replay is byte-identical on rerun.

## Evaluation tables and interpretation

Final trained-policy results, last completed generation; no final-data checkpoint selection.

| Population | Opponent | Learner seat A score % (95% CI) | Learner seat B | Both seats |
|---|---|---:|---:|---:|
| final168_x_final168 | obsh2 | 50.57 [48.77, 52.37] | 51.03 [49.17, 52.77] | 50.80 [49.08, 52.52] |
| final168_x_final168 | generic | 47.33 [45.53, 49.20] | 47.50 [45.63, 49.20] | 47.42 [45.68, 49.15] |
| final168_x_final168 | learned | 50.57 [48.83, 52.33] | 51.03 [49.10, 52.83] | 50.80 [49.13, 52.48] |
| final168_x_bench8_ST | obsh2 | 72.72 [71.05, 74.33] | 72.68 [71.00, 74.25] | 72.70 [71.10, 74.27] |
| final168_x_bench8_ST | generic | 68.97 [67.37, 70.62] | 68.87 [67.17, 70.60] | 68.92 [67.30, 70.57] |
| final168_x_bench8_ST | learned | 72.72 [71.05, 74.33] | 72.68 [71.08, 74.35] | 72.70 [71.15, 74.25] |


`final168_x_final168`: learner and opponent independently uniformly sampled from all **168 run1 finalists**. `final168_x_bench8_ST`: learner uniform over those finalists, foe uniform over the **8 run1 benchmarks**, with the three-move eighth benchmark copied and padded with legal ST. Full provenance/specs in `population.json`. The original run1 inputs are unchanged. These are population-pair averages; there are 3,000 games per named population pairing/seat, not 3,000 per every build combination. All policies are repiloted for the experiment.

Score = win + half draw. The table gives nominal 95% CIs from 2,000 bootstrap resamples of seed/build blocks. Both-seat intervals first average the paired seats within each seed; reused seeds are not counted as independent. All per-seat W/L/D, mean turns and cap counts are in the adjacent JSON/CSV; per-game score/turn bytes reconstruct every aggregate. Protocol includes a conservative zero-variance convention. No multiple-comparison correction or unseen-build generalization claim.

## Version and PC verification

Schema: `metagross_observation_v1/MGO1`; encoder: `mgo1_residual_v1`.

Weights SHA-256: **`bd4c56e14ec14e5a297b9afe21d7e947920a572e02ec8329eff66915cf7d3da5`** (canonical float32 payload including fixed prior).

This is a trained export.

PC probe: `{"python": "3.10.6 (tags/v3.10.6:9c7b4bd, Aug  1 2022, 21:53:49) [MSC v.1932 64 bit (AMD64)]", "executable": "<python>", "torch": "2.5.1+cu124", "cuda_available": true, "device_name": "NVIDIA GeForce RTX 4070", "training_smoke_loss": 0.26026451587677}`.

## Regression, purity and harness

- Observer disabled and `ENGINE_OBSERVER=1 ENGINE_OBS_COMPAT=1`: both required matrix MD5s **`21f0b28488d25682bb05ceba5ef5d8b6`** (`matrix_disabled.txt`, `matrix_compat.txt`).
- **30 fixtures pass**: all 28 pre-stage-3 fixtures unchanged, plus learned boundary and learned replay/rejection fixtures. Original 3,750 off/on paired battles, existing 1,785 obsh2 mixed battles and 1,785 additional learned mixed battles pass independent public-event replay. Hidden sweep varies 1,000 hidden-state configurations while comparing canonical observations, masks, nonzero neural logits/value and actions. All 31 nonempty masks, purity, reverse callback order and normal/trace/sequential rejection are covered.
- Full **AddressSanitizer + UndefinedBehaviorSanitizer pass**, with LeakSanitizer disabled because of managed `/proc` restrictions, as in stage 2.
- Synthetic nonzero weights: **812 C/NumPy queries**, exact encoder and argmax parity; maximum logit error **8.35e-7**, value error **1.87e-9**. Both binary-loaded weights and generated/compiled C-header paths pass. This does not replace the pending C/PyTorch post-training check.
- Generation sampler: **1,000 hidden-state sweeps** compare exact sampled records/probabilities/actions under the same independent policy state; 31 masks and **300 paired battles** with extra independent policy draws preserve results, turns, final state and battle RNG. See `sampling_test.txt`.
- PyTorch behavior and exported C inference parity results are recorded per generation.
- Evaluation fallback count **0 / 36,000** diagnostic games; collection fallback count **0 / 12,000**; full harness acceptance counter **0**. The original deliberately invalid injection tests remain separately logged and restore their counter.
- `frozen_code_audit.json` verifies unchanged legacy choose/damage/resolution helpers, RNG, observation builder/mask/replay/emission, obsh2/obsdemo, and the 17-move table. `engine.diff` shows the routing additions. `run1_integrity.txt`: all original file SHA-256 checks pass. No observer semantics or resolution RNG consumption changed.

## What remains and stage 4

The bounded campaign and checks completed. Raw results and per-generation metrics above determine strength; no build-generalization claim follows from training-population evaluation.

Judge whether learned beats obsh2 from the final confidence intervals, not the collection reward. After this campaign, stage 4 should test genuinely held-out builds and broader matchup floors, retain both seats/matched streams, compare the residual against its obsh2 prior and ablate that prior, inspect draw/turn-cap behavior, and assess whether sparse terminal rewards and the conservative PPO KL cap change argmax actions within this small budget. Benchmark/finalist populations here are training populations with fresh evaluation seeds, not held-out build populations. Do not promote stage 3 to complete or infer generalization from the bootstrap table.

## Reproduction and evidence

`README.md`, `protocol.md`, `FORMAT.md`, `prepare.py`, `loop.c`, `policy.h`, `model.py`, `data.py`, `train.py`, `probe_gpu.py`, `campaign.py`, `parity.py`, `analyze.py`, `verify.py`, and `report.py` describe/reproduce the work. `pc_bundle.zip` contains the manual first-generation PC inputs only. Source/input/artifact hashes are in `artifacts.sha256`; campaign status in `campaign_status.json`. Re-render this report with `python3 stage3/report.py` once real PC artifacts exist.

---

## Campaign execution addendum (Muse, 2026-11-15)

The Codex sandbox could not use SSH, so Muse ran `stage3/campaign.py` from outside the sandbox. The full loop executed end-to-end on the user's PC (RTX 4070, torch 2.5.1+cu124, CUDA verified in the campaign log): 3 generations × 12,000 games of self-play generation on the VM, PPO training on the GPU (6 epochs per generation; loss 0.485 → 0.300 across the campaign; value decisive accuracy 0.59 → 0.75), weight export, C inference, and a final 36,000-game deterministic evaluation (3,000 games per population/opponent/seat). Fallbacks: 0 everywhere. Post-campaign `verify.py` passes (both matrix MD5s, all 30 fixtures, run1 integrity). Final weights: `models/g03.bin`, SHA-256 `bd4c56e14ec14e5a297b9afe21d7e947920a572e02ec8329eff66915cf7d3da5` (recorded in `final_version.json`).

**Result: no measurable strength change yet.** The final evaluation table (`results/final.md`) is game-for-game identical to the untrained bootstrap diagnostic: 50.80% vs obsh2 and 47.42% vs generic on final168×final168; 72.70% / 68.92% on final168×bench8_ST. Muse verified this is not a loading error: re-running `loop evaluate` with `models/g03.bin` at the final protocol seed reproduces `results/final.bin` byte-for-byte. The trained weights differ from the bootstrap, and the value head clearly learned, but at this budget (strong obsh2 prior, approx. KL ≈ 0.005/epoch, 18 epochs total) the deterministic argmax never diverges from the prior on any state reached in 36,000 evaluation games. Per-generation snapshot evaluations (500 games/seat, different seeds) fluctuated around the same level (vs obsh2: 49.0 / 52.9 / 48.7), consistent with a policy still glued to its prior.

Stage 4 levers, in order of expected value: (1) let the policy actually move — weaker prior weight, larger KL budget or more epochs per generation; (2) more generations and data per generation; (3) also evaluate the stochastic policy, not only argmax; (4) then the strict held-out evaluation protocol.

## Campaign 2 results (Muse, 2026-11-15, protocol2.md)

Scaled campaign at the user's instruction to run the GPU at full scale: started from g03, prior annealed 1.0 → 0.5 → 0.25 → 0.0, 12 generations × 48,000 games = 576,000 games, 192 PPO epochs total (no KL early stops), lr 8e-4, KL stop 0.08, ~16 minutes wall-clock on the RTX 4070. Final weights `campaign2/models/g12.bin`, SHA-256 `858500f293781166afee3fbb3b30eb012676b7c537c1f00ff47618fa462f6928`. Fallbacks 0; verify.py passes.

Headline, fresh seed 20261601, both seats, 3,000 games per population/opponent/seat:

| Population | vs obsh2 | vs generic | vs learned (mirror) |
|---|---:|---:|---:|
| final168 × final168 | **58.93 [57.20, 60.53]** | **57.55 [55.73, 59.23]** | 49.52 [47.77, 51.20] |
| final168 × bench8_ST | 68.05 [66.38, 69.82] | 69.22 [67.62, 70.85] | 51.63 [49.78, 53.48] |

Stage 3's final table for comparison: 50.80 / 47.42 on final168; 72.70 / 68.92 on bench8_ST. The stage-3 seed (20260501) rerun agrees: 59.67 [58.00, 61.30] vs obsh2 on final168. Per-generation snapshots climbed from 51.4 (gen 1) into a noisy 55–60 band and held after the prior reached 0.

Reading: under matched information the learned policy now beats obsh2 by ~9 points on the finalist distribution, and it also beats the full-information generic heuristic (57.55) — the information handicap against that opponent is overcome. The cost is visible on the bench pairing, where vs-obsh2 dropped from 72.70 to 68.05: the policy specialized toward the finalist-mirror distribution that dominates its training mixture. These are pilot-vs-pilot results on named build populations, not unseen-build generalization; that claim belongs to stage 4's held-out protocol. Campaign-2 weights are not promoted to `stage3/weights.h`.

## Stage 4 — held-out evaluation (Muse, 2026-11-15, stage4_protocol.md)

Held-out pool: 176 builds sampled evenly from run1/candidates2.txt (99,162 stage-2 expanded builds) after prepare.py-identical adaptation, excluding all 176 training-population builds and duplicates (verified overlap 0; 98,994 unique non-training builds available). Both pairings are fully held-out: heldout168 × heldout168 and heldout168 × heldout8. Identical protocol for the subject (campaign-2 g12) and the control (untrained bootstrap): seed 20261701, 3,000 games per pairing/opponent/seat, 36,000 games each, fallbacks 0.

Both seats, heldout168 × heldout168:

| Learner | vs obsh2 | vs generic | vs learned (mirror) |
|---|---:|---:|---:|
| g12 (learned) | **73.72 [72.21, 75.20]** | **74.60 [73.12, 76.08]** | 49.13 [47.43, 50.89] |
| bootstrap (control) | 49.18 [47.36, 50.93] | 48.33 [46.52, 50.10] | 49.18 [47.47, 50.88] |

Heldout168 × heldout8: g12 scores 75.02 vs obsh2 and 74.18 vs generic; the control scores 55.13 / 50.92. The control's above-50 scores in this pairing (including 55.13 in the identical-pilot mirror row) measure build-pool asymmetry between the 168 and the 8, which is why conclusions rest on the g12-minus-control differences on identical builds and seeds: +24.5pt vs obsh2 and +26.3pt vs generic on the main pairing, +19.9pt vs obsh2 on the bench pairing.

Conclusion: the learned improvement is not confined to the training pool — it is larger on unseen builds (+24.5pt) than on the training pool (+8.1pt over the same control baseline). A plausible mechanism is that the hand-written obsh2 branches, developed against finalist-style builds, misfire more often off-distribution, while the learned value/policy generalizes across the build space; weaker average held-out builds also let pilot skill express more strongly. Scope limit: the held-out builds come from the same generator, 17-move space and spread family as training; generalization to the full 68-move space (stage 6) remains untested. Recommendation: promote g12 to stage3/weights.h as the production learned policy (decision left to the user).

## Stage 5 (run2, 2026-11-15): re-ranking under the learned policy
Full protocol and execution record: run2/protocol_run2.md. Headlines:
- Stage A #1 was a regular balloon Clear Body build (0.8875 vs pool8), but it did
  not survive stage B's iterated pool: all 168 run2 finalists are Mega.
- Stage B's top family was BD/SB/REST/TRK (special-leaning, C144); in the finals
  RR its best build placed 18th of 168.
- Finals #1 by average: 187,165,158,132,140,155 BD/EQ/SB/SUB (avg 0.775) — but
  its worst matchup is 0.003. NO finalist keeps a worst matchup >= 0.30 (run1's
  generic-policy finals had 3 builds at >= 0.35): under learned piloting the
  rock-paper-scissors structure is sharper, not softer.
- Run1's #1 build did not reach the run2 finals at all.
