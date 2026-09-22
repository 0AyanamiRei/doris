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
#include <string>
#include <vector>

#include "core/assert_cast.h"
#include "core/block/block.h"
#include "core/column/column_nullable.h"
#include "core/column/column_vector.h"
#include "core/data_type/data_type_array.h"
#include "core/data_type/data_type_decimal.h"
#include "core/data_type/data_type_nullable.h"
#include "core/data_type/data_type_number.h"
#include "core/data_type/data_type_string.h"
#include "format/json/new_json_reader.h"
#include "runtime/descriptors.h"

namespace doris {

static constexpr size_t kDefaultBatchSize = 4064;

TEST(NewJsonReaderValueTest, ConvertsValuesWithoutReaderState) {
    struct TestCase {
        std::string json;
        DataTypePtr type;
        std::string expected;
    };
    const std::vector<TestCase> cases {
            {R"("line\nquote\"\u4e2d")", std::make_shared<DataTypeString>(), "line\nquote\"中"},
            {"true", std::make_shared<DataTypeUInt8>(), "1"},
            {"false", std::make_shared<DataTypeString>(), "0"},
            {"9007199254740993", std::make_shared<DataTypeInt64>(), "9007199254740993"},
            {"123456789.123456789", std::make_shared<DataTypeDecimal128>(20, 9),
             "123456789.123456789"},
            {"[1, 2, 3]",
             std::make_shared<DataTypeArray>(make_nullable(std::make_shared<DataTypeInt32>())),
             "[1, 2, 3]"},
            {R"({"a":1,"b":[2,3]})", std::make_shared<DataTypeString>(), R"({"a":1,"b":[2,3]})"},
    };
    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.json);
        simdjson::padded_string json("{\"value\":" + test_case.json + "}");
        simdjson::ondemand::parser parser;
        auto document = parser.iterate(json);
        auto value = document["value"].value();
        auto column = test_case.type->create_column();
        auto serde = test_case.type->get_serde();
        DataTypeSerDe::FormatOptions options;

        auto status = NewJsonReader::write_json_value_to_column(value, *column, *serde, options);
        ASSERT_TRUE(status.ok()) << status.to_string();
        ASSERT_EQ(column->size(), 1);
        EXPECT_EQ(test_case.type->to_string(*column, 0), test_case.expected);
    }
}

TEST(NewJsonReaderValueTest, ReturnsConversionErrorWithoutAppending) {
    simdjson::padded_string json(std::string(R"({"value":"not-an-integer"})"));
    simdjson::ondemand::parser parser;
    auto document = parser.iterate(json);
    auto value = document["value"].value();
    DataTypeInt64 type;
    auto column = type.create_column();
    auto serde = type.get_serde();
    DataTypeSerDe::FormatOptions options;

    auto status = NewJsonReader::write_json_value_to_column(value, *column, *serde, options);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(column->size(), 0);
}

class NewJsonReaderRowConversionTest : public testing::Test {
protected:
    void SetUp() override {
        _params.__set_format_type(TFileFormatType::FORMAT_JSON);
        _params.file_attributes.text_params.line_delimiter = "\n";
        _params.file_attributes.__isset.text_params = true;
        _params.__isset.file_attributes = true;
        _range.path = "/nonexistent/test.json";
        _reader = NewJsonReader::create_unique(nullptr, _params, _range, _slots, kDefaultBatchSize,
                                               nullptr);
    }

    void add_column(const std::string& name, DataTypePtr type) {
        const auto index = static_cast<int>(_slots.size());
        TSlotDescriptor desc;
        desc.__set_id(index);
        desc.__set_parent(0);
        desc.__set_slotType(type->to_thrift());
        desc.__set_columnPos(index);
        desc.__set_byteOffset(0);
        desc.__set_nullIndicatorByte(0);
        desc.__set_nullIndicatorBit(type->is_nullable() ? index : -1);
        desc.__set_slotIdx(index);
        desc.__set_isMaterialized(true);
        desc.__set_colName(name);
        _slot_storage.emplace_back(std::make_unique<SlotDescriptor>(desc));
        auto* slot = _slot_storage.back().get();
        _slots.push_back(slot);
        _reader->_slot_desc_index[StringRef(slot->col_name())] = index;
        _reader->_serdes.emplace_back(type->get_serde());
        _block.insert({type->create_column(), type, name});
    }

