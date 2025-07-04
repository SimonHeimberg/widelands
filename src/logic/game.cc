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

#include "logic/game.h"

#include <cstdlib>
#include <memory>
#include <string>

#ifndef _WIN32
#include <unistd.h>  // for usleep
#else
#include <SDL_events.h>  // for a dirty hack.
#include <windows.h>
#endif

#include "base/i18n.h"
#include "base/log.h"
#include "base/macros.h"
#include "base/multithreading.h"
#include "base/time_string.h"
#include "base/warning.h"
#include "build_info.h"
#include "commands/cmd_attack.h"
#include "commands/cmd_build_building.h"
#include "commands/cmd_build_flag.h"
#include "commands/cmd_build_road.h"
#include "commands/cmd_build_waterway.h"
#include "commands/cmd_building_name.h"
#include "commands/cmd_bulldoze.h"
#include "commands/cmd_calculate_statistics.h"
#include "commands/cmd_call_economy_balance.h"
#include "commands/cmd_change_soldier_capacity.h"
#include "commands/cmd_change_training_options.h"
#include "commands/cmd_delete_message.h"
#include "commands/cmd_diplomacy.h"
#include "commands/cmd_dismantle_building.h"
#include "commands/cmd_drop_soldier.h"
#include "commands/cmd_enhance_building.h"
#include "commands/cmd_evict_worker.h"
#include "commands/cmd_expedition_config.h"
#include "commands/cmd_extend_trade.h"
#include "commands/cmd_flag_action.h"
#include "commands/cmd_fleet_targets.h"
#include "commands/cmd_incorporate.h"
#include "commands/cmd_luacoroutine.h"
#include "commands/cmd_luascript.h"
#include "commands/cmd_mark_map_object_for_removal.h"
#include "commands/cmd_pick_custom_starting_position.h"
#include "commands/cmd_pinned_note.h"
#include "commands/cmd_propose_trade.h"
#include "commands/cmd_queue.h"
#include "commands/cmd_set_input_max_fill.h"
#include "commands/cmd_set_soldier_preference.h"
#include "commands/cmd_set_stock_policy.h"
#include "commands/cmd_set_ware_priority.h"
#include "commands/cmd_set_ware_target_quantity.h"
#include "commands/cmd_set_worker_target_quantity.h"
#include "commands/cmd_ship_cancel_expedition.h"
#include "commands/cmd_ship_construct_port.h"
#include "commands/cmd_ship_explore_island.h"
#include "commands/cmd_ship_refit.h"
#include "commands/cmd_ship_scout_direction.h"
#include "commands/cmd_ship_set_destination.h"
#include "commands/cmd_ship_sink.h"
#include "commands/cmd_start_or_cancel_expedition.h"
#include "commands/cmd_start_stop_building.h"
#include "commands/cmd_toggle_infinite_production.h"
#include "commands/cmd_toggle_mute_messages.h"
#include "commands/cmd_trade_action.h"
#include "commands/cmd_warship_command.h"
#include "economy/economy.h"
#include "economy/portdock.h"
#include "game_io/game_loader.h"
#include "game_io/game_preload_packet.h"
#include "io/fileread.h"
#include "io/filesystem/filesystem_exceptions.h"
#include "io/filesystem/layered_filesystem.h"
#include "io/filewrite.h"
#include "logic/addons.h"
#include "logic/filesystem_constants.h"
#include "logic/game_settings.h"
#include "logic/map_objects/checkstep.h"
#include "logic/map_objects/tribes/carrier.h"
#include "logic/map_objects/tribes/market.h"
#include "logic/map_objects/tribes/militarysite.h"
#include "logic/map_objects/tribes/ship.h"
#include "logic/map_objects/tribes/soldier.h"
#include "logic/map_objects/tribes/trainingsite.h"
#include "logic/map_objects/tribes/tribe_descr.h"
#include "logic/map_objects/tribes/warehouse.h"
#include "logic/player.h"
#include "logic/replay.h"
#include "logic/replay_game_controller.h"
#include "logic/single_player_game_controller.h"
#if 0  // TODO(Nordfriese): Re-add training wheels code after v1.0
#include "logic/training_wheels.h"
#endif
#include "map_io/widelands_map_loader.h"
#include "scripting/lua_table.h"
#include "sound/sound_handler.h"
#include "ui_basic/progresswindow.h"
#include "wlapplication_options.h"
#include "wui/interactive_player.h"
#include "wui/interactive_spectator.h"

