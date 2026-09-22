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

#include "format/cdc/cdc_decoder.h"

#include <rapidjson/error/en.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/reader.h>

#include <limits>
#include <unordered_set>
#include <utility>

#include "format/cdc/dynamodb_cdc_decoder.h"
#include "format/cdc/mongodb_cdc_decoder.h"

namespace doris {
namespace {

struct JsonValidationHandler
        : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, JsonValidationHandler> {
    size_t depth = 0;
    std::vector<std::unordered_set<std::string>> object_keys;

    bool StartObject() {
        object_keys.emplace_back();
        return ++depth <= cdc_json::MAX_NESTING_DEPTH;
    }
    bool EndObject(rapidjson::SizeType) {
        object_keys.pop_back();
        --depth;
        return true;
    }
    bool StartArray() { return ++depth <= cdc_json::MAX_NESTING_DEPTH; }
    bool EndArray(rapidjson::SizeType) {
        --depth;
        return true;
    }
    bool Key(const char* key, rapidjson::SizeType length, bool) {
        return object_keys.back().emplace(key, length).second;
    }
};

} // namespace

Status cdc_json::validate_json(std::string_view json) {
    if (json.empty()) {
        return Status::DataQualityError("Empty CDC message");
    }
    if (json.size() > std::numeric_limits<rapidjson::SizeType>::max()) {
        return Status::DataQualityError("CDC JSON exceeds the supported document size");
    }
    // Validate ignored metadata too. Raw numbers avoid converting decimal text
    // through double, and the iterative parser bounds its C++ call stack.
    rapidjson::Reader reader;
    rapidjson::MemoryStream input(json.data(), json.size());
    JsonValidationHandler handler;
    if (!reader.Parse<rapidjson::kParseNumbersAsStringsFlag |
                      rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(
                input, handler)) {
        return Status::DataQualityError(
                "Invalid CDC JSON at byte {}: {} (duplicate fields and nesting over {} are "
                "rejected)",
                reader.GetErrorOffset(), rapidjson::GetParseError_En(reader.GetParseErrorCode()),
                cdc_json::MAX_NESTING_DEPTH);
    }
    return Status::OK();
}

Status CdcDecoder::create(CdcFormat format, std::unique_ptr<CdcDecoder>* decoder) {
    switch (format) {
    case CdcFormat::MONGODB_CHANGE_STREAM:
        *decoder = std::make_unique<MongoDbCdcDecoder>();
        return Status::OK();
    case CdcFormat::DYNAMODB_KINESIS:
        *decoder = std::make_unique<DynamoDbCdcDecoder>();
        return Status::OK();
    }
    return Status::InvalidArgument("Unsupported CDC format");
}

Status CdcDecoder::decode(std::string_view message, CdcEvent* event) const {
    RETURN_IF_ERROR(cdc_json::validate_json(message));
    try {
        simdjson::padded_string json(message);
        simdjson::ondemand::parser parser;
        auto document = parser.iterate(json);
        simdjson::ondemand::object envelope = document.get_object();
        CdcEvent decoded;
        RETURN_IF_ERROR(_decode(envelope, &decoded));
        RETURN_IF_ERROR(cdc_json::validate_keys(decoded));
        *event = std::move(decoded);
        return Status::OK();
    } catch (const simdjson::simdjson_error& error) {
        return Status::DataQualityError("Invalid CDC event: {}", error.what());
    }
}

namespace cdc_json {

Status required_field(simdjson::ondemand::object& object, std::string_view name,
                      simdjson::ondemand::value* value) {
    auto error = object.find_field_unordered(name).get(*value);
    if (error) {
        return Status::DataQualityError("Invalid or missing CDC field '{}': {}", name,
                                        simdjson::error_message(error));
    }
    return Status::OK();
}

Status required_string(simdjson::ondemand::object& object, std::string_view name,
                       std::string* output) {
    simdjson::ondemand::value value;
    RETURN_IF_ERROR(required_field(object, name, &value));
    if (value.type() != simdjson::ondemand::json_type::string) {
        return Status::DataQualityError("CDC field '{}' must be a string", name);
    }
    *output = std::string(value.get_string().value());
    if (output->empty()) {
        return Status::DataQualityError("CDC field '{}' must not be empty", name);
    }
    return Status::OK();
}

Status required_object_json(simdjson::ondemand::object& object, std::string_view name,
                            std::string* output) {
    simdjson::ondemand::value value;
    RETURN_IF_ERROR(required_field(object, name, &value));
    if (value.type() != simdjson::ondemand::json_type::object) {
        return Status::DataQualityError("CDC field '{}' must be an object", name);
    }
    *output = std::string(value.raw_json().value());
    return Status::OK();
}

void write_string(std::string_view value, Writer& writer) {
    writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
}

Status write_number(std::string_view number, Writer& writer) {
    // Also used for MongoDB numeric wrappers and DynamoDB N strings.
    if (number.empty() ||
        (number.front() != '-' && (number.front() < '0' || number.front() > '9'))) {
        return Status::DataQualityError("Invalid CDC numeric value");
    }
    rapidjson::Reader reader;
    rapidjson::MemoryStream input(number.data(), number.size());
    rapidjson::BaseReaderHandler<> handler;
    if (!reader.Parse<rapidjson::kParseNumbersAsStringsFlag>(input, handler)) {
        return Status::DataQualityError("Invalid CDC numeric value");
    }
    writer.RawValue(number.data(), number.size(), rapidjson::kNumberType);
    return Status::OK();
}

Status write_object(simdjson::ondemand::object& object, Writer& writer, size_t depth,
                    ValueWriter write_value) {
    if (depth > MAX_NESTING_DEPTH) {
        return Status::DataQualityError("CDC value exceeds nesting limit");
    }
    writer.StartObject();
    for (auto field : object) {
        std::string_view name = field.unescaped_key();
        writer.Key(name.data(), static_cast<rapidjson::SizeType>(name.size()));
        simdjson::ondemand::value value = field.value();
        RETURN_IF_ERROR(write_value(value, writer, depth + 1));
    }
    writer.EndObject();
    return Status::OK();
}

Status write_json_value(simdjson::ondemand::value& value, Writer& writer, size_t depth) {
    if (depth > MAX_NESTING_DEPTH) {
        return Status::DataQualityError("CDC value exceeds nesting limit");
    }
    switch (value.type().value()) {
    case simdjson::ondemand::json_type::object: {
        auto object = value.get_object().value();
        return write_object(object, writer, depth, write_json_value);
    }
    case simdjson::ondemand::json_type::array:
        writer.StartArray();
        for (auto item : value.get_array()) {
            simdjson::ondemand::value element = item.value();
            RETURN_IF_ERROR(write_json_value(element, writer, depth + 1));
        }
        writer.EndArray();
        return Status::OK();
    case simdjson::ondemand::json_type::string:
        write_string(value.get_string().value(), writer);
        return Status::OK();
    case simdjson::ondemand::json_type::number:
        return write_number(simdjson::to_json_string(value).value(), writer);
    case simdjson::ondemand::json_type::boolean:
        writer.Bool(value.get_bool().value());
        return Status::OK();
    case simdjson::ondemand::json_type::null:
        writer.Null();
        return Status::OK();
    }
    return Status::DataQualityError("Unsupported CDC JSON value");
}

Status normalize_object(std::string_view json, ValueWriter write_value, std::string* output,
                        std::vector<std::string>* field_names) {
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    auto document = parser.iterate(padded);
    auto object = document.get_object().value();
    rapidjson::StringBuffer buffer;
    Writer writer(buffer);
    writer.StartObject();
    for (auto field : object) {
        std::string_view name = field.unescaped_key();
        if (field_names != nullptr) {
            field_names->emplace_back(name);
        }
        writer.Key(name.data(), static_cast<rapidjson::SizeType>(name.size()));
        simdjson::ondemand::value value = field.value();
        RETURN_IF_ERROR(write_value(value, writer, 1));
    }
    writer.EndObject();
    output->assign(buffer.GetString(), buffer.GetSize());
    return Status::OK();
}

Status validate_keys(const CdcEvent& event) {
    if (event.key_names.empty()) {
        return Status::DataQualityError("CDC event contains no business key");
    }
    simdjson::padded_string json(event.keys_json);
    simdjson::ondemand::parser parser;
    auto document = parser.iterate(json);
    for (auto field : document.get_object()) {
        simdjson::ondemand::value value = field.value();
        if (value.is_null().value()) {
            return Status::DataQualityError("CDC business key must not be null");
        }
    }
    return Status::OK();
}

Status validate_after_keys(const CdcEvent& event, std::string_view after_json) {
    simdjson::padded_string key_buffer(event.raw_keys_json);
    simdjson::padded_string after_buffer(after_json);
    simdjson::ondemand::parser key_parser;
    simdjson::ondemand::parser after_parser;
    auto key_document = key_parser.iterate(key_buffer);
    auto after_document = after_parser.iterate(after_buffer);
    auto after = after_document.get_object().value();
    for (auto field : key_document.get_object()) {
        std::string_view name = field.unescaped_key();
        simdjson::ondemand::value key_value = field.value();
        std::string key_json(key_value.raw_json().value());
        simdjson::ondemand::value after_value;
        RETURN_IF_ERROR(required_field(after, name, &after_value));
        rapidjson::StringBuffer buffer;
        Writer writer(buffer);
        RETURN_IF_ERROR(write_json_value(after_value, writer, 1));
        if (key_json != std::string_view(buffer.GetString(), buffer.GetSize())) {
            return Status::DataQualityError("CDC after image disagrees with business key '{}'",
                                            name);
        }
    }
    return Status::OK();
}

} // namespace cdc_json
} // namespace doris
