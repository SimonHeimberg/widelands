/*
 * Copyright (C) 2006-2025 by the Widelands Development Team
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

#ifndef WL_SCRIPTING_LUA_INTERFACE_H
#define WL_SCRIPTING_LUA_INTERFACE_H

#include <memory>

#include "scripting/lua.h"
#include "scripting/lua_errors.h"

class LuaTable;

// Provides an interface to call and execute Lua Code.
class LuaInterface {
public:
	explicit LuaInterface(bool is_main_menu = false);
	virtual ~LuaInterface();

	// Interpret the given string, will throw 'LuaError' on any error.
	void interpret_string(const std::string&);

	// Runs 'script' and returns the table it returned.
	virtual std::unique_ptr<LuaTable> run_script(const std::string& path);

	// Returns an empty table.
	std::unique_ptr<LuaTable> empty_table();

	lua_State* L() {
		return lua_state_;
	}

protected:
	lua_State* lua_state_;
};

#endif  // end of include guard: WL_SCRIPTING_LUA_INTERFACE_H
