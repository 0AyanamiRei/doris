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

#include <memory>
#include <string>
#include <vector>

#include "core/data_type/data_type_nullable.h"
#include "core/data_type/data_type_number.h"
#include "core/data_type/data_type_string.h"
#include "format/cdc/cdc_row_mapper.h"

namespace doris::cdc_test {

inline std::string mongodb_message(const std::string& operation, const std::string& keys,
                                   const std::string& after = "") {
    std::string result = "{\"operationType\":\"" + operation +
                         "\",\"ns\":{\"db\":\"shop\",\"coll\":\"users\"},\"documentKey\":" + keys;
    if (!after.empty()) {
        result += ",\"fullDocument\":" + after;
    }
    return result + "}";
}

inline std::string dynamodb_message(const std::string& operation, const std::string& keys,
                                    const std::string& after = "") {
    std::string result = "{\"eventName\":\"" + operation +
                         "\",\"tableName\":\"users\",\"dynamodb\":{\"Keys\":" + keys;
    if (!after.empty()) {
        result += ",\"NewImage\":" + after;
    }
    return result + "}}";
}

inline std::vector<CdcColumnMapping> mongodb_columns() {
    return {
            {"id", "_id", std::make_shared<DataTypeInt64>(), CdcColumnSource::KEY},
            {"name", "name", make_nullable(std::make_shared<DataTypeString>()),
             CdcColumnSource::AFTER},
            {"score", "score", make_nullable(std::make_shared<DataTypeInt64>()),
             CdcColumnSource::AFTER},
            {"__DORIS_DELETE_SIGN__", "", std::make_shared<DataTypeInt8>(),
             CdcColumnSource::DELETE_SIGN},
    };
}

} // namespace doris::cdc_test
