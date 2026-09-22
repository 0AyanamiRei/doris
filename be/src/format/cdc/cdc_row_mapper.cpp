// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "format/cdc/cdc_row_mapper.h"

#include <simdjson/simdjson.h>

#include <utility>

#include "core/assert_cast.h"
#include "core/block/block.h"
#include "core/column/column_nullable.h"
#include "core/column/column_string.h"
#include "core/column/column_vector.h"
#include "core/data_type/primitive_type.h"
#include "core/string_ref.h"
#include "format/cdc/cdc_decoder.h"
#include "format/json/new_json_reader.h"

namespace doris {
namespace {

Status append_value(simdjson::ondemand::value& value, IColumn& column, const DataTypeSerDe& serde,
                    const DataTypePtr& type) {
    if (value.is_null().value()) {
        if (!is_column_nullable(column)) {
            return Status::DataQualityError("CDC null cannot be written to a non-nullable column");
        }
        column.insert_default();
        return Status::OK();
    }

    IColumn* data_column = &column;
    const DataTypeSerDe* data_serde = &serde;
    ColumnNullable* nullable = nullptr;
    if (is_column_nullable(column)) {
        nullable = &assert_cast<ColumnNullable&>(column);
        data_column = &nullable->get_nested_column();
        data_serde = serde.get_nested_serdes()[0].get();
    }
    DataTypeSerDe::FormatOptions options;
    if (is_complex_type(type->get_primitive_type())) {
        // The ordinary JSON SerDe may turn failed nested conversions into NULL.
        // CDC must report those failures instead of applying a different document.
        std::string_view text = value.type() == simdjson::ondemand::json_type::string
                                        ? value.get_string().value()
                                        : simdjson::to_json_string(value).value();
        StringRef input(text.data(), text.size());
        RETURN_IF_ERROR(data_serde->from_string_strict_mode(input, *data_column, options));
    } else {
        RETURN_IF_ERROR(NewJsonReader::write_json_value_to_column(value, *data_column, *data_serde,
                                                                  options));
    }
    if (nullable != nullptr) {
        nullable->get_null_map_data().push_back(0);
    }
    return Status::OK();
}

Status append_default(const CdcColumnMapping& mapping, IColumn& column,
                      const DataTypeSerDe& serde) {
    if (mapping.default_json.has_value()) {
        // Wrapping also permits a scalar default to be accessed as an ondemand::value.
        simdjson::padded_string json("{\"value\":" + *mapping.default_json + "}");
        simdjson::ondemand::parser parser;
        auto document = parser.iterate(json);
        simdjson::ondemand::value value = document["value"];
        return append_value(value, column, serde, mapping.type);
    }
    if (is_column_nullable(column)) {
        column.insert_default();
        return Status::OK();
    }
    return Status::DataQualityError("CDC after image is missing required column '{}'",
                                    mapping.source_name);
}

void append_raw_key(simdjson::ondemand::value& value, IColumn& column) {
    IColumn* data_column = &column;
    ColumnNullable* nullable = nullptr;
    if (is_column_nullable(column)) {
        nullable = &assert_cast<ColumnNullable&>(column);
        data_column = &nullable->get_nested_column();
    }
    std::string_view json = value.raw_json().value();
    assert_cast<ColumnString&>(*data_column).insert_data(json.data(), json.size());
    if (nullable != nullptr) {
        nullable->get_null_map_data().push_back(0);
    }
}

} // namespace

CdcRowMapper::CdcRowMapper(std::vector<CdcColumnMapping> columns) : _columns(std::move(columns)) {}

Status CdcRowMapper::init() {
    DORIS_CHECK(!_initialized);
    std::unordered_set<std::string> names;
    std::unordered_set<std::string> after_names;
    size_t delete_signs = 0;
    _serdes.clear();
    _key_names.clear();
    for (const auto& column : _columns) {
        DORIS_CHECK(column.type != nullptr);
        if (column.column_name.empty() || !names.emplace(column.column_name).second) {
            return Status::InvalidArgument("CDC output column names must be non-empty and unique");
        }
        if (column.source == CdcColumnSource::DELETE_SIGN) {
            if (column.type->get_primitive_type() != TYPE_TINYINT || column.type->is_nullable()) {
                return Status::InvalidArgument("CDC delete sign must be a non-nullable TINYINT");
            }
            ++delete_signs;
        } else {
            if (column.source_name.empty()) {
                return Status::InvalidArgument("CDC source column name must not be empty");
            }
            if (column.source == CdcColumnSource::AFTER) {
                if (!after_names.emplace(column.source_name).second) {
                    return Status::InvalidArgument("Duplicate CDC after-field mapping");
                }
            } else {
                if (!_key_names.emplace(column.source_name).second) {
                    return Status::InvalidArgument("Duplicate CDC key-field mapping");
                }
                if (column.source == CdcColumnSource::RAW_KEY &&
                    !is_string_type(column.type->get_primitive_type())) {
                    return Status::InvalidArgument(
                            "CDC raw key projection requires a string column");
                }
            }
        }
        if (column.default_json.has_value() && column.source != CdcColumnSource::AFTER) {
            return Status::InvalidArgument("CDC defaults only apply to after-image columns");
        }
        if (column.default_json.has_value()) {
            RETURN_IF_ERROR(cdc_json::validate_json(*column.default_json));
        }
        _serdes.emplace_back(column.type->get_serde());
    }
    if (_key_names.empty() || delete_signs != 1) {
        return Status::InvalidArgument("CDC mapping requires business keys and one delete sign");
    }
    _initialized = true;
    return Status::OK();
}

Block CdcRowMapper::create_block() const {
    DORIS_CHECK(_initialized);
    Block block;
    for (const auto& mapping : _columns) {
        block.insert({mapping.type->create_column(), mapping.type, mapping.column_name});
    }
    return block;
}

Status CdcRowMapper::append(const CdcEvent& event, Block* block) const {
    DORIS_CHECK(_initialized);
    DORIS_CHECK(block->columns() == _columns.size());
    const size_t old_rows = block->rows();
    for (const auto& column : block->get_columns_with_type_and_name()) {
        DCHECK(column.column->size() == old_rows);
    }
    // A failing complex SerDe can leave nested data without updating an outer
    // nullable size/array offset. Stage the event so those partial values never
    // enter the caller's Block, rather than trying to reconstruct their sizes.
    Block row = create_block();
    Status status;
    try {
        status = _append(event, &row);
    } catch (const simdjson::simdjson_error& error) {
        status = Status::DataQualityError("Invalid CDC field value: {}", error.what());
    }
    RETURN_IF_ERROR(status);
    for (size_t i = 0; i < _columns.size(); ++i) {
        DCHECK(row.get_by_position(i).column->size() == 1);
        auto guard = block->mutate_column_scoped(i);
        guard.mutable_column()->insert_from(*row.get_by_position(i).column, 0);
    }
    return Status::OK();
}

Status CdcRowMapper::_append(const CdcEvent& event, Block* block) const {
    if (event.format == CdcFormat::DYNAMODB_KINESIS) {
        for (const auto& key : event.key_names) {
            if (!_key_names.contains(key)) {
                return Status::DataQualityError("DynamoDB key '{}' has no target key mapping", key);
            }
        }
    } else if (!_key_names.contains("_id")) {
        return Status::DataQualityError("MongoDB _id has no target key mapping");
    }

    simdjson::padded_string keys_json(event.keys_json);
    simdjson::padded_string raw_keys_json(event.raw_keys_json);
    simdjson::padded_string after_json(event.operation == CdcOperation::UPSERT ? event.after_json
                                                                               : std::string("{}"));
    simdjson::ondemand::parser key_parser;
    simdjson::ondemand::parser raw_key_parser;
    simdjson::ondemand::parser after_parser;
    auto key_document = key_parser.iterate(keys_json);
    auto raw_key_document = raw_key_parser.iterate(raw_keys_json);
    auto after_document = after_parser.iterate(after_json);
    auto keys = key_document.get_object().value();
    auto raw_keys = raw_key_document.get_object().value();
    auto after = after_document.get_object().value();

    for (size_t i = 0; i < _columns.size(); ++i) {
        const auto& mapping = _columns[i];
        DCHECK(block->get_by_position(i).name == mapping.column_name);
        DCHECK(block->get_by_position(i).type->equals(*mapping.type));
        auto guard = block->mutate_column_scoped(i);
        auto& column = *guard.mutable_column();

        if (mapping.source == CdcColumnSource::DELETE_SIGN) {
            assert_cast<ColumnInt8&>(column).insert_value(event.operation == CdcOperation::DELETE);
            continue;
        }
        if (mapping.source == CdcColumnSource::AFTER && event.operation == CdcOperation::DELETE) {
            // Typed placeholders, not partial updates: the delete sign makes payload
            // columns irrelevant. Key columns still come from the event.
            column.insert_default();
            continue;
        }

        auto& object = mapping.source == CdcColumnSource::AFTER
                               ? after
                               : (mapping.source == CdcColumnSource::RAW_KEY ? raw_keys : keys);
        simdjson::ondemand::value value;
        auto error = object.find_field_unordered(mapping.source_name).get(value);
        if (error == simdjson::NO_SUCH_FIELD && mapping.source == CdcColumnSource::AFTER) {
            RETURN_IF_ERROR(append_default(mapping, column, *_serdes[i]));
            continue;
        }
        if (error) {
            return Status::DataQualityError("Invalid or missing CDC field '{}': {}",
                                            mapping.source_name, simdjson::error_message(error));
        }
        if (mapping.source != CdcColumnSource::AFTER && value.is_null().value()) {
            return Status::DataQualityError("CDC key '{}' must not be null", mapping.source_name);
        }
        if (mapping.source == CdcColumnSource::RAW_KEY) {
            append_raw_key(value, column);
        } else {
            if (mapping.source == CdcColumnSource::KEY &&
                (value.type() == simdjson::ondemand::json_type::object ||
                 value.type() == simdjson::ondemand::json_type::array)) {
                return Status::DataQualityError("Structured CDC keys require RAW_KEY projection");
            }
            auto status = append_value(value, column, *_serdes[i], mapping.type);
            if (!status.ok()) {
                return status.prepend("CDC column '" + mapping.column_name + "': ");
            }
        }
    }
    return Status::OK();
}

} // namespace doris
