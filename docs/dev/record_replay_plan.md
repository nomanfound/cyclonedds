# Cyclone DDS Record & Replay Plan

## Goals and Scope

* Provide a persistent capture format that is IDL agnostic by storing the
  serialized samples together with all type information needed for replay.
* Focus this iteration on capturing topic type metadata and serialized data
  payloads. QoS, security and discovery state are considered out of scope for
  now.
* Enable later extensions (QoS, timing control, security context) by keeping
  the container format chunk-based and versioned.

## Recorder Architecture

1. **Entity introspection**
   * Use `dds_get_typeinfo` on a writer or reader to obtain the XTypes
     description of the associated topic type without requiring the generated
     IDL code.
   * Convert the returned `dds_typeinfo_t` into a `dds_topic_descriptor_t` via
     `dds_create_topic_descriptor`. This yields the serializer meta-data
     (`m_ops`, `m_keys`, `m_meta`) alongside the serialized
     `TypeInformation/TypeMapping` blobs.
   * Fetch the topic entity and name through `dds_get_topic` and `dds_get_name`
     so the recorder can map samples to logical topic names during replay.

2. **File layout**
   * Emit a fixed header that contains a magic identifier, format version and
     reserved flags.
   * Persist data as a sequence of self-describing chunks:
     - **TYPE** chunks encode a type identifier together with
       `dds_topic_descriptor_t` fields (`m_size`, `m_align`, `m_flagset`,
       serialized key descriptors, marshaling opcodes, metadata XML, serialized
       `TypeInformation`/`TypeMapping`, `restrict_data_representation`).
     - **TOPIC** chunks bind a topic identifier to a type identifier and the
       discovered topic name.
     - **SAMPLE** chunks carry the topic identifier, data representation id,
       source timestamp and serialized payload bytes.
   * Use little-endian layout for all numeric fields and store string lengths to
     avoid null-termination assumptions.

3. **Recording workflow**
   * When an entity is registered, deduplicate type definitions by comparing the
     serialized `TypeInformation` blob (if available). Only emit a TYPE chunk if
     a new type is encountered.
   * Allocate topic ids sequentially and emit a TOPIC chunk for each new topic
     registration.
   * When samples arrive, capture their serialized representation (e.g. via loan
     or `dds_writecdr`) together with the data representation flag and a
     timestamp and persist them as SAMPLE chunks.
   * Flush the writer on demand and at close to guarantee durability.

## Replayer Architecture

1. **Chunk reader**
   * Parse the file header and validate magic/version.
   * Iterate chunk-by-chunk, exposing a stream of records to the caller.
   * For TYPE chunks, reconstruct an in-memory representation mirroring
     `dds_topic_descriptor_t` so that the replayer can re-register serializers
     (`dds_create_topic_descriptor` / `dds_topic_descriptor_fini`).
   * For TOPIC chunks, populate a mapping of topic id to name and type id.
   * For SAMPLE chunks, expose the serialized payload, associated topic id,
     source timestamp and data representation so the caller can schedule writes.

2. **Replay workflow**
   * Allow applications to build replayers by iterating the stream and
     re-registering types/topics on a domain participant before re-publishing
     serialized samples using Cyclone DDS APIs (e.g. `dds_writecdr_ex`).
   * Keep replay scheduling policy out of scope for now; the API simply provides
     timestamps so callers can decide how to pace playback.

## Public API Surface

* **Recorder**
  - `dds_record_writer_open` / `dds_record_writer_close`
  - `dds_record_writer_register_entity` (introspects a reader/writer) and
    `dds_record_writer_register_descriptor` (manual registration) returning a
    topic id.
  - `dds_record_writer_write_serialized` to append serialized samples along with
    timestamps and representation ids.
  - `dds_record_writer_flush` for explicit durability control.

* **Replayer**
  - `dds_record_reader_open` / `dds_record_reader_close`
  - `dds_record_reader_next` producing a discriminated union of TYPE, TOPIC or
    SAMPLE records.
  - `dds_record_entry_fini` to free dynamically allocated payloads/strings
    returned by `dds_record_reader_next`.

## Extensibility and Future Work

* Add QoS snapshots, discovery traffic and writer/reader lifecycle events as new
  chunk kinds once type/data capture is stable.
* Record relative timing metadata and provide scheduling helpers for playback.
* Introduce integrity checksums per chunk and optional compression.
* Support secure deployments by capturing cryptographic context or by requiring
  recording after decryption.

This plan defines the minimal infrastructure to persist type descriptions and
serialized payloads so that Cyclone DDS data can be replayed without access to
original IDL artifacts. Subsequent iterations can layer richer metadata without
breaking compatibility thanks to the chunked file format.