    void append_row(const std::string& input, bool use_jsonpaths = false) {
        simdjson::padded_string json(input);
        simdjson::ondemand::parser parser;
        auto document = parser.iterate(json);
        simdjson::ondemand::object object = document.get_object();
        bool valid = false;
        auto status = use_jsonpaths ? _reader->_simdjson_write_columns_by_jsonpath(&object, _slots,
                                                                                   _block, &valid)
                                    : _reader->_simdjson_set_column_value(&object, _block, _slots,
                                                                          &valid);
        ASSERT_TRUE(status.ok()) << status.to_string();
        ASSERT_TRUE(valid);
    }

    TFileScanRangeParams _params;
    TFileRangeDesc _range;
    std::vector<std::unique_ptr<SlotDescriptor>> _slot_storage;
    std::vector<SlotDescriptor*> _slots;
    std::unique_ptr<NewJsonReader> _reader;
    Block _block;
};

TEST_F(NewJsonReaderRowConversionTest, PreservesNullableAndMissingColumns) {
    auto name_type = make_nullable(std::make_shared<DataTypeString>());
    auto flag_type = make_nullable(std::make_shared<DataTypeUInt8>());
    add_column("name", name_type);
    add_column("flag", flag_type);

    append_row(R"({"name":"line\nquote\"","flag":true})");
    append_row(R"({"name":null,"flag":false})");
    append_row(R"({"flag":true})");

    ASSERT_EQ(_block.rows(), 3);
    const auto& names = *_block.get_by_position(0).column;
    EXPECT_EQ(name_type->to_string(names, 0), "line\nquote\"");
    EXPECT_TRUE(assert_cast<const ColumnNullable&>(names).is_null_at(1));
    EXPECT_TRUE(assert_cast<const ColumnNullable&>(names).is_null_at(2));
    const auto& flags = *_block.get_by_position(1).column;
    EXPECT_EQ(flag_type->to_string(flags, 0), "1");
    EXPECT_EQ(flag_type->to_string(flags, 1), "0");
    EXPECT_EQ(flag_type->to_string(flags, 2), "1");
}

TEST_F(NewJsonReaderRowConversionTest, PreservesRepeatedJsonPathStringDecoding) {
    auto type = make_nullable(std::make_shared<DataTypeString>());
    add_column("first", type);
    add_column("second", type);
    for (int i = 0; i < 2; ++i) {
        _reader->_parsed_jsonpaths.emplace_back();
        JsonFunctions::parse_json_paths("$.name", &_reader->_parsed_jsonpaths.back());
    }

    append_row(R"({"name":"line\nquote\""})", true);
    append_row(R"({"name":"next\tvalue"})", true);

    ASSERT_EQ(_block.rows(), 2);
    for (size_t i = 0; i < 2; ++i) {
        const auto& column = *_block.get_by_position(i).column;
        EXPECT_EQ(type->to_string(column, 0), "line\nquote\"");
        EXPECT_EQ(type->to_string(column, 1), "next\tvalue");
    }
    EXPECT_TRUE(_reader->_cached_string_values.empty());
}

// Test that set_batch_size stores the value correctly.
TEST(NewJsonReaderSetBatchSizeTest, SetBatchSizeStoresValue) {
    TFileScanRangeParams params;
    params.format_type = TFileFormatType::FORMAT_JSON;
    params.__isset.file_attributes = true;
    params.file_attributes.__isset.text_params = true;
    params.file_attributes.text_params.line_delimiter = "\n";

    TFileRangeDesc range;
    range.path = "/nonexistent/test.json";
    range.start_offset = 0;
    range.size = 0;

    std::vector<SlotDescriptor*> file_slot_descs;
    // Use the second constructor (profile, params, range, file_slot_descs, io_ctx)
    // to avoid the first constructor's ADD_TIMER(_profile, ...) which crashes on nullptr.
    auto reader = NewJsonReader::create_unique(nullptr, params, range, file_slot_descs,
                                               kDefaultBatchSize, nullptr);

    // Default: _batch_size is initialized to _MIN_BATCH_SIZE.
    EXPECT_EQ(reader->get_batch_size(), 4064U);

    // After set_batch_size, it should store the value (clamped to >=_MIN_BATCH_SIZE).
    reader->set_batch_size(8192);
    EXPECT_EQ(reader->get_batch_size(), 8192U);

    // Calling set_batch_size multiple times should update the value.
    reader->set_batch_size(16384);
    EXPECT_EQ(reader->get_batch_size(), 16384U);

    // Setting below _MIN_BATCH_SIZE (or 0) clamps to 1 so the
    // reader never spins on empty blocks.
    reader->set_batch_size(0);
    EXPECT_EQ(reader->get_batch_size(), 1UL);
}

