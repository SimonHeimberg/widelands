/*
 * Copyright (C) 2002-2025 by the Widelands Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef WL_LOGIC_MAP_OBJECTS_TRIBES_WORKER_DESCR_H
#define WL_LOGIC_MAP_OBJECTS_TRIBES_WORKER_DESCR_H

#include <memory>

#include "ai/ai_hints.h"
#include "base/macros.h"
#include "graphic/animation/diranimations.h"
#include "logic/map_objects/bob.h"
#include "logic/map_objects/immovable.h"
#include "scripting/lua_table.h"

namespace Widelands {

// TODO(Antonio Trueba#1#): Get rid of forward class declaration
// (chicked-and-egg problem)
struct WorkerProgram;

class WorkerDescr : public BobDescr {
	friend class TribeDescr;
	friend class Warehouse;
	friend struct WorkerProgram;

public:
	using Buildcost = std::map<std::string, Quantity>;

	WorkerDescr(const std::string& init_descname,
	            MapObjectType type,
	            const LuaTable& table,
	            const std::vector<std::string>& attribs,
	            Descriptions& descriptions);
	WorkerDescr(const std::string& init_descname,
	            const LuaTable& t,
	            const std::vector<std::string>& attribs,
	            Descriptions& descriptions);
	~WorkerDescr() override;

	[[nodiscard]] Bob& create_object() const override;

	[[nodiscard]] bool is_buildable() const {
		return buildable_;
	}
	[[nodiscard]] bool is_promoted() const {
		return promoted_from_ != INVALID_INDEX;
	}

	[[nodiscard]] const Buildcost& buildcost() const {
		assert(is_buildable());
		return buildcost_;
	}
	[[nodiscard]] Buildcost& mutable_buildcost() {
		return buildcost_;
	}

	[[nodiscard]] const Vector2i& ware_hotspot() const {
		return ware_hotspot_;
	}

	/// How much of the worker type that an economy should store in warehouses.
	/// The special value std::numeric_limits<uint32_t>::max() means that the
	/// the target quantity of this ware type will never be checked and should
	/// not be configurable.
	[[nodiscard]] Quantity default_target_quantity() const {
		return default_target_quantity_;
	}
	/// Sets the default target quantity. Overwrites if it already exists.
	void set_default_target_quantity(int quantity);

	/// This is an AI hint
	void set_preciousness(const std::string& tribename, int preciousness);

	[[nodiscard]] bool has_demand_check() const {
		return default_target_quantity() != kInvalidWare;
	}

	/// Called when a demand check for this ware type is encountered during
	/// parsing. If there was no default target quantity set in the ware type's
	/// configuration, set the default value 1.
	void set_has_demand_check() {
		if (default_target_quantity_ == kInvalidWare) {
			default_target_quantity_ = 1;
		}
	}

	virtual const DirAnimations& get_right_walk_anims(bool const carries_ware, Worker*) const {
		return carries_ware ? walkload_anims_ : walk_anims_;
	}
	[[nodiscard]] WorkerProgram const* get_program(const std::string&) const;

	// For leveling
	[[nodiscard]] int32_t get_needed_experience() const {
		return needed_experience_;
	}
	[[nodiscard]] DescriptionIndex becomes() const {
		return becomes_;
	}
	void set_becomes(Descriptions&, const std::string&);

	// Returns the worker that this worker was promoted from by experience or
	// INVALID_INDEX if it cannot be promoted by experience.
	[[nodiscard]] const DescriptionIndex& promoted_from() const {
		return promoted_from_;
	}
	void set_promoted_from(const DescriptionIndex& index) {
		promoted_from_ = index;
	}
	[[nodiscard]] DescriptionIndex worker_index() const;
	[[nodiscard]] bool can_act_as(DescriptionIndex) const;

	// Add a building to the list of employers
	void add_employer(const DescriptionIndex& building_index);

	// The buildings where this worker can work
	[[nodiscard]] const std::set<DescriptionIndex>& employers() const;

	Worker& create(EditorGameBase&, Player*, PlayerImmovable*, Coords) const;

	[[nodiscard]] uint32_t movecaps() const override;

	using Programs = std::map<std::string, std::unique_ptr<WorkerProgram>>;
	[[nodiscard]] const Programs& programs() const {
		return programs_;
	}
	Programs& mutable_programs() {
		return programs_;
	}

	/// AI hints for this worker type. Can be nullptr.
	[[nodiscard]] const AI::WareWorkerHints* ai_hints() const {
		return ai_hints_.get();
	}

	void set_needed_experience(int32_t x) {
		needed_experience_ = x;
	}

protected:
	Programs programs_;

private:
	Vector2i ware_hotspot_;

	DirAnimations walk_anims_;
	DirAnimations walkload_anims_;

	Quantity default_target_quantity_;
	const bool buildable_;
	Buildcost buildcost_;

	/**
	 * Type that this worker can become, i.e. level up to, or INVALID_INDEX if the worker cannot
	 * level up.
	 */
	DescriptionIndex becomes_;

	/**
	 * Number of experience points required for leveling up,
	 * or INVALID_INDEX if the worker cannot level up.
	 */
	int32_t needed_experience_;

	DescriptionIndex promoted_from_{
	   INVALID_INDEX};  // The worker this worker was promoted from, or INVALID_INDEX

	/// Buildings where this worker can work
	std::set<DescriptionIndex> employers_;

	// Hints for the AI
	std::unique_ptr<AI::WareWorkerHints> ai_hints_;

	const Descriptions& descriptions_;
	DISALLOW_COPY_AND_ASSIGN(WorkerDescr);
};
}  // namespace Widelands

#endif  // end of include guard: WL_LOGIC_MAP_OBJECTS_TRIBES_WORKER_DESCR_H
