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

#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "core/assert_cast.h"
#include "core/block/block.h"
#include "core/column/column_array.h"
#include "core/column/column_nullable.h"
#include "core/column/column_string.h"
#include "core/data_type/data_type_array.h"
#include "core/data_type/data_type_number.h"
#include "core/data_type/data_type_string.h"
#include "format/cdc/cdc_test_util.h"
#include "format/cdc/dynamodb_cdc_decoder.h"
#include "format/cdc/mongodb_cdc_decoder.h"

namespace doris {

TEST(CdcRowMapperTest, AppendsUpsertAndKeyOnlyDeleteWithoutChangingSharedRows) {
    CdcRowMapper mapper(cdc_test::mongodb_columns());
    ASSERT_TRUE(mapper.init().ok());
    auto block = mapper.create_block();
    MongoDbCdcDecoder decoder;
    CdcEvent event;
    ASSERT_TRUE(
            decoder.decode(cdc_test::mongodb_message(
                                   "insert", R"({"_id":7})",
                                   R"({"_id":7,"name":"line\nquote\"","score":9007199254740993})"),
                           &event)
                    .ok());
    auto status = mapper.append(event, &block);
    ASSERT_TRUE(status.ok()) << status.to_string();
    Block previous = block;

    ASSERT_TRUE(decoder.decode(cdc_test::mongodb_message("delete", R"({"_id":8})"), &event).ok());
    ASSERT_TRUE(mapper.append(event, &block).ok());
    ASSERT_EQ(block.rows(), 2);
    EXPECT_EQ(previous.rows(), 1);
    const auto& id = block.get_by_position(0);
    EXPECT_EQ(id.type->to_string(*id.column, 0), "7");
    EXPECT_EQ(id.type->to_string(*id.column, 1), "8");
    const auto& name = block.get_by_position(1);
    EXPECT_EQ(name.type->to_string(*name.column, 0), "line\nquote\"");
    EXPECT_TRUE(assert_cast<const ColumnNullable&>(*name.column).is_null_at(1));
    const auto& sign = block.get_by_position(3);
    EXPECT_EQ(sign.type->to_string(*sign.column, 0), "0");
    EXPECT_EQ(sign.type->to_string(*sign.column, 1), "1");
}

TEST(CdcRowMapperTest, RollsBackAnEntireRowOnConversionError) {
    CdcRowMapper mapper(cdc_test::mongodb_columns());
    ASSERT_TRUE(mapper.init().ok());
    auto block = mapper.create_block();
    MongoDbCdcDecoder decoder;
    CdcEvent event;
    ASSERT_TRUE(decoder.decode(cdc_test::mongodb_message("insert", R"({"_id":7})",
                                                         R"({"_id":7,"name":"before","score":1})"),
                               &event)
                        .ok());
    ASSERT_TRUE(mapper.append(event, &block).ok());
    ASSERT_TRUE(decoder.decode(cdc_test::mongodb_message(
                                       "update", R"({"_id":7})",
                                       R"({"_id":7,"name":"partial row","score":"invalid"})"),
                               &event)
                        .ok());
    EXPECT_FALSE(mapper.append(event, &block).ok());
    for (const auto& column : block.get_columns_with_type_and_name()) {
        EXPECT_EQ(column.column->size(), 1);
    }
    const auto& name = block.get_by_position(1);
    EXPECT_EQ(name.type->to_string(*name.column, 0), "before");
}

TEST(CdcRowMapperTest, RejectsInvalidNestedValuesWithoutLeavingPartialArrays) {
    auto columns = cdc_test::mongodb_columns();
    columns[2].type = make_nullable(
            std::make_shared<DataTypeArray>(make_nullable(std::make_shared<DataTypeInt32>())));
    CdcRowMapper mapper(std::move(columns));
    ASSERT_TRUE(mapper.init().ok());
    auto block = mapper.create_block();
    MongoDbCdcDecoder decoder;
    CdcEvent event;
    ASSERT_TRUE(decoder.decode(cdc_test::mongodb_message("insert", R"({"_id":7})",
                                                         R"({"_id":7,"score":[1,2]})"),
                               &event)
                        .ok());
    ASSERT_TRUE(mapper.append(event, &block).ok());
    ASSERT_TRUE(decoder.decode(cdc_test::mongodb_message("update", R"({"_id":7})",
                                                         R"({"_id":7,"score":[3,"bad"]})"),
                               &event)
                        .ok());
    EXPECT_FALSE(mapper.append(event, &block).ok());
    ASSERT_EQ(block.rows(), 1);
    const auto& nullable = assert_cast<const ColumnNullable&>(*block.get_by_position(2).column);
    const auto& array = assert_cast<const ColumnArray&>(nullable.get_nested_column());
    EXPECT_EQ(nullable.size(), 1);
    EXPECT_EQ(array.size(), 1);
    EXPECT_EQ(array.get_data().size(), 2);
}