// Test that set_batch_size is callable via the GenericReader interface.
TEST(NewJsonReaderSetBatchSizeTest, SetBatchSizeViaGenericInterface) {
    TFileScanRangeParams params;
    params.format_type = TFileFormatType::FORMAT_JSON;
    params.__isset.file_attributes = true;
    params.file_attributes.__isset.text_params = true;
    params.file_attributes.text_params.line_delimiter = "\n";

    TFileRangeDesc range;
    range.path = "/nonexistent/test.json";
    range.start_offset = 0;
    range.size = 0;

    std::vector<SlotDescriptor*> file_slot_descs;
    // Use the second constructor to avoid nullptr profile crash in ADD_TIMER.
    auto reader = NewJsonReader::create_unique(nullptr, params, range, file_slot_descs,
                                               kDefaultBatchSize, nullptr);

    // Access through base class pointer — this is how FileScanner calls it.
    GenericReader* base_reader = reader.get();
    base_reader->set_batch_size(8192);
    EXPECT_EQ(base_reader->get_batch_size(), 8192U);
    base_reader->set_batch_size(4096);
    EXPECT_EQ(base_reader->get_batch_size(), 4096U);
}

TEST(NewJsonReaderCowTest, AppendNullForMalformedJsonMutatesOwnerColumn) {
    auto nested_column = ColumnInt32::create();
    nested_column->insert_value(7);
    auto null_map = ColumnUInt8::create();
    null_map->insert_value(0);
    ColumnPtr shared_column = ColumnNullable::create(std::move(nested_column), std::move(null_map));
    const auto* original_column = shared_column.get();

    Block block;
    block.insert({shared_column, make_nullable(std::make_shared<DataTypeInt32>()), "c0"});

    ASSERT_TRUE(json_reader_detail::append_null_for_malformed_json(block).ok());
    ASSERT_EQ(block.rows(), 2);
    EXPECT_NE(block.get_by_position(0).column.get(), original_column);

    const auto& result_column =
            assert_cast<const ColumnNullable&>(*block.get_by_position(0).column);
    EXPECT_FALSE(result_column.is_null_at(0));
    EXPECT_TRUE(result_column.is_null_at(1));

    const auto& original_nullable = assert_cast<const ColumnNullable&>(*shared_column);
    EXPECT_EQ(original_nullable.size(), 1);
    EXPECT_FALSE(original_nullable.is_null_at(0));
}

TEST(NewJsonReaderCowTest, TruncateBlockToRowsMutatesOwnerColumn) {
    auto nested_column = ColumnInt32::create();
    nested_column->insert_value(7);
    nested_column->insert_value(8);
    auto null_map = ColumnUInt8::create();
    null_map->insert_value(0);
    null_map->insert_value(0);
    ColumnPtr shared_column = ColumnNullable::create(std::move(nested_column), std::move(null_map));
    const auto* original_column = shared_column.get();

    Block block;
    block.insert({shared_column, make_nullable(std::make_shared<DataTypeInt32>()), "c0"});

    json_reader_detail::truncate_block_to_rows(block, 1);
    ASSERT_EQ(block.rows(), 1);
    EXPECT_NE(block.get_by_position(0).column.get(), original_column);

    const auto& result_column =
            assert_cast<const ColumnNullable&>(*block.get_by_position(0).column);
    EXPECT_EQ(result_column.size(), 1);
    EXPECT_FALSE(result_column.is_null_at(0));

    const auto& original_nullable = assert_cast<const ColumnNullable&>(*shared_column);
    EXPECT_EQ(original_nullable.size(), 2);
}

TEST(NewJsonReaderCowTest, PopBackLastInsertedValueMutatesOwnerColumn) {
    auto column = ColumnInt32::create();
    column->insert_value(7);
    column->insert_value(8);
    ColumnPtr shared_column = std::move(column);
    const auto* original_column = shared_column.get();

    Block block;
    block.insert({shared_column, std::make_shared<DataTypeInt32>(), "c0"});

    json_reader_detail::pop_back_last_inserted_value(block, 0);
    ASSERT_EQ(block.rows(), 1);
    EXPECT_NE(block.get_by_position(0).column.get(), original_column);

    const auto& result_column = assert_cast<const ColumnInt32&>(*block.get_by_position(0).column);
    EXPECT_EQ(result_column.size(), 1);
    EXPECT_EQ(result_column.get_data()[0], 7);

    const auto& original_int_column = assert_cast<const ColumnInt32&>(*shared_column);
    EXPECT_EQ(original_int_column.size(), 2);
    EXPECT_EQ(original_int_column.get_data()[0], 7);
    EXPECT_EQ(original_int_column.get_data()[1], 8);
}

} // namespace doris
