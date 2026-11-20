#include "rest/ducklake_rest_arrow.hpp"

#include "functions/ducklake_inlined_variant.hpp"
#include "rest/ducklake_rest_arrow_ipc.hpp"

#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/arrow/arrow_wrapper.hpp"
#include "duckdb/common/arrow/schema_metadata.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/table/arrow.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

namespace {

//! Whether the type holds a VARIANT, also inside a nested type
bool HoldsVariant(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::VARIANT:
		return true;
	case LogicalTypeId::STRUCT:
		for (auto &child : StructType::GetChildTypes(type)) {
			if (HoldsVariant(child.second)) {
				return true;
			}
		}
		return false;
	case LogicalTypeId::LIST:
		return HoldsVariant(ListType::GetChildType(type));
	case LogicalTypeId::MAP:
		return HoldsVariant(MapType::KeyType(type)) || HoldsVariant(MapType::ValueType(type));
	case LogicalTypeId::ARRAY:
		return HoldsVariant(ArrayType::GetChildType(type));
	default:
		return false;
	}
}

//! The type with every VARIANT replaced by the BLOB of its variant bytes
LogicalType VariantBytesType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::VARIANT:
		return LogicalType::BLOB;
	case LogicalTypeId::STRUCT: {
		child_list_t<LogicalType> children;
		for (auto &child : StructType::GetChildTypes(type)) {
			children.emplace_back(child.first, VariantBytesType(child.second));
		}
		return LogicalType::STRUCT(std::move(children));
	}
	case LogicalTypeId::LIST:
		return LogicalType::LIST(VariantBytesType(ListType::GetChildType(type)));
	case LogicalTypeId::MAP:
		return LogicalType::MAP(VariantBytesType(MapType::KeyType(type)), VariantBytesType(MapType::ValueType(type)));
	case LogicalTypeId::ARRAY:
		return LogicalType::ARRAY(VariantBytesType(ArrayType::GetChildType(type)), ArrayType::GetSize(type));
	default:
		return type;
	}
}

//! The type of a field of a stream, with a VARIANT where the field holds variant bytes
LogicalType StreamType(const ArrowSchema &field, const LogicalType &type) {
	ArrowSchemaMetadata metadata(field.metadata);
	if (metadata.GetOption(ArrowSchemaMetadata::ARROW_EXTENSION_NAME) == REST_VARIANT_EXTENSION) {
		return LogicalType::VARIANT();
	}
	switch (type.id()) {
	case LogicalTypeId::STRUCT: {
		child_list_t<LogicalType> children;
		auto &child_types = StructType::GetChildTypes(type);
		for (idx_t i = 0; i < child_types.size(); i++) {
			children.emplace_back(child_types[i].first, StreamType(*field.children[i], child_types[i].second));
		}
		return LogicalType::STRUCT(std::move(children));
	}
	case LogicalTypeId::LIST:
		return LogicalType::LIST(StreamType(*field.children[0], ListType::GetChildType(type)));
	case LogicalTypeId::MAP: {
		auto &entries = *field.children[0];
		return LogicalType::MAP(StreamType(*entries.children[0], MapType::KeyType(type)),
		                        StreamType(*entries.children[1], MapType::ValueType(type)));
	}
	case LogicalTypeId::ARRAY:
		return LogicalType::ARRAY(StreamType(*field.children[0], ArrayType::GetChildType(type)),
		                          ArrayType::GetSize(type));
	default:
		return type;
	}
}

//! Names the fields that hold variant bytes with the extension, which tells them from other BLOB fields
void MarkVariantFields(ArrowSchema &field, const LogicalType &type, DuckDBArrowSchemaHolder &holder) {
	if (!HoldsVariant(type)) {
		return;
	}
	switch (type.id()) {
	case LogicalTypeId::VARIANT:
		holder.metadata_info.emplace_back(
		    ArrowSchemaMetadata::ArrowCanonicalType(REST_VARIANT_EXTENSION).SerializeMetadata());
		field.metadata = holder.metadata_info.back().get();
		break;
	case LogicalTypeId::STRUCT: {
		auto &child_types = StructType::GetChildTypes(type);
		for (idx_t i = 0; i < child_types.size(); i++) {
			MarkVariantFields(*field.children[i], child_types[i].second, holder);
		}
		break;
	}
	case LogicalTypeId::LIST:
		MarkVariantFields(*field.children[0], ListType::GetChildType(type), holder);
		break;
	case LogicalTypeId::MAP: {
		auto &entries = *field.children[0];
		MarkVariantFields(*entries.children[0], MapType::KeyType(type), holder);
		MarkVariantFields(*entries.children[1], MapType::ValueType(type), holder);
		break;
	}
	case LogicalTypeId::ARRAY:
		MarkVariantFields(*field.children[0], ArrayType::GetChildType(type), holder);
		break;
	default:
		break;
	}
}

