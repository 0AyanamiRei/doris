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

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "core/block/block.h"
#include "core/custom_allocator.h"
#include "format/cdc/cdc_test_util.h"
#include "io/fs/stream_load_pipe.h"

namespace doris {

TEST(CdcReaderTest, ReadsBoundedBatchesAndThenEof) {
    auto pipe = std::make_shared<io::StreamLoadPipe>();
    const auto insert = cdc_test::mongodb_message("insert", R"({"_id":7})",
                                                  R"({"_id":7,"name":"Alice","score":1})");
    const auto update = cdc_test::mongodb_message("update", R"({"_id":7})",
                                                  R"({"_id":7,"name":"Alice","score":2})");
    const auto remove = cdc_test::mongodb_message("delete", R"({"_id":7})");
    for (const auto& message : {insert, update, remove}) {
        ASSERT_TRUE(pipe->append_json(message.data(), message.size()).ok());
    }
    ASSERT_TRUE(pipe->finish().ok());
    CdcReader reader(pipe, CdcFormat::MONGODB_CHANGE_STREAM, cdc_test::mongodb_columns(), 2,
                     "shop.users");
    ASSERT_TRUE(reader.init_reader().ok());
    auto block = reader.create_block();
    size_t rows = 0;
    bool eof = false;
    ASSERT_TRUE(reader.get_next_block(&block, &rows, &eof).ok());
    EXPECT_EQ(rows, 2);
    EXPECT_FALSE(eof);

    block = reader.create_block();
    ASSERT_TRUE(reader.get_next_block(&block, &rows, &eof).ok());
    EXPECT_EQ(rows, 1);
    EXPECT_TRUE(eof);
    EXPECT_EQ(reader.messages_read(), 3);
    EXPECT_EQ(block.get_by_position(3).type->to_string(*block.get_by_position(3).column, 0), "1");

    block = reader.create_block();
    ASSERT_TRUE(reader.get_next_block(&block, &rows, &eof).ok());
    EXPECT_EQ(rows, 0);
    EXPECT_TRUE(eof);
}

TEST(CdcReaderTest, ReadsDynamoDbEventsThroughTheSameInterface) {
    auto pipe = std::make_shared<io::StreamLoadPipe>();
    const auto message =
            cdc_test::dynamodb_message("INSERT", R"({"pk":{"S":"u7"}})",
                                       R"({"pk":{"S":"u7"},"score":{"N":"9007199254740993"}})");
    ASSERT_TRUE(pipe->append_json(message.data(), message.size()).ok());
    ASSERT_TRUE(pipe->finish().ok());
    std::vector<CdcColumnMapping> columns {
            {"id", "pk", std::make_shared<DataTypeString>(), CdcColumnSource::KEY},
            {"score", "score", std::make_shared<DataTypeInt64>(), CdcColumnSource::AFTER},
            {"__DORIS_DELETE_SIGN__", "", std::make_shared<DataTypeInt8>(),
             CdcColumnSource::DELETE_SIGN}};
    CdcReader reader(pipe, CdcFormat::DYNAMODB_KINESIS, std::move(columns), 10);
    ASSERT_TRUE(reader.init_reader().ok());
    auto block = reader.create_block();
    size_t rows = 0;
    bool eof = false;
    auto status = reader.get_next_block(&block, &rows, &eof);
    ASSERT_TRUE(status.ok()) << status.to_string();
    EXPECT_EQ(rows, 1);
    EXPECT_TRUE(eof);
    const auto& key = block.get_by_position(0);
    const auto& score = block.get_by_position(1);
    EXPECT_EQ(key.type->to_string(*key.column, 0), "u7");
    EXPECT_EQ(score.type->to_string(*score.column, 0), "9007199254740993");
}

TEST(CdcReaderTest, FailsTheBatchAndCancelsThePipeOnBadEvent) {
    auto pipe = std::make_shared<io::StreamLoadPipe>();
    const auto good = cdc_test::mongodb_message("insert", R"({"_id":7})", R"({"_id":7})");
    const auto bad = cdc_test::mongodb_message("update", R"({"_id":7})");
    ASSERT_TRUE(pipe->append_json(good.data(), good.size()).ok());
    ASSERT_TRUE(pipe->append_json(bad.data(), bad.size()).ok());
    ASSERT_TRUE(pipe->finish().ok());
    CdcReader reader(pipe, CdcFormat::MONGODB_CHANGE_STREAM, cdc_test::mongodb_columns(), 10,
                     "shop.users");
    ASSERT_TRUE(reader.init_reader().ok());
    auto block = reader.create_block();
    size_t rows = 0;
    bool eof = false;
    EXPECT_FALSE(reader.get_next_block(&block, &rows, &eof).ok());
    EXPECT_EQ(rows, 0);
    EXPECT_EQ(block.rows(), 0);
    EXPECT_TRUE(pipe->closed());
    EXPECT_FALSE(reader.get_next_block(&block, &rows, &eof).ok());
}

TEST(CdcReaderTest, RejectsEmptyMessagesInsteadOfTreatingThemAsEof) {
    auto pipe = std::make_shared<io::StreamLoadPipe>();
    ASSERT_TRUE(pipe->append_json("", 0).ok());
    const auto next = cdc_test::mongodb_message("delete", R"({"_id":7})");
    ASSERT_TRUE(pipe->append_json(next.data(), next.size()).ok());
    ASSERT_TRUE(pipe->finish().ok());
    CdcReader reader(pipe, CdcFormat::MONGODB_CHANGE_STREAM, cdc_test::mongodb_columns(), 10,
                     "shop.users");
    ASSERT_TRUE(reader.init_reader().ok());
    auto block = reader.create_block();
    size_t rows = 0;
    bool eof = false;
    EXPECT_FALSE(reader.get_next_block(&block, &rows, &eof).ok());
    EXPECT_FALSE(eof);
    EXPECT_EQ(reader.messages_read(), 1);
    EXPECT_EQ(block.rows(), 0);
}

TEST(CdcReaderTest, RejectsOtherNamespacesAndPropagatesCancellation) {
    auto pipe = std::make_shared<io::StreamLoadPipe>();
    const auto message = cdc_test::mongodb_message("delete", R"({"_id":7})");
    ASSERT_TRUE(pipe->append_json(message.data(), message.size()).ok());
    ASSERT_TRUE(pipe->finish().ok());
    CdcReader reader(pipe, CdcFormat::MONGODB_CHANGE_STREAM, cdc_test::mongodb_columns(), 1,
                     "other.users");
    ASSERT_TRUE(reader.init_reader().ok());
    auto block = reader.create_block();
    size_t rows = 0;
    bool eof = false;
    EXPECT_FALSE(reader.get_next_block(&block, &rows, &eof).ok());
    EXPECT_EQ(block.rows(), 0);

    auto cancelled_pipe = std::make_shared<io::StreamLoadPipe>();
    cancelled_pipe->cancel("test cancellation");
    CdcReader cancelled(cancelled_pipe, CdcFormat::MONGODB_CHANGE_STREAM,
                        cdc_test::mongodb_columns(), 1, "shop.users");
    ASSERT_TRUE(cancelled.init_reader().ok());
    block = cancelled.create_block();
    EXPECT_FALSE(cancelled.get_next_block(&block, &rows, &eof).ok());
}

TEST(CdcReaderTest, ExplicitPipeEofDistinguishesEmptyRecordAndPreservesLegacyApi) {
    io::StreamLoadPipe pipe;
    ASSERT_TRUE(pipe.append_json("", 0).ok());
    ASSERT_TRUE(pipe.append_json("{}", 2).ok());
    ASSERT_TRUE(pipe.finish().ok());
    DorisUniqueBufferPtr<uint8_t> data;
    size_t length = 0;
    bool eof = true;
    ASSERT_TRUE(pipe.read_one_message(&data, &length, &eof).ok());
    EXPECT_EQ(length, 0);
    EXPECT_FALSE(eof);
    ASSERT_TRUE(pipe.read_one_message(&data, &length).ok());
    EXPECT_EQ(length, 2);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(data.get()), length), "{}");
    ASSERT_TRUE(pipe.read_one_message(&data, &length, &eof).ok());
    EXPECT_EQ(length, 0);
    EXPECT_TRUE(eof);
}

} // namespace doris
