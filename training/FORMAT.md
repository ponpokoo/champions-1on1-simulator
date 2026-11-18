# Stage 3 interchange formats

All multibyte integers and IEEE-754 floats are **little endian**, with no native struct serialization. Readers reject dimension/version mismatches and nonfinite parameters. Model float32 hash is independent of C text formatting.

## MGT3 trajectory, version 1

64-byte file header:

| Offset | Type | Meaning |
|---:|---|---|
| 0 | 4 bytes | `MGT3` |
| 4 | u32 | format version = 1 |
| 8 | u32 | reconstructed MGO1 words = 256 |
| 12 | u32 | active words = 181 |
| 16 | u32 | record bytes = 390 |
| 20 | u32 | games |
| 24 | u64 | collection seed, **metadata only** |
| 32 | 32 bytes | SHA-256 of behavior model's canonical float payload |

Each record is one learner pre-choice boundary; nominal choices while asleep are included. Sleep Talk's automatic call is never an additional action. Records are grouped in game order and then turn order.

| Offset | Type | Meaning |
|---:|---|---|
| 0 | 181 × i16 | MGO1 v[0..180], except v[117] stored as zero |
| 362 | u32 | full v[117] Scarf-lock belief bitset |
| 366 | u8 | legal bits 0..4, own slots then Struggle |
| 367 | u8 | chosen action 0..4 |
| 368 | u8 | learner outcome: loss 0 / draw 1 / win 2 |
| 369 | u8 | learner engine seat, 0 A / 1 B |
| 370 | u8 | opponent: generic 0 / obsh2 1 / obsdemo 2 / snapshot index + 3 |
| 371 | u8 | final game turn count |
| 372 | u16 | own population ID, metadata only |
| 374 | u16 | foe population ID, metadata only |
| 376 | u32 | game ID, metadata/grouping only |
| 380 | f32 | log behavior probability of selected action |
| 384 | f32 | frozen behavior value |
| 388 | u8 | obsh2's action from this same observation/mask (fixed policy prior) |
| 389 | u8 | reserved zero |

Reconstruct v[117] from offset 362 and v[181..255] as zero, then prepend ASCII `MGO1` + u32 256 and serialize 256 signed LE32 words to recover the existing **exact** canonical observation. No observation semantics or precision change. Writer asserts all remaining active values fit i16 and reserved words are zero, rather than truncating. The duplicated mask must equal v[176..180]. Data loader validates profile, terminal flag, legal selected/prior actions, group consistency, decision counts and timings. Gzip is transport compression; the format inside is identical.

Only the reconstructed observation and mask go to the encoder. The prior action is a pure obsh2 transform of those same inputs; outcome/behavior estimates are training targets or PPO bookkeeping. IDs, seed, opponent, seat and final turn count never enter actor or critic inputs. Population provenance and evaluation identifiers are isolated from the network.

## MWP1 network

16-byte header: ASCII `MWP1`, u32 415 inputs, u32 64 hidden1, u32 32 hidden2. Payload comprises contiguous float32 arrays in order:

`w1[64,415], b1[64], w2[32,64], b2[32], wp[5,32], bp[5], wv[1,32], bv[1], prior[1]`.

Matrix rows are output units; affine is `x @ w.T + b`. ReLU hidden layers; tanh value; add `prior=2.0` to obsh2's chosen action logit, then mask illegal actions before softmax (training) / deterministic argmax (deployment; lowest-slot tie break). There are 28,902 learned parameters plus one fixed prior float, 115,628 bytes including header. SHA-256 covers the entire **payload including prior**, excluding the 16-byte dimension header; schema and encoder names accompany it. C header is generated with 9 significant decimal digits for round-trip float32 values.

Encoder `mgo1_residual_v1`: first 181 words scaled by the frozen `scales.h` and clipped [-1,1]; numeric copies of own/revealed move IDs, item IDs and belief bitsets are zeroed. Append eight 17-way move one-hots gated by occupancy/revelation, own and known-foe 35-way item one-hots, 18 lock bits, 4 ability-belief bits and two 3-bit duration sets. Total 415. Unknown is represented by the original validity fields and zero one-hot payloads. `model.py` and `policy.h` implement the same encoder, with exact encoder parity required.

## Evaluation records

`prefix.csv` indexes blocks in `prefix.bin`; each game has two bytes: doubled learner score (0/1/2), turns (1..100). CSV retains population/opponent/seat, n, W/L/D, turn total, cap count, byte offset and fallback count. Population/seed/build-sampling algorithms are fixed in `loop.c`/`protocol.md`; `analyze.py` independently reconstructs all aggregate counts before reporting CIs.