//! Copies the rows of the source into the target, which differ in their VARIANT leaves alone. The variant type is
//! the type of the side that holds the VARIANT, and the leaf function converts a VARIANT leaf
template <class LEAF>
void ConvertVariantLeaves(Vector &source, Vector &target, idx_t count, const LogicalType &variant_type, LEAF &&leaf) {
	if (!HoldsVariant(variant_type)) {
		target.Reference(source);
		return;
	}
	if (variant_type.id() == LogicalTypeId::VARIANT) {
		leaf(source, target, count);
		return;
	}
	source.Flatten(count);
	FlatVector::SetValidity(target, FlatVector::Validity(source));
	switch (variant_type.id()) {
	case LogicalTypeId::STRUCT: {
		auto &source_entries = StructVector::GetEntries(source);
		auto &target_entries = StructVector::GetEntries(target);
		auto &child_types = StructType::GetChildTypes(variant_type);
		for (idx_t i = 0; i < child_types.size(); i++) {
			ConvertVariantLeaves(source_entries[i], target_entries[i], count, child_types[i].second, leaf);
		}
		break;
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP: {
		auto child_count = ListVector::GetListSize(source);
		memcpy(FlatVector::GetDataMutable<list_entry_t>(target), FlatVector::GetData<list_entry_t>(source),
		       count * sizeof(list_entry_t));
		ListVector::Reserve(target, child_count);
		ListVector::SetListSize(target, child_count);
		ConvertVariantLeaves(ListVector::GetEntry(source), ListVector::GetEntry(target), child_count,
		                     ListType::GetChildType(variant_type), leaf);
		break;
	}
	case LogicalTypeId::ARRAY:
		ConvertVariantLeaves(ArrayVector::GetEntry(source), ArrayVector::GetEntry(target),
		                     count * ArrayType::GetSize(variant_type), ArrayType::GetChildType(variant_type), leaf);
		break;
	default:
		throw InternalException("ConvertVariantLeaves cannot convert a %s", variant_type.ToString());
	}
}

void EncodeVariant(Vector &source, Vector &target, idx_t count) {
	DuckLakeInlinedVariant::Encode(source, count, target);
}

void DecodeVariant(Vector &source, Vector &target, idx_t count) {
	DuckLakeInlinedVariant::Decode(source, count, target);
}

//! The chunk with its VARIANT leaves written as variant bytes, or read back from them
void ConvertChunk(DataChunk &source, DataChunk &target, const vector<LogicalType> &variant_types, bool encode) {
	target.Reset();
	for (idx_t col = 0; col < source.ColumnCount(); col++) {
		if (encode) {
			ConvertVariantLeaves(source.data[col], target.data[col], source.size(), variant_types[col], EncodeVariant);
		} else {
			ConvertVariantLeaves(source.data[col], target.data[col], source.size(), variant_types[col], DecodeVariant);
		}
	}
	target.SetCardinality(source.size());
}

} // namespace

