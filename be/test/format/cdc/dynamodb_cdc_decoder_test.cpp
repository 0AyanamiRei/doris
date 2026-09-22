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

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "format/cdc/cdc_test_util.h"

namespace doris {

TEST(DynamoDbCdcDecoderTest, DecodesTypedItemAndCompositeKey) {
    DynamoDbCdcDecoder decoder;
    CdcEvent event;
    auto status = decoder.decode(
            cdc_test::dynamodb_message(
                    "MODIFY", R"({"pk":{"S":"u7"},"sk":{"N":"42"}})",
                    R"({"pk":{"S":"u7"},"sk":{"N":"42"},"price":{"N":"123456789.123456789"},"enabled":{"BOOL":true},"nothing":{"NULL":true},"profile":{"M":{"name":{"S":"Alice"}}},"items":{"L":[{"N":"1"},{"S":"two"}]},"tags":{"SS":["a","b"]},"numbers":{"NS":["9007199254740993","2"]},"binary":{"B":"YQ=="},"binary_set":{"BS":["Yg=="]}})"),
            &event);
    ASSERT_TRUE(status.ok()) << status.to_string();
    EXPECT_EQ(event.operation, CdcOperation::UPSERT);
    EXPECT_EQ(event.source_namespace, "users");
    EXPECT_EQ(event.keys_json, R"({"pk":"u7","sk":42})");
    EXPECT_EQ(event.raw_keys_json, R"({"pk":{"S":"u7"},"sk":{"N":"42"}})");
    EXPECT_EQ(
            event.after_json,
            R"({"pk":"u7","sk":42,"price":123456789.123456789,"enabled":true,"nothing":null,"profile":{"name":"Alice"},"items":[1,"two"],"tags":["a","b"],"numbers":[9007199254740993,2],"binary":{"B":"YQ=="},"binary_set":{"BS":["Yg=="]}})");
}

TEST(DynamoDbCdcDecoderTest, AcceptsNumericWireNAndRemovesByKeys) {
    DynamoDbCdcDecoder decoder;
    CdcEvent event;
    auto status = decoder.decode(
            cdc_test::dynamodb_message("INSERT", R"({"pk":{"S":"u7"}})",
                                       R"({"pk":{"S":"u7"},"n":{"N":9007199254740993}})"),
            &event);
    ASSERT_TRUE(status.ok()) << status.to_string();
    EXPECT_EQ(event.after_json, R"({"pk":"u7","n":9007199254740993})");
    ASSERT_TRUE(decoder.decode(cdc_test::dynamodb_message("REMOVE", R"({"pk":{"S":"u7"}})"), &event)
                        .ok());
    EXPECT_EQ(event.operation, CdcOperation::DELETE);
    EXPECT_EQ(event.keys_json, R"({"pk":"u7"})");
    EXPECT_TRUE(event.after_json.empty());
}

TEST(DynamoDbCdcDecoderTest, RejectsMalformedAttributesKeysAndMissingImages) {
    DynamoDbCdcDecoder decoder;
    const std::vector<std::string> invalid {
            cdc_test::dynamodb_message("MODIFY", R"({"pk":{"S":"u7"}})"),
            cdc_test::dynamodb_message("INSERT", R"({"pk":{"S":"u7"}})", R"({"pk":{"S":"other"}})"),
            cdc_test::dynamodb_message("REMOVE", "{}"),
            cdc_test::dynamodb_message("REMOVE", R"({"pk":{"NULL":true}})"),
            cdc_test::dynamodb_message("REMOVE", R"({"pk":{"S":"u7","N":"7"}})"),
            cdc_test::dynamodb_message("INSERT", R"({"pk":{"S":"u7"}})",
                                       R"({"pk":{"S":"u7"},"bad":{"N":"1,\"injected\":true"}})"),
            cdc_test::dynamodb_message("INSERT", R"({"pk":{"S":"u7"}})",
                                       R"({"pk":{"S":"u7"},"bad":{"NULL":false}})"),
            cdc_test::dynamodb_message("INSERT", R"({"pk":{"S":"u7"}})",
                                       R"({"pk":{"S":"u7"},"bad":{}})"),
            cdc_test::dynamodb_message("INSERT", R"({"pk":{"S":"u7"}})",
                                       R"({"pk":{"S":"u7"},"bad":{"UNKNOWN":"value"}})"),
            cdc_test::dynamodb_message("TRUNCATE", R"({"pk":{"S":"u7"}})"),
            R"({"Records":[]})",
    };
    for (const auto& message : invalid) {
        SCOPED_TRACE(message);
        CdcEvent event;
        EXPECT_FALSE(decoder.decode(message, &event).ok());
    }
}

} // namespace doris
