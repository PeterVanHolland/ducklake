#include "duckdb/common/types/variant/parquet_variant_iterator.hpp"
#include "duckdb/common/types/variant/variant_binary_decoder.hpp"
#include "duckdb/common/types/variant/variant_builder.hpp"
#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

namespace {

//! An inlined VARIANT is its Parquet Variant (the metadata followed by the value), followed by the declared types of
//! its decimals when it has any, since the Parquet encoding holds the scale of a decimal but not its width. The types
//! are a format byte and a (width, scale) byte pair per decimal in the order the values are built.
constexpr uint8_t DECIMAL_TYPES_FORMAT = 1;

idx_t ReadLittleEndian(const_data_ptr_t data, idx_t size) {
	idx_t result = 0;
	for (idx_t i = 0; i < size; i++) {
		result |= static_cast<idx_t>(data[i]) << (8 * i);
	}
	return result;
}

void CheckValueSize(idx_t size, idx_t capacity) {
	if (size > capacity) {
		throw IOException("Corrupted inlined VARIANT, the value exceeds its buffer");
	}
}

//! The size of the Parquet Variant value at the start of the buffer
idx_t ParquetVariantValueSize(const_data_ptr_t value, idx_t capacity) {
	CheckValueSize(1, capacity);
	auto header = VariantValueMetadata::FromHeaderByte(value[0]);
	switch (header.basic_type) {
	case VariantBasicType::PRIMITIVE: {
		idx_t payload;
		switch (header.primitive_type) {
		case VariantPrimitiveType::NULL_TYPE:
		case VariantPrimitiveType::BOOLEAN_TRUE:
		case VariantPrimitiveType::BOOLEAN_FALSE:
			payload = 0;
			break;
		case VariantPrimitiveType::INT8:
			payload = 1;
			break;
		case VariantPrimitiveType::INT16:
			payload = 2;
			break;
		case VariantPrimitiveType::INT32:
		case VariantPrimitiveType::FLOAT:
		case VariantPrimitiveType::DATE:
			payload = 4;
			break;
		case VariantPrimitiveType::INT64:
		case VariantPrimitiveType::DOUBLE:
		case VariantPrimitiveType::TIMESTAMP_MICROS:
		case VariantPrimitiveType::TIMESTAMP_NTZ_MICROS:
		case VariantPrimitiveType::TIME_NTZ_MICROS:
		case VariantPrimitiveType::TIMESTAMP_NANOS:
		case VariantPrimitiveType::TIMESTAMP_NTZ_NANOS:
			payload = 8;
			break;
		case VariantPrimitiveType::DECIMAL4:
			payload = 1 + 4;
			break;
		case VariantPrimitiveType::DECIMAL8:
			payload = 1 + 8;
			break;
		case VariantPrimitiveType::DECIMAL16:
			payload = 1 + 16;
			break;
		case VariantPrimitiveType::UUID:
			payload = 16;
			break;
		case VariantPrimitiveType::BINARY:
		case VariantPrimitiveType::STRING:
			CheckValueSize(1 + 4, capacity);
			payload = 4 + ReadLittleEndian(value + 1, 4);
			break;
		default:
			throw IOException("Corrupted inlined VARIANT, unknown primitive type");
		}
		CheckValueSize(1 + payload, capacity);
		return 1 + payload;
	}
	case VariantBasicType::SHORT_STRING:
		CheckValueSize(1 + header.string_size, capacity);
		return 1 + header.string_size;
	case VariantBasicType::OBJECT:
	case VariantBasicType::ARRAY: {
		idx_t count_size = header.is_large ? 4 : 1;
		CheckValueSize(1 + count_size, capacity);
		auto count = ReadLittleEndian(value + 1, count_size);
		idx_t offsets = 1 + count_size;
		if (header.basic_type == VariantBasicType::OBJECT) {
			offsets += count * header.field_id_size;
		}
		auto values = offsets + (count + 1) * header.field_offset_size;
		CheckValueSize(values, capacity);
		auto last_offset =
		    ReadLittleEndian(value + offsets + count * header.field_offset_size, header.field_offset_size);
		CheckValueSize(values + last_offset, capacity);
		return values + last_offset;
	}
	default:
		throw IOException("Corrupted inlined VARIANT, unknown basic type");
	}
}

//! The size of the Parquet Variant at the start of the blob
idx_t ParquetVariantSize(const string_t &blob) {
	VariantMetadata metadata(blob);
	auto size = metadata.total_size;
	if (size >= blob.GetSize()) {
		throw IOException("Corrupted inlined VARIANT, no value follows the metadata");
	}
	auto data = const_data_ptr_cast(blob.GetData());
	return size + ParquetVariantValueSize(data + size, blob.GetSize() - size);
}

//! Appends the (width, scale) of every decimal of the value in the order the builder emits the values
void CollectDecimalTypes(const VariantNode &node, string &types) {
	if (node.IsNull() || node.IsMissing()) {
		return;
	}
	switch (node.GetTypeId()) {
	case VariantLogicalType::OBJECT:
		for (auto &entry : node.GetObjectChildren(VariantIterationOrder::LEXICOGRAPHIC)) {
			CollectDecimalTypes(entry.value, types);
		}
		break;
	case VariantLogicalType::ARRAY:
		for (auto child : node.GetArrayChildren()) {
			CollectDecimalTypes(child, types);
		}
		break;
	case VariantLogicalType::DECIMAL: {
		auto properties = node.GetDecimalProperties();
		types.push_back(static_cast<char>(properties.width));
		types.push_back(static_cast<char>(properties.scale));
		break;
	}
	default:
		break;
	}
}

//! Whether a row of an unshredded VARIANT holds a decimal anywhere, read off its type ids
bool HoldsDecimal(const UnifiedVariantVectorData &variant, idx_t row) {
	auto values = variant.GetValuesListEntry(row);
	for (idx_t i = 0; i < values.length; i++) {
		if (variant.GetTypeId(row, values.offset + i) == VariantLogicalType::DECIMAL) {
			return true;
		}
	}
	return false;
}

void EncodeInlinedVariant(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	auto count = args.size();
	child_list_t<LogicalType> parquet_children;
	parquet_children.emplace_back("metadata", LogicalType::BLOB);
	parquet_children.emplace_back("value", LogicalType::BLOB);
	Vector parquet(LogicalType::STRUCT(std::move(parquet_children)), count);
	ParquetVariantConversion::ToParquetVariant(input, count, parquet);
	auto &entries = StructVector::GetEntries(parquet);
	UnifiedVectorFormat metadata_format;
	UnifiedVectorFormat value_format;
	entries[0].ToUnifiedFormat(metadata_format);
	entries[1].ToUnifiedFormat(value_format);
	auto metadata_data = UnifiedVectorFormat::GetData<string_t>(metadata_format);
	auto value_data = UnifiedVectorFormat::GetData<string_t>(value_format);

	// the decimals of a shredded vector sit in its typed part, so only an unshredded one is scanned for them
	VariantIterator iterator(input);
	unique_ptr<RecursiveUnifiedVectorFormat> unshredded_format;
	unique_ptr<UnifiedVariantVectorData> unshredded;
	if (input.GetVectorType() != VectorType::SHREDDED_VECTOR) {
		unshredded_format = make_uniq<RecursiveUnifiedVectorFormat>();
		Vector::RecursiveToUnifiedFormat(input, *unshredded_format);
		unshredded = make_uniq<UnifiedVariantVectorData>(*unshredded_format);
	}
	auto writer = FlatVector::Writer<string_t>(result, count);
	string types;
	for (idx_t row = 0; row < count; row++) {
		auto root = iterator.Root(row);
		if (root.IsNull()) {
			writer.WriteNull();
			continue;
		}
		types.clear();
		if (!unshredded || HoldsDecimal(*unshredded, row)) {
			CollectDecimalTypes(root, types);
		}
		auto &metadata = metadata_data[metadata_format.sel->get_index(row)];
		auto &value = value_data[value_format.sel->get_index(row)];
		auto size = metadata.GetSize() + value.GetSize() + (types.empty() ? 0 : 1 + types.size());
		auto blob = StringVector::EmptyString(result, size);
		auto data = blob.GetDataWriteable();
		memcpy(data, metadata.GetData(), metadata.GetSize());
		data += metadata.GetSize();
		memcpy(data, value.GetData(), value.GetSize());
		data += value.GetSize();
		if (!types.empty()) {
			*data++ = static_cast<char>(DECIMAL_TYPES_FORMAT);
			memcpy(data, types.data(), types.size());
		}
		blob.Finalize();
		writer.WriteStringRef(blob);
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

//! The declared decimal types of a row, handed out in the order the builder emits the values
struct DecimalTypes {
	const_data_ptr_t data = nullptr;
	idx_t count = 0;
	idx_t next = 0;
};

//! The payload of a decimal, whatever its physical size
hugeint_t LoadDecimalPayload(const VariantDecimalData &decimal) {
	switch (VariantDecimalPhysicalType(decimal.width)) {
	case PhysicalType::INT16:
		return hugeint_t(Load<int16_t>(decimal.value_ptr));
	case PhysicalType::INT32:
		return hugeint_t(Load<int32_t>(decimal.value_ptr));
	case PhysicalType::INT64:
		return hugeint_t(Load<int64_t>(decimal.value_ptr));
	default:
		return Load<hugeint_t>(decimal.value_ptr);
	}
}

//! The payload of a decimal at the physical type of its declared width, which the decoded digits need not share
template <class T>
T ConvertDecimalPayload(const VariantDecimalData &decimal) {
	return Hugeint::Cast<T>(LoadDecimalPayload(decimal));
}

template <>
hugeint_t ConvertDecimalPayload<hugeint_t>(const VariantDecimalData &decimal) {
	return LoadDecimalPayload(decimal);
}

//! The payload of a value, which the builder loads as the type of a decimal only for the decimal physical types
template <class T>
struct VariantPayload {
	static T Load(const VariantNode &node) {
		return node.GetData<T>();
	}
};

template <class T>
T LoadVariantPayload(const VariantNode &node) {
	if (node.GetTypeId() != VariantLogicalType::DECIMAL) {
		return node.GetData<T>();
	}
	return ConvertDecimalPayload<T>(node.GetDecimal());
}

template <>
struct VariantPayload<int16_t> {
	static int16_t Load(const VariantNode &node) {
		return LoadVariantPayload<int16_t>(node);
	}
};

template <>
struct VariantPayload<int32_t> {
	static int32_t Load(const VariantNode &node) {
		return LoadVariantPayload<int32_t>(node);
	}
};

template <>
struct VariantPayload<int64_t> {
	static int64_t Load(const VariantNode &node) {
		return LoadVariantPayload<int64_t>(node);
	}
};

template <>
struct VariantPayload<hugeint_t> {
	static hugeint_t Load(const VariantNode &node) {
		return LoadVariantPayload<hugeint_t>(node);
	}
};

//! A cursor over a decoded VARIANT that reports the declared types of its decimals, for the builder
class TypedVariantNode {
public:
	TypedVariantNode(VariantNode node, DecimalTypes &types) : node(node), types(types) {
	}

public:
	bool IsNull() const {
		return node.IsNull();
	}
	bool IsMissing() const {
		return node.IsMissing();
	}
	VariantLogicalType GetTypeId() const {
		return node.GetTypeId();
	}
	template <class T>
	T GetData() const {
		return VariantPayload<T>::Load(node);
	}
	string_t GetString() const {
		return node.GetString();
	}
	VariantDecimalProperties GetDecimalProperties() const {
		auto decoded = node.GetDecimal();
		if (types.count == 0) {
			// written without declared types, the digits of the value stay its width
			return VariantDecimalProperties(decoded.width, decoded.scale);
		}
		if (types.next >= types.count) {
			throw IOException("Corrupted inlined VARIANT, it holds more decimals than declared types");
		}
		uint32_t width = types.data[2 * types.next];
		uint32_t scale = types.data[2 * types.next + 1];
		types.next++;
		// the payload has to fit the declared type, the decoded digits can be off by one
		bool fits = Decimal::IsValidWidthScale(width, scale) && scale == decoded.scale;
		if (fits) {
			auto value = LoadDecimalPayload(decoded);
			auto &limit = Hugeint::POWERS_OF_TEN[width];
			fits = value < limit && value > -limit;
		}
		if (!fits) {
			throw IOException("Corrupted inlined VARIANT, the declared DECIMAL(%d, %d) does not hold DECIMAL(%d, %d)",
			                  width, scale, decoded.width, decoded.scale);
		}
		return VariantDecimalProperties(width, scale);
	}
	class ObjectChildren;
	class ArrayChildren;
	ObjectChildren GetObjectChildren(VariantIterationOrder order) const;
	ArrayChildren GetArrayChildren() const;

private:
	VariantNode node;
	DecimalTypes &types;
};

//! The children of an object, each a typed cursor
class TypedVariantNode::ObjectChildren {
public:
	struct Entry {
		string_t key;
		TypedVariantNode value;
	};

	ObjectChildren(const VariantNode &object, VariantIterationOrder order, DecimalTypes &types) {
		for (auto &entry : object.GetObjectChildren(order)) {
			entries.push_back(Entry {entry.key, TypedVariantNode(entry.value, types)});
		}
	}
	vector<Entry>::const_iterator begin() const { // NOLINT
		return entries.begin();
	}
	vector<Entry>::const_iterator end() const { // NOLINT
		return entries.end();
	}

private:
	vector<Entry> entries;
};

//! The elements of an array, each a typed cursor
class TypedVariantNode::ArrayChildren {
public:
	ArrayChildren(const VariantNode &array, DecimalTypes &types) : elements(array.GetArrayChildren()), types(types) {
	}
	idx_t size() const { // NOLINT
		return elements.size();
	}
	TypedVariantNode operator[](idx_t i) const {
		return TypedVariantNode(elements[i], types);
	}

private:
	VariantArrayIterator elements;
	DecimalTypes &types;
};

TypedVariantNode::ObjectChildren TypedVariantNode::GetObjectChildren(VariantIterationOrder order) const {
	return ObjectChildren(node, order, types);
}

TypedVariantNode::ArrayChildren TypedVariantNode::GetArrayChildren() const {
	return ArrayChildren(node, types);
}

//! Emits the decoded VARIANT of every row with the declared types of its decimals
struct TypedVariantSource {
	TypedVariantSource(const VariantIterator &iterator, const vector<string_t> &row_types)
	    : iterator(iterator), row_types(row_types) {
	}

	bool Emit(idx_t row, VariantBuilder &builder) {
		auto root = iterator.Root(row);
		if (root.IsNull()) {
			return true;
		}
		DecimalTypes types;
		types.data = const_data_ptr_cast(row_types[row].GetData());
		types.count = row_types[row].GetSize() / 2;
		EmitIterator(TypedVariantNode(root, types), builder);
		if (types.next != types.count) {
			throw IOException("Corrupted inlined VARIANT, it declares %d decimal types for %d decimals", types.count,
			                  types.next);
		}
		return false;
	}

	const VariantIterator &iterator;
	const vector<string_t> &row_types;
};

void DecodeInlinedVariant(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	auto count = args.size();
	UnifiedVectorFormat input_format;
	input.ToUnifiedFormat(input_format);
	auto blobs = UnifiedVectorFormat::GetData<string_t>(input_format);

	// the Parquet Variant of each row and the declared decimal types that follow it
	Vector parquet(LogicalType::BLOB, count);
	auto parquet_writer = FlatVector::Writer<string_t>(parquet, count);
	vector<string_t> row_types(count);
	bool any_types = false;
	for (idx_t row = 0; row < count; row++) {
		auto idx = input_format.sel->get_index(row);
		if (!input_format.validity.RowIsValid(idx)) {
			parquet_writer.WriteNull();
			continue;
		}
		auto &blob = blobs[idx];
		auto size = ParquetVariantSize(blob);
		// the bytes of the input outlive this call
		parquet_writer.WriteStringRef(string_t(blob.GetData(), NumericCast<uint32_t>(size)));
		auto types_size = blob.GetSize() - size;
		if (types_size == 0) {
			continue;
		}
		auto types = blob.GetData() + size;
		if (types[0] != static_cast<char>(DECIMAL_TYPES_FORMAT) || types_size % 2 == 0) {
			throw IOException("Corrupted inlined VARIANT, unknown decimal types format");
		}
		row_types[row] = string_t(types + 1, NumericCast<uint32_t>(types_size - 1));
		any_types = true;
	}
	if (!any_types) {
		ParquetVariantConversion::ConvertBinary(parquet, result, count);
		return;
	}
	Vector decoded(LogicalType::VARIANT(), count);
	ParquetVariantConversion::ConvertBinary(parquet, decoded, count);
	VariantIterator iterator(decoded);
	TypedVariantSource source(iterator, row_types);
	BuildVariant(source, count, result);
}

} // namespace

ScalarFunction DuckLakeEncodeInlinedVariantFunction() {
	ScalarFunction function("ducklake_encode_inlined_variant", {LogicalType::VARIANT()}, LogicalType::BLOB,
	                        EncodeInlinedVariant);
	function.SetFallible();
	return function;
}

ScalarFunction DuckLakeDecodeInlinedVariantFunction() {
	ScalarFunction function("ducklake_decode_inlined_variant", {LogicalType::BLOB}, LogicalType::VARIANT(),
	                        DecodeInlinedVariant);
	function.SetFallible();
	return function;
}

} // namespace duckdb
