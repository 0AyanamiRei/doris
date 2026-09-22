# BE CDC reader

This module implements the BE components for the agreed first CDC phase:

- MongoDB Kafka Source Connector's complete Change Stream **JSON** envelope.
- DynamoDB's native Kinesis destination **JSON** envelope.
- Full after-image upserts and deletes whose business keys are in the message value/Data.
- A fixed output Block schema, provided explicitly by the caller.

The module is not wired into `FileScanner` or FE/Thrift configuration yet. An existing
Routine Load SQL statement cannot select it. These files and their tests have not been
compiled or executed as part of this change, at the user's request.

## Components

| File | Responsibility |
| --- | --- |
| `cdc_event.h` | Owned normalized key/after JSON, original key type wrappers, operation and source namespace. |
| `cdc_decoder.h/.cpp` | Decoder factory, one-message validation and shared JSON mechanics. |
| `mongodb_cdc_decoder.h/.cpp` | `operationType`, `documentKey`, `fullDocument`, supported Extended JSON normalization. |
| `dynamodb_cdc_decoder.h/.cpp` | `eventName`, `Keys`, `NewImage`, recursive AttributeValue normalization. |
| `cdc_row_mapper.h/.cpp` | Explicit source-field to output-column projection, delete signs, null/default handling and row rollback. |
| `cdc_reader.h/.cpp` | Read complete Pipe messages, select the decoder, produce bounded Blocks and propagate errors/cancellation. |

```text
existing Consumer -> existing StreamLoadPipe
                             |
                         CdcReader
                             |
               MongoDB / DynamoDB Decoder
                             |
                         CdcEvent
                             |
                       CdcRowMapper
                             |
          NewJsonReader::write_json_value_to_column
                             |
                      source data Block
                             |
           existing load projection / sink (future integration)
```

The shared conversion method stays in `new_json_reader.h/.cpp`. No separate
`json_value_converter` files or changes to ordinary JSON row handling are introduced here.

## BE entry point

The caller supplies a protocol and the **source Block schema** that its load plan expects:

```cpp
std::vector<CdcColumnMapping> columns {
    {"id", "_id", std::make_shared<DataTypeInt64>(), CdcColumnSource::KEY},
    {"name", "name", make_nullable(std::make_shared<DataTypeString>()),
     CdcColumnSource::AFTER},
    {"__DORIS_DELETE_SIGN__", "", std::make_shared<DataTypeInt8>(),
     CdcColumnSource::DELETE_SIGN},
};

CdcReader reader(pipe, CdcFormat::MONGODB_CHANGE_STREAM, std::move(columns), 4096);
RETURN_IF_ERROR(reader.init_reader());
Block block = reader.create_block();
size_t rows = 0;
bool eof = false;
RETURN_IF_ERROR(reader.get_next_block(&block, &rows, &eof));
```

An optional final constructor argument checks a source namespace (`db.collection` for
MongoDB, table name for DynamoDB). When omitted, source selection is the upstream
connector/topic's responsibility; there is no automatic target-table routing.

`KEY` reads normalized key fields. Its source key domain and destination type must be
compatible: for example, an integral MongoDB `_id` to BIGINT or a DynamoDB string key
to a string column. MongoDB `_id` must be mapped. DynamoDB requires mappings for every
key component, including a sort key when present.

`RAW_KEY` writes a source key value as compact JSON text, retaining source type wrappers.
For example, MongoDB ObjectId `{"$oid":"..."}` remains different from a string `"..."`.
Use this string projection when the source uses heterogeneous identifier types. Fix the
upstream serializer and key representation; the component does not infer schema changes
or canonicalize all numerically equivalent key encodings. MongoDB shard-key changes still
need an explicit identity/migration contract.

`AFTER` reads a top-level field of the complete normalized after image. Missing nullable
fields become null; a missing non-nullable field needs an explicit constant `default_json`
or fails. Explicit null never uses the default. This replaces a complete row; it does not
retain columns from an older row. Duplicate mappings of the same source field within
KEY/AFTER projections are rejected to keep ondemand-parser ownership explicit.

Exactly one `DELETE_SIGN` projection is required, with a non-nullable TINYINT type. It is
generated from the operation and cannot be overwritten by a field in the source document.
Delete events produce keys, a delete sign of 1 and typed placeholders for AFTER columns.
The eventual FE plan must preserve this marker and validate the target table/constraints.

## Protocol rules and representation

MongoDB `insert`, `update` and `replace` produce UPSERT; `delete` produces DELETE. Other
operations, missing/null `fullDocument`, missing/null keys, and disagreement between
document keys and after-image keys fail. Requiring an after image does not prove that
`updateLookup` represents the exact event-time image: the upstream post-image contract
remains necessary.

MongoDB ObjectId and numeric Extended JSON wrappers are normalized. Numbers are preserved
as JSON number text without a double conversion. Integer wrappers are range checked;
non-finite values are rejected. Other Extended JSON objects, such as date/timestamp/binary
wrappers, retain their object representation; they are not automatically converted to a
Doris date or binary type. Nested objects and arrays are supported.

DynamoDB INSERT/MODIFY produce UPSERT; REMOVE produces DELETE. S, N, BOOL, NULL, M and L
are normalized recursively. N accepts either a numeric string or a JSON number and keeps
its numeric text. SS/NS project to arrays without a set-order guarantee. B/BS retain
their tagged encoded representation; no base64 layer is guessed or removed. Unknown,
empty or multiple type tags fail. This is not the DynamoDB Streams API or Lambda Records
batch protocol.

The decoder validates the complete envelope, including ignored metadata, UTF-8, duplicate
fields and a nesting limit of 64. Events own their normalized JSON strings. This avoids
views escaping parser/message lifetimes, but entails normalization and subsequent parsing
by the row mapper. No zero-copy or throughput improvement is claimed; reducing these
passes is a separate optimization after correctness tests are enabled.

## Failure and Pipe contract

The mapper constructs each event in a temporary typed row and appends it only after all
conversions succeed. Complex columns use the existing strict SerDe interface, avoiding
silent replacement of invalid nested values with null. Partial nullable/complex values
never enter the caller's Block; preceding rows and shared snapshots are preserved. The reader
discards rows produced by the failed `get_next_block` call, cancels its Pipe to release
blocked producers, and retains the error for later calls. It does not skip invalid CDC
events using an error-row ratio. There is no implicit heartbeat/tombstone NOOP policy.

A small compatibility addition to `StreamLoadPipe` supplies an optional output parameter:
`read_one_message(data, length, bool* eof = nullptr)`. Existing calls omit the parameter;
CDC supplies it to distinguish an empty record from a drained stream, rejecting empty
messages instead of stopping early and losing subsequent events.

The reader does not commit offsets, generate a source version, deduplicate source events,
or reorder events. Those remain responsibilities of the transaction/source-version
integration. Kinesis checkpoint/reshard and delete-after-compaction replay concerns from
the design investigation remain unresolved by this format module.

## Tests added, not run

`be/test/format/cdc/` contains decoder tests, mapper tests and in-memory Pipe/reader tests.
They cover full-image operations, key-only deletion, numeric precision, malformed events,
composite keys, missing versus null, defaults, row rollback, shared Block ownership,
bounded batches, cancellation and empty-message versus EOF behavior. Existing CMake globs
already include these source/test directories; no additional build-system registration
is needed.
