//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_geo_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/ducklake_stats.hpp"
#include "duckdb/common/types/geometry.hpp"

namespace duckdb {

struct DuckLakeColumnGeoStats final : public DuckLakeColumnExtraStats {
	static constexpr const DuckLakeExtraStatsType TYPE = DuckLakeExtraStatsType::GEOMETRY;

	DuckLakeColumnGeoStats();
	void Merge(const DuckLakeColumnExtraStats &new_stats) override;
	unique_ptr<DuckLakeColumnExtraStats> Copy() const override;
	bool ParseStats(const string &stats_name, const vector<Value> &stats_children) override;

	bool TrySerialize(string &result) const override;
	void Serialize(DuckLakeColumnStatsInfo &column_stats) const override;
	void Deserialize(const string &stats) override;

	unique_ptr<BaseStatistics> ToStats() const;
	//! Cover every finite coordinate, which older clients read too, unlike an infinite extent
	void SetUnknownExtent();
	//! Covers every finite coordinate of one axis, as SetUnknownExtent does for all of them
	static void SetUnknownRange(double &min, double &max);
	void SetUnknownTypes();

public:
	GeometryExtent extent = GeometryExtent::Empty();
	set<string> geo_types;
};

} // namespace duckdb
