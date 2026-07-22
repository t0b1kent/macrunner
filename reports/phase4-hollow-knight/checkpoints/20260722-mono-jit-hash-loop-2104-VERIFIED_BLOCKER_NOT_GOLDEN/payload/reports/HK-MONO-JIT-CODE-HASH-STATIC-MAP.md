# HK Mono JIT Code Hash Static Map

Classification: `STATIC_MAP_PASS_NOT_GOLDEN`

This note maps the stable post-scene guest loop captured in
`laneA-post-scene-guest-loop-capture-hash-retraction-20260722-2104`. It does not
claim a product fix or a visible-pixel result.

## Exact binary identity

- Binary: `MonoBleedingEdge/EmbedRuntime/mono-2.0-bdwgc.dll`
- SHA-256: `c9c3d552f0e2abaa19d7233e9595290ae10812642a13486a94bcf3d8f7a6e365`
- Captured runtime module base: `0x87ef2400000`
- Captured guest PC: `0x87ef2470d79`
- Captured RVA: `0x70d79`

The byte sequence at RVA `0x70d79` is unique in this PE:

```text
48 3b c6 74 20 48 8b cb ff 57 10 48 8b 18 48 85 db 75 e7
```

## Function and data structure

RVA `0x70d20` is `mono_internal_hash_table_lookup`. Its loop is:

```text
key_extract(current) == requested_key ? return current
next_address = next_value(current)
current = *next_address
repeat while current != NULL
```

The exact caller is the `lookup_method` path in Unity Mono's
`mono/mini/mini-runtime.c`. The call at RVA `0x2832f0` passes:

- `rcx = domain + 0xc0`, the address of `MonoDomain::jit_code_hash`;
- `rdx = MonoMethod *`, the lookup key;
- return RVA `0x2832f5`.

The captured state is consistent with that caller:

- table `rdi = 0x126862de0`;
- domain `r14 = 0x126862d20`;
- `rdi == r14 + 0xc0`;
- requested `MonoMethod *` in `rsi/rbp = 0xf1d6286d0`;
- current `MonoJitInfo *` in `rbx = 0x709cd8af20`;
- current node key in `rax = 0x709ced2da0`, not equal to the requested key;
- current bucket index in `rdx = 0x2b3`.

The table callbacks are established by Unity Mono source and the exact binary:

- `hash_func = mono_aligned_addr_hash`;
- `key_extract(node) = node->d.method`, at node offset `0x0`;
- `next_value(node) = &node->n.next_jit_code_hash`, at node offset `0x8`.

Therefore the chain can be adjudicated directly from guest memory without
calling guest code: each node is `{ method = *(node+0), next = *(node+8) }`.

## Insert path and remaining hypotheses

RVA `0x70ef0` is `mono_internal_hash_table_insert`. The JIT registration caller
at RVA `0x27ddce` inserts the `MonoJitInfo` at `[compile_state+0x78]` into
`domain->jit_code_hash` using the node's method as key.

The insertion routine asserts:

- `table->key_extract(value) == key`;
- `*(table->next_value(value)) == NULL`.

Those checks do not by themselves exclude reinserting the same sole bucket-head
node: its next field can still be NULL immediately before reinsertion, after
which assigning the old bucket head creates a self-edge. This is only a
hypothesis until the live chain is read.

The next direct measurement must distinguish:

1. a self-cycle or multi-node cycle already present in `MonoJitInfo::next_jit_code_hash`;
2. a finite memory chain but an incorrect result from the indirect
   `next_value` call/return path in HyperBridge;
3. neither, which leaves the current account inconsistent and must remain
   `UNKNOWN`.

No decoder or instruction-family defect is proven by the existing byte/IR pair.

## Primary source correspondence

- Unity-Technologies Mono, `mono/utils/mono-internal-hash.c`
- Unity-Technologies Mono, `mono/metadata/jit-info.c`
- Unity-Technologies Mono, `mono/metadata/domain-internals.h`
- Unity-Technologies Mono, `mono/mini/mini-runtime.c`
