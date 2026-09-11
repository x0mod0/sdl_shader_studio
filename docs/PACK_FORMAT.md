# Pack format (`.s3pack`, v1.0)

All integers are little-endian. Offsets are absolute, from the start of the file.

The format is customizable: what follows describes the *shape*, and the header
records which choices were made. A generated `s3pack.h` embeds a signature string
like `S3PK/v1.0/compact/by_key/table_first/key32/align16/names` and refuses a
pack whose magic or version does not match, rather than misreading it.

## Field width

Two header presets change how every 32-bit field is stored:

| Preset | Field size | Alignment | Use |
|---|---|---|---|
| `compact` | 4 bytes | 4 | default; smallest |
| `padded64` | 8 bytes (high half zero) | 8 | header/entries castable to a struct on any ABI |

Below, *field* means "4 bytes, or 8 with the high half zero under `padded64`".

## Header

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 | magic, four printable ASCII bytes (default `S3PK`) |
| 4 | 2 | version major (`1`) |
| 6 | 2 | version minor (`0`) |
| 8 | field | flags |
| .. | field | shader count |
| .. | field | format mask (union of all blob formats) |
| .. | field | stage mask (bit per stage) |
| .. | field | compression id: 0 none, 1 LZ4, 2 zstd |
| .. | field | blob alignment (16, 64 or 256) |
| .. | field | entry table offset |
| .. | field | first blob offset |
| .. | field | string table offset |
| .. | field | user section offset (0 if absent) |
| .. | field | user section size |

The header is padded to its alignment. Note that flags sit at a fixed offset 8 in
both presets, so a reader can learn the preset before decoding anything else.

### Flags

| Bit | Meaning |
|---|---|
| 0 | at least one blob is compressed |
| 1 | reflection JSON is stored |
| 2 | shader names are stored |
| 3 | compute-only pack |
| 4 | a user section is present |
| 5 | keys are 16-bit |
| 6 | entries are in manifest order (linear scan, not sorted) |
| 7 | the blob section precedes the entry table |
| 8 | `padded64` header preset |

## Entry table

One entry per shader, each padded to `entry_size(blob_count)`:

```
key                  2 or 4 bytes   (16-bit when flag 5 is set)
stage                1 byte         0 vertex, 1 fragment, 2 compute
language             1 byte         0 HLSL, 1 GLSL
blob_count           1 byte
num_samplers         1 byte
num_storage_textures 1 byte         read-only count for compute
num_storage_buffers  1 byte         read-only count for compute
num_uniform_buffers  1 byte
num_rw_storage_textures 1 byte      compute only, else 0
num_rw_storage_buffers  1 byte      compute only, else 0
reserved             1 byte
entry_point_offset   field          1-based into the string table, 0 = absent
name_offset          field          1-based, 0 when names were stripped
reflection_offset    field          absolute, 0 when reflection was stripped
reflection_size      field
thread_count_x/y/z   3 fields       1,1,1 for graphics shaders
format_mask          field          which formats this entry stores
blob[i].offset       field  ]
blob[i].size         field  ]       repeated blob_count times, ascending format
blob[i].uncompressed field  ]
```

The storage counts are split read-only/read-write precisely so a loader can fill
`SDL_GPUComputePipelineCreateInfo` without parsing reflection.

Blobs are written in ascending `SDL_GPU_SHADERFORMAT_*` value order, so a reader
recovers each blob's format by walking the set bits of `format_mask` in the same
order. `size == uncompressed_size` means the blob is stored raw even in a
compressed pack, which is what happens below the compression threshold.

When entries are sorted by key (the default), a loader binary-searches. With
`manifest_order`, flag 6 is set and the loader scans linearly.

## String table

A leading NUL byte, then NUL-terminated UTF-8 strings, deduplicated. Offsets are
1-based so that 0 can mean "absent".

## Reflection and user sections

Reflection, when kept, is one compact JSON document per shader at the offset the
entry records. The user section is arbitrary bytes the build wrote (a CI id, a
content hash); nothing in the loader interprets it.

## Alignment

Every blob starts on the recorded alignment boundary, so a loader can hand a
pointer straight to the driver where the API allows it. 256 suits platforms with
strict mapping requirements; 16 is the default.

## Compression

Only blobs at or above the profile's threshold are compressed, and only if the
result is smaller. If the codec was unavailable at build time the packer stores
raw and warns; it never writes a pack it could not read back. The generated
loader fails loudly when a compressed pack is opened without a decoder macro
(`S3PACK_LZ4_DECODE` / `S3PACK_ZSTD_DECODE`) rather than passing garbage to the
driver.
