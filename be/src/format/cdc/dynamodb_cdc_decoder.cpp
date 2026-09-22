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

#include "format/cdc/dynamodb_cdc_decoder.h"

namespace doris {
namespace {

Status write_attribute(simdjson::ondemand::value& value, cdc_json::Writer& writer, size_t depth);

Status write_attribute_body(std::string_view tag, simdjson::ondemand::value& value,
                            cdc_json::Writer& writer, size_t depth) {
    if (tag == "N" && value.type() == simdjson::ondemand::json_type::number) {
        return cdc_json::write_number(simdjson::to_json_string(value).value(), writer);
    }
    if (tag == "S" || tag == "N" || tag == "B") {
        if (value.type() != simdjson::ondemand::json_type::string) {
            return Status::DataQualityError("DynamoDB '{}' attribute must contain a string", tag);
        }
        std::string_view text = value.get_string().value();
        if (tag == "N") {
            return cdc_json::write_number(text, writer);
        }
        if (tag == "B") {
            // Preserve the wire representation; do not guess its base64 encoding layers.
            writer.StartObject();
            writer.Key("B");
        }
        cdc_json::write_string(text, writer);
        if (tag == "B") {
            writer.EndObject();
        }
        return Status::OK();
    }
    if (tag == "BOOL") {
        writer.Bool(value.get_bool().value());
        return Status::OK();
    }
    if (tag == "NULL") {
        if (!value.get_bool().value()) {
            return Status::DataQualityError("DynamoDB NULL attribute must be true");
        }
        writer.Null();
        return Status::OK();
    }
    if (tag == "M") {
        auto object = value.get_object().value();
        return cdc_json::write_object(object, writer, depth, write_attribute);
    }
    if (tag == "L") {
        writer.StartArray();
        for (auto item : value.get_array()) {
            simdjson::ondemand::value element = item.value();
            RETURN_IF_ERROR(write_attribute(element, writer, depth + 1));
        }
        writer.EndArray();
        return Status::OK();
    }
    if (tag == "SS" || tag == "NS" || tag == "BS") {
        // Sets project to arrays without claiming an order. Binary sets retain their tag.
        if (tag == "BS") {
            writer.StartObject();
            writer.Key("BS");
        }
        writer.StartArray();
        for (auto item : value.get_array()) {
            simdjson::ondemand::value element = item.value();
            std::string_view text = element.get_string().value();
            if (tag == "NS") {
                RETURN_IF_ERROR(cdc_json::write_number(text, writer));
            } else {
                cdc_json::write_string(text, writer);
            }
        }
        writer.EndArray();
        if (tag == "BS") {
            writer.EndObject();
        }
        return Status::OK();
    }
    return Status::NotSupported("Unsupported DynamoDB attribute type '{}'", tag);
}

Status write_attribute(simdjson::ondemand::value& value, cdc_json::Writer& writer, size_t depth) {
    if (depth > cdc_json::MAX_NESTING_DEPTH) {
        return Status::DataQualityError("DynamoDB item exceeds nesting limit");
    }
    auto attribute = value.get_object().value();
    size_t members = 0;
    for (auto member : attribute) {
        if (++members != 1) {
            return Status::DataQualityError("DynamoDB attribute must have exactly one type tag");
        }
        std::string_view tag = member.unescaped_key();
        simdjson::ondemand::value nested = member.value();
        RETURN_IF_ERROR(write_attribute_body(tag, nested, writer, depth));
    }
    if (members == 0) {
        return Status::DataQualityError("DynamoDB attribute has no type tag");
    }
    return Status::OK();
}

Status write_key(simdjson::ondemand::value& value, cdc_json::Writer& writer, size_t depth) {
    auto attribute = value.get_object().value();
    size_t members = 0;
    for (auto member : attribute) {
        if (++members != 1) {
            return Status::DataQualityError("DynamoDB key must have exactly one type tag");
        }
        std::string_view tag = member.unescaped_key();
        if (tag != "S" && tag != "N" && tag != "B") {
            return Status::DataQualityError("DynamoDB key type must be S, N or B");
        }
        simdjson::ondemand::value nested = member.value();
        RETURN_IF_ERROR(write_attribute_body(tag, nested, writer, depth));
    }
    if (members == 0) {
        return Status::DataQualityError("DynamoDB key has no type tag");
    }
    return Status::OK();
}

} // namespace

Status DynamoDbCdcDecoder::_decode(simdjson::ondemand::object& envelope, CdcEvent* event) const {
    event->format = CdcFormat::DYNAMODB_KINESIS;
    std::string operation;
    RETURN_IF_ERROR(cdc_json::required_string(envelope, "eventName", &operation));
    if (operation == "INSERT" || operation == "MODIFY") {
        event->operation = CdcOperation::UPSERT;
    } else if (operation == "REMOVE") {
        event->operation = CdcOperation::DELETE;
    } else {
        return Status::NotSupported("Unsupported DynamoDB CDC operation '{}'", operation);
    }
    RETURN_IF_ERROR(cdc_json::required_string(envelope, "tableName", &event->source_namespace));
    simdjson::ondemand::value dynamodb_value;
    RETURN_IF_ERROR(cdc_json::required_field(envelope, "dynamodb", &dynamodb_value));
    auto dynamodb = dynamodb_value.get_object().value();

    std::string keys_json;
    RETURN_IF_ERROR(cdc_json::required_object_json(dynamodb, "Keys", &keys_json));
    RETURN_IF_ERROR(
            cdc_json::normalize_object(keys_json, write_key, &event->keys_json, &event->key_names));
    RETURN_IF_ERROR(cdc_json::normalize_object(keys_json, cdc_json::write_json_value,
                                               &event->raw_keys_json));
    if (event->operation == CdcOperation::UPSERT) {
        std::string after_json;
        RETURN_IF_ERROR(cdc_json::required_object_json(dynamodb, "NewImage", &after_json));
        RETURN_IF_ERROR(cdc_json::validate_after_keys(*event, after_json));
        RETURN_IF_ERROR(
                cdc_json::normalize_object(after_json, write_attribute, &event->after_json));
    }
    return Status::OK();
}

} // namespace doris
