# DragonGL+ — extension of the network protocol

DragonGL+ speaks Dragon GL's protocol v5 (see [PROTOCOL.md](PROTOCOL.md)) and adds a few
server → client messages that the DragonGL+ client uses for its windows, creature models
and effects. Nothing changes for a Dragon GL client: it never receives them.

## Negotiation

The extension is agreed in the TLS handshake (ALPN, `src/shared/tls_common.c`):

| Peer | Offers / accepts | Result |
|---|---|---|
| DragonGL+ client | `dragongl-plus/5.1`, then `dragongl/5` | extension with a DragonGL+ server, plain v5 with a Dragon GL server (compatibility mode) |
| DragonGL+ server | prefers `dragongl-plus/5.1`, accepts `dragongl/5` | extension only for clients that offer it (`conn_sock_is_plus()`) |
| Dragon GL client | `dragongl/5` | plain v5 |

## Frames

Every frame keeps the v5 header (version 5, type, length). The extension uses message
types from 256, so they never collide with v5. Payloads follow the v5 rules: every field
written explicitly in little-endian, strings as a `u8` length followed by the bytes, and a
payload must decode completely. Codec: `include/wire_plus.h`, `src/shared/wire_plus.c`; types:
`include/protocol_plus.h`; tests: `tests/test_protocol_plus.c`, `tests/integration/test_plus.py`.

| Type | Message | Sent | Payload |
|---:|---|---|---|
| 256 | `PLUS_ENTITY_INFO` | after the `MSG_STATE` that introduces a creature or player | `i32` id, `u8` kind (`KIND_*`), `u8` archetype, `u8` level, `i8` race, `i8` class, `u32` look seed, `str(32)` name |
| 257 | `PLUS_PLAYER_EXT` | when the hero's progress changes | `i32` XP of the next level, `i32` unspent ability points, `i32` deepest floor |
| 258 | `PLUS_INVENTORY` | when backpack, belt or equipment change | `u8` count (≤ 64), `f32` weight, `f32` capacity (kg), then per item: `str(48)` name, `u8` category, `u8` where, `u8` slot, `u8` flags, `u16` quantity, `i16` durability, `i16` max durability, `i16` damage dice count and sides, `i16` AC, `i16` heal, `i16` light, `u32` value, `f32` weight |
| 259 | `PLUS_SHOP` | when the hero talks to a merchant, the stock changes, or the hero steps away (merchant id 0: closed) | `i32` merchant id, `str(64)` shop name, `u8` count (≤ 50), then per item: `str(40)` name, `str(12)` type, `str(24)` stats, `u32` price, `i16` stock, `u8` category |
| 260 | `PLUS_SPELLBOOK` | when the known spells change | `u16` count (≤ 160), then per spell: `str(40)` name, `i8` level, `u8` target, `u8` effect, `u8` damage type, `i16` range, `i16` radius |
| 261 | `PLUS_GAME_EVENT` | attacks, kills, spell damage, level up, death, boss defeated | `u8` type (`GEV_*`), `i32` source id, `i32` target id, `i32` value, `i16` x, `i16` y |

Inventory, spellbook, progress and shop are sent only when they change (a hash per
client in `src/server/server_plus.c`).
