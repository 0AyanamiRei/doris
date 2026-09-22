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

#include "format/cdc/mongodb_cdc_decoder.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <system_error>

namespace doris {
namespace {

Status write_mongodb_value(simdjson::ondemand::value& value, cdc_json::Writer& writer,
                           size_t depth) {
    if (depth > cdc_json::MAX_NESTING_DEPTH) {
        return Status::DataQualityError("MongoDB document exceeds nesting limit");
    }
    if (value.type() == simdjson::ondemand::json_type::array) {
        writer.StartArray();
        for (auto item : value.get_array()) {
            simdjson::ondemand::value element = item.value();
            RETURN_IF_ERROR(write_mongodb_value(element, writer, depth + 1));
        }
        writer.EndArray();
        return Status::OK();
    }
    if (value.type() != simdjson::ondemand::json_type::object) {
        return cdc_json::write_json_value(value, writer, depth);
    }

    auto object = value.get_object().value();
    size_t fields = 0;
    bool scalar_wrapper = false;
    for (auto member : object) {
        std::string_view name = member.unescaped_key();
        simdjson::ondemand::value nested = member.value();
        if (++fields == 1) {
            scalar_wrapper = name == "$oid" || name == "$numberInt" || name == "$numberLong" ||
                             name == "$numberDouble" || name == "$numberDecimal";
            if (!scalar_wrapper) {
                writer.StartObject();
            }
        } else if (scalar_wrapper) {
            return Status::DataQualityError("MongoDB scalar type wrapper has extra fields");
        }
        if (scalar_wrapper) {
            if (nested.type() != simdjson::ondemand::json_type::string) {
                return Status::DataQualityError(
                        "MongoDB scalar type wrapper must contain a string");
            }
            std::string_view text = nested.get_string().value();
            if (name == "$oid") {
                if (text.size() != 24 || !std::all_of(text.begin(), text.end(), [](char c) {
                        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                               (c >= 'A' && c <= 'F');
                    })) {
                    return Status::DataQualityError("Invalid MongoDB ObjectId");
                }
                std::string oid(text);
                std::transform(oid.begin(), oid.end(), oid.begin(), [](char c) {
                    return c >= 'A' && c <= 'F' ? static_cast<char>(c + ('a' - 'A')) : c;
                });
                cdc_json::write_string(oid, writer);
            } else {
                if (name == "$numberInt" || name == "$numberLong") {
                    int64_t number = 0;
                    auto result = std::from_chars(text.data(), text.data() + text.size(), number);
                    if (result.ec != std::errc() || result.ptr != text.data() + text.size() ||
                        (name == "$numberInt" && (number < std::numeric_limits<int32_t>::min() ||
                                                  number > std::numeric_limits<int32_t>::max()))) {
                        return Status::DataQualityError("Invalid MongoDB integer wrapper");
                    }
                }
                RETURN_IF_ERROR(cdc_json::write_number(text, writer));
            }
        } else {
            writer.Key(name.data(), static_cast<rapidjson::SizeType>(name.size()));
            RETURN_IF_ERROR(write_mongodb_value(nested, writer, depth + 1));
        }
    }
    if (!scalar_wrapper) {
        if (fields == 0) {
            writer.StartObject();
        }
        writer.EndObject();
    }
    return Status::OK();
}

} // namespace

Status MongoDbCdcDecoder::_decode(simdjson::ondemand::object& envelope, CdcEvent* event) const {
    event->format = CdcFormat::MONGODB_CHANGE_STREAM;
    std::string operation;
    RETURN_IF_ERROR(cdc_json::required_string(envelope, "operationType", &operation));
    if (operation == "insert" || operation == "update" || operation == "replace") {
        event->operation = CdcOperation::UPSERT;
    } else if (operation == "delete") {
        event->operation = CdcOperation::DELETE;
    } else {
        return Status::NotSupported("Unsupported MongoDB CDC operation '{}'", operation);
    }

    simdjson::ondemand::value ns_value;
    RETURN_IF_ERROR(cdc_json::required_field(envelope, "ns", &ns_value));
    auto ns = ns_value.get_object().value();
    std::string database;
    std::string collection;
    RETURN_IF_ERROR(cdc_json::required_string(ns, "db", &database));
    RETURN_IF_ERROR(cdc_json::required_string(ns, "coll", &collection));
    event->source_namespace = database + "." + collection;

    std::string key_json;
    RETURN_IF_ERROR(cdc_json::required_object_json(envelope, "documentKey", &key_json));
    RETURN_IF_ERROR(cdc_json::normalize_object(key_json, write_mongodb_value, &event->keys_json,
                                               &event->key_names));
    if (std::find(event->key_names.begin(), event->key_names.end(), "_id") ==
        event->key_names.end()) {
        return Status::DataQualityError("MongoDB documentKey must contain _id");
    }
    RETURN_IF_ERROR(cdc_json::normalize_object(key_json, cdc_json::write_json_value,
                                               &event->raw_keys_json));
    if (event->operation == CdcOperation::UPSERT) {
        std::string after_json;
        RETURN_IF_ERROR(cdc_json::required_object_json(envelope, "fullDocument", &after_json));
        RETURN_IF_ERROR(cdc_json::validate_after_keys(*event, after_json));
        RETURN_IF_ERROR(
                cdc_json::normalize_object(after_json, write_mongodb_value, &event->after_json));
    }
    return Status::OK();
}

} // namespace doris
