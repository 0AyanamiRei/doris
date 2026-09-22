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
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "format/cdc/cdc_event.h"
#include "format/cdc/cdc_row_mapper.h"
#include "format/generic_reader.h"

namespace doris {
class CdcDecoder;
namespace io {
class StreamLoadPipe;
}

// BE-only entry point. A future FileScanner branch supplies its pipe, protocol
// and planned source-column mappings; no FE or Thrift configuration is added here.
class CdcReader final : public GenericReader {
public:
    ENABLE_FACTORY_CREATOR(CdcReader);

    CdcReader(std::shared_ptr<io::StreamLoadPipe> pipe, CdcFormat format,
              std::vector<CdcColumnMapping> columns, size_t batch_size,
              std::string source_namespace = {});
    ~CdcReader() override;

    using GenericReader::init_reader;
    Status init_reader();

    Status _do_get_next_block(Block* block, size_t* read_rows, bool* eof) override;
    Status _get_columns_impl(std::unordered_map<std::string, DataTypePtr>* name_to_type) override;
    void set_batch_size(size_t batch_size) override;
    size_t get_batch_size() const override { return _batch_size; }

    Block create_block() const;
    uint64_t messages_read() const { return _messages_read; }

protected:
    Status _do_init_reader(ReaderInitContext*) override { return init_reader(); }

private:
    Status _fail(Status status, Block* block, size_t old_rows, size_t* read_rows);

    std::shared_ptr<io::StreamLoadPipe> _pipe;
    CdcFormat _format;
    std::unique_ptr<CdcDecoder> _decoder;
    CdcRowMapper _mapper;
    size_t _batch_size;
    std::string _source_namespace;
    uint64_t _messages_read = 0;
    bool _initialized = false;
    bool _eof = false;
    Status _status;
};

} // namespace doris
