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

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "format/cdc/cdc_test_util.h"

namespace doris {

TEST(MongoDbCdcDecoderTest, DecodesFullImagesAndDeletion) {
    MongoDbCdcDecoder decoder;
    CdcEvent event;
    for (const std::string operation : {"insert", "update", "replace"}) {
        auto status = decoder.decode(
                cdc_test::mongodb_message(operation, R"({"_id":7})",
                                          R"({"_id":7,"name":"Alice","score":9007199254740993})"),
                &event);
        ASSERT_TRUE(status.ok()) << status.to_string();
        EXPECT_EQ(event.operation, CdcOperation::UPSERT);
        EXPECT_EQ(event.source_namespace, "shop.users");
        EXPECT_EQ(event.key_names, std::vector<std::string>({"_id"}));
        EXPECT_EQ(event.keys_json, R"({"_id":7})");
        EXPECT_EQ(event.after_json, R"({"_id":7,"name":"Alice","score":9007199254740993})");
    }
    ASSERT_TRUE(decoder.decode(cdc_test::mongodb_message("delete", R"({"_id":7})"), &event).ok());
    EXPECT_EQ(event.operation, CdcOperation::DELETE);
    EXPECT_EQ(event.keys_json, R"({"_id":7})");
    EXPECT_TRUE(event.after_json.empty());
}

TEST(MongoDbCdcDecoderTest, PreservesOwnedDataAndNumericPrecision) {
    MongoDbCdcDecoder decoder;
    CdcEvent event;
    auto status = decoder.decode(
            cdc_test::mongodb_message(
                    "insert", R"({"_id":{"$oid":"64b000000000000000000007"}})",
                    R"({"_id":{"$oid":"64b000000000000000000007"},"n":{"$numberLong":"9007199254740993"},"price":{"$numberDecimal":"123456789.123456789"},"nested":[{"v":{"$numberInt":"3"}}]})"),
            &event);
    ASSERT_TRUE(status.ok()) << status.to_string();
    EXPECT_EQ(event.keys_json, R"({"_id":"64b000000000000000000007"})");
    EXPECT_EQ(event.raw_keys_json, R"({"_id":{"$oid":"64b000000000000000000007"}})");
    EXPECT_EQ(
            event.after_json,
            R"({"_id":"64b000000000000000000007","n":9007199254740993,"price":123456789.123456789,"nested":[{"v":3}]})");

    CdcEvent next;
    ASSERT_TRUE(decoder.decode(cdc_test::mongodb_message("delete", R"({"_id":8})"), &next).ok());
    EXPECT_EQ(event.keys_json, R"({"_id":"64b000000000000000000007"})");
}

TEST(MongoDbCdcDecoderTest, RejectsUnsupportedOrIncompleteEventsWithoutChangingOutput) {
    MongoDbCdcDecoder decoder;
    CdcEvent event;
    ASSERT_TRUE(decoder.decode(cdc_test::mongodb_message("delete", R"({"_id":7})"), &event).ok());
    const std::vector<std::string> invalid {
            cdc_test::mongodb_message("update", R"({"_id":7})"),
            cdc_test::mongodb_message("update", R"({"_id":7})", "null"),
            cdc_test::mongodb_message("insert", R"({"_id":7})", R"({"_id":8})"),
            cdc_test::mongodb_message("insert", R"({"_id":7})", R"({"name":"missing id"})"),
            cdc_test::mongodb_message("delete", "{}"),
            cdc_test::mongodb_message("delete", R"({"_id":null})"),
            cdc_test::mongodb_message("drop", R"({"_id":7})"),
            cdc_test::mongodb_message("insert", R"({"_id":7})",
                                      R"({"_id":7,"n":{"$numberDecimal":"NaN"}})"),
            R"({"operationType":"delete","operationType":"insert"})",
            R"({"op":"d","payload":{"id":7}})",
            R"({"operationType":"delete","ns":{"db":"shop","coll":"users"},"documentKey":{"_id":7},"bad":truex})",
            "null",
            "",
    };
    for (const auto& message : invalid) {
        SCOPED_TRACE(message);
        EXPECT_FALSE(decoder.decode(message, &event).ok());
        EXPECT_EQ(event.operation, CdcOperation::DELETE);
        EXPECT_EQ(event.keys_json, R"({"_id":7})");
    }
}

TEST(MongoDbCdcDecoderTest, RejectsExcessiveNesting) {
    MongoDbCdcDecoder decoder;
    CdcEvent event;
    const std::string nested = std::string(cdc_json::MAX_NESTING_DEPTH + 1, '[') + "0" +
                               std::string(cdc_json::MAX_NESTING_DEPTH + 1, ']');
    EXPECT_FALSE(decoder.decode(cdc_test::mongodb_message("insert", R"({"_id":7})",
                                                          "{\"_id\":7,\"nested\":" + nested + "}"),
                                &event)
                         .ok());
}

} // namespace doris
