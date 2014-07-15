// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "geo/indexing.hpp"

#include <string>
#include <vector>

#include "btree/keys.hpp"
#include "btree/leaf_node.hpp"
#include "geo/exceptions.hpp"
#include "geo/geojson.hpp"
#include "geo/geo_visitor.hpp"
#include "geo/s2/s2cellid.h"
#include "geo/s2/s2polygon.h"
#include "geo/s2/s2polyline.h"
#include "geo/s2/s2regioncoverer.h"
#include "geo/s2/strings/strutil.h"
#include "rdb_protocol/datum.hpp"

#include "debug.hpp" // TODO!

using ql::datum_t;

class compute_covering_t : public s2_geo_visitor_t {
public:
    compute_covering_t(int goal_cells, std::vector<S2CellId> *result_out) :
        result_(result_out) {

        rassert(result_->empty());
        coverer_.set_max_cells(goal_cells);
    }

    void on_point(const S2Point &point) {
        result_->push_back(S2CellId::FromPoint(point));
    }
    void on_line(const S2Polyline &line) {
        coverer_.GetCovering(line, result_);
    }
    void on_polygon(const S2Polygon &polygon) {
        coverer_.GetCovering(polygon, result_);
    }

private:
    S2RegionCoverer coverer_;
    std::vector<S2CellId> *result_;
};

std::string s2cellid_to_key(S2CellId id) {
    // The important property of the result is that its lexicographic
    // ordering as a string must be equivalent to the integer ordering of id.
    // FastHex64ToBuffer() generates a hex representation of id that fulfills this
    // property (it comes padded with leading '0's).
    char buffer[kFastToBufferSize];
    return std::string(FastHex64ToBuffer(id.id(), buffer));
}

S2CellId key_to_s2cellid(const std::string &sid) {
    return S2CellId::FromToken(sid);
}

std::vector<std::string> compute_index_grid_keys(
        const counted_t<const ql::datum_t> &key, int goal_cells) {
    rassert(key.has());

    if (!key->is_ptype("geometry")) {
        throw geo_exception_t("Expected geometry, got " + key->get_type_name());
    }
    if (goal_cells <= 0) {
        throw geo_exception_t("goal_cells must be positive (and should be >= 4)");
    }

    // Compute a cover of grid cells
    std::vector<S2CellId> covering;
    covering.reserve(goal_cells);
    compute_covering_t coverer(goal_cells, &covering);
    visit_geojson(&coverer, key);

    // Generate keys
    std::vector<std::string> result;
    result.reserve(covering.size());
    debugf("Computing grid keys:\n"); // TODO!
    for (size_t i = 0; i < covering.size(); ++i) {
        result.push_back(s2cellid_to_key(covering[i]));
        debugf(" K: %s\n", s2cellid_to_key(covering[i]).c_str()); // TODO!
    }

    return result;
}

geo_index_traversal_helper_t::geo_index_traversal_helper_t(
        const std::vector<std::string> &query_grid_keys) {
    query_cells_.reserve(query_grid_keys.size());
    for (size_t i = 0; i < query_grid_keys.size(); ++i) {
        query_cells_.push_back(key_to_s2cellid(query_grid_keys[i]));
    }
}

void geo_index_traversal_helper_t::process_a_leaf(buf_lock_t *leaf_node_buf,
        const btree_key_t *left_exclusive_or_null,
        const btree_key_t *right_inclusive_or_null,
        UNUSED signal_t *interruptor, // TODO! Should we pass it through?
        int *population_change_out) THROWS_ONLY(interrupted_exc_t) {

    *population_change_out = 0;

    if (!any_query_cell_intersects(left_exclusive_or_null, right_inclusive_or_null)) {
        return;
    }

    buf_read_t read(leaf_node_buf);
    const leaf_node_t *node = static_cast<const leaf_node_t *>(read.get_data_read());

    for (auto it = leaf::begin(*node); it != leaf::end(*node); ++it) {
        const btree_key_t *k = (*it).first;
        if (!k) {
            break;
        }

        if (any_query_cell_intersects(k, k)) {
            on_candidate(k, (*it).second, buf_parent_t(leaf_node_buf));
        }
    }
}

void geo_index_traversal_helper_t::filter_interesting_children(
        UNUSED buf_parent_t parent,
        ranged_block_ids_t *ids_source,
        interesting_children_callback_t *cb) {

    for (int i = 0, e = ids_source->num_block_ids(); i < e; ++i) {
        block_id_t block_id;
        const btree_key_t *left, *right;
        ids_source->get_block_id_and_bounding_interval(i, &block_id, &left, &right);

        if (any_query_cell_intersects(left, right)) {
            cb->receive_interesting_child(i);
        }
    }

    cb->no_more_interesting_children();
}

bool geo_index_traversal_helper_t::any_query_cell_intersects(
        const btree_key_t *left_excl, const btree_key_t *right_incl) {
    // Check if any of the query cells intersects with the given range
    for (size_t j = 0; j < query_cells_.size(); ++j) {
        if (cell_intersects_with_range(query_cells_[j], left_excl, right_incl)) {
            return true;
        }
    }
    return false;
}

bool geo_index_traversal_helper_t::cell_intersects_with_range(
        const S2CellId c,
        const btree_key_t *left_excl, const btree_key_t *right_incl) {

    // TODO! Double-check that this works.

    // We ignore the fact that left_excl is exclusive and not inclusive.
    // In rare cases this costs us a little bit of efficiency, but saves us
    // some complexity.
    S2CellId range_min =
        left_excl == NULL
        ? S2CellId::None()
        : key_to_s2cellid(datum_t::extract_secondary(
            std::string(reinterpret_cast<const char *>(left_excl->contents),
                left_excl->size))).range_min();
    S2CellId range_max =
        right_incl == NULL
        ? S2CellId::Sentinel()
        : key_to_s2cellid(datum_t::extract_secondary(
            std::string(reinterpret_cast<const char *>(right_incl->contents),
                right_incl->size))).range_max();

    return range_min <= c.range_max() && range_max >= c.range_min();
}