namespace Widelands {

/// Define this to get lots of debugging output concerned with syncs
// #define SYNC_DEBUG

Game::SyncWrapper::~SyncWrapper() {
	if (dump_ != nullptr) {
		if (!syncstreamsave_) {
			try {
				g_fs->fs_unlink(dumpfname_);
			} catch (const FileError& e) {
				// not really a problem if deletion fails, but we'll log it
				log_warn_time(game_.get_gametime(), "Deleting syncstream file %s failed: %s\n",
				              dumpfname_.c_str(), e.what());
			}
		}
	}
}

void Game::SyncWrapper::start_dump(const std::string& fname) {
	dumpfname_ = fname + kSyncstreamExtension;
	dump_.reset(g_fs->open_stream_write(dumpfname_));
	current_excerpt_id_ = 0;
	excerpts_buffer_[current_excerpt_id_].clear();
}

void Game::SyncWrapper::data(void const* const sync_data, size_t const size) {
#ifdef SYNC_DEBUG
	const Time& time = game_.get_gametime();
	std::string logtext = format("[sync:%08u t=%6u]", counter_, time.get());
	for (size_t i = size; i > 0; --i) {
		logtext += format(" %02x", (static_cast<uint8_t const*>(sync_data))[i - 1]);
	}
	log_dbg_time(game_.get_gametime(), "%s", logtext.c_str());
#endif

	if (dump_ != nullptr && static_cast<int32_t>(counter_ - next_diskspacecheck_) >= 0) {
		next_diskspacecheck_ = counter_ + 16 * 1024 * 1024;

		if (g_fs->disk_space() < kMinimumDiskSpace) {
			log_warn_time(
			   game_.get_gametime(), "Stop writing to syncstream file: disk is getting full.\n");
			dump_.reset();
		}
	}

	if (dump_ != nullptr) {
		try {
			dump_->data(sync_data, size);
		} catch (const WException&) {
			log_warn_time(game_.get_gametime(),
			              "Writing to syncstream file %s failed. Stop syncstream dump.\n",
			              dumpfname_.c_str());
			dump_.reset();
		}
		assert(current_excerpt_id_ < kExcerptSize);
		excerpts_buffer_[current_excerpt_id_].append(static_cast<const char*>(sync_data), size);
	}

	target_.data(sync_data, size);
	counter_ += size;
}

Game::Game()
   : EditorGameBase(new LuaGameInterface(this)),
     syncwrapper_(*this, synchash_),
     cmdqueue_(*this),
     /** TRANSLATORS: Win condition for this game has not been set. */
     win_condition_displayname_(_("Not set")) {
	last_economy_serial_ = 0;
	last_detectedportspace_serial_ = 0;
}

Game::~Game() {  // NOLINT
	              // ReplayWriter needs this
}

void Game::sync_reset() {
	syncwrapper_.counter_ = 0;

	synchash_.reset();
	verb_log_info_time(get_gametime(), "[sync] Reset\n");
}

/**
 * \return a pointer to the \ref InteractivePlayer if any.
 * \note This function may return 0 (in particular, it will return 0 during
 * playback or if player is spectator)
 */
InteractivePlayer* Game::get_ipl() {
	return dynamic_cast<InteractivePlayer*>(get_ibase());
}
const InteractivePlayer* Game::get_ipl() const {
	return dynamic_cast<const InteractivePlayer*>(get_ibase());
}

void Game::set_game_controller(std::shared_ptr<GameController> c) {
	ctrl_ = c;
}

void Game::set_ai_training_mode(const bool mode) {
	ai_training_mode_ = mode;
}

void Game::set_auto_speed(const bool mode) {
	auto_speed_ = mode;
}

GameController* Game::game_controller() {
	return ctrl_.get();
}

void Game::set_write_replay(bool const wr) {
	//  we want to allow for the possibility to stop writing our replay
	//  this is to ensure we do not crash because of diskspace
	//  still this is only possibe to go from true->false
	//  still probally should not do this with an assert but with better checks
	assert(state_ == gs_notrunning || !wr);

	writereplay_ = wr;
}

void Game::set_write_syncstream(bool const wr) {
	assert(state_ == gs_notrunning);

	writesyncstream_ = wr;
}

/**
 * Set whether the syncstream dump should be copied to a permanent location
 * at the end of the game.
 */
void Game::save_syncstream(bool const save) {
	syncwrapper_.syncstreamsave_ = save;
}

void Game::postload_addons_before_loading() {
	if (did_postload_addons_before_loading_) {
		return;
	}
	did_postload_addons_before_loading_ = true;
	delete_world_and_tribes();
	mutable_descriptions()->ensure_tribes_are_registered();
	postload_addons();
}

bool Game::run_splayer_scenario_direct(const std::list<std::string>& list_of_scenarios,
                                       const std::string& script_to_run) {
	full_cleanup();
	list_of_scenarios_ = list_of_scenarios;
	assert(!list_of_scenarios_.empty());
	// Replays can't handle scenarios
	set_write_replay(false);
#if 0  // TODO(Nordfriese): Re-add training wheels code after v1.0
	training_wheels_wanted_ = false;
#endif

	std::unique_ptr<MapLoader> maploader(
	   mutable_map()->get_correct_loader(list_of_scenarios.front()));
	if (!maploader) {
		throw wexception("could not load \"%s\"", list_of_scenarios.front().c_str());
	}

	// Need to do this first so we can set the theme and background.
	maploader->preload_map(true, &enabled_addons());

	create_loader_ui({"general_game"}, false /* no game tips in scenarios */,
	                 map().get_background_theme(), map().get_background(), true);

	Notifications::publish(UI::NoteLoadingMessage(_("Preloading map…")));

	// If the map is a scenario with custom tribe entites, load them too.
	mutable_descriptions()->register_scenario_tribes(map().filesystem());

	// We have to create the players here.
	PlayerNumber const nr_players = map().get_nrplayers();
	iterate_player_numbers(p, nr_players) {
		// If tribe name is empty, pick a random tribe
		std::string tribe = map().get_scenario_player_tribe(p);
		if (tribe.empty()) {
			verb_log_info_time(
			   get_gametime(), "Setting random tribe for Player %u\n", static_cast<unsigned int>(p));
			const DescriptionIndex random = RNG::static_rand(descriptions().nr_tribes());
			tribe = descriptions().get_tribe_descr(random)->name();
		}
		add_player(p, 0, kPlayerColors[p - 1], tribe, map().get_scenario_player_name(p));
		get_player(p)->set_ai(map().get_scenario_player_ai(p));
	}
	win_condition_displayname_ = "Scenario";

	set_ibase(new InteractivePlayer(*this, get_config_section(), 1, false));

	maploader->load_map_complete(*this, Widelands::MapLoader::LoadType::kScenario);
	maploader.reset();

	set_game_controller(std::make_shared<SinglePlayerGameController>(*this, true, 1));
	try {
		bool const result = run(StartGameType::kSinglePlayerScenario, script_to_run, "single_player");
		ctrl_.reset();
		return result;
	} catch (...) {
		ctrl_.reset();
		throw;
	}
}

/**
 * Initialize the game based on the given settings.
 */
void Game::init_newgame(const GameSettings& settings) {
	assert(has_loader_ui());

	Notifications::publish(UI::NoteLoadingMessage(_("Preloading map…")));

	std::unique_ptr<MapLoader> maploader;
	if (!settings.mapfilename.empty()) {
		maploader = mutable_map()->get_correct_loader(settings.mapfilename);
		assert(maploader);
		maploader->preload_map(settings.scenario, &enabled_addons());
		postload_addons_before_loading();
	} else {
		// TODO(matthiakl): Once random games support world Add-Ons, call
		// postload_addons_before_loading() here as well
		postload_addons();
		did_postload_addons_before_loading_ = true;
	}

	std::vector<PlayerSettings> shared;
	std::vector<uint8_t> shared_num;
	for (uint32_t i = 0; i < settings.players.size(); ++i) {
		const PlayerSettings& playersettings = settings.players[i];

		if (playersettings.state == PlayerSettings::State::kClosed ||
		    playersettings.state == PlayerSettings::State::kOpen) {
			continue;
		}
		if (playersettings.state == PlayerSettings::State::kShared) {
			shared.push_back(playersettings);
			shared_num.push_back(i + 1);
			continue;
		}

		add_player(i + 1, playersettings.initialization_index, playersettings.color,
		           playersettings.tribe, playersettings.name, playersettings.team);
		get_player(i + 1)->set_random_tribe(playersettings.random_tribe);
		get_player(i + 1)->set_ai(playersettings.ai);
	}

	// Add shared in starting positions
	for (uint8_t n = 0; n < shared.size(); ++n) {
		// This player's starting position is used in another (shared) kingdom
		get_player(shared.at(n).shared_in)
		   ->add_further_starting_position(shared_num.at(n), shared.at(n).initialization_index);
	}

	if (!settings.mapfilename.empty()) {
		assert(maploader);
		maploader->load_map_complete(*this, settings.scenario ?
		                                       Widelands::MapLoader::LoadType::kScenario :
		                                       Widelands::MapLoader::LoadType::kGame);
	} else {
		// Normally the map loader takes care of this, but if the map was
		// previously created for us we need to call this manually
		allocate_player_maps();
	}

	// Check for win_conditions
	if (!settings.scenario) {
		Notifications::publish(UI::NoteLoadingMessage(_("Initializing game…")));
		if ((settings.flags & GameSettings::Flags::kPeaceful) != 0) {
			for (uint32_t i = 1; i < settings.players.size(); ++i) {
				if (Player* first = get_player(i); first != nullptr) {
					for (uint32_t j = i + 1; j <= settings.players.size(); ++j) {
						if (Player* second = get_player(j); second != nullptr) {
							first->set_attack_forbidden(j, true);
							second->set_attack_forbidden(i, true);
						}
					}
				}
			}
		}
		if ((settings.flags & GameSettings::Flags::kCustomStartingPositions) != 0) {
			iterate_players_existing(p, map().get_nrplayers(), *this, pl) {
				if (settings.get_tribeinfo(pl->tribe().name())
				       .initializations[pl->initialization_index()]
				       .uses_map_starting_position) {
					pl->start_picking_custom_starting_position();
				}
			}
		}

		if ((settings.flags & GameSettings::Flags::kFogless) != 0) {
			Coords c;
			iterate_players_existing(p, map().get_nrplayers(), *this, pl) {
				c.x = 0;
				for (; c.x < map().get_width(); ++c.x) {
					c.y = 0;
					for (; c.y < map().get_height(); ++c.y) {
						pl->hide_or_reveal_field(c, HideOrRevealFieldMode::kReveal);
					}
				}
			}
		}

		diplomacy_allowed_ = ((settings.flags & GameSettings::Flags::kForbidDiplomacy) == 0);
		naval_warfare_allowed_ = ((settings.flags & GameSettings::Flags::kAllowNavalWarfare) != 0);
		win_condition_duration_ = settings.win_condition_duration;
		std::unique_ptr<LuaTable> table(lua().run_script(settings.win_condition_script));
		table->do_not_warn_about_unaccessed_keys();
		win_condition_displayname_ = table->get_string("name");
		if (table->has_key<std::string>("init")) {
			std::unique_ptr<LuaCoroutine> cr = table->get_coroutine("init");
			cr->resume();
		}
		std::unique_ptr<LuaCoroutine> cr = table->get_coroutine("func");
		enqueue_command(new CmdLuaCoroutine(get_gametime() + Duration(100), std::move(cr)));
	} else {
		win_condition_displayname_ = "Scenario";
	}
}

/**
 * Initialize the savegame based on the given settings.
 * At return the game is at the same state like a map loaded with Game::init()
 * Only difference is, that players are already initialized.
 * run<Returncode>() takes care about this difference.
 */
void Game::init_savegame(const GameSettings& settings) {
	assert(has_loader_ui());

	Notifications::publish(UI::NoteLoadingMessage(_("Preloading map…")));

	try {
		GameLoader gl(settings.mapfilename, *this);
		Widelands::GamePreloadPacket gpdp;
		gl.preload_game(gpdp);

		win_condition_displayname_ = gpdp.get_win_condition();
		win_condition_duration_ = gpdp.get_win_condition_duration();

#if 0  // TODO(Nordfriese): Re-add training wheels code after v1.0
		training_wheels_wanted_ =
		   gpdp.get_training_wheels_wanted() && get_config_bool("training_wheels", true);
		if (training_wheels_wanted_ && !gpdp.get_active_training_wheel().empty()) {
			training_wheels_.reset(new TrainingWheels(lua()));
			training_wheels_->acquire_lock(gpdp.get_active_training_wheel());
			verb_log_dbg("Training wheel from savegame");
		}
#endif
		if (win_condition_displayname_ == "Scenario") {
			// Replays can't handle scenarios
			set_write_replay(false);
		}

		gl.load_game(settings.multiplayer);

		// Discover the links between resources and geologist flags,
		// dependencies of productionsites etc.
		postload_addons();

		// Players might have selected a different AI type
		for (uint8_t i = 0; i < settings.players.size(); ++i) {
			const PlayerSettings& playersettings = settings.players[i];
			if (playersettings.state == PlayerSettings::State::kComputer) {
				get_player(i + 1)->set_ai(playersettings.ai);
			}
		}
	} catch (...) {
		throw;
	}
}

bool Game::run_load_game(const std::string& filename, const std::string& script_to_run) {
	full_cleanup();  // Reset and cleanup all values

	int8_t player_nr;
	{
		GameLoader gl(filename, *this);

		Widelands::GamePreloadPacket gpdp;
		// Need to do this first so we can set the theme and background
		gl.preload_game(gpdp);

		create_loader_ui({"general_game", "singleplayer"}, true, gpdp.get_background_theme(),
		                 gpdp.get_background(), true);
		Notifications::publish(UI::NoteLoadingMessage(_("Preloading map…")));

		win_condition_displayname_ = gpdp.get_win_condition();
		win_condition_duration_ = gpdp.get_win_condition_duration();
#if 0  // TODO(Nordfriese): Re-add training wheels code after v1.0
		training_wheels_wanted_ =
		   gpdp.get_training_wheels_wanted() && get_config_bool("training_wheels", true);
		if (training_wheels_wanted_ && !gpdp.get_active_training_wheel().empty()) {
			training_wheels_.reset(new TrainingWheels(lua()));
			training_wheels_->acquire_lock(gpdp.get_active_training_wheel());
			verb_log_dbg("Training wheel from savegame");
		}
#endif
		if (win_condition_displayname_ == "Scenario") {
			// Replays can't handle scenarios
			set_write_replay(false);
		}

		player_nr = gpdp.get_player_nr();
		InteractivePlayer* ipl = new InteractivePlayer(*this, get_config_section(), player_nr, false);
		set_ibase(ipl);

		gl.load_game();
		postload_addons();

		ipl->info_panel_fast_forward_message_queue();
	}

	// Store the filename for further saves
	save_handler().set_current_filename(filename);

	set_game_controller(std::make_shared<SinglePlayerGameController>(*this, true, player_nr));
	try {
		bool const result = run(StartGameType::kSaveGame, script_to_run, "single_player");
		ctrl_.reset();
		return result;
	} catch (...) {
		ctrl_.reset();
		throw;
	}
}

#if 0  // TODO(Nordfriese): Re-add training wheels code after v1.0
bool Game::acquire_training_wheel_lock(const std::string& objective) {
	if (training_wheels_ != nullptr) {
		return training_wheels_->acquire_lock(objective);
	}
	return false;
}
void Game::release_training_wheel_lock() {
	if (training_wheels_ != nullptr) {
		training_wheels_->release_lock();
	}
}
void Game::mark_training_wheel_as_solved(const std::string& objective) {
	if (training_wheels_ == nullptr) {
		training_wheels_.reset(new TrainingWheels(lua()));
	}
	training_wheels_->mark_as_solved(objective, training_wheels_wanted_);
}
void Game::run_training_wheel(const std::string& objective, bool force) {
	if (training_wheels_ == nullptr) {
		training_wheels_.reset(new TrainingWheels(lua()));
	}
	training_wheels_->run(objective, force);
}
void Game::skip_training_wheel(const std::string& objective) {
	if (training_wheels_ != nullptr) {
		training_wheels_->skip(objective, training_wheels_wanted_);
	}
}
bool Game::training_wheels_wanted() const {
	return training_wheels_wanted_;
}
std::string Game::active_training_wheel() const {
	return training_wheels_ ? training_wheels_->current_objective() : "";
}
#endif

/**
 * Called for every game after loading (from a savegame or just from a map
 * during single/multiplayer/scenario).
 *
 * Ensure that players and player controllers are setup properly (in particular
 * AI and the \ref InteractivePlayer if any).
 */
void Game::postload() {
	EditorGameBase::postload();
	get_ibase()->postload();
}

/**
 * This runs a game, including game creation phase.
 *
 * The setup and loading of a game happens (or rather: will happen) in three
 * stages.
 * 1.  First of all, the host (or single player) configures the game. During
 *     this time, only short descriptions of the game data (such as map
 *     headers)are loaded to minimize loading times.
 * 2a. Once the game is about to start and the configuration screen is finished,
 *     all logic data (map, tribe information, building information) is loaded
 *     during postload.
 * 2b. If a game is created, initial player positions are set. This step is
 *     skipped when a game is loaded.
 * 3.  After this has happened, the game graphics are loaded.
 *
 * \return true if a game actually took place, false otherwise
 */
bool Game::run(StartGameType const start_game_type,
               const std::string& script_to_run,
               const std::string& prefix_for_replays) {
	assert(has_loader_ui());

	postload();

	InteractivePlayer* ipl = get_ipl();

	if (start_game_type != StartGameType::kSaveGame) {
		// Check whether we need to disable replays because of add-ons.
		// For savegames this has already been done by the game class packet.
		if (writereplay_) {
			for (const auto& a : enabled_addons()) {
				if (!a->sync_safe) {
					set_write_replay(false);
					break;
				}
			}
			if (writereplay_) {
				// We need to check all enabled add-ons as well because enabled_addons() does
				// not contain e.g. desync-prone starting condition or win condition add-ons.
				for (const auto& pair : AddOns::g_addons) {
					if (pair.second && !pair.first->sync_safe) {
						set_write_replay(false);
						break;
					}
				}
			}
		}

		PlayerNumber const nr_players = map().get_nrplayers();
		if (start_game_type == StartGameType::kMap) {
			/** TRANSLATORS: All players (plural) */
			Notifications::publish(UI::NoteLoadingMessage(_("Creating player infrastructure…")));
			iterate_players_existing(p, nr_players, *this, plr) {
				plr->create_default_infrastructure();
			}
#if 0  // TODO(Nordfriese): Re-add training wheels code after v1.0
			training_wheels_wanted_ =
			   get_config_bool("training_wheels", true) && ipl && !ipl->is_multiplayer();
#endif
		} else {
			// Is a scenario!
			// Replays can't handle scenarios
			set_write_replay(false);
#if 0  // TODO(Nordfriese): Re-add training wheels code after v1.0
			training_wheels_wanted_ = false;
#endif
			iterate_players_existing_novar(p, nr_players, *this) {
				if (!map().get_starting_pos(p).valid()) {
					throw WLWarning(_("Missing starting position"),
					                _("Widelands could not start the game, because player %u has "
					                  "no starting position.\n"
					                  "You can manually add a starting position with the Widelands "
					                  "Editor to fix this problem."),
					                static_cast<unsigned int>(p));
				}
			}
		}

		if (ipl != nullptr) {
			// Scroll map to starting position for new games.
			// Loaded games are handled in GameInteractivePlayerPacket for single player, and in
			// InteractiveGameBase::start() for multiplayer.
			ipl->map_view()->scroll_to_field(
			   map().get_starting_pos(ipl->player_number()), MapView::Transition::Jump);
		}

		// Prepare the map, set default textures
		mutable_map()->recalc_default_resources(descriptions());

		// Finally, set the scenario names and tribes to represent
		// the correct names of the players
		iterate_player_numbers(p, nr_players) {
			const Player* const plr = get_player(p);
			const std::string no_name;
			const std::string& player_tribe = plr != nullptr ? plr->tribe().name() : no_name;
			const std::string& player_name = plr != nullptr ? plr->get_name() : no_name;
			const std::string& player_ai = plr != nullptr ? plr->get_ai() : no_name;
			mutable_map()->set_scenario_player_tribe(p, player_tribe);
			mutable_map()->set_scenario_player_name(p, player_name);
			mutable_map()->set_scenario_player_ai(p, player_ai);
			mutable_map()->set_scenario_player_closeable(p, false);  // player is already initialized.
		}

		// Run the init script, if the map provides one.
		if (start_game_type == StartGameType::kSinglePlayerScenario) {
			enqueue_command(new CmdLuaScript(get_gametime(), "map:scripting/init.lua"));
		} else if (start_game_type == StartGameType::kMultiPlayerScenario) {
			enqueue_command(new CmdLuaScript(get_gametime(), "map:scripting/multiplayer_init.lua"));
		} else {
			// Run all selected add-on scripts (not in scenarios)
			for (const auto& addon : enabled_addons()) {
				if (addon->category == AddOns::AddOnCategory::kScript) {
					enqueue_command(new CmdLuaScript(
					   get_gametime() + Duration(1), kAddOnDir + FileSystem::file_separator() +
					                                    addon->internal_name +
					                                    FileSystem::file_separator() + "init.lua"));
				}
			}
		}

		// Queue first statistics calculation
		enqueue_command(new CmdCalculateStatistics(get_gametime() + Duration(1)));
	}

	dynamic_cast<InteractiveGameBase&>(*get_ibase()).rebuild_main_menu();

	if (!script_to_run.empty()) {
		enqueue_command(new CmdLuaScript(get_gametime() + Duration(1), script_to_run));
	}

#if 0  // TODO(Nordfriese): Re-add training wheels code after v1.0
	// We don't run the training wheel objectives in scenarios, but we want the objectives available
	// for marking them as solved if a scenario teaches the same content.
	if (training_wheels_wanted_) {
		if (training_wheels_ == nullptr) {
			training_wheels_.reset(new TrainingWheels(lua()));
		}
		if (!training_wheels_->has_objectives()) {
			// Nothing to do, so let's free the memory
			training_wheels_.reset(nullptr);
		} else {
			// Just like with scenarios, replays will desync, so we switch them off.
			set_write_replay(false);
		}
	}
#endif

	if (writereplay_ || writesyncstream_) {
		// Derive a replay filename from the current time
		const std::string fname = kReplayDir + FileSystem::file_separator() +
		                          std::string(timestring()) + std::string("_") + prefix_for_replays +
		                          kReplayExtension;
		if (writereplay_) {
			verb_log_info_time(get_gametime(), "Starting replay writer\n");

			assert(!replaywriter_);
			replaywriter_.reset(new ReplayWriter(*this, fname));

			verb_log_info_time(get_gametime(), "Replay writer has started\n");
		}

		if (writesyncstream_) {
			syncwrapper_.start_dump(fname);
		}
	}

	postload_addons();

	sync_reset();
	Notifications::publish(UI::NoteLoadingMessage(_("Initializing…")));

#ifdef _WIN32
	//  Clear the event queue before starting game because we don't want
	//  to handle events at game start that happened during loading procedure.
	SDL_Event event;
	while (SDL_PollEvent(&event))
		;
#endif

	g_sh->change_music(Songset::kIngame);

	state_ = gs_running;

	// Immediately progress the game by 1 tick before removing the loader UI
	// to ensure that there is no black screen in the brief interval between
	// the initialization and the loading of the initial scripts.
	think();
	remove_loader_ui();

#if 0  // TODO(Nordfriese): Re-add training wheels code after v1.0
	// If this is a singleplayer map or non-scenario savegame, put on our training wheels unless the
	// user switched off the option
	if (training_wheels_ != nullptr && training_wheels_wanted_) {
		verb_log_dbg("Running training wheels. Current active is %s", active_training_wheel().c_str());
		training_wheels_->run_objectives();
	}
#endif

	get_ibase()->run<UI::Panel::Returncodes>();

	state_ = gs_ending;

	cleanup_objects();
	delete_pending_player_commands();
	set_ibase(nullptr);

	state_ = gs_notrunning;

	if (next_game_to_load_.empty()) {
		return true;
	}

	const std::string load = next_game_to_load_;  // Pass-by-reference does have its disadvantages…

	if (FileSystem::filename_ext(load) == kReplayExtension) {
		return run_replay(load, script_to_run);
	}
	if (FileSystem::filename_ext(load) == kSavegameExtension) {
		create_loader_ui(
		   {"general_game"}, false, map().get_background_theme(), map().get_background(), true);
		return run_load_game(load, script_to_run);
	}

	/* Load a scenario. This should be either the current one, or the next one if existent. */
	assert(!list_of_scenarios_.empty());
	std::list<std::string> list = list_of_scenarios_;
	if (list.front() != load) {
		list.pop_front();
		assert(!list.empty());
		assert(list.front() == load);
	}
	return run_splayer_scenario_direct(list, script_to_run);
}

bool Game::run_replay(const std::string& filename, const std::string& script_to_run) {
	full_cleanup();
	replay_filename_ = filename;

	create_loader_ui(
	   {"general_game"}, false, map().get_background_theme(), map().get_background(), true);
	set_ibase(new InteractiveSpectator(*this, get_config_section()));

	set_write_replay(false);
	set_game_controller(std::make_shared<ReplayGameController>(*this));
	save_handler().set_allow_saving(false);

	return run(Widelands::Game::StartGameType::kSaveGame, script_to_run, "replay");
}

void Game::set_next_game_to_load(const std::string& file) {
	next_game_to_load_ = file;
}

void Game::do_send_player_command(PlayerCommand* pc) {
	// At this point, the command has not yet been distributed to the other
	// clients, nor written to the replay. If multithreading has caused the
	// command's duetime to lie in the past, we can just safely increase it.
	if (pc->duetime() <= get_gametime()) {
		const Time new_time = get_gametime() + Duration(1);
		verb_log_info_time(get_gametime(),
		                   "Increasing a PlayerCommand's duetime from %u to %u (delta %u)",
		                   pc->duetime().get(), new_time.get(), (new_time - pc->duetime()).get());
		pc->set_duetime(new_time);
	}

	ctrl_->send_player_command(pc);
}

/**
 * think() is called by the UI objects initiated during Game::run()
 * during their modal loop.
 * Depending on the current state we advance game logic and stuff,
 * running the cmd queue etc.
 */
void Game::think() {
	assert(ctrl_);

	while (!pending_player_commands_.empty()) {
		MutexLock m(MutexLock::ID::kCommands);
		do_send_player_command(pending_player_commands_.front());
		pending_player_commands_.pop_front();
	}

	ctrl_->think();

	if (state_ == gs_running) {
		// TODO(sirver): This is not good. Here, it depends on the speed of the
		// computer and the fps if and when the game is saved - this is very bad
		// for scenarios and even worse for the regression suite (which relies on
		// the timings of savings.
		cmdqueue().run_queue(Duration(ctrl_->get_frametime()), get_gametime_pointer());

		// check if autosave is needed
		savehandler_.think(*this);
	}
}

/**
 * Cleanup for load
 * \deprecated
 */
// TODO(unknown): Get rid of this. Prefer to delete and recreate Game-style objects
// Note that this needs fixes in the editor.
void Game::cleanup_for_load() {
	state_ = gs_notrunning;

	EditorGameBase::cleanup_for_load();

	delete_pending_player_commands();
	cmdqueue().flush();

	trade_agreements_.clear();
	trade_extension_proposals_.clear();
	next_trade_agreement_id_ = 1;

	pending_diplomacy_actions_.clear();
	diplomacy_allowed_ = true;
	naval_warfare_allowed_ = false;

	// Statistics
	general_stats_.clear();
}

void Game::full_cleanup() {
	EditorGameBase::full_cleanup();

	delete_pending_player_commands();
	did_postload_addons_before_loading_ = false;
	ctrl_.reset();
	replaywriter_.reset();
	writereplay_ = true;  // Not using `set_write_replay()` on purpose.
	next_game_to_load_.clear();
	list_of_scenarios_.clear();
	replay_filename_.clear();
	forester_cache_.clear();
	last_economy_serial_ = 0;
	last_detectedportspace_serial_ = 0;

	if (has_loader_ui()) {
		remove_loader_ui();
	}
}

void Game::delete_pending_player_commands() {
	MutexLock m(MutexLock::ID::kCommands);
	for (PlayerCommand* pc : pending_player_commands_) {
		delete pc;
	}
	pending_player_commands_.clear();
}

/**
 * Game logic code may write to the synchronization
 * token stream. All written data will be hashed and can be used to
 * check for network or replay desyncs.
 *
 * \return the synchronization token stream
 *
 * \note This is returned as a \ref StreamWrite object to prevent
 * the caller from messing with the checksumming process.
 */
StreamWrite& Game::syncstream() {
	return syncwrapper_;
}

/**
 * Switches to the next part of the syncstream excerpt.
 */
void Game::report_sync_request() {
	syncwrapper_.current_excerpt_id_ =
	   (syncwrapper_.current_excerpt_id_ + 1) % SyncWrapper::kExcerptSize;
	syncwrapper_.excerpts_buffer_[syncwrapper_.current_excerpt_id_].clear();
}

/**
 * Triggers writing of syncstream excerpt and adds the playernumber of the desynced player
 * to the stream.
 * Playernumber should be negative when called by network clients
 */
void Game::report_desync(int32_t playernumber) {
	if (syncwrapper_.dumpfname_.empty()) {
		log_err_time(get_gametime(),
		             "Error: A desync occurred but no filename for the syncstream has been set.");
		return;
	}
	// Replace .wss extension of syncstream file with .wse extension for syncstream extract
	std::string filename = syncwrapper_.dumpfname_;
	assert(syncwrapper_.dumpfname_.length() > kSyncstreamExtension.length());
	filename.replace(filename.length() - kSyncstreamExtension.length(),
	                 kSyncstreamExtension.length(), kSyncstreamExcerptExtension);
	std::unique_ptr<StreamWrite> file(g_fs->open_stream_write(filename));
	assert(file != nullptr);
	// Write revision, branch and build type of this build to the file
	file->unsigned_32(build_id().length());
	file->text(build_id());
	file->unsigned_32(build_type().length());
	file->text(build_type());
	file->signed_32(playernumber);
	// Write our buffers to the file. Start with the oldest one
	const size_t i2 = (syncwrapper_.current_excerpt_id_ + 1) % SyncWrapper::kExcerptSize;
	size_t i = i2;
	do {
		file->text(syncwrapper_.excerpts_buffer_[i]);
		syncwrapper_.excerpts_buffer_[i].clear();
		i = (i + 1) % SyncWrapper::kExcerptSize;
	} while (i != i2);
	file->unsigned_8(SyncEntry::kDesync);
	file->signed_32(playernumber);
	// Restart buffers
	syncwrapper_.current_excerpt_id_ = 0;
}

/**
 * Calculate the current synchronization checksum and copy
 * it into the given array, without affecting the subsequent
 * checksumming process.
 *
 * \return the checksum
 */
crypto::MD5Checksum Game::get_sync_hash() const {
	crypto::MD5Checksummer copy(synchash_);
	return copy.finish_checksum_raw();
}

/**
 * Return a random value that can be used in parallel game logic
 * simulation.
 *
 * \note Do NOT use for random events in the UI or other display code.
 */
uint32_t Game::logic_rand() {
	uint32_t const result = rng().rand();
	syncstream().unsigned_8(SyncEntry::kRandom);
	syncstream().unsigned_32(result);
	return result;
}

/**
 * All player-issued commands must enter the queue through this function.
 * It takes the appropriate action, i.e. either add to the cmd_queue or send
 * across the network.
 */
void Game::send_player_command(PlayerCommand* pc) {
	MutexLock m(MutexLock::ID::kCommands);
	if (is_logic_thread()) {
		do_send_player_command(pc);
	} else {
		pending_player_commands_.push_back(pc);
	}
}

/**
 * Actually enqueue a command.
 *
 * \note In a network game, player commands are only allowed to enter the
 * command queue after being accepted by the networking logic via
 * \ref send_player_command, so you must never enqueue a player command
 * directly.
 */
void Game::enqueue_command(Command* const cmd) {
	if (writereplay_ && replaywriter_) {
		if (upcast(PlayerCommand, plcmd, cmd)) {
			replaywriter_->send_player_command(plcmd);
		}
	}
	cmdqueue().enqueue(cmd);
}

// we might want to make these inlines:
void Game::send_player_bulldoze(PlayerImmovable& pi, bool const recurse) {
	send_player_command(new CmdBulldoze(get_gametime(), pi.owner().player_number(), pi, recurse));
}

void Game::send_player_dismantle(PlayerImmovable& pi, bool keep_wares) {
	send_player_command(
	   new CmdDismantleBuilding(get_gametime(), pi.owner().player_number(), pi, keep_wares));
}

void Game::send_player_build_building(int32_t const pid,
                                      const Coords& coords,
                                      DescriptionIndex const id) {
	assert(descriptions().building_exists(id));
	send_player_command(new CmdBuildBuilding(get_gametime(), pid, coords, id));
}

void Game::send_player_build_flag(int32_t const pid, const Coords& coords) {
	send_player_command(new CmdBuildFlag(get_gametime(), pid, coords));
}

void Game::send_player_build_road(int32_t pid, Path& path) {
	send_player_command(new CmdBuildRoad(get_gametime(), pid, path));
}

void Game::send_player_build_waterway(int32_t pid, Path& path) {
	send_player_command(new CmdBuildWaterway(get_gametime(), pid, path));
}

void Game::send_player_flagaction(Flag& flag, FlagJob::Type t) {
	send_player_command(new CmdFlagAction(get_gametime(), flag.owner().player_number(), flag, t));
}

void Game::send_player_start_stop_building(Building& building) {
	send_player_command(
	   new CmdStartStopBuilding(get_gametime(), building.owner().player_number(), building));
}

void Game::send_player_toggle_infinite_production(Building& building) {
	send_player_command(
	   new CmdToggleInfiniteProduction(get_gametime(), building.owner().player_number(), building));
}

void Game::send_player_set_soldier_preference(MapObject& mo, SoldierPreference my_preference) {
	send_player_command(
	   new CmdSetSoldierPreference(get_gametime(), mo.owner().player_number(), mo, my_preference));
}

void Game::send_player_start_or_cancel_expedition(Building& building) {
	send_player_command(
	   new CmdStartOrCancelExpedition(get_gametime(), building.owner().player_number(), building));
}

void Game::send_player_enhance_building(Building& building,
                                        DescriptionIndex const id,
                                        bool keep_wares) {
	assert(building.descr().type() == MapObjectType::CONSTRUCTIONSITE ||
	       building.owner().tribe().has_building(id));
	send_player_command(new CmdEnhanceBuilding(
	   get_gametime(), building.owner().player_number(), building, id, keep_wares));
}

void Game::send_player_evict_worker(Worker& worker) {
	send_player_command(new CmdEvictWorker(get_gametime(), worker.owner().player_number(), worker));
}

void Game::send_player_set_ware_priority(PlayerImmovable& imm,
                                         const WareWorker type,
                                         const DescriptionIndex index,
                                         const WarePriority& prio,
                                         bool cs,
                                         uint32_t disambiguator_id) {
	send_player_command(new CmdSetWarePriority(
	   get_gametime(), imm.owner().player_number(), imm, type, index, prio, cs, disambiguator_id));
}

void Game::send_player_set_input_max_fill(PlayerImmovable& imm,
                                          DescriptionIndex const index,
                                          WareWorker type,
                                          uint32_t const max_fill,
                                          bool cs,
                                          uint32_t disambiguator_id) {
	send_player_command(new CmdSetInputMaxFill(get_gametime(), imm.owner().player_number(), imm,
	                                           index, type, max_fill, cs, disambiguator_id));
}

void Game::send_player_change_training_options(TrainingSite& ts,
                                               TrainingAttribute attr,
                                               int32_t const val) {
	send_player_command(
	   new CmdChangeTrainingOptions(get_gametime(), ts.owner().player_number(), ts, attr, val));
}

void Game::send_player_drop_soldier(MapObject& b, int32_t const ser) {
	assert(ser != -1);
	send_player_command(new CmdDropSoldier(get_gametime(), b.owner().player_number(), b, ser));
}

void Game::send_player_change_soldier_capacity(Building& b, int32_t const val) {
	send_player_command(
	   new CmdChangeSoldierCapacity(get_gametime(), b.owner().player_number(), b, val));
}

void Game::send_player_attack(const Flag& flag,
                              PlayerNumber const who_attacks,
                              const std::vector<Serial>& soldiers,
                              const bool allow_conquer) {
	for (Widelands::Coords& coords : flag.get_building()->get_positions(*this)) {
		if (player(who_attacks).is_seeing(Map::get_index(coords, map().get_width()))) {
			send_player_command(
			   new CmdAttack(get_gametime(), who_attacks, flag, soldiers, allow_conquer));
			break;
		}
	}
}

void Game::send_player_ship_scouting_direction(const Ship& ship, WalkingDir direction) {
	send_player_command(new CmdShipScoutDirection(
	   get_gametime(), ship.get_owner()->player_number(), ship.serial(), direction));
}

void Game::send_player_ship_construct_port(const Ship& ship, Coords coords) {
	send_player_command(new CmdShipConstructPort(
	   get_gametime(), ship.get_owner()->player_number(), ship.serial(), coords));
}

void Game::send_player_ship_explore_island(const Ship& ship, IslandExploreDirection direction) {
	send_player_command(new CmdShipExploreIsland(
	   get_gametime(), ship.get_owner()->player_number(), ship.serial(), direction));
}

void Game::send_player_ship_set_destination(const Ship& ship, const MapObject* dest) {
	send_player_command(new CmdShipSetDestination(get_gametime(), ship.get_owner()->player_number(),
	                                              ship.serial(),
	                                              dest == nullptr ? 0 : dest->serial()));
}
void Game::send_player_ship_set_destination(const Ship& ship, const DetectedPortSpace& dest) {
	send_player_command(new CmdShipSetDestination(
	   get_gametime(), ship.get_owner()->player_number(), ship.serial(), dest));
}

void Game::send_player_sink_ship(const Ship& ship) {
	send_player_command(
	   new CmdShipSink(get_gametime(), ship.get_owner()->player_number(), ship.serial()));
}

void Game::send_player_refit_ship(const Ship& ship, const ShipType t) {
	send_player_command(
	   new CmdShipRefit(get_gametime(), ship.get_owner()->player_number(), ship.serial(), t));
}

void Game::send_player_warship_command(const Ship& ship,
                                       const WarshipCommand cmd,
                                       const std::vector<uint32_t>& parameters) {
	send_player_command(new CmdWarshipCommand(
	   get_gametime(), ship.get_owner()->player_number(), ship.serial(), cmd, parameters));
}

void Game::send_player_cancel_expedition_ship(const Ship& ship) {
	send_player_command(new CmdShipCancelExpedition(
	   get_gametime(), ship.get_owner()->player_number(), ship.serial()));
}

void Game::send_player_expedition_config(PortDock& pd,
                                         WareWorker ww,
                                         DescriptionIndex di,
                                         bool add) {
	send_player_command(
	   new CmdExpeditionConfig(get_gametime(), pd.get_owner()->player_number(), pd, ww, di, add));
}

void Game::send_player_diplomacy(PlayerNumber p1, DiplomacyAction a, PlayerNumber p2) {
	send_player_command(new CmdDiplomacy(get_gametime(), p1, a, p2));
}

void Game::send_player_propose_trade(const TradeInstance& trade) {
	Market* object = trade.initiator.get(*this);
	assert(object != nullptr);
	send_player_command(
	   new CmdProposeTrade(get_gametime(), object->get_owner()->player_number(), trade));
}

void Game::send_player_extend_trade(PlayerNumber sender,
                                    TradeID trade_id,
                                    TradeAction action,
                                    int32_t batches) {
	send_player_command(new CmdExtendTrade(get_gametime(), sender, trade_id, action, batches));
}

void Game::send_player_trade_action(
   PlayerNumber sender, TradeID trade_id, TradeAction action, Serial accepter, Serial source) {
	send_player_command(
	   new CmdTradeAction(get_gametime(), sender, trade_id, action, accepter, source));
}

void Game::send_player_set_stock_policy(Building& imm,
                                        WareWorker ww,
                                        DescriptionIndex di,
                                        StockPolicy sp) {
	send_player_command(new CmdSetStockPolicy(
	   get_gametime(), imm.get_owner()->player_number(), imm, ww == wwWORKER, di, sp));
}

void Game::send_player_toggle_mute(const Building& b, bool all) {
	send_player_command(
	   new CmdToggleMuteMessages(get_gametime(), b.owner().player_number(), b, all));
}

void Game::send_player_mark_object_for_removal(PlayerNumber p, Immovable& mo, bool mark) {
	send_player_command(new CmdMarkMapObjectForRemoval(get_gametime(), p, mo, mark));
}

void Game::send_player_pinned_note(
   PlayerNumber p, Coords pos, const std::string& text, const RGBColor& rgb, bool del) {
	send_player_command(new CmdPinnedNote(get_gametime(), p, text, pos, rgb, del));
}

void Game::send_player_building_name(PlayerNumber p, Serial s, const std::string& name) {
	send_player_command(new CmdBuildingName(get_gametime(), p, s, name));
}

void Game::send_player_fleet_targets(PlayerNumber p, Serial i, Quantity q) {
	send_player_command(new CmdFleetTargets(get_gametime(), p, i, q));
}

bool Game::check_trade_player_matches(const TradeInstance& trade,
                                      const PlayerNumber sender,
                                      const PlayerNumber proposer,
                                      const bool check_recipient,
                                      Player** p1,
                                      Player** p2,
                                      const Market** market) {
	if (check_recipient) {
		// Check if this is the correct recipient player
		if (proposer == trade.sending_player) {
			if (sender != trade.receiving_player) {
				return false;
			}
		} else if (proposer == trade.receiving_player) {
			if (sender != trade.sending_player) {
				return false;
			}
		} else {
			return false;
		}
	}

	const bool proposer_is_initiator = proposer == trade.sending_player;
	*p1 = get_safe_player(proposer_is_initiator ? trade.sending_player : trade.receiving_player);
	*p2 = get_safe_player(proposer_is_initiator ? trade.receiving_player : trade.sending_player);
	*market = (proposer_is_initiator ? trade.initiator : trade.receiver).get(*this);

	return true;
}

TradeID Game::propose_trade(TradeInstance trade) {
	assert(trade.check_illegal().empty());

	MutexLock m(MutexLock::ID::kObjects);
	const TradeID id = next_trade_agreement_id_++;

	Market* initiator = trade.initiator.get(*this);
	assert(initiator != nullptr);

	trade.state = TradeInstance::State::kProposed;
	trade_agreements_[id] = trade;

	get_safe_player(trade.receiving_player)
	   ->add_message(*this, std::unique_ptr<Message>(new Message(
	                           Message::Type::kTrading, get_gametime(), _("Trade Offer"),
	                           // TODO(Nordfriese): Use receiver's own tribe's market here
	                           initiator->descr().icon_filename(), _("New trade offer received"),
	                           format_l(_("You have received a new trade offer from %s."),
	                                    initiator->owner().get_name()))));
	Notifications::publish(NoteTradeChanged(id, NoteTradeChanged::Action::kProposed));

	return id;
}

void Game::accept_trade(const TradeID trade_id, Market& receiver) {
	MutexLock m(MutexLock::ID::kObjects);

	auto it = trade_agreements_.find(trade_id);
	if (it == trade_agreements_.end() || it->second.state != TradeInstance::State::kProposed) {
		return;
	}

	const TradeInstance& trade = it->second;
	Market* initiator = trade.initiator.get(*this);
	if (initiator == nullptr) {
		trade_agreements_.erase(it);
		return;
	}

	// TODO(sirver,trading): Check connectivity between the markets.

	it->second.receiver = &receiver;
	it->second.state = TradeInstance::State::kRunning;

	initiator->new_trade(trade_id, trade.items_to_send, trade.num_batches, &receiver);
	receiver.new_trade(trade_id, trade.items_to_receive, trade.num_batches, trade.initiator);

	initiator->send_message(*this, Message::Type::kTrading, _("Trade Accepted"),
	                        initiator->descr().icon_filename(), _("Trade offer accepted"),
	                        format_l(_("%1$s has accepted your trade offer at %2$s."),
	                                 receiver.owner().get_name(), initiator->get_market_name()),
	                        false);
	Notifications::publish(NoteTradeChanged(trade_id, NoteTradeChanged::Action::kAccepted));
}

void Game::reject_trade(const TradeID trade_id) {
	MutexLock m(MutexLock::ID::kObjects);

	auto it = trade_agreements_.find(trade_id);
	if (it == trade_agreements_.end() || it->second.state != TradeInstance::State::kProposed) {
		return;
	}

	const TradeInstance& trade = it->second;
	Market* initiator = trade.initiator.get(*this);
	if (initiator != nullptr) {
		initiator->send_message(
		   *this, Message::Type::kTrading, _("Trade Rejected"), initiator->descr().icon_filename(),
		   _("Trade offer rejected"),
		   format_l(_("%1$s has rejected your trade offer at %2$s."),
		            player(trade.receiving_player).get_name(), initiator->get_market_name()),
		   false);
	}

	trade_agreements_.erase(it);
	Notifications::publish(NoteTradeChanged(trade_id, NoteTradeChanged::Action::kRejected));
}

void Game::retract_trade(const TradeID trade_id) {
	MutexLock m(MutexLock::ID::kObjects);

	auto it = trade_agreements_.find(trade_id);
	if (it == trade_agreements_.end() || it->second.state != TradeInstance::State::kProposed) {
		return;
	}

	const TradeInstance& trade = it->second;
	Market* initiator = trade.initiator.get(*this);

	get_safe_player(trade.receiving_player)
	   ->add_message(*this, std::unique_ptr<Message>(new Message(
	                           Message::Type::kTrading, get_gametime(), _("Trade Retracted"),
	                           // TODO(Nordfriese): Use receiver's own tribe's market here
	                           initiator->descr().icon_filename(), _("Trade offer retracted"),
	                           format_l(_("The trade offer by %s has been retracted."),
	                                    initiator->owner().get_name()))));

	trade_agreements_.erase(it);
	Notifications::publish(NoteTradeChanged(trade_id, NoteTradeChanged::Action::kRetracted));
}

void Game::cancel_trade(TradeID trade_id, bool reached_regular_end, const Player* canceller) {
	MutexLock m(MutexLock::ID::kObjects);

	// The trade id might be long gone - since we never disconnect from the
	// 'removed' signal of the two buildings, we might be invoked long after the
	// trade was deleted for other reasons.
	const auto it = trade_agreements_.find(trade_id);
	if (it == trade_agreements_.end()) {
		return;
	}
	const TradeInstance& trade = it->second;

	Market* initiator = trade.initiator.get(*this);
	if (initiator != nullptr) {
		initiator->cancel_trade(*this, trade_id, reached_regular_end,
		                        reached_regular_end || canceller != initiator->get_owner());
	}

	Market* receiver = it->second.receiver.get(*this);
	if (receiver != nullptr) {
		receiver->cancel_trade(*this, trade_id, reached_regular_end,
		                       reached_regular_end || canceller != receiver->get_owner());
	}

	trade_agreements_.erase(trade_id);

	// TODO(Nordfriese): Turn pending extension proposals into new trade proposals
	// automatically if the trade reached its regular end?
	trade_extension_proposals_.erase(
	   std::remove_if(trade_extension_proposals_.begin(), trade_extension_proposals_.end(),
	                  [trade_id](const TradeExtension& te) { return te.trade_id == trade_id; }),
	   trade_extension_proposals_.end());

	Notifications::publish(NoteTradeChanged(trade_id, reached_regular_end ?
	                                                     NoteTradeChanged::Action::kCompleted :
	                                                     NoteTradeChanged::Action::kCancelled));
}

void Game::move_trade(const TradeID trade_id, Market& old_market, Market& new_market) {
	MutexLock m(MutexLock::ID::kObjects);

	if (old_market.get_owner() != new_market.get_owner() || &old_market == &new_market) {
		return;  // Doesn't make sense
	}

	auto instance = trade_agreements_.find(trade_id);
	if (instance == trade_agreements_.end()) {
		return;
	}

	if (instance->second.initiator.serial() == new_market.serial() ||
	    instance->second.receiver.serial() == new_market.serial()) {
		return;  // Trade is already there
	}

	const bool is_sender = (instance->second.initiator.serial() == old_market.serial());
	if (!is_sender && instance->second.receiver.serial() != old_market.serial()) {
		return;  // Old market is neither sender nor receiver
	}

	if (is_sender) {
		instance->second.initiator = &new_market;
	} else {
		instance->second.receiver = &new_market;
	}

	if (instance->second.state == TradeInstance::State::kRunning) {
		old_market.move_trade_to(*this, trade_id, new_market);
	}

	Notifications::publish(NoteBuilding(old_market.serial(), NoteBuilding::Action::kChanged));
	Notifications::publish(NoteBuilding(new_market.serial(), NoteBuilding::Action::kChanged));
	Notifications::publish(NoteTradeChanged(trade_id, NoteTradeChanged::Action::kMoved));
}

void Game::propose_trade_extension(const PlayerNumber sender,
                                   const TradeID trade_id,
                                   const int32_t batches) {
	if ((batches <= 0 && batches != kInfiniteTrade) || batches > kMaxWaresPerBatch) {
		return;
	}

	MutexLock m(MutexLock::ID::kObjects);

	const auto trade = trade_agreements_.find(trade_id);
	if (trade == trade_agreements_.end() ||
	    (trade->second.sending_player != sender && trade->second.receiving_player != sender) ||
	    trade->second.num_batches == kInfiniteTrade) {
		return;
	}

	trade_extension_proposals_.emplace_back(trade_id, sender, batches);

	{
		Player* p1;
		Player* p2;
		const Market* market;
		if (!check_trade_player_matches(trade->second, sender, sender, false, &p1, &p2, &market)) {
			NEVER_HERE();
		}

		p2->add_message(
		   *this,
		   std::unique_ptr<Message>(new Message(
		      Message::Type::kTrading, get_gametime(), _("Trade Extension Proposal"),
		      market != nullptr ? market->descr().icon_filename() : "images/wui/menus/diplomacy.png",
		      _("New trade extension proposal received"),
		      format_l(_("%s has proposed to extend a trade."), p1->get_name()))));
	}

	Notifications::publish(NoteTradeChanged(trade_id, NoteTradeChanged::Action::kExtensionProposal));
}

void Game::retract_trade_extension(const PlayerNumber sender,
                                   const TradeID trade_id,
                                   const int32_t batches) {
	for (auto it = trade_extension_proposals_.begin(); it != trade_extension_proposals_.end();
	     ++it) {
		if (it->trade_id == trade_id && it->proposer == sender && it->batches == batches) {
			MutexLock m(MutexLock::ID::kObjects);

			if (const auto trade = trade_agreements_.find(trade_id);
			    trade != trade_agreements_.end()) {

				Player* p1;
				Player* p2;
				const Market* market;
				if (!check_trade_player_matches(
				       trade->second, sender, it->proposer, false, &p1, &p2, &market)) {
					NEVER_HERE();
				}

				p2->add_message(
				   *this, std::unique_ptr<Message>(new Message(
				             Message::Type::kTrading, get_gametime(), _("Trade Extension Retracted"),
				             market != nullptr ? market->descr().icon_filename() :
				                                 "images/wui/menus/diplomacy.png",
				             _("Trade extension proposal retracted"),
				             format_l(_("The proposal by %s to extend a trade has been retracted."),
				                      p1->get_name()))));
			}

			trade_extension_proposals_.erase(it);
			Notifications::publish(
			   NoteTradeChanged(trade_id, NoteTradeChanged::Action::kExtensionProposal));
			return;
		}
	}

	verb_log_warn("retract_trade_extension(%u): not found, ignoring", trade_id);
}

void Game::reject_trade_extension(const PlayerNumber sender,
                                  const TradeID trade_id,
                                  const int32_t batches) {
	for (auto it = trade_extension_proposals_.begin(); it != trade_extension_proposals_.end();
	     ++it) {
		if (it->trade_id == trade_id && it->batches == batches) {
			MutexLock m(MutexLock::ID::kObjects);

			const auto trade = trade_agreements_.find(trade_id);
			if (trade == trade_agreements_.end()) {
				continue;
			}

			Player* p1;
			Player* p2;
			const Market* market;
			if (!check_trade_player_matches(
			       trade->second, sender, it->proposer, true, &p1, &p2, &market)) {
				continue;
			}

			p1->add_message(
			   *this,
			   std::unique_ptr<Message>(new Message(
			      Message::Type::kTrading, get_gametime(), _("Trade Extension Rejected"),
			      market != nullptr ? market->descr().icon_filename() :
			                          "images/wui/menus/diplomacy.png",
			      _("Trade extension proposal rejected"),
			      format_l(_("%s has rejected your proposal to extend a trade."), p2->get_name()))));

			trade_extension_proposals_.erase(it);
			Notifications::publish(
			   NoteTradeChanged(trade_id, NoteTradeChanged::Action::kExtensionProposal));
			return;
		}
	}

	verb_log_warn("reject_trade_extension(%u): not found, ignoring", trade_id);
}

void Game::accept_trade_extension(const PlayerNumber sender,
                                  const TradeID trade_id,
                                  const int32_t batches) {
	for (auto it = trade_extension_proposals_.begin(); it != trade_extension_proposals_.end();
	     ++it) {
		if (it->trade_id == trade_id && it->batches == batches) {
			MutexLock m(MutexLock::ID::kObjects);

			auto trade = trade_agreements_.find(trade_id);
			if (trade == trade_agreements_.end()) {
				continue;
			}

			Player* p1;
			Player* p2;
			const Market* proposing_market;
			if (!check_trade_player_matches(
			       trade->second, sender, it->proposer, true, &p1, &p2, &proposing_market)) {
				continue;
			}

			if (trade->second.num_batches != kInfiniteTrade) {
				if (batches == kInfiniteTrade) {
					trade->second.num_batches = kInfiniteTrade;
				} else {
					trade->second.num_batches += batches;
				}

				if (Market* market = trade->second.initiator.get(*this); market != nullptr) {
					market->notify_trade_extended(trade_id, trade->second.num_batches);
				}
				if (Market* market = trade->second.receiver.get(*this); market != nullptr) {
					market->notify_trade_extended(trade_id, trade->second.num_batches);
				}
			}

			p1->add_message(
			   *this,
			   std::unique_ptr<Message>(new Message(
			      Message::Type::kTrading, get_gametime(), _("Trade Extension Accepted"),
			      proposing_market != nullptr ? proposing_market->descr().icon_filename() :
			                                    "images/wui/menus/diplomacy.png",
			      _("Trade extension proposal accepted"),
			      format_l(_("%s has accepted your proposal to extend a trade."), p2->get_name()))));

			trade_extension_proposals_.erase(it);

			if (trade->second.num_batches == kInfiniteTrade) {
				// If the trade is infinite now, further extension don't make sense.
				trade_extension_proposals_.erase(
				   std::remove_if(
				      trade_extension_proposals_.begin(), trade_extension_proposals_.end(),
				      [trade_id](const TradeExtension& te) { return te.trade_id == trade_id; }),
				   trade_extension_proposals_.end());
			}

			Notifications::publish(
			   NoteTradeChanged(trade_id, NoteTradeChanged::Action::kExtensionProposal));
			return;
		}
	}

	verb_log_warn("accept_trade_extension(%u): not found, ignoring", trade_id);
}

std::vector<TradeExtension> Game::find_trade_extensions(const TradeID trade_id,
                                                        const PlayerNumber player,
                                                        const bool as_proposer) const {
	std::vector<TradeExtension> result;
	MutexLock m(MutexLock::ID::kObjects);
	for (const TradeExtension& te : trade_extension_proposals_) {
		if (te.trade_id == trade_id) {
			if ((te.proposer == player) == as_proposer) {
				result.push_back(te);
			}
		}
	}
	return result;
}

std::vector<TradeID> Game::find_trade_offers(PlayerNumber receiver, Coords accept_at) const {
	std::vector<TradeID> result;
	Path unused;
	for (const auto& pair : trade_agreements_) {
		if (pair.second.state == TradeInstance::State::kProposed &&
		    pair.second.receiving_player == receiver) {

			if (!accept_at.valid()) {
				result.push_back(pair.first);
			} else {
				MutexLock m(MutexLock::ID::kObjects);
				Market* initiator = pair.second.initiator.get(*this);
				if (initiator != nullptr &&
				    map().findpath(map().br_n(accept_at), map().br_n(initiator->get_position()), 0,
				                   unused, CheckStepDefault(MOVECAPS_WALK), 0, 0, wwWORKER) >= 0) {
					result.push_back(pair.first);
				}
			}
		}
	}
	return result;
}

std::vector<TradeID> Game::find_trade_proposals(PlayerNumber initiator,
                                                Serial market_filter) const {
	std::vector<TradeID> result;
	for (const auto& pair : trade_agreements_) {
		if (pair.second.state == TradeInstance::State::kProposed) {
			if (market_filter == 0 || pair.second.initiator.serial() == market_filter) {
				if (Market* market = pair.second.initiator.get(*this);
				    market != nullptr && market->owner().player_number() == initiator) {
					result.push_back(pair.first);
				}
			}
		}
	}
	return result;
}

std::vector<TradeID> Game::find_active_trades(PlayerNumber player) const {
	std::vector<TradeID> result;
	for (const auto& pair : trade_agreements_) {
		if (pair.second.state == TradeInstance::State::kRunning) {
			if (pair.second.receiving_player == player || pair.second.sending_player == player) {
				result.push_back(pair.first);
			}
		}
	}
	return result;
}

LuaGameInterface& Game::lua() {
	return dynamic_cast<LuaGameInterface&>(EditorGameBase::lua());
}

const std::string& Game::get_win_condition_displayname() const {
	return win_condition_displayname_;
}
void Game::set_win_condition_displayname(const std::string& name) {
	win_condition_displayname_ = name;
}
int32_t Game::get_win_condition_duration() const {
	return win_condition_duration_;
}
void Game::set_win_condition_duration(int32_t d) {
	win_condition_duration_ = d;
}

Serial Game::generate_economy_serial() {
	return last_economy_serial_++;
}
Serial Game::generate_detectedportspace_serial() {
	return last_detectedportspace_serial_++;
}
void Game::notify_economy_serial(Serial serial) {
	last_economy_serial_ = std::max(last_economy_serial_, serial + 1);
}
void Game::notify_detectedportspace_serial(Serial serial) {
	last_detectedportspace_serial_ = std::max(last_detectedportspace_serial_, serial + 1);
}

/**
 * Sample global statistics for the game.
 */
void Game::sample_statistics() {
	// Update general stats
	PlayerNumber const nr_plrs = map().get_nrplayers();
	std::vector<uint32_t> land_size;
	std::vector<uint32_t> nr_buildings;
	std::vector<uint32_t> nr_ships;
	std::vector<uint32_t> nr_naval_victories;
	std::vector<uint32_t> nr_naval_losses;
	std::vector<uint32_t> nr_casualties;
	std::vector<uint32_t> nr_kills;
	std::vector<uint32_t> nr_msites_lost;
	std::vector<uint32_t> nr_msites_defeated;
	std::vector<uint32_t> nr_civil_blds_lost;
	std::vector<uint32_t> nr_civil_blds_defeated;
	std::vector<float> miltary_strength;
	std::vector<uint32_t> nr_workers;
	std::vector<uint32_t> nr_wares;
	std::vector<uint32_t> productivity;
	std::vector<uint32_t> nr_production_sites;
	std::vector<uint32_t> custom_statistic;
	land_size.resize(nr_plrs);
	nr_buildings.resize(nr_plrs);
	nr_ships.resize(nr_plrs);
	nr_naval_victories.resize(nr_plrs);
	nr_naval_losses.resize(nr_plrs);
	nr_casualties.resize(nr_plrs);
	nr_kills.resize(nr_plrs);
	nr_msites_lost.resize(nr_plrs);
	nr_msites_defeated.resize(nr_plrs);
	nr_civil_blds_lost.resize(nr_plrs);
	nr_civil_blds_defeated.resize(nr_plrs);
	miltary_strength.resize(nr_plrs);
	nr_workers.resize(nr_plrs);
	nr_wares.resize(nr_plrs);
	productivity.resize(nr_plrs);
	nr_production_sites.resize(nr_plrs);
	custom_statistic.resize(nr_plrs);

	//  We walk the map, to gain all needed information.
	const Map& themap = map();
	Extent const extent = themap.extent();
	iterate_Map_FCoords(themap, extent, fc) {
		if (PlayerNumber const owner = fc.field->get_owned_by()) {
			++land_size[owner - 1];
		}

		// Get the immovable
		if (fc.field->get_immovable() != nullptr &&
		    fc.field->get_immovable()->descr().type() >= MapObjectType::BUILDING) {
			upcast(Building, building, fc.field->get_immovable());
			if (building->get_position() == fc) {  // only count main location
				uint8_t const player_index = building->owner().player_number() - 1;
				++nr_buildings[player_index];

				//  If it is a productionsite, add its productivity.
				if (building->descr().type() == MapObjectType::PRODUCTIONSITE ||
				    building->descr().type() == MapObjectType::TRAININGSITE) {
					++nr_production_sites[player_index];
					productivity[player_index] +=
					   dynamic_cast<const ProductionSite&>(*building).get_statistics_percent();
				}
			}
		}

		// Now, walk the bobs
		for (Bob const* b = fc.field->get_first_bob(); b != nullptr; b = b->get_next_bob()) {
			if (b->descr().type() != MapObjectType::SOLDIER) {
				continue;
			}

			constexpr int kHeroValue = 19;
			upcast(Soldier const, soldier, b);
			assert(soldier != nullptr);
			const float total_level = soldier->get_level(TrainingAttribute::kTotal);
			const float max_level = std::max<float>(soldier->descr().get_max_total_level(), 1);
			miltary_strength[soldier->owner().player_number() - 1] +=
			   (total_level * total_level * kHeroValue) / (max_level * max_level) + 1.f;
		}
	}

	//  Number of workers / wares / casualties / kills.
	iterate_players_existing(p, nr_plrs, *this, plr) {
		const TribeDescr& tribe = plr->tribe();
		uint32_t wostock = 0;
		uint32_t wastock = 0;

		for (const auto& economy : plr->economies()) {
			switch (economy.second->type()) {
			case wwWARE:
				for (const DescriptionIndex& ware_index : tribe.wares()) {
					wastock += economy.second->stock_ware_or_worker(ware_index);
				}
				break;
			case wwWORKER:
				for (const DescriptionIndex& worker_index : tribe.workers()) {
					if (worker_index == tribe.soldier()) {
						continue;  // Ignore soldiers.
					}
					const WorkerDescr& d = *tribe.get_worker_descr(worker_index);
					if (d.is_buildable() && d.buildcost().empty()) {
						continue;  // Don't count free workers.
					}
					wostock += economy.second->stock_ware_or_worker(worker_index);
				}
				break;
			default:
				NEVER_HERE();
			}
		}
		nr_wares[p - 1] = wastock;
		nr_workers[p - 1] = wostock;
		nr_ships[p - 1] = plr->ships().size();
		nr_naval_victories[p - 1] = plr->naval_victories();
		nr_naval_losses[p - 1] = plr->naval_losses();
		nr_casualties[p - 1] = plr->casualties();
		nr_kills[p - 1] = plr->kills();
		nr_msites_lost[p - 1] = plr->msites_lost();
		nr_msites_defeated[p - 1] = plr->msites_defeated();
		nr_civil_blds_lost[p - 1] = plr->civil_blds_lost();
		nr_civil_blds_defeated[p - 1] = plr->civil_blds_defeated();
	}

	// Now, divide the statistics
	for (uint32_t i = 0; i < map().get_nrplayers(); ++i) {
		if (productivity[i] != 0u) {
			productivity[i] /= nr_production_sites[i];
		}
	}

	// If there is a hook function defined to sample special statistics in this
	// game, call the corresponding Lua function
	std::unique_ptr<LuaTable> hook = lua().get_hook("custom_statistic");
	if (hook) {
		hook->do_not_warn_about_unaccessed_keys();
		iterate_players_existing(p, nr_plrs, *this, plr) {
			std::unique_ptr<LuaCoroutine> cr(hook->get_coroutine("calculator"));
			cr->push_arg(plr);
			cr->resume();
			custom_statistic[p - 1] = cr->pop_uint32();
		}
	}

	// Now, push this on the general statistics
	general_stats_.resize(map().get_nrplayers());
	for (uint32_t i = 0; i < map().get_nrplayers(); ++i) {
		general_stats_[i].land_size.push_back(land_size[i]);
		general_stats_[i].nr_buildings.push_back(nr_buildings[i]);
		general_stats_[i].nr_ships.push_back(nr_ships[i]);
		general_stats_[i].nr_naval_victories.push_back(nr_naval_victories[i]);
		general_stats_[i].nr_naval_losses.push_back(nr_naval_losses[i]);
		general_stats_[i].nr_casualties.push_back(nr_casualties[i]);
		general_stats_[i].nr_kills.push_back(nr_kills[i]);
		general_stats_[i].nr_msites_lost.push_back(nr_msites_lost[i]);
		general_stats_[i].nr_msites_defeated.push_back(nr_msites_defeated[i]);
		general_stats_[i].nr_civil_blds_lost.push_back(nr_civil_blds_lost[i]);
		general_stats_[i].nr_civil_blds_defeated.push_back(nr_civil_blds_defeated[i]);
		general_stats_[i].miltary_strength.push_back(lroundf(miltary_strength[i]));
		general_stats_[i].nr_workers.push_back(nr_workers[i]);
		general_stats_[i].nr_wares.push_back(nr_wares[i]);
		general_stats_[i].productivity.push_back(productivity[i]);
		general_stats_[i].custom_statistic.push_back(custom_statistic[i]);
	}

	// Calculate statistics for the players
	const PlayerNumber nr_players = map().get_nrplayers();
	iterate_players_existing(p, nr_players, *this, plr) plr->sample_statistics();
}

/**
 * Read statistics data from a file.
 *
 * \param fr file to read from
 * \param packet_version from GamePlayerInfoPacket in game_io/game_player_info_packet.cc
 */
void Game::read_statistics(FileRead& fr, uint16_t packet_version) {
	fr.unsigned_32();  // used to be last stats update time

	// Read general statistics
	uint32_t entries = fr.unsigned_16();
	const PlayerNumber nr_players = map().get_nrplayers();
	general_stats_.resize(nr_players);

	iterate_players_existing_novar(p, nr_players, *this) {
		general_stats_[p - 1].land_size.resize(entries);
		general_stats_[p - 1].nr_workers.resize(entries);
		general_stats_[p - 1].nr_buildings.resize(entries);
		general_stats_[p - 1].nr_wares.resize(entries);
		general_stats_[p - 1].productivity.resize(entries);
		general_stats_[p - 1].nr_ships.resize(entries);
		general_stats_[p - 1].nr_naval_victories.resize(entries);
		general_stats_[p - 1].nr_naval_losses.resize(entries);
		general_stats_[p - 1].nr_casualties.resize(entries);
		general_stats_[p - 1].nr_kills.resize(entries);
		general_stats_[p - 1].nr_msites_lost.resize(entries);
		general_stats_[p - 1].nr_msites_defeated.resize(entries);
		general_stats_[p - 1].nr_civil_blds_lost.resize(entries);
		general_stats_[p - 1].nr_civil_blds_defeated.resize(entries);
		general_stats_[p - 1].miltary_strength.resize(entries);
		general_stats_[p - 1].custom_statistic.resize(entries);
	}

	iterate_players_existing_novar(
	   p, nr_players, *this) for (uint32_t j = 0; j < general_stats_[p - 1].land_size.size(); ++j) {
		general_stats_[p - 1].land_size[j] = fr.unsigned_32();
		general_stats_[p - 1].nr_workers[j] = fr.unsigned_32();
		general_stats_[p - 1].nr_buildings[j] = fr.unsigned_32();
		general_stats_[p - 1].nr_wares[j] = fr.unsigned_32();
		general_stats_[p - 1].productivity[j] = fr.unsigned_32();
		general_stats_[p - 1].nr_casualties[j] = fr.unsigned_32();
		general_stats_[p - 1].nr_kills[j] = fr.unsigned_32();
		general_stats_[p - 1].nr_msites_lost[j] = fr.unsigned_32();
		general_stats_[p - 1].nr_msites_defeated[j] = fr.unsigned_32();
		general_stats_[p - 1].nr_civil_blds_lost[j] = fr.unsigned_32();
		general_stats_[p - 1].nr_civil_blds_defeated[j] = fr.unsigned_32();
		general_stats_[p - 1].miltary_strength[j] = fr.unsigned_32();
		general_stats_[p - 1].custom_statistic[j] = fr.unsigned_32();
		// TODO(Nordfriese): Savegame compatibility v1.1
		general_stats_[p - 1].nr_ships[j] = packet_version >= 32 ? fr.unsigned_32() : 0;
		general_stats_[p - 1].nr_naval_victories[j] = packet_version >= 32 ? fr.unsigned_32() : 0;
		general_stats_[p - 1].nr_naval_losses[j] = packet_version >= 32 ? fr.unsigned_32() : 0;
	}
}

/**
 * Write general statistics to the given file.
 */
void Game::write_statistics(FileWrite& fw) {
	fw.unsigned_32(0);  // Used to be last stats update time. No longer needed

	// General statistics
	// First, we write the size of the statistics arrays
	uint32_t entries = 0;

	const PlayerNumber nr_players = map().get_nrplayers();
	iterate_players_existing_novar(p, nr_players, *this) if (!general_stats_.empty()) {
		entries = general_stats_[p - 1].land_size.size();
		break;
	}

	fw.unsigned_16(entries);

	iterate_players_existing_novar(p, nr_players, *this) for (uint32_t j = 0; j < entries; ++j) {
		fw.unsigned_32(general_stats_[p - 1].land_size[j]);
		fw.unsigned_32(general_stats_[p - 1].nr_workers[j]);
		fw.unsigned_32(general_stats_[p - 1].nr_buildings[j]);
		fw.unsigned_32(general_stats_[p - 1].nr_wares[j]);
		fw.unsigned_32(general_stats_[p - 1].productivity[j]);
		fw.unsigned_32(general_stats_[p - 1].nr_casualties[j]);
		fw.unsigned_32(general_stats_[p - 1].nr_kills[j]);
		fw.unsigned_32(general_stats_[p - 1].nr_msites_lost[j]);
		fw.unsigned_32(general_stats_[p - 1].nr_msites_defeated[j]);
		fw.unsigned_32(general_stats_[p - 1].nr_civil_blds_lost[j]);
		fw.unsigned_32(general_stats_[p - 1].nr_civil_blds_defeated[j]);
		fw.unsigned_32(general_stats_[p - 1].miltary_strength[j]);
		fw.unsigned_32(general_stats_[p - 1].custom_statistic[j]);
		fw.unsigned_32(general_stats_[p - 1].nr_ships[j]);
		fw.unsigned_32(general_stats_[p - 1].nr_naval_victories[j]);
		fw.unsigned_32(general_stats_[p - 1].nr_naval_losses[j]);
	}
}
}  // namespace Widelands