TEST(CdcRowMapperTest, DistinguishesMissingNullAndDeletePlaceholders) {
    auto columns = cdc_test::mongodb_columns();
    columns[1].type = std::make_shared<DataTypeString>();
    columns[1].default_json = R"("default name")";
    CdcRowMapper mapper(std::move(columns));
    ASSERT_TRUE(mapper.init().ok());
    auto block = mapper.create_block();
    MongoDbCdcDecoder decoder;
    CdcEvent event;
    ASSERT_TRUE(decoder.decode(cdc_test::mongodb_message("insert", R"({"_id":7})", R"({"_id":7})"),
                               &event)
                        .ok());
    ASSERT_TRUE(mapper.append(event, &block).ok());
    const auto& name = block.get_by_position(1);
    EXPECT_EQ(name.type->to_string(*name.column, 0), "default name");

    ASSERT_TRUE(decoder.decode(cdc_test::mongodb_message("update", R"({"_id":7})",
                                                         R"({"_id":7,"name":null})"),
                               &event)
                        .ok());
    EXPECT_FALSE(mapper.append(event, &block).ok());
    ASSERT_EQ(block.rows(), 1);

    ASSERT_TRUE(decoder.decode(cdc_test::mongodb_message("delete", R"({"_id":7})"), &event).ok());
    ASSERT_TRUE(mapper.append(event, &block).ok());
    EXPECT_EQ(name.type->to_string(*name.column, 1), "");
}

TEST(CdcRowMapperTest, RawKeyProjectionRetainsObjectIdVersusStringIdentity) {
    auto columns = cdc_test::mongodb_columns();
    columns[0].type = std::make_shared<DataTypeString>();
    columns[0].source = CdcColumnSource::RAW_KEY;
    CdcRowMapper mapper(std::move(columns));
    ASSERT_TRUE(mapper.init().ok());
    auto block = mapper.create_block();
    MongoDbCdcDecoder decoder;
    CdcEvent event;
    ASSERT_TRUE(decoder.decode(cdc_test::mongodb_message(
                                       "delete", R"({"_id":{"$oid":"64b000000000000000000007"}})"),
                               &event)
                        .ok());
    ASSERT_TRUE(mapper.append(event, &block).ok());
    ASSERT_TRUE(decoder.decode(cdc_test::mongodb_message("delete",
                                                         R"({"_id":"64b000000000000000000007"})"),
                               &event)
                        .ok());
    ASSERT_TRUE(mapper.append(event, &block).ok());
    const auto& key = assert_cast<const ColumnString&>(*block.get_by_position(0).column);
    EXPECT_EQ(key.get_data_at(0).to_string(), R"({"$oid":"64b000000000000000000007"})");
    EXPECT_EQ(key.get_data_at(1).to_string(), R"("64b000000000000000000007")");
}

TEST(CdcRowMapperTest, RejectsIncompleteDynamoDbCompositeKeyMapping) {
    std::vector<CdcColumnMapping> columns {
            {"pk", "pk", std::make_shared<DataTypeString>(), CdcColumnSource::KEY},
            {"__DORIS_DELETE_SIGN__", "", std::make_shared<DataTypeInt8>(),
             CdcColumnSource::DELETE_SIGN}};
    CdcRowMapper mapper(std::move(columns));
    ASSERT_TRUE(mapper.init().ok());
    auto block = mapper.create_block();
    DynamoDbCdcDecoder decoder;
    CdcEvent event;
    ASSERT_TRUE(decoder.decode(cdc_test::dynamodb_message("REMOVE",
                                                          R"({"pk":{"S":"u7"},"sk":{"N":"42"}})"),
                               &event)
                        .ok());
    EXPECT_FALSE(mapper.append(event, &block).ok());
    EXPECT_EQ(block.rows(), 0);
}

TEST(CdcRowMapperTest, RequiresExplicitDeleteSignAndValidDefaults) {
    auto columns = cdc_test::mongodb_columns();
    columns.pop_back();
    CdcRowMapper missing_sign(std::move(columns));
    EXPECT_FALSE(missing_sign.init().ok());

    columns = cdc_test::mongodb_columns();
    columns[1].default_json = R"(1,"extra":2)";
    CdcRowMapper invalid_default(std::move(columns));
    EXPECT_FALSE(invalid_default.init().ok());
}

} // namespace doris
