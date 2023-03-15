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

#include <vec/columns/column_array.h>
#include <vec/columns/column_object.h>
#include <vec/common/schema_util.h>
#include <vec/core/field.h>
#include <vec/data_types/data_type_array.h>
#include <vec/data_types/data_type_object.h>
#include <vec/functions/simple_function_factory.h>
#include <vec/json/parse2column.h>

#include <vec/data_types/data_type_factory.hpp>

#include "gen_cpp/FrontendService.h"
#include "gen_cpp/HeartbeatService_types.h"
#include "olap/rowset/rowset_writer_context.h"
#include "runtime/client_cache.h"
#include "runtime/exec_env.h"
#include "util/thrift_rpc_helper.h"

namespace doris::vectorized::schema_util {

size_t get_number_of_dimensions(const IDataType& type) {
    if (const auto* type_array = typeid_cast<const DataTypeArray*>(&type)) {
        return type_array->get_number_of_dimensions();
    }
    return 0;
}
size_t get_number_of_dimensions(const IColumn& column) {
    if (const auto* column_array = check_and_get_column<ColumnArray>(column)) {
        return column_array->get_number_of_dimensions();
    }
    return 0;
}

DataTypePtr get_base_type_of_array(const DataTypePtr& type) {
    /// Get raw pointers to avoid extra copying of type pointers.
    const DataTypeArray* last_array = nullptr;
    const auto* current_type = type.get();
    while (const auto* type_array = typeid_cast<const DataTypeArray*>(current_type)) {
        current_type = type_array->get_nested_type().get();
        last_array = type_array;
    }
    return last_array ? last_array->get_nested_type() : type;
}

Array create_empty_array_field(size_t num_dimensions) {
    DCHECK(num_dimensions > 0);
    Array array;
    Array* current_array = &array;
    for (size_t i = 1; i < num_dimensions; ++i) {
        current_array->push_back(Array());
        current_array = &current_array->back().get<Array&>();
    }
    return array;
}

FieldType get_field_type(const IDataType* data_type) {
    switch (data_type->get_type_id()) {
    case TypeIndex::UInt8:
        return FieldType::OLAP_FIELD_TYPE_UNSIGNED_TINYINT;
    case TypeIndex::UInt16:
        return FieldType::OLAP_FIELD_TYPE_UNSIGNED_SMALLINT;
    case TypeIndex::UInt32:
        return FieldType::OLAP_FIELD_TYPE_UNSIGNED_INT;
    case TypeIndex::UInt64:
        return FieldType::OLAP_FIELD_TYPE_UNSIGNED_BIGINT;
    case TypeIndex::Int8:
        return FieldType::OLAP_FIELD_TYPE_TINYINT;
    case TypeIndex::Int16:
        return FieldType::OLAP_FIELD_TYPE_SMALLINT;
    case TypeIndex::Int32:
        return FieldType::OLAP_FIELD_TYPE_INT;
    case TypeIndex::Int64:
        return FieldType::OLAP_FIELD_TYPE_BIGINT;
    case TypeIndex::Float32:
        return FieldType::OLAP_FIELD_TYPE_FLOAT;
    case TypeIndex::Float64:
        return FieldType::OLAP_FIELD_TYPE_DOUBLE;
    case TypeIndex::Decimal32:
        return FieldType::OLAP_FIELD_TYPE_DECIMAL;
    case TypeIndex::Array:
        return FieldType::OLAP_FIELD_TYPE_ARRAY;
    case TypeIndex::String:
        return FieldType::OLAP_FIELD_TYPE_STRING;
    case TypeIndex::Date:
        return FieldType::OLAP_FIELD_TYPE_DATE;
    case TypeIndex::DateTime:
        return FieldType::OLAP_FIELD_TYPE_DATETIME;
    case TypeIndex::Tuple:
        return FieldType::OLAP_FIELD_TYPE_STRUCT;
    // TODO add more types
    default:
        LOG(FATAL) << "unknow type";
        return FieldType::OLAP_FIELD_TYPE_UNKNOWN;
    }
}

bool is_conversion_required_between_integers(const IDataType& lhs, const IDataType& rhs) {
    WhichDataType which_lhs(lhs);
    WhichDataType which_rhs(rhs);
    bool is_native_int = which_lhs.is_native_int() && which_rhs.is_native_int();
    bool is_native_uint = which_lhs.is_native_uint() && which_rhs.is_native_uint();
    return (is_native_int || is_native_uint) &&
           lhs.get_size_of_value_in_memory() <= rhs.get_size_of_value_in_memory();
}

bool is_conversion_required_between_integers(FieldType lhs, FieldType rhs) {
    // We only support signed integers for semi-structure data at present
    // TODO add unsigned integers
    if (lhs == OLAP_FIELD_TYPE_BIGINT) {
        return !(rhs == OLAP_FIELD_TYPE_TINYINT || rhs == OLAP_FIELD_TYPE_SMALLINT ||
                 rhs == OLAP_FIELD_TYPE_INT || rhs == OLAP_FIELD_TYPE_BIGINT);
    }
    if (lhs == OLAP_FIELD_TYPE_INT) {
        return !(rhs == OLAP_FIELD_TYPE_TINYINT || rhs == OLAP_FIELD_TYPE_SMALLINT ||
                 rhs == OLAP_FIELD_TYPE_INT);
    }
    if (lhs == OLAP_FIELD_TYPE_SMALLINT) {
        return !(rhs == OLAP_FIELD_TYPE_TINYINT || rhs == OLAP_FIELD_TYPE_SMALLINT);
    }
    if (lhs == OLAP_FIELD_TYPE_TINYINT) {
        return !(rhs == OLAP_FIELD_TYPE_TINYINT);
    }
    return true;
}

Status cast_column(const ColumnWithTypeAndName& arg, const DataTypePtr& type, ColumnPtr* result) {
    ColumnsWithTypeAndName arguments {arg,
                                      {type->create_column_const_with_default_value(1), type, ""}};
    auto function = SimpleFunctionFactory::instance().get_function("CAST", arguments, type);
    Block tmp_block {arguments};
    // the 0 position is input argument, the 1 position is to type argument, the 2 position is result argument
    vectorized::ColumnNumbers argnum;
    argnum.emplace_back(0);
    argnum.emplace_back(1);
    size_t result_column = tmp_block.columns();
    tmp_block.insert({nullptr, type, arg.name});
    RETURN_IF_ERROR(
            function->execute(nullptr, tmp_block, argnum, result_column, arg.column->size()));
    *result = std::move(tmp_block.get_by_position(result_column).column);
    return Status::OK();
}

static void get_column_def(const vectorized::DataTypePtr& data_type, const std::string& name,
                           TColumnDef* column) {
    if (!name.empty()) {
        column->columnDesc.__set_columnName(name);
    }
    if (data_type->is_nullable()) {
        const auto& real_type = static_cast<const DataTypeNullable&>(*data_type);
        column->columnDesc.__set_isAllowNull(true);
        get_column_def(real_type.get_nested_type(), "", column);
        return;
    }
    column->columnDesc.__set_columnType(to_thrift(get_primitive_type(data_type->get_type_id())));
    if (data_type->get_type_id() == TypeIndex::Array) {
        TColumnDef child;
        column->columnDesc.__set_children({});
        get_column_def(assert_cast<const DataTypeArray*>(data_type.get())->get_nested_type(), "",
                       &child);
        column->columnDesc.columnLength =
                TabletColumn::get_field_length_by_type(column->columnDesc.columnType, 0);
        column->columnDesc.children.push_back(child.columnDesc);
        return;
    }
    if (data_type->get_type_id() == TypeIndex::Tuple) {
        // TODO
        // auto tuple_type = assert_cast<const DataTypeTuple*>(data_type.get());
        // DCHECK_EQ(tuple_type->get_elements().size(), tuple_type->get_element_names().size());
        // for (size_t i = 0; i < tuple_type->get_elements().size(); ++i) {
        //     TColumnDef child;
        //     get_column_def(tuple_type->get_element(i), tuple_type->get_element_names()[i], &child);
        //     column->columnDesc.children.push_back(child.columnDesc);
        // }
        // return;
    }
    if (data_type->get_type_id() == TypeIndex::String) {
        return;
    }
    if (WhichDataType(*data_type).is_simple()) {
        column->columnDesc.__set_columnLength(data_type->get_size_of_value_in_memory());
        return;
    }
}

// send an empty add columns rpc, the rpc response will fill with base schema info
// maybe we could seperate this rpc from add columns rpc
Status send_fetch_full_base_schema_view_rpc(FullBaseSchemaView* schema_view) {
    TAddColumnsRequest req;
    TAddColumnsResult res;
    TTabletInfo tablet_info;
    req.__set_table_name(schema_view->table_name);
    req.__set_db_name(schema_view->db_name);
    req.__set_table_id(schema_view->table_id);
    // Set empty columns
    req.__set_addColumns({});
    auto master_addr = ExecEnv::GetInstance()->master_info()->network_address;
    Status rpc_st = ThriftRpcHelper::rpc<FrontendServiceClient>(
            master_addr.hostname, master_addr.port,
            [&req, &res](FrontendServiceConnection& client) { client->addColumns(res, req); },
            config::txn_commit_rpc_timeout_ms);
    if (!rpc_st.ok()) {
        return Status::InternalError("Failed to fetch schema info, encounter rpc failure");
    }
    // TODO(lhy) handle more status code
    if (res.status.status_code != TStatusCode::OK) {
        LOG(WARNING) << "failed to fetch schema info, code:" << res.status.status_code
                     << ", msg:" << res.status.error_msgs[0];
        return Status::InvalidArgument(
                fmt::format("Failed to fetch schema info, {}", res.status.error_msgs[0]));
    }
    for (const auto& column : res.allColumns) {
        schema_view->column_name_to_column[column.column_name] = column;
    }
    schema_view->schema_version = res.schema_version;
    return Status::OK();
}

// Do batch add columns schema change
// only the base table supported
Status send_add_columns_rpc(ColumnsWithTypeAndName column_type_names,
                            FullBaseSchemaView* schema_view) {
    if (column_type_names.empty()) {
        return Status::OK();
    }
    TAddColumnsRequest req;
    TAddColumnsResult res;
    TTabletInfo tablet_info;
    req.__set_table_name(schema_view->table_name);
    req.__set_db_name(schema_view->db_name);
    req.__set_table_id(schema_view->table_id);
    // TODO(lhy) more configurable
    req.__set_allow_type_conflict(true);
    req.__set_addColumns({});
    for (const auto& column_type_name : column_type_names) {
        TColumnDef col;
        get_column_def(column_type_name.type, column_type_name.name, &col);
        req.addColumns.push_back(col);
    }
    auto master_addr = ExecEnv::GetInstance()->master_info()->network_address;
    Status rpc_st = ThriftRpcHelper::rpc<FrontendServiceClient>(
            master_addr.hostname, master_addr.port,
            [&req, &res](FrontendServiceConnection& client) { client->addColumns(res, req); },
            config::txn_commit_rpc_timeout_ms);
    if (!rpc_st.ok()) {
        return Status::InternalError("Failed to do schema change, rpc error");
    }
    // TODO(lhy) handle more status code
    if (res.status.status_code != TStatusCode::OK) {
        LOG(WARNING) << "failed to do schema change, code:" << res.status.status_code
                     << ", msg:" << res.status.error_msgs[0];
        return Status::InvalidArgument(
                fmt::format("Failed to do schema change, {}", res.status.error_msgs[0]));
    }
    size_t sz = res.allColumns.size();
    if (sz < column_type_names.size()) {
        return Status::InternalError(
                fmt::format("Unexpected result columns {}, expected at least {}",
                            res.allColumns.size(), column_type_names.size()));
    }
    for (const auto& column : res.allColumns) {
        schema_view->column_name_to_column[column.column_name] = column;
    }
    schema_view->schema_version = res.schema_version;
    return Status::OK();
}

void unfold_object(size_t dynamic_col_position, std::vector<MutableColumnPtr>& columns,
                   const HashMap<StringRef, size_t, StringRefHash>& column_offset_map,
                   const std::vector<SlotDescriptor*>& slot_descs, bool cast_to_original_type) {
    auto* column_object_ptr = assert_cast<ColumnObject*>(columns[dynamic_col_position].get());
    if (column_object_ptr->empty()) {
        return;
    }
    size_t num_rows = column_object_ptr->size();
    CHECK(columns[0]->size() <= num_rows);
    CHECK(column_object_ptr->is_finalized());
    Columns subcolumns;
    DataTypes types;
    Names names;
    std::unordered_set<std::string> static_column_names;
    for (auto& subcolumn : column_object_ptr->get_subcolumns()) {
        subcolumns.push_back(subcolumn->data.get_finalized_column().get_ptr());
        types.push_back(subcolumn->data.get_least_common_type());
        names.push_back(subcolumn->path.get_path());
    }
    for (size_t i = 0; i < subcolumns.size(); ++i) {
        // block may already contains this column, eg. key columns, we should ignore
        // or replcace the same column from object subcolumn
        auto iter = column_offset_map.find(names[i]);
        if (iter != column_offset_map.end()) {
            ColumnPtr column = subcolumns[i];
            SlotDescriptor* slot_desc = slot_descs[iter->get_second()];
            DataTypePtr dst_type = slot_desc->get_data_type_ptr();
            if (cast_to_original_type && !dst_type->equals(*types[i])) {
                // Cast static columns to original slot type
                schema_util::cast_column({subcolumns[i], types[i], ""}, dst_type, &column);
            }
            // TODO swap to avoid memcpy
            columns[iter->get_second()]->insert_range_from(*column, 0, column->size());
            static_column_names.emplace(names[i]);
            continue;
        }
    }
    // remove static ones remain extra dynamic columns
    column_object_ptr->remove_subcolumns(static_column_names);
    // fill default value
    for (auto& column : columns) {
        if (column->size() < num_rows) {
            column->insert_many_defaults(num_rows - column->size());
        }
    }
    // column_object_ptr->clear();
}

void LocalSchemaChangeRecorder::add_extended_columns(const TabletColumn& new_column,
                                                     int32_t schema_version) {
    std::lock_guard<std::mutex> lock(_lock);
    _schema_version = std::max(_schema_version, schema_version);
    auto it = _extended_columns.find(new_column.name());
    if (it != _extended_columns.end()) {
        return;
    }
    _extended_columns.emplace_hint(it, new_column.name(), new_column);
}

bool LocalSchemaChangeRecorder::has_extended_columns() {
    std::lock_guard<std::mutex> lock(_lock);
    return !_extended_columns.empty();
}

std::map<std::string, TabletColumn> LocalSchemaChangeRecorder::copy_extended_columns() {
    std::lock_guard<std::mutex> lock(_lock);
    return _extended_columns;
}

const TabletColumn& LocalSchemaChangeRecorder::column(const std::string& col_name) {
    std::lock_guard<std::mutex> lock(_lock);
    assert(_extended_columns.find(col_name) != _extended_columns.end());
    return _extended_columns[col_name];
}

int32_t LocalSchemaChangeRecorder::schema_version() {
    std::lock_guard<std::mutex> lock(_lock);
    return _schema_version;
}

} // namespace doris::vectorized::schema_util