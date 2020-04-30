#include "utilities/CommonOperations.h"
#include "utilities/StringUtils.h"

#include "CalciteExpressionParsing.h"
#include "Traits/RuntimeTraits.h"
#include <cudf/filling.hpp>
#include <cudf/concatenate.hpp>
#include <cudf/scalar/scalar_factories.hpp>
#include <blazingdb/io/Library/Logging/Logger.h>
#include <cudf/copying.hpp>
#include <cudf/unary.hpp>
#include <cudf/column/column_factories.hpp>
#include <cudf/strings/strings_column_view.hpp>
#include <numeric>

namespace ral {
namespace utilities {
namespace experimental {


std::unique_ptr<BlazingTable> concatTables(const std::vector<BlazingTableView> & tables) {
	assert(tables.size() >= 0);

	std::vector<std::unique_ptr<CudfTable>> temp_holder;
	std::vector<std::string> names;
	std::vector<CudfTableView> table_views_to_concat;
	for(size_t i = 0; i < tables.size(); i++) {
		if (tables[i].names().size() > 0){ // lets make sure we get the names from a table that is not empty
			names = tables[i].names();
		}
		if(tables[i].view().num_columns() > 0) { // lets make sure we are trying to concatenate tables that are not empty
			table_views_to_concat.push_back(tables[i].view());
		}
	}
	// TODO want to integrate data type normalization.
	// Data type normalization means that only some columns from a table would get normalized,
	// so we would need to manage the lifecycle of only a new columns that get allocated

	size_t empty_count = 0;	
	for(size_t i = 0; i < table_views_to_concat.size(); i++) {	
		ral::frame::BlazingTableView tview(table_views_to_concat[i], names);	

 		if (tview.num_rows() == 0) {	
			++empty_count;	
		}	
	}	

 	// All tables are empty so we just need to return the 1st one	
	if (empty_count == table_views_to_concat.size()) {	
		return std::make_unique<ral::frame::BlazingTable>(table_views_to_concat[0], names);	
	}	

	std::unique_ptr<CudfTable> concatenated_tables = cudf::experimental::concatenate(table_views_to_concat);
	return std::make_unique<BlazingTable>(std::move(concatenated_tables), names);
}

std::unique_ptr<ral::frame::BlazingTable> create_empty_table(const std::vector<std::string> &column_names, 
	const std::vector<cudf::type_id> &dtypes, std::vector<size_t> column_indices) {
	
	if (column_indices.size() == 0){
		column_indices.resize(column_names.size());
		std::iota(column_indices.begin(), column_indices.end(), 0);
	}

	std::vector<std::unique_ptr<cudf::column>> columns(column_indices.size());

	for (auto idx : column_indices) {
		columns[idx] =  make_empty_column(cudf::data_type(dtypes[idx]));
	}
	auto table = std::make_unique<cudf::experimental::table>(std::move(columns));
	return std::make_unique<ral::frame::BlazingTable>(std::move(table), column_names);
} 

std::unique_ptr<cudf::experimental::table> create_empty_table(const std::vector<cudf::type_id> &dtypes) {
	std::vector<std::unique_ptr<cudf::column>> columns(dtypes.size());
	for (size_t idx =0; idx < dtypes.size(); idx++) {
		columns[idx] =  make_empty_column(cudf::data_type(dtypes[idx]));
	}
	return std::make_unique<cudf::experimental::table>(std::move(columns));
} 

std::unique_ptr<ral::frame::BlazingTable> create_empty_table(const BlazingTableView & table) {

	std::unique_ptr<CudfTable> empty = cudf::experimental::empty_like(table.view());
	return std::make_unique<ral::frame::BlazingTable>(std::move(empty), table.names());	
}

std::vector<std::unique_ptr<ral::frame::BlazingColumn>> normalizeColumnTypes(std::vector<std::unique_ptr<ral::frame::BlazingColumn>> columns) {
	if(columns.size() < 2) {
		return columns;
	}

	cudf::type_id common_type = columns[0]->view().type().id();
	for(size_t j = 1; j < columns.size(); j++) {
		cudf::type_id type_out = get_common_type(common_type, columns[j]->view().type().id());

		if(type_out == cudf::type_id::EMPTY) {
			throw std::runtime_error("In normalizeColumnTypes function: no common type between " +
									 std::to_string(common_type) + " and " + std::to_string(columns[j]->view().type().id()));
		}
		common_type = type_out;
	}

	std::vector<std::unique_ptr<ral::frame::BlazingColumn>> columns_out;
	for(size_t j = 0; j < columns.size(); j++) {
		if(columns[j]->view().type().id() == common_type) {
			columns_out.emplace_back(std::move(columns[j]));
		} else {
			Library::Logging::Logger().logWarn(buildLogString("", "", "",
					"WARNING: normalizeColumnTypes casting " + std::to_string(columns[j]->view().type().id()) +
						" to " + std::to_string(common_type)));
			std::unique_ptr<CudfColumn> casted = cudf::experimental::cast(columns[j]->view(), cudf::data_type(common_type));
			columns_out.emplace_back(std::make_unique<ral::frame::BlazingColumnOwner>(std::move(casted)));
		}
	}
	return columns_out;
}

std::unique_ptr<cudf::column> make_string_column_from_scalar(const std::string& str, cudf::size_type rows) {

	std::unique_ptr<cudf::column> temp_no_data = std::make_unique<cudf::column>( 
		cudf::data_type{cudf::type_id::STRING}, rows,
		rmm::device_buffer{0}, // no data
		cudf::create_null_mask(rows, cudf::mask_state::ALL_NULL),
		rows );
	if (rows == 0){
		return temp_no_data;
	} else {
		auto scalar_value = cudf::make_string_scalar(str);
		scalar_value->set_valid(true); // https://github.com/rapidsai/cudf/issues/4085

		auto scalar_filled_column = cudf::experimental::fill(temp_no_data->view(), 0, rows, *scalar_value);
		return scalar_filled_column;
	}
}

int64_t get_table_size_bytes(const ral::frame::BlazingTableView & table){
	if (table.num_rows() == 0){
		return 0;
	} else {
		int64_t bytes = 0;
		CudfTableView table_view = table.view();
		for(size_t i = 0; i < table_view.num_columns(); i++) {
			if(table_view.column(i).type().id() == cudf::type_id::STRING){
				cudf::strings_column_view str_col_view{table_view.column(i)};
				auto offsets_column = str_col_view.offsets();
				auto chars_column = str_col_view.chars();
				bytes += (int64_t)(chars_column.size());
				bytes += (int64_t)(offsets_column.size()) * (int64_t)(sizeof(int32_t));
			} else {
				bytes += (int64_t)(ral::traits::get_dtype_size_in_bytes(table_view.column(i).type().id())) * (int64_t)(table_view.num_rows());
			}
		}
		return bytes;
	}
}

}  // namespace experimental
}  // namespace utilities
}  // namespace ral