string RESTArrow::Write(ClientContext &context, const vector<string> &names, const ColumnDataCollection &rows,
                        bool variant_bytes) {
	auto &types = rows.Types();
	vector<LogicalType> arrow_types;
	bool convert = false;
	for (auto &type : types) {
		arrow_types.push_back(variant_bytes ? VariantBytesType(type) : type);
		convert = convert || arrow_types.back() != type;
	}
	// types that Arrow does not have are sent as extension types
	ClientProperties options("UTC", ArrowOffsetSize::REGULAR, false, false, true, ArrowFormatVersion::V1_0, &context);
	ArrowSchemaWrapper schema;
	ArrowConverter::ToArrowSchema(&schema.arrow_schema, arrow_types, names, options);
	if (convert) {
		auto &holder = *static_cast<DuckDBArrowSchemaHolder *>(schema.arrow_schema.private_data);
		for (idx_t col = 0; col < types.size(); col++) {
			MarkVariantFields(*schema.arrow_schema.children[col], types[col], holder);
		}
	}
	vector<ArrowArrayWrapper> wrappers;
	vector<ArrowArray> batches;
	auto extensions = ArrowTypeExtensionData::GetExtensionTypes(context, arrow_types);
	DataChunk encoded;
	if (convert) {
		encoded.Initialize(Allocator::DefaultAllocator(), arrow_types);
	}
	for (auto &chunk : rows.Chunks()) {
		wrappers.emplace_back();
		if (convert) {
			ConvertChunk(chunk, encoded, types, true);
			ArrowConverter::ToArrowArray(encoded, &wrappers.back().arrow_array, options, extensions);
		} else {
			ArrowConverter::ToArrowArray(chunk, &wrappers.back().arrow_array, options, extensions);
		}
		batches.push_back(wrappers.back().arrow_array);
	}
	return RESTArrowIPC::Write(schema.arrow_schema, batches);
}

unique_ptr<ColumnDataCollection> RESTArrow::Read(ClientContext &context, shared_ptr<const string> data, idx_t offset,
                                                 idx_t size, const vector<LogicalType> &types, vector<string> &names) {
	auto stream = RESTArrowIPC::Read(std::move(data), offset, size);
	ArrowTableSchema arrow_table;
	ArrowTableFunction::PopulateArrowTableSchema(context, arrow_table, stream->schema);
	vector<LogicalType> arrow_types;
	// the types of the stream, with the VARIANT of the fields that hold variant bytes
	vector<LogicalType> stream_types;
	bool convert = false;
	names.clear();
	for (idx_t col_idx = 0; col_idx < NumericCast<idx_t>(stream->schema.n_children); col_idx++) {
		auto &field = *stream->schema.children[col_idx];
		arrow_types.push_back(arrow_table.GetColumns().at(col_idx)->GetDuckType());
		stream_types.push_back(StreamType(field, arrow_types.back()));
		convert = convert || stream_types.back() != arrow_types.back();
		names.push_back(field.name);
	}
	if (!types.empty() && types.size() != arrow_types.size()) {
		// LCOV_EXCL_START
		// the DuckLake server writes the columns of the API
		throw InvalidInputException("The Arrow stream has %llu columns instead of %llu", arrow_types.size(),
		                            types.size());
		// LCOV_EXCL_STOP
	}
	auto &result_types = types.empty() ? stream_types : types;
	auto result = make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator(), result_types);
	DataChunk scanned;
	scanned.Initialize(Allocator::DefaultAllocator(), arrow_types);
	DataChunk decoded;
	if (convert) {
		decoded.Initialize(Allocator::DefaultAllocator(), stream_types);
	}
	DataChunk converted;
	if (!types.empty()) {
		converted.Initialize(Allocator::DefaultAllocator(), types);
	}
	for (auto &batch : stream->batches) {
		auto wrapper = make_uniq<ArrowArrayWrapper>();
		wrapper->arrow_array = batch;
		batch.release = nullptr;
		ArrowScanLocalState state(std::move(wrapper), context);
		auto length = NumericCast<idx_t>(state.chunk->arrow_array.length);
		while (state.chunk_offset < length) {
			auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, length - state.chunk_offset);
			scanned.Reset();
			scanned.SetChildCardinality(count);
			ArrowTableFunction::ArrowToDuckDB(state, arrow_table.GetColumns(), scanned);
			state.chunk_offset += count;
			if (convert) {
				ConvertChunk(scanned, decoded, stream_types, false);
			}
			auto &rows = convert ? decoded : scanned;
			if (types.empty()) {
				result->Append(rows);
				continue;
			}
			converted.Reset();
			for (idx_t col_idx = 0; col_idx < types.size(); col_idx++) {
				if (rows.data[col_idx].GetType() == types[col_idx]) {
					converted.data[col_idx].Reference(rows.data[col_idx]);
				} else {
					// LCOV_EXCL_START
					// the DuckLake server writes the types of the API
					VectorOperations::Cast(context, rows.data[col_idx], converted.data[col_idx], count);
					// LCOV_EXCL_STOP
				}
			}
			converted.SetChildCardinality(count);
			result->Append(converted);
		}
	}
	return result;
}

} // namespace duckdb
