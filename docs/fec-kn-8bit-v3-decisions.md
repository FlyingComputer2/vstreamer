# FEC 8-bit k/n — design decisions

Branch: `cursor/fec-kn-8bit-5121`

## Scope

- **8-bit `k`, `n`, `shard_idx`, and `sdu_n`** on the FEC shard header (ISA-L supports `n ≤ 255`).
- Stream **`k_stream_wire_version = 3`**; v2 peers fail version check (same rule as v1→v2).
- **`sdu_base` remains u16 BE** (8-bit block index is a separate follow-up in `fec-block-index-v3-decisions.md`).

## FEC shard header (6 bytes before `orig_len`)

| Off | Size | Field |
|-----|------|--------|
| 0–1 | 2 | `sdu_base` u16 BE |
| 2 | 1 | `k` u8 |
| 3 | 1 | `n` u8 |
| 4 | 1 | `shard_idx` u8 |
| 5 | 1 | `sdu_n` u8 |

## Encode matrix cache

Static `32×32` grid replaced with an **LRU of 32** `(k,n)` entries (on-demand Cauchy matrix generation).

## Deployment

Upgrade sender and receiver together. Winject forwards opaque UDP; no L3 change.
