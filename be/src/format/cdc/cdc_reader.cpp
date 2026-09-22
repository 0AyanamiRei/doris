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

#include "format/cdc/cdc_reader.h"

#include <algorithm>
#include <utility>

#include "core/block/block.h"
#include "core/custom_allocator.h"
#include "format/cdc/cdc_decoder.h"
#include "format/json/new_json_reader.h"
#include "io/fs/stream_load_pipe.h"

namespace doris {

CdcReader::CdcReader(std::shared_ptr<io::StreamLoadPipe> pipe, CdcFormat format,
                     std::vector<CdcColumnMapping> columns, size_t batch_size,
                     std::string source_namespace)
        : _pipe(std::move(pipe)),
          _format(format),
          _mapper(std::move(columns)),
          _batch_size(std::max(batch_size, size_t {1})),
          _source_namespace(std::move(source_namespace)) {
    DORIS_CHECK(_pipe != nullptr);
}

CdcReader::~CdcReader() = default;

Status CdcReader::init_reader() {
    DORIS_CHECK(!_initialized);
    RETURN_IF_ERROR(CdcDecoder::create(_format, &_decoder));
    RETURN_IF_ERROR(_mapper.init());
    _initialized = true;
    return Status::OK();
}

void CdcReader::set_batch_size(size_t batch_size) {
    _batch_size = std::max(batch_size, size_t {1});
}

Block CdcReader::create_block() const {
    DORIS_CHECK(_initialized);
    return _mapper.create_block();
}

Status CdcReader::_get_columns_impl(std::unordered_map<std::string, DataTypePtr>* name_to_type) {
    for (const auto& mapping : _mapper.columns()) {
        name_to_type->emplace(mapping.column_name, mapping.type);
    }
    return Status::OK();
}

Status CdcReader::_fail(Status status, Block* block, size_t old_rows, size_t* read_rows) {
    json_reader_detail::truncate_block_to_rows(*block, old_rows);
    *read_rows = 0;
    _status = std::move(status);
    _pipe->cancel(_status.to_string());
    return _status;
}

Status CdcReader::_do_get_next_block(Block* block, size_t* read_rows, bool* eof) {
    DORIS_CHECK(_initialized);
    *read_rows = 0;
    *eof = _eof;
    RETURN_IF_ERROR(_status);
    const size_t old_rows = block->rows();
    while (!_eof && *read_rows < _batch_size) {
        DorisUniqueBufferPtr<uint8_t> message;
        size_t length = 0;
        auto status = _pipe->read_one_message(&message, &length, &_eof);
        if (!status.ok()) {
            return _fail(std::move(status), block, old_rows, read_rows);
        }
        if (_eof) {
            break;
        }
        ++_messages_read;
        if (length == 0) {
            return _fail(Status::DataQualityError("Empty CDC message {}", _messages_read), block,
                         old_rows, read_rows);
        }
        CdcEvent event;
        status = _decoder->decode(
                std::string_view(reinterpret_cast<const char*>(message.get()), length), &event);
        if (!status.ok()) {
            status.prepend("CDC message " + std::to_string(_messages_read) + ": ");
            return _fail(std::move(status), block, old_rows, read_rows);
        }
        if (!_source_namespace.empty() && event.source_namespace != _source_namespace) {
            return _fail(Status::DataQualityError("Unexpected CDC namespace '{}', expected '{}'",
                                                  event.source_namespace, _source_namespace),
                         block, old_rows, read_rows);
        }
        status = _mapper.append(event, block);
        if (!status.ok()) {
            status.prepend("CDC message " + std::to_string(_messages_read) + ": ");
            return _fail(std::move(status), block, old_rows, read_rows);
        }
        ++*read_rows;
    }
    *eof = _eof;
    return Status::OK();
}

} // namespace doris
