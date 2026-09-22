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

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/status.h"
#include "core/data_type/data_type.h"
#include "core/data_type_serde/data_type_serde.h"
#include "format/cdc/cdc_event.h"

namespace doris {

class Block;

enum class CdcColumnSource {
    KEY,
    // Preserve source type wrappers as compact JSON text in a string column.
    // Use this for heterogeneous MongoDB _id values instead of coercing them.
    RAW_KEY,
    AFTER,
    DELETE_SIGN
};

struct CdcColumnMapping {
    std::string column_name;
    std::string source_name;
    DataTypePtr type;
    CdcColumnSource source = CdcColumnSource::AFTER;
    // Constant JSON default for a missing AFTER field. Explicit null does not use it.
    std::optional<std::string> default_json = std::nullopt;
};

// Maps to the source Block schema supplied by the caller. FE expressions,
// destination casts and the actual sink remain outside this component.
class CdcRowMapper {
public:
    explicit CdcRowMapper(std::vector<CdcColumnMapping> columns);

    Status init();
    Block create_block() const;
    // Stage one complete event before appending it. Conversion errors cannot
    // leave partial nullable/complex values in the caller's Block.
    Status append(const CdcEvent& event, Block* block) const;

    const std::vector<CdcColumnMapping>& columns() const { return _columns; }

private:
    Status _append(const CdcEvent& event, Block* block) const;

    std::vector<CdcColumnMapping> _columns;
    std::vector<DataTypeSerDeSPtr> _serdes;
    std::unordered_set<std::string> _key_names;
    bool _initialized = false;
};

} // namespace doris
