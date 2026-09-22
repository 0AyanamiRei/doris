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

#pragma once

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <simdjson/simdjson.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "format/cdc/cdc_event.h"

namespace doris {

class CdcDecoder {
public:
    virtual ~CdcDecoder() = default;

    static Status create(CdcFormat format, std::unique_ptr<CdcDecoder>* decoder);

    // Decode exactly one JSON envelope. On failure, leave the output unchanged.
    // No database IO, target-table lookup or source-version synthesis occurs here.
    Status decode(std::string_view message, CdcEvent* event) const;

protected:
    virtual Status _decode(simdjson::ondemand::object& envelope, CdcEvent* event) const = 0;
};

// Shared JSON mechanics; protocol-specific rules remain in the concrete decoder.
namespace cdc_json {
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
using ValueWriter = Status (*)(simdjson::ondemand::value&, Writer&, size_t);
constexpr size_t MAX_NESTING_DEPTH = 64;

Status validate_json(std::string_view json);
Status required_field(simdjson::ondemand::object& object, std::string_view name,
                      simdjson::ondemand::value* value);
Status required_string(simdjson::ondemand::object& object, std::string_view name,
                       std::string* value);
Status required_object_json(simdjson::ondemand::object& object, std::string_view name,
                            std::string* value);
Status normalize_object(std::string_view json, ValueWriter write_value, std::string* output,
                        std::vector<std::string>* field_names = nullptr);
Status write_object(simdjson::ondemand::object& object, Writer& writer, size_t depth,
                    ValueWriter write_value);
Status write_json_value(simdjson::ondemand::value& value, Writer& writer, size_t depth);
Status write_number(std::string_view number, Writer& writer);
void write_string(std::string_view value, Writer& writer);
Status validate_keys(const CdcEvent& event);
Status validate_after_keys(const CdcEvent& event, std::string_view after_json);
} // namespace cdc_json
} // namespace doris
