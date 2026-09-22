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

#include <string>
#include <vector>

namespace doris {

enum class CdcFormat { MONGODB_CHANGE_STREAM, DYNAMODB_KINESIS };
enum class CdcOperation { UPSERT, DELETE };

// Owns its documents: no views into a decoder's parser survive decode().
// keys_json/after_json contain normalized values. raw_keys_json preserves source
// type wrappers for callers that need a type-preserving document-key projection.
struct CdcEvent {
    CdcFormat format = CdcFormat::MONGODB_CHANGE_STREAM;
    CdcOperation operation = CdcOperation::UPSERT;
    std::string source_namespace;
    std::vector<std::string> key_names;
    std::string keys_json;
    std::string raw_keys_json;
    std::string after_json;
};

} // namespace doris
