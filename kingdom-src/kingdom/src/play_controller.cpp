/* $Id: play_controller.cpp 47608 2010-11-21 01:56:29Z shadowmaster $ */
/*
   Copyright (C) 2006 - 2010 by Joerg Hinrichs <joerg.hinrichs@alice-dsl.de>
   wesnoth playlevel Copyright (C) 2003 by David White <dave@whitevine.net>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

/**
 *  @file
 *  Handle input via mouse & keyboard, events, schedule commands.
 */

#include "play_controller.hpp"
#include "dialogs.hpp"
#include "game_events.hpp"
#include "gettext.hpp"
#include "halo.hpp"
#include "loadscreen.hpp"
#include "log.hpp"
#include "resources.hpp"
#include "savegame.hpp"
#include "sound.hpp"
#include "unit_id.hpp"
#include "terrain_filter.hpp"
#include "save_blocker.hpp"
#include "preferences_display.hpp"
#include "replay.hpp"
#include "soundsource.hpp"
#include "tooltips.hpp"
#include "game_preferences.hpp"
#include "wml_exception.hpp"
#include "formula_string_utils.hpp"
#include "ai/manager.hpp"
#include "scripting/lua.hpp"
#include "artifical.hpp"
#include "unit_display.hpp"

#include "gui/dialogs/employ.hpp"
#include "gui/dialogs/camp_armory.hpp"
#include "gui/dialogs/transient_message.hpp"
#include "gui/dialogs/recruit.hpp"
#include "gui/dialogs/rpg_detail.hpp"
#include "gui/dialogs/treasure.hpp"
#include "gui/dialogs/select_unit.hpp"
#include "gui/dialogs/final_battle.hpp"
#include "gui/dialogs/exchange.hpp"
#include "gui/dialogs/independence.hpp"
#include "gui/widgets/window.hpp"

#include <boost/foreach.hpp>

static lg::log_domain log_engine("engine");
#define LOG_NG LOG_STREAM(info, log_engine)
#define DBG_NG LOG_STREAM(debug, log_engine)

static lg::log_domain log_display("display");
#define ERR_DP LOG_STREAM(err, log_display)

namespace null_val {
	const std::vector<map_location> vector_location;
}

double tower_cost_ratio = 9.0;

static void clear_resources()
{
	resources::game_map = NULL;
	resources::units = NULL;
	resources::heros = NULL;
	resources::teams = NULL;
	resources::state_of_game = NULL;
	resources::controller = NULL;
	resources::screen = NULL;
	resources::soundsources = NULL;
	resources::tod_manager = NULL;
	resources::persist = NULL;
}

play_controller::play_controller(const config& level, game_state& state_of_game, hero_map& heros, hero_map& heros_start,
		card_map& cards,
		int ticks, int num_turns, const config& game_config, CVideo& video,
		bool skip_replay, bool replaying) :
	controller_base(ticks, game_config, video),
	observer(),
	animation_cache(),
	prefs_disp_manager_(),
	tooltips_manager_(),
	events_manager_(),
	halo_manager_(),
	labels_manager_(),
	help_manager_(&game_config, &map_),
	mouse_handler_(NULL, teams_, units_, heros, map_, tod_manager_, undo_stack_),
	menu_handler_(NULL, units_, heros, cards, teams_, level, map_, game_config, tod_manager_, state_of_game, undo_stack_),
	soundsources_manager_(),
	tod_manager_(level, num_turns, &state_of_game),
	persist_(),
	gui_(),
	statistics_context_(level["name"]),
	level_(level),
	teams_(),
	gamestate_(state_of_game),
	map_(game_config, level["map_data"]),
	units_(),
	heros_(heros),
	heros_start_(heros_start),
	cards_(cards),
	undo_stack_(),
	xp_mod_(level["experience_modifier"].to_int(100)),
	loading_game_(level["playing_team"].empty() == false),
	first_human_team_(-1),
	player_number_(1),
	first_player_(level_["playing_team"].to_int() + 1),
	start_turn_(tod_manager_.turn()), // tod_manager_ constructed above
	is_host_(true),
	skip_replay_(skip_replay),
	replaying_(replaying),
	linger_(false),
	previous_turn_(0),
	savenames_(),
	wml_commands_(),
	victory_when_enemies_defeated_(true),
	end_level_data_(),
	victory_music_(),
	defeat_music_(),
	all_ai_allied_(level_["ai_allied"].to_bool()),
	final_capital_(std::make_pair<artifical*, artifical*>(NULL, NULL)),
	human_players_(0),
	rpging_(false),
	troops_cache_(NULL),
	troops_cache_vsize_(0),
	roads_(),
	fallen_to_unstage_(level_["fallen_to_unstage"].to_bool()),
	card_mode_(level_["card_mode"].to_bool()),
	duel_(RANDOM_DUEL),
	treasures_(), 
	emploies_()
{
	resources::game_map = &map_;
	resources::units = &units_;
	resources::heros = &heros_;
	resources::teams = &teams_;
	resources::state_of_game = &gamestate_;
	resources::controller = this;
	resources::tod_manager = &tod_manager_;
	resources::persist = &persist_;
	persist_.start_transaction();

	troops_cache_vsize_ = 40;
	troops_cache_ = (unit**)malloc(troops_cache_vsize_ * sizeof(unit*));

	// Setup victory and defeat music
	set_victory_music_list(level_["victory_music"]);
	set_defeat_music_list(level_["defeat_music"]);

	game_config::add_color_info(level);
	hotkey::deactivate_all_scopes();
	hotkey::set_scope_active(hotkey::SCOPE_GENERAL);
	hotkey::set_scope_active(hotkey::SCOPE_GAME);
	try {
		init(video);
	
		// Setup treasures
		std::vector<std::string> vstr = utils::split(level["treasures"]);
		const std::vector<ttreasure>& treasures = unit_types.treasures();
		for (std::vector<std::string>::const_iterator it = vstr.begin(); it != vstr.end(); ++ it) {
			int t = lexical_cast_default<int>(*it, -1);
			if (t < 0 || t >= (int)treasures.size()) {
				throw game::load_game_failed("treasures");
			}
			treasures_.push_back(t);
		}

		// emploies
		vstr = utils::split(level["emploies"]);
		for (std::vector<std::string>::const_iterator it = vstr.begin(); it != vstr.end(); ++ it) {
			int number = lexical_cast_default<int>(*it, HEROS_INVALID_NUMBER);
			if (number == HEROS_INVALID_NUMBER || number >= (int)heros_.size()) {
				throw game::load_game_failed("emploies, invalid number!");
			}
			hero& h = heros_[number];
			if (std::find(emploies_.begin(), emploies_.end(), &h) != emploies_.end()) {
				throw game::load_game_failed("emploies, duplicate number!");
			}
			emploies_.push_back(&h);
		}

		if (!level["final_capital"].empty()) {
			const std::vector<std::string> capitals = utils::split(level_["final_capital"]);
			final_capital_.first = units_.city_from_cityno(lexical_cast_default<int>(capitals[0]));
			final_capital_.second = units_.city_from_cityno(lexical_cast_default<int>(capitals[1]));
		}
		if (!level["maximal_defeated_activity"].empty()) {
			game_config::maximal_defeated_activity = level["maximal_defeated_activity"].to_int(100);
		}
		if (tent::duel == -1) {
			if (level.has_attribute("duel")) {
				const std::string& duel = level["duel"];
				if (duel == "no") {
					duel_ = NO_DUEL;
				} else if (duel == "always") {
					duel_ = ALWAYS_DUEL;
				} else {
					duel_ = RANDOM_DUEL;
				}
			}
		} else {
			duel_ = tent::duel;
			tent::duel = -1;
		}
	} catch (...) {
		clear_resources();
		throw;
	}
}

std::string vector_2_str(const std::vector<size_t>& vec)
{
	std::stringstream str;
	if (!vec.empty()) {
		bool first = true;
		str.str("");
		for (std::vector<size_t>::const_iterator i = vec.begin(); i != vec.end(); ++ i) {
			if (!first) {
				str << ",";
			} else {
				first = false;
			}
			str << *i;
		}
	}
	return str.str();
}

play_controller::~play_controller()
{
	// update player variables from human team.
	if (!gamestate_.get_variables().empty()) {
		// when load-game use menu command, gamestate_.get_variables() is empty. in this case don't update player variable.
		for (size_t side = 0; side != teams_.size(); side ++) {
			if (teams_[side].is_human()) {
				team& t = teams_[side];
				const std::vector<size_t>& holded_cards = t.holded_cards();
				gamestate_.set_variable("player.holded_cards", vector_2_str(holded_cards));

				gamestate_.set_variable("player.navigation", lexical_cast<std::string>(t.navigation()));
				gamestate_.set_variable("player.stratum", lexical_cast<std::string>(rpg::stratum));
				gamestate_.set_variable("player.forbids", lexical_cast<std::string>(rpg::forbids));

				gamestate_.write_armory_2_variable(t);
				break;
			}
		}
	}
	
	clear_resources();

	if (troops_cache_) {
		free(troops_cache_);
	}
}

void play_controller::init(CVideo& video)
{
	provoke_cache_ready = false;
	tactic_cache::ready = false;
	uint32_t start = SDL_GetTicks();

	util::scoped_resource<loadscreen::global_loadscreen_manager*, util::delete_item> scoped_loadscreen_manager;
	loadscreen::global_loadscreen_manager* loadscreen_manager = loadscreen::global_loadscreen_manager::get();
	if (!loadscreen_manager) {
		scoped_loadscreen_manager.assign(new loadscreen::global_loadscreen_manager(video));
		loadscreen_manager = scoped_loadscreen_manager.get();
	}

	loadscreen::start_stage("load level");
	// If the recorder has no event, adds an "game start" event
	// to the recorder, whose only goal is to initialize the RNG
	bool tent_valid = false;
	if (recorder.empty()) {
		recorder.add_start();
		tent_valid = true;
	} else {
		recorder.pre_replay();
	}
	recorder.set_skip(false);

	bool snapshot = level_["snapshot"].to_bool();

	BOOST_FOREACH (const config &t, level_.child_range("time_area")) {
		tod_manager_.add_time_area(t);
	}

	// en-clean unit_map
	units_.create_coor_map(resources::game_map->w(), resources::game_map->h());

	while (!replaying_ && level_.child_count("camp")) {
		std::vector<unit*> candidate_troops;
		std::vector<hero*> candidate_heros;

		config& player_cfg = gamestate_.get_variables().child("player");
		if (!player_cfg) {
			break;
		}
		tent::leader = &heros_[player_cfg["leader"].to_int()];

		config& armory_cfg = player_cfg.child("armory");
		if (!armory_cfg) {
			break;
		}
		// troops of armory
		BOOST_FOREACH (config &i, armory_cfg.child_range("unit")) {
			// i["cityno"] = 0;
			unit* u = new unit(units_, heros_, teams_, i); // ???
			u->new_turn();
			// u->heal_all();
			u->set_state(unit::STATE_SLOWED, false);
			u->set_state(unit::STATE_BROKEN, false);
			u->set_state(unit::STATE_POISONED, false);
			u->set_state(unit::STATE_PETRIFIED, false);
			if (u->packed()) {
				u->pack_to(NULL);
			}
			candidate_troops.push_back(u);
		}

		// heros of armory
		const std::vector<std::string> armory_heros = utils::split(armory_cfg["heros"]);
		for (std::vector<std::string>::const_iterator itor = armory_heros.begin(); itor != armory_heros.end(); ++ itor) {
			candidate_heros.push_back(&heros_[lexical_cast_default<int>(*itor)]);
		}

		gui2::tcamp_armory dlg(candidate_troops, candidate_heros);
		dlg.show(video);
		int res = dlg.get_retval();
		if (res == gui2::twindow::CANCEL) {
			// release resource
			for (std::vector<unit*>::iterator itor = candidate_troops.begin(); itor != candidate_troops.end(); ++ itor) {
				delete *itor;
			}
			candidate_troops.clear();
			candidate_heros.clear();
			throw game::load_game_failed("init");
		}
		// save result to [player].[armory]
		std::stringstream armory_heros_str;

		armory_cfg.clear_children("unit");

		for (std::vector<unit*>::iterator itor = candidate_troops.begin(); itor != candidate_troops.end(); ++ itor) {
			config& u = armory_cfg.add_child("unit");
			(*itor)->get_location().write(u);
			(*itor)->write(u);
		}

		for (std::vector<hero*>::iterator h = candidate_heros.begin(); h != candidate_heros.end(); ++ h) {
			if (!armory_heros_str.str().empty()) {
				armory_heros_str << ", ";
			}
			armory_heros_str << lexical_cast<std::string>((*h)->number_);
		}
		armory_cfg["heros"] = armory_heros_str.str();

		// (support replay)save result to starting_pos. it is sure that there is armory block in starting_pos(reference play_game in playcampaign.cpp).
		// gui2::tcamp_armory may change [armory], write changed [armory] to starting_pos.
		config& player_cfg_dst = gamestate_.starting_pos.child("variables").child("player");
		player_cfg_dst.clear_children("armory");

		player_cfg_dst.add_child("armory", armory_cfg);

		// release resource
		for (std::vector<unit*>::iterator itor = candidate_troops.begin(); itor != candidate_troops.end(); ++ itor) {
			delete *itor;
		}
		candidate_troops.clear();
		candidate_heros.clear();
		break;
	}

	// initialized teams...
	loadscreen::start_stage("init teams");

	// This *needs* to be created before the show_intro and show_map_scene
	// as that functions use the manager state_of_game
	// Has to be done before registering any events!
	events_manager_.reset(new game_events::manager(level_));

	// if restart, rpg::h's memory maybe change. reset.
	rpg::h = &hero_invalid;
	rpg::stratum = hero_stratum_wander;
	rpg::forbids = 0;
	team::empty_side_ = -1;

	tent::mode = NONE_MODE;
	if (gamestate_.classification().mode == "rpg") {
		tent::mode = RPG_MODE;
	} else if (gamestate_.classification().mode == "tower") {
		tent::mode = TOWER_MODE;
	}
	if (tent::mode != TOWER_MODE) {
		increase_xp::ability_per_xp = 1;
		increase_xp::arms_per_xp = 3;
		increase_xp::skill_per_xp = 16;
		increase_xp::meritorious_per_xp = 5;
		increase_xp::navigation_per_xp = 2;
	} else {
		increase_xp::ability_per_xp = 3;
		increase_xp::arms_per_xp = 9;
		increase_xp::skill_per_xp = 48;
		increase_xp::meritorious_per_xp = 15;
		increase_xp::navigation_per_xp = 6;
	}
	if (level_["modify_placing"].to_bool()) {
		// modifying placing...
		place_sides_in_preferred_locations();
	}

	config& player_cfg = gamestate_.get_variables().child("player");
	if (!player_cfg || !player_cfg.has_attribute("hero")) {
		rpg::stratum = hero_stratum_leader;
	} else {
		rpg::stratum = player_cfg["stratum"].to_int();
	}

	int team_num = 0;
	size_t team_size = 0;
	config side_cfg;
	if (level_.child_count("side")) {
		// calculate count of team.
		BOOST_FOREACH (const config &side, level_.child_range("side")) {
			if (side["controller"].str() == "null") {
				team_size ++;
			}
		}
		VALIDATE(team_size <= 1, "play_controller::init, there is at most one null side in scenario");

		if (team_size) {
			team_size = level_.child_count("side");
		} else {
			team_size = level_.child_count("side") + 1;
		}
		if (rpg::stratum != hero_stratum_leader) {
			team_size ++;
		}

		// [side] ---> class team
		BOOST_FOREACH (const config &side, level_.child_range("side")) {
			side_cfg = side;
			game_events::wml_expand_if(side_cfg);
			if (first_human_team_ == -1) {
				const std::string &controller = side_cfg["controller"];
				if (controller == preferences::client_type() &&
					side_cfg["id"] == preferences::login()) {
					first_human_team_ = team_num;
				} else if (controller == "human") {
					first_human_team_ = team_num;
				}
			}
			gamestate_.build_team(side_cfg, teams_, level_, map_, units_, heros_, cards_, snapshot, team_size);
			if (teams_[team_num].is_human()) {
				human_players_ ++;
			}
			++ team_num;
		}
		// create autonomy team if necessary.
		if (team::empty_side_ == -1) {
			side_cfg.clear();
			side_cfg["side"] = team_num + 1;
			side_cfg["controller"] = "null";
			gamestate_.build_team(side_cfg, teams_, level_, map_, units_, heros_, cards_, snapshot, team_size);
			++ team_num;
		}
	} else {
		unit_segment2* sides = (unit_segment2*)unit::savegame_cache_;
		int offset = sizeof(unit_segment2);
		for (uint32_t i = 0; i < sides->count_; i ++) {
			gamestate_.build_team(unit::savegame_cache_ + offset, teams_, level_, map_, units_, heros_, cards_, snapshot);
			offset += ((unit_segment2*)(unit::savegame_cache_ + offset))->size_;

			if (teams_[team_num].is_human()) {
				if (first_human_team_ == -1) {
					first_human_team_ = team_num;
				}
				human_players_ ++;
			}
			++ team_num;
		}
	}
	if (first_human_team_ == -1) {
		VALIDATE(false, "Must define one human team at least.");
	}

	if (!player_cfg || !player_cfg.has_attribute("hero")) {
		team& human = teams_[first_human_team_];
		rpg::h = &heros_[human.leader()->number_];
		rpg::forbids = 0;
		
	} else {
		rpg::h = &heros_[player_cfg["hero"].to_int()];
		rpg::forbids = player_cfg["forbids"].to_int();
		if (rpg::h->status_ == hero_status_unstage) {
			// this is player hero. allocat it to proper city.
			hero& city_h = heros_[player_cfg["city"].to_int()];
			artifical* city = units_.city_from_cityno(city_h.city_);
			city->fresh_into(*rpg::h);
		}
	}

	if (level_.child_count("side")) {
		// if select rpg, create 
		if (rpg::stratum != hero_stratum_leader) {
			side_cfg.clear();
			side_cfg["side"] = (int)teams_.size() + 1;
			side_cfg["controller"] = "null";
			side_cfg["leader"] = rpg::h->number_;
			int original = rpg::h->official_;
			gamestate_.build_team(side_cfg, teams_, level_, map_, units_, heros_, cards_, snapshot, team_size);
			rpg::h->official_ = original;
		}
		// if necessary, set begin fields troop to human.
		team& current_team = teams_[rpg::h->side_];
		const std::pair<unit**, size_t> p = current_team.field_troop();
		unit** troops = p.first;
		size_t troops_vsize = p.second;
		for (size_t i = 0; i < troops_vsize; i ++) {
			if (troops[i]->is_artifical()) {
				continue;
			}
			if (rpg::stratum == hero_stratum_leader) {
				troops[i]->set_human(true);
			} else if (rpg::stratum == hero_stratum_mayor) {
				if (troops[i]->cityno() == rpg::h->city_) {
					troops[i]->set_human(true);
				}
			} else if (rpg::stratum == hero_stratum_citizen) {
				if (&troops[i]->master() == rpg::h || &troops[i]->second() == rpg::h || &troops[i]->third() == rpg::h) {
					troops[i]->set_human(true);
				}
			}
		}
	}

	if (level_.has_attribute("employ_count") || level_.has_attribute("ai_count")) {
		int random = get_random();
		std::set<int> candidate;
		for (hero_map::iterator it = heros_.begin(); it != heros_.end(); ++ it) {
			hero& h = *it;
			if (hero::is_commoner(h.number_) || hero::is_system(h.number_)) {
				continue;
			}
			if (h.status_ == hero_status_unstage && h.gender_ != hero_gender_neutral && !h.get_flag(hero_flag_robber)) {
				candidate.insert(h.number_);
			}
		}
		int employ_count = level_["employ_count"].to_int();
		for (std::vector<team>::iterator it = teams_.begin(); it != teams_.end(); ++ it) {
			team& t = *it;
			if (!t.is_ai()) continue;
			fill_employ_hero(candidate, employ_count, random);
		}
	
		int ai_count = level_["ai_count"].to_int();
		for (std::vector<team>::iterator it = teams_.begin(); it != teams_.end(); ++ it) {
			team& t = *it;
			if (!t.is_ai()) continue;
			std::vector<artifical*>& holded = t.holded_cities();
			for (std::vector<artifical*>::iterator it2 = holded.begin(); it2 != holded.end(); ++ it2) {
				artifical& c = **it2;
				c.fill_ai_hero(candidate, ai_count, random);
				std::vector<hero*>& freshes = c.fresh_heros();
				if (t.leader()->number_ == hero::number_empty_leader && !freshes.empty()) {
					hero* l = c.select_leader(random);
					if (l) {
						t.set_leader(l);
					}
				}
			}
		}
	}

	str_2_provoke_cache(units_, heros_, level_["provoke_cache"]);
	tactic_cache::str_2_cache(units_, heros_, level_["tactic_cache"]);

	// mouse_handler expects at least one team for linger mode to work.
	if (teams_.empty()) end_level_data_.linger_mode = false;

	// loading units...
	loadscreen::start_stage("load units");
	preferences::encounter_recruitable_units(teams_);
	preferences::encounter_start_units(units_);
	preferences::encounter_recallable_units(teams_);
	preferences::encounter_map_terrain(map_);

	// initializing theme...
	loadscreen::start_stage("init theme");
	const config &theme_cfg = get_theme(game_config_, level_["theme"]);

	// building terrain rules...
	loadscreen::start_stage("build terrain");
	gui_.reset(new game_display(units_, this, video, map_, tod_manager_, teams_, theme_cfg, level_));
	if (!gui_->video().faked()) {
		if (gamestate_.mp_settings().mp_countdown)
			gui_->get_theme().modify_label("time-icon", _ ("time left for current turn"));
		else
			gui_->get_theme().modify_label("time-icon", _ ("current local time"));
	}

	loadscreen::start_stage("init display");
	mouse_handler_.set_gui(gui_.get());
	menu_handler_.set_gui(gui_.get());
	resources::screen = gui_.get();
	theme::set_known_themes(&game_config_);

	// done initializing display...
	if (first_human_team_ != -1) {
		gui_->set_team(first_human_team_);
		if (tent::mode == TOWER_MODE) {
			std::vector<artifical*>& cities = teams_[first_human_team_].holded_cities();
			for (std::vector<artifical*>::iterator it = cities.begin(); it != cities.end(); ++ it) {
				artifical& city = **it;
				if (!city.mayor()->valid()) {
					city.select_mayor(NULL, false);
				}
			}
		}

	} else if (is_observer()) {
		// Find first team that is allowed to be observered.
		// If not set here observer would be without fog untill
		// the first turn of observerable side
		size_t i;
		for (i=0;i < teams_.size();++i) {
			if (!teams_[i].get_disallow_observers()) {
				gui_->set_team(i);
			}
		}
	}

	browse_ = true;

	init_managers();
	// add era events for MP game
	if (const config &era_cfg = level_.child("era")) {
		game_events::add_events(era_cfg.child_range("event"), "era_events");
	}

	loadscreen::global_loadscreen->start_stage("draw theme");

	// construct roads
	construct_road();

	uint32_t stop = SDL_GetTicks();
	posix_print("play_controller::init, used time: %u ms\n", stop - start);
}

void play_controller::init_managers()
{
	LOG_NG << "initializing managers... " << (SDL_GetTicks() - ticks_) << "\n";
	prefs_disp_manager_.reset(new preferences::display_manager(gui_.get()));
	tooltips_manager_.reset(new tooltips::manager(gui_->video()));
	soundsources_manager_.reset(new soundsource::manager(*gui_));

	resources::soundsources = soundsources_manager_.get();

	halo_manager_.reset(new halo::manager(*gui_));
	LOG_NG << "done initializing managers... " << (SDL_GetTicks() - ticks_) << "\n";
}

static int placing_score(int mode, const config& side, const gamemap& map, const map_location& pos)
{
	int positions = 0, liked = 0;
	const t_translation::t_list terrain = t_translation::read_list(side["terrain_liked"]);

	for(int i = pos.x-8; i != pos.x+8; ++i) {
		for(int j = pos.y-8; j != pos.y+8; ++j) {
			const map_location pos(i,j);
			if(map.on_board(pos)) {
				++positions;
				if(std::count(terrain.begin(),terrain.end(),map[pos])) {
					++liked;
				}
			}
		}
	}

	int mode_bonus = 0;
	if (mode == TOWER_MODE) {
		// ensure side#1 to right, side#2 to left
		int s = side["side"].to_int();
		if (s == 1) {
			mode_bonus = pos.x * 100;
		} else {
			mode_bonus = (map.w() - pos.x) * 100;
		}
	}

	return (100*liked)/positions + mode_bonus;
}

struct placing_info {

	placing_info() :
		side(0),
		score(0),
		pos()
	{
	}

	int side, score;
	map_location pos;
};

static bool operator<(const placing_info& a, const placing_info& b) { return a.score > b.score; }

void play_controller::place_sides_in_preferred_locations()
{
	std::vector<placing_info> placings;

	int num_pos = map_.num_valid_starting_positions();

	int side_num = 1;
	BOOST_FOREACH (const config &side, level_.child_range("side"))
	{
		for(int p = 1; p <= num_pos; ++p) {
			const map_location& pos = map_.starting_position(p);
			int score = placing_score(tent::mode, side, map_, pos);
			placing_info obj;
			obj.side = side_num;
			obj.score = score;
			obj.pos = pos;
			placings.push_back(obj);
		}
		++side_num;
	}

	std::sort(placings.begin(),placings.end());
	std::set<int> placed;
	std::set<map_location> positions_taken;

	for (std::vector<placing_info>::const_iterator i = placings.begin(); i != placings.end() && int(placed.size()) != side_num - 1; ++i) {
		if(placed.count(i->side) == 0 && positions_taken.count(i->pos) == 0) {
			placed.insert(i->side);
			positions_taken.insert(i->pos);
			map_.set_starting_position(i->side,i->pos);
			LOG_NG << "placing side " << i->side << " at " << i->pos << '\n';
		}
	}
}

void play_controller::objectives(){
	menu_handler_.objectives(gui_->viewing_team()+1);
}

void play_controller::show_statistics(){
	menu_handler_.show_statistics(gui_->viewing_team() + 1);
}

void play_controller::show_rpg()
{
	if (tent::mode == TOWER_MODE || rpg::h->status_ == hero_status_wander) {
		return;
	}
	rpging_ = true;
	gui_->goto_main_context_menu();
	rpging_ = false;
}

void play_controller::rpg_detail() 
{
	gui2::trpg_detail dlg(*gui_, teams_, units_, heros_);
	try {
		dlg.show(gui_->video());
	} catch(twml_exception& e) {
		e.show(*gui_);
		return;
	}
}

void play_controller::assemble_treasure() 
{
	team& rpg_team = teams_[rpg::h->side_];
	std::vector<std::pair<size_t, unit*> > human_pairs;
	std::vector<int> original_holded;
		
	std::vector<artifical*>& holded_cities = rpg_team.holded_cities();
	for (std::vector<artifical*>::iterator it = holded_cities.begin(); it != holded_cities.end(); ++ it) {
		artifical* city = *it;

		std::vector<unit*>& resides = city->reside_troops();
		for (std::vector<unit*>::iterator it2 = resides.begin(); it2 != resides.end(); ++ it2) {
			unit* u = *it2;
			if (!u->human()) {
				continue;
			}
			human_pairs.push_back(std::make_pair<size_t, unit*>(u->master().number_, u));
			if (u->second().valid()) {
				human_pairs.push_back(std::make_pair<size_t, unit*>(u->second().number_, u));
			}
			if (u->third().valid()) {
				human_pairs.push_back(std::make_pair<size_t, unit*>(u->third().number_, u));
			}
		}
/*
		std::vector<unit*>& fields = city->field_troops();
		for (std::vector<unit*>::iterator it2 = fields.begin(); it2 != fields.end(); ++ it2) {
			unit* u = *it2;
			if (!u->human()) {
				continue;
			}
			human_pairs.push_back(std::make_pair<size_t, unit*>(u->master().number_, u));
			if (u->second().valid()) {
				human_pairs.push_back(std::make_pair<size_t, unit*>(u->second().number_, u));
			}
			if (u->third().valid()) {
				human_pairs.push_back(std::make_pair<size_t, unit*>(u->third().number_, u));
			}
		}
*/
		std::vector<hero*>& freshes = city->fresh_heros();
		std::vector<hero*>& finishes = city->finish_heros();
		if (city->mayor() == rpg::h || rpg::stratum == hero_stratum_leader) {
			for (std::vector<hero*>::iterator it2 = freshes.begin(); it2 != freshes.end(); ++ it2) {
				hero& h = **it2;
				human_pairs.push_back(std::make_pair<size_t, unit*>(h.number_, city));
			}
			for (std::vector<hero*>::iterator it2 = finishes.begin(); it2 != finishes.end(); ++ it2) {
				hero& h = **it2;
				human_pairs.push_back(std::make_pair<size_t, unit*>(h.number_, city));
			}
		}
		if (rpg::stratum == hero_stratum_citizen) {
			if (std::find(freshes.begin(), freshes.end(), rpg::h) != freshes.end()) {
				human_pairs.push_back(std::make_pair<size_t, unit*>(rpg::h->number_, city));
			}
			if (std::find(finishes.begin(), finishes.end(), rpg::h) != finishes.end()) {
				human_pairs.push_back(std::make_pair<size_t, unit*>(rpg::h->number_, city));
			}
		}
	}
	if (human_pairs.empty()) {
		gui2::show_transient_message(gui_->video(), "", _("There are no hero available to assemble treasure."));
		return;
	}

	for (std::vector<std::pair<size_t, unit*> >::iterator it = human_pairs.begin(); it != human_pairs.end(); ++ it) {
		const hero& h = heros_[it->first];
		original_holded.push_back(h.treasure_);
	}
	
	std::map<int, int> diff;
	std::set<unit*> diff_troops;
	{
		gui2::ttreasure dlg(teams_, units_, heros_, human_pairs, replaying_);
		try {
			dlg.show(gui_->video());
		} catch(twml_exception& e) {
			e.show(*gui_);
			return;
		}
		int index = 0;
		for (std::vector<std::pair<size_t, unit*> >::iterator it = human_pairs.begin(); it != human_pairs.end(); ++ it, index ++) {
			const int orig = original_holded[index];
			const hero& h = heros_[it->first];

			if (orig != h.treasure_) {
				diff[h.number_] = h.treasure_;
				if (!it->second->is_artifical()) {
					diff_troops.insert(it->second);
				}
			}
		}
	}

	for (std::set<unit*>::iterator it = diff_troops.begin(); it != diff_troops.end(); ++ it) {
		unit& u = **it;
		u.adjust();
	}

	if (!diff.empty()) {
		recorder.add_assemble_treasure(diff);
	}
}

void play_controller::rpg_exchange(const std::vector<size_t>& human_p, size_t ai_p) 
{
	team& rpg_team = teams_[rpg::h->side_];
	std::vector<std::pair<size_t, unit*> > human_pairs;
	std::vector<std::pair<size_t, unit*> > ai_pairs;
	std::vector<std::pair<size_t, unit*> >* tmp_pairs;

	std::vector<artifical*>& holded_cities = rpg_team.holded_cities();
	for (std::vector<artifical*>::iterator it = holded_cities.begin(); it != holded_cities.end(); ++ it) {
		artifical* city = *it;

		std::vector<unit*>& resides = city->reside_troops();
		for (std::vector<unit*>::iterator it2 = resides.begin(); it2 != resides.end(); ++ it2) {
			unit* u = *it2;
			if (u->human()) {
				tmp_pairs = &human_pairs;
			} else {
				tmp_pairs = &ai_pairs;
			}
			tmp_pairs->push_back(std::make_pair<size_t, unit*>(u->master().number_, u));
			if (u->second().valid()) {
				tmp_pairs->push_back(std::make_pair<size_t, unit*>(u->second().number_, u));
			}
			if (u->third().valid()) {
				tmp_pairs->push_back(std::make_pair<size_t, unit*>(u->third().number_, u));
			}
		}
		std::vector<unit*>& fields = city->field_troops();
		for (std::vector<unit*>::iterator it2 = fields.begin(); it2 != fields.end(); ++ it2) {
			unit* u = *it2;
			if (u->human()) {
				tmp_pairs = &human_pairs;
			} else {
				tmp_pairs = &ai_pairs;
			}
			tmp_pairs->push_back(std::make_pair<size_t, unit*>(u->master().number_, u));
			if (u->second().valid()) {
				tmp_pairs->push_back(std::make_pair<size_t, unit*>(u->second().number_, u));
			}
			if (u->third().valid()) {
				tmp_pairs->push_back(std::make_pair<size_t, unit*>(u->third().number_, u));
			}
		}
		if (city->mayor() == rpg::h) {
			tmp_pairs = &human_pairs;
		} else {
			tmp_pairs = &ai_pairs;
		}
		std::vector<hero*>& freshes = city->fresh_heros();
		for (std::vector<hero*>::iterator it2 = freshes.begin(); it2 != freshes.end(); ++ it2) {
			hero& h = **it2;
			tmp_pairs->push_back(std::make_pair<size_t, unit*>(h.number_, city));
		}
		std::vector<hero*>& finishes = city->finish_heros();
		for (std::vector<hero*>::iterator it2 = finishes.begin(); it2 != finishes.end(); ++ it2) {
			hero& h = **it2;
			tmp_pairs->push_back(std::make_pair<size_t, unit*>(h.number_, city));
		}
	}

	std::set<size_t> checked_human;
	size_t checked_ai;
	if (human_p.empty()) {
		gui2::texchange dlg(teams_, units_, heros_, human_pairs, ai_pairs);
		try {
			dlg.show(gui_->video());
		} catch(twml_exception& e) {
			e.show(*gui_);
			return;
		}
		if (dlg.get_retval() != gui2::twindow::OK) {
			return;
		}
		checked_human = dlg.checked_human_pairs();
		checked_ai = dlg.checked_ai_pairs();
	} else {
		for (std::vector<size_t>::const_iterator it = human_p.begin(); it != human_p.end(); ++ it) {
			checked_human.insert(*it);
		}
		checked_ai = ai_p;
	}

	std::vector<hero*> human_heros;
	hero* ai_hero = NULL;
	std::vector<size_t> checked;
	for (std::set<size_t>::const_iterator it = checked_human.begin(); it != checked_human.end(); ++ it) {
		checked.push_back(*it);
	}
	for (int i = 0; i < 4; i ++) {
		std::pair<size_t, unit*>* pair;
		if (i < 3) {
			pair = &human_pairs[checked[i]];
		} else {
			pair = &ai_pairs[checked_ai];
		}
		unit* u = pair->second;
		hero& h = heros_[pair->first];
		if (i < 3) {
			human_heros.push_back(&h);
		} else {
			ai_hero = &h;
		}

		std::vector<hero*> captains;
		if (artifical* city = units_.city_from_loc(u->get_location())) {
			// fresh/finish/reside troop
			if (u->is_city()) {
				for (int type = 0; type < 3; type ++) { 
					std::vector<hero*>* list;
					if (type == 0) {
						list = &city->fresh_heros();
					} else if (type == 1) {
						list = &city->finish_heros();
					} else {
						list = &city->wander_heros();
					}
					std::vector<hero*>::iterator i2 = std::find(list->begin(), list->end(), &h);
					if (i2 != list->end()) {
						list->erase(i2);
						break;
					}
				}
			} else {
				std::vector<unit*>& reside_troops = city->reside_troops();
				int index = 0;
				for (std::vector<unit*>::iterator i2 = reside_troops.begin(); i2 != reside_troops.end(); ++ i2, index ++) {
					if (*i2 != u) {
						continue;
					}
					u->replace_captains_internal(h, captains);
					if (captains.empty()) {
						city->troop_go_out(index);
					} else {
						u->replace_captains(captains);
					}
					break;
				}
			}
		} else {
			// field troop
			u->replace_captains_internal(h, captains);
			if (captains.empty()) {
				units_.erase(u);
			} else {
				u->replace_captains(captains);
			}
		}
	}

	std::stringstream str;
	utils::string_map symbols;
	for (std::vector<hero*>::iterator it = human_heros.begin(); it != human_heros.end(); ++ it) {
		hero& h = **it;
		if (it != human_heros.begin()) {
			str << ", ";
		}
		str << help::tintegrate::generate_format(h.name(), help::tintegrate::hero_color);
	}
	symbols["human"] = str.str();
	symbols["ai"] = help::tintegrate::generate_format(ai_hero->name(), help::tintegrate::hero_color);
	game_events::show_hero_message(rpg::h, NULL, vgettext("You exchanged $ai using $human.", symbols), game_events::INCIDENT_INVALID);


	// ai to human city
	artifical* city = units_.city_from_cityno(rpg::h->city_);
	city->move_into(*ai_hero);

	// human to ai city
	if (!human_heros.empty()) {
		std::vector<mr_data> mrs;
		units_.calculate_mrs_data(mrs, rpg::h->side_ + 1, false);
		artifical* aggressing = mrs[0].own_front_cities[0];

		for (std::vector<hero*>::iterator it = human_heros.begin(); it != human_heros.end(); ++ it) {
			hero& h = **it;
			aggressing->move_into(h);
		}
	}

	if (human_p.empty()) {
		recorder.add_rpg_exchange(checked_human, checked_ai);
	}
}

void play_controller::rpg_independence(bool replaying) 
{
	int from_side = rpg::h->side_ + 1;
	std::vector<mr_data> mrs;
	units_.calculate_mrs_data(mrs, from_side, false);
	artifical* aggressing = mrs[0].own_front_cities[0];

	if (!replaying) {
		int side_num = gui_->viewing_team() + 1;

		gui2::tindependence dlg(*gui_, teams_, units_, heros_, aggressing);
		try {
			dlg.show(gui_->video());
		} catch(twml_exception& e) {
			e.show(*gui_);
			return;
		}
		if (dlg.get_retval() != gui2::twindow::OK) {
			return;
		}
	}

	artifical* rpg_city = units_.city_from_cityno(rpg::h->city_);
	team& from_team = teams_[rpg::h->side_];
	std::string message;
	utils::string_map symbols;
	
	symbols["first"] = help::tintegrate::generate_format(rpg::h->name(), help::tintegrate::hero_color);
	symbols["second"] = help::tintegrate::generate_format(rpg_city->name(), help::tintegrate::hero_color);
	game_events::show_hero_message(rpg::h, rpg_city, vgettext("$first declared independence.", symbols), game_events::INCIDENT_INDEPENDENCE);

	if (from_team.defeat_vote()) {
		aggressing = NULL;

		symbols["first"] = help::tintegrate::generate_format(from_team.name(), help::tintegrate::hero_color);
		game_events::show_hero_message(&heros_[hero::number_scout], NULL, vgettext("$first is defeated.", symbols), game_events::INCIDENT_DEFEAT);
	}
	hero* from_leader = from_team.leader();
	unit* from_leader_unit = find_unit(units_, *from_leader);
	
	// if from_lead and rpg::h is in same troop, disband this troop.
	if (from_leader_unit) {
		bool same = false;
		if (&from_leader_unit->master() == rpg::h) {
			same = true;
		}
		if (!same && from_leader_unit->second().valid() && &from_leader_unit->second() == rpg::h) {
			same = true;
		}
		if (!same && from_leader_unit->third().valid() && &from_leader_unit->third() == rpg::h) {
			same = true;
		}
		if (same && aggressing) {
			if (rpg_city->get_location() == from_leader_unit->get_location()) {
				// reside troop
				std::vector<unit*>& reside_troops = rpg_city->reside_troops();
				int index = 0;
				for (std::vector<unit*>::iterator i2 = reside_troops.begin(); i2 != reside_troops.end(); ++ i2, index ++) {
					if (*i2 != from_leader_unit) {
						continue;
					}
					rpg_city->troop_go_out(index);
					break;
				}
			} else {
				// field troop
				units_.erase(from_leader_unit);
			}
			rpg_city->fresh_into(from_leader_unit);
		}
	}

	// change in from_side
	from_team.rpg_changed(true);
	from_team.change_controller(team::team_info::AI);
	from_team.set_base_income(150);

	// change in to_side
	team& to_team = teams_.back();
	int to_side = to_team.side();
	to_team.change_controller(team::team_info::HUMAN);
	// reset float_catalog equal to base_catalog_
	rpg::h->float_catalog_ = ftofxp8(rpg::h->base_catalog_);

	// select_mayor will change rpg stratum, first callit.
	rpg_city->select_mayor(&hero_invalid);
	// change rpg stratum to hero_stratum_leader.
	rpg::stratum = hero_stratum_leader;
	// artifical::troop_come_into/unit_belong_to need correct rpg::h->side_ to decide set_human.
	rpg::h->side_ = to_side - 1;
	rpg::h->official_ = hero_official_leader;
	if (!aggressing) {
		from_leader->official_ = HEROS_NO_OFFICIAL;
	}

	std::vector<artifical*> holded_cities = from_team.holded_cities();
	std::set<int> independenced_cities, undependenced_cities;
	for (std::vector<artifical*>::iterator it = holded_cities.begin(); it != holded_cities.end(); ++ it) {
		artifical* city = *it;
		bool independenced = aggressing? false: true;
		if (!independenced) {
			independenced = city->independence_vote(aggressing);
		}
		if (independenced) {
			independenced_cities.insert(city->cityno());
		} else {
			undependenced_cities.insert(city->cityno());
		}
	}

	// navigation
	int navigation = from_team.navigation();
	to_team.set_navigation(navigation * independenced_cities.size() / (independenced_cities.size() + undependenced_cities.size()));

	for (std::vector<artifical*>::iterator it = holded_cities.begin(); it != holded_cities.end(); ++ it) {
		artifical* city = *it;
		bool independenced = aggressing? false: true;
		if (!independenced) {
			independenced = independenced_cities.find(city->cityno()) != independenced_cities.end();
		}
		city->independence(independenced, to_team, rpg_city, from_team, aggressing, from_leader, from_leader_unit);
		if (independenced && city != rpg_city) {
			symbols["first"] = help::tintegrate::generate_format(city->name(), help::tintegrate::object_color);
			symbols["second"] = help::tintegrate::generate_format(rpg::h->name(), help::tintegrate::hero_color);
			game_events::show_hero_message(&heros_[hero::number_scout], city, vgettext("$first declared belonging to $second.", symbols), game_events::INCIDENT_INVALID);
		}
	}

	std::set<std::string> type_ids;
	const std::set<const unit_type*>& can_build = from_team.builds();
	for (std::set<const unit_type*>::const_iterator cr = can_build.begin(); cr != can_build.end(); ++ cr) {
		type_ids.insert((*cr)->id());
	}
	to_team.set_builds(type_ids);

	// shourd/fog
	if (from_team.uses_shroud()) {
		from_team.set_shroud(false);
		to_team.set_shroud(true);
	}
	if (from_team.uses_fog()) {
		from_team.set_fog(false);
		to_team.set_fog(true);
	}

	// card
	std::vector<size_t>& candidate_cards = to_team.candidate_cards(); 
	candidate_cards = from_team.candidate_cards();
	from_team.candidate_cards().clear();
	std::vector<size_t>& holded_cards = to_team.holded_cards();
	holded_cards = from_team.holded_cards();
	from_team.holded_cards().clear();

	// treasure
	to_team.holded_treasures() = from_team.holded_treasures();
	from_team.holded_treasures().clear();

	// technology
	to_team.holded_technologies() = from_team.holded_technologies();
	to_team.half_technologies() = from_team.half_technologies();
	to_team.apply_holded_technologies_finish();
	to_team.apply_holded_technologies_modify();

	// statistic
	to_team.cause_damage_ = from_team.cause_damage_;
	to_team.been_damage_ = from_team.been_damage_;
	to_team.defeat_units_ = from_team.defeat_units_;

	// villages
	std::set<map_location> villages = from_team.villages();
	from_team.clear_villages();
	for (std::set<map_location>::const_iterator it = villages.begin(); it != villages.end(); ++ it) {
		if (rand() % (independenced_cities.size() + undependenced_cities.size()) < independenced_cities.size()) {
			to_team.get_village(*it);
		} else {
			from_team.get_village(*it);
		}
	}

	if (!replaying) {
		recorder.add_event(replay::EVENT_RPG_INDEPENDENCE, map_location());

		gui_->hide_context_menu(NULL, true);
		end_turn();
	}
}

void play_controller::interior()
{
	menu_handler_.interior(gui_->viewing_team() + 1);
}

void play_controller::technology_tree()
{
	menu_handler_.technology_tree(gui_->viewing_team() + 1);
}

void play_controller::employ()
{
	if (emploies_.empty()) {
		gui2::show_transient_message(gui_->video(), "", _("There is no employable hero."));
		return;
	}
	double ratio = 2.0;
	gui2::temploy dlg(*gui_, teams_, units_, heros_, *this, emploies_, gui_->viewing_team() + 1, ratio, replaying_);
	try {
		dlg.show(gui_->video());
	} catch(twml_exception& e) {
		e.show(*gui_);
		return;
	}
	if (dlg.get_retval() != gui2::twindow::OK) {
		return;
	}
	team& current_team = teams_[gui_->viewing_team()];
	hero& h = *emploies_[dlg.get_cursel()];
	int cost = h.cost_ * ratio;
	do_employ(*this, units_, current_team, h, cost, false);

	show_context_menu(NULL, *gui_);
}

void play_controller::final_battle(int side_num, int human_capital, int ai_capital)
{
	// menu_handler_.final_battle(gui_->viewing_team()+1, human_capital, ai_capital);
	if (side_num == -1) {
		side_num = gui_->viewing_team() + 1;
	}
	artifical* capital_of_human;
	artifical* capital_of_ai;

	if (final_capital().first) {
		utils::string_map symbols;
		symbols["first"] = final_capital().first->master().name();
		symbols["second"] = final_capital().second? final_capital().second->master().name(): dgettext("wesnoth-lib", "Void");
		gui2::show_transient_message(gui_->video(), _("final battle"), vgettext("human capital: $first\n\n\n\nai capital: $second", symbols));
		return;
	}

	if (human_capital < 0) {
		// popup dialog, let player select capital
		std::vector<artifical*>& human_holded_cities = teams_[side_num - 1].holded_cities();
		if (human_holded_cities.size() >= 1) {

			gui2::tfinal_battle dlg(*gui_, teams_, units_, heros_, side_num);
			try {
				dlg.show(gui_->video());
			} catch(twml_exception& e) {
				e.show(*gui_);
				return;
			}
			if (dlg.get_retval() != gui2::twindow::OK) {
				return;
			}
			capital_of_human = dlg.dst_city();

		} else if (human_holded_cities.size() == 1) {
			capital_of_human = human_holded_cities[0];
		} else {
			return;
		}
	} else {
		capital_of_human = units_.city_from_cityno(human_capital);
	}

	if (!all_ai_allied()) {
		ally_all_ai(true);
	}

	if (ai_capital < 0) {
		capital_of_ai = decide_ai_capital();
	} else {
		capital_of_ai = units_.city_from_cityno(ai_capital);
	}
	set_final_capital(capital_of_human, capital_of_ai);

	utils::string_map symbols;
	symbols["first"] = final_capital().first->master().name();
	symbols["second"] = final_capital().second? final_capital().second->master().name(): dgettext("wesnoth-lib", "Void");
	gui2::show_transient_message(gui_->video(), _("final battle"), vgettext("human capital: $first\n\n\n\nai capital: $second", symbols));

	if (human_capital < 0) {
		recorder.add_final_battle(capital_of_human, capital_of_ai);
	}
}

void play_controller::list()
{
	menu_handler_.list(gui_->viewing_team() + 1);
}

void play_controller::system()
{
	menu_handler_.system(gui_->viewing_team() + 1);
}

void play_controller::remove_active_tactic(int slot)
{
	const team& current_team = teams_[gui_->viewing_team()];
	std::vector<unit*> actives = current_team.active_tactics();
	if (slot < 0 || slot >= (int)actives.size()) {
		return;
	}
	unit* u = actives[slot];
	do_remove_active_tactic(*u, true);
}

void play_controller::bomb()
{
	team& current_team = teams_[gui_->viewing_team()];
	mouse_handler_.set_card_playing(current_team, -1);
}

void play_controller::switch_list()
{
	menu_handler_.switch_list(gui_->viewing_team() + 1);
}

void play_controller::unit_detail()
{
	unit_map::iterator u_itor = find_visible_unit(mouse_handler_.get_selected_hex(), teams_[gui_->viewing_team()]);
	if (u_itor.valid()) {
		menu_handler_.unit_detail(&*u_itor);
	}
}

void play_controller::save_game()
{
	if(save_blocker::try_block()) {
		save_blocker::save_unblocker unblocker;
		config snapshot;
		to_config(snapshot);
		savegame::game_savegame save(heros_, gamestate_, *gui_, snapshot);
		save.save_game_interactive(gui_->video(), "", gui::OK_CANCEL);
	} else {
		save_blocker::on_unblock(this,&play_controller::save_game);
	}
}

void play_controller::save_replay()
{
	if(save_blocker::try_block()) {
		save_blocker::save_unblocker unblocker;
		savegame::replay_savegame save(heros_, gamestate_);
		save.save_game_interactive(gui_->video(), "", gui::OK_CANCEL);
	} else {
		save_blocker::on_unblock(this,&play_controller::save_replay);
	}
}

void play_controller::save_map()
{
	if(save_blocker::try_block()) {
		save_blocker::save_unblocker unblocker;
		menu_handler_.save_map();
	} else {
		save_blocker::on_unblock(this,&play_controller::save_map);
	}
}

void play_controller::sync_undo()
{
	menu_handler_.clear_undo_stack(gui_->viewing_team() + 1);
}

void play_controller::load_game()
{
	// 这里用了偷懒办法，真正正确应该是在玩家确实已经加载了一个存档文件之后再清空
	gamestate_.start_scenario_ss.str("");
	// gamestate_.start_hero_ss.str("");
	gamestate_.clear_start_hero_data();

	game_state state_copy;
	savegame::loadgame load(*gui_, game_config_, state_copy);
	load.load_game();
}

void play_controller::preferences()
{
	menu_handler_.preferences();
}

void play_controller::show_chat_log(){
	menu_handler_.show_chat_log();
}

void play_controller::show_help(){
	menu_handler_.show_help();
}

void play_controller::undo(){
	// deselect unit (only here, not to be done when undoing attack-move)
	mouse_handler_.deselect_hex();
	menu_handler_.undo(player_number_);
}

void play_controller::redo(){
	// deselect unit (only here, not to be done when undoing attack-move)
	mouse_handler_.deselect_hex();
	menu_handler_.redo(player_number_);
}

void play_controller::toggle_ellipses(){
	menu_handler_.toggle_ellipses();
}

void play_controller::toggle_grid(){
	menu_handler_.toggle_grid();
}

void play_controller::fire_prestart(bool execute)
{
	// Run initialization scripts, even if loading from a snapshot.
	resources::state_of_game->set_phase(game_state::PRELOAD);
	resources::lua_kernel->initialize();
	uint32_t start = SDL_GetTicks();
	game_events::fire("preload");
	uint32_t end_fire_preload = SDL_GetTicks();
	posix_print("play_controller::fire_prestart, fire preload, used time: %u ms\n", end_fire_preload - start);

	// pre-start events must be executed before any GUI operation,
	// as those may cause the display to be refreshed.
	if (execute){
		update_locker lock_display(gui_->video());
		resources::state_of_game->set_phase(game_state::PRESTART);

		uint32_t end_set_phase = SDL_GetTicks();
		posix_print("play_controller::fire_prestart, set_phase, used time: %u ms\n", end_set_phase - end_fire_preload);

		game_events::fire("prestart");
		uint32_t end_fire_prestart = SDL_GetTicks();
		posix_print("play_controller::fire_prestart, fire prestart, used time: %u ms\n", end_fire_prestart - end_set_phase);

		check_end_level();
		// prestart event may modify start turn with WML, reflect any changes.
		start_turn_ = turn();
	}
}

void play_controller::fire_start(bool execute){
	if(execute) {
		resources::state_of_game->set_phase(game_state::START);
		game_events::fire("start");
		check_end_level();
		// start event may modify start turn with WML, reflect any changes.
		start_turn_ = turn();
		gamestate_.get_variable("turn_number") = int(start_turn_);
	} else {
		previous_turn_ = turn();
	}
	resources::state_of_game->set_phase(game_state::PLAY);
}

void play_controller::init_gui(){
	gui_->begin_game();
	gui_->adjust_colors(0,0,0);

	for(std::vector<team>::iterator t = teams_.begin(); t != teams_.end(); ++t) {
		::clear_shroud(t - teams_.begin() + 1);
	}
}

// @team_index: base 0
void play_controller::init_side(const unsigned int team_index, bool is_replay, bool need_record)
{
	log_scope("player turn");
	team& current_team = teams_[team_index];

	mouse_handler_.set_side(team_index + 1);

	// If we are observers we move to watch next team if it is allowed
	if (is_observer()
		&& !current_team.get_disallow_observers()) {
		gui_->set_team(size_t(team_index));
	}
	gui_->set_playing_team(size_t(team_index));

	gamestate_.get_variable("side_number") = player_number_;
	gamestate_.last_selected = map_location::null_location;

	/**
	 * We do this only for local side when we are not replaying.
	 * For all other sides it is recorded in replay and replay handler has to handle calling do_init_side()
	 * functions.
	 **/
	if (!current_team.is_local() || is_replay)
		return;
	if (need_record) {
		recorder.init_side();
	}
	do_init_side(team_index);
}

/**
 * Called by replay handler or init_side() to do actual work for turn change.
 */
void play_controller::do_init_side(const unsigned int team_index)
{
	team& current_team = teams_[team_index];
	std::stringstream strstr;

	const std::string turn_num = str_cast(turn());
	const std::string side_num = str_cast(team_index + 1);

	// calculate interior exploiture
	std::vector<artifical*>& holded = current_team.holded_cities();
	for (std::vector<artifical*>::iterator it = holded.begin(); it != holded.end(); ++ it) {
		artifical& city = **it;
		if (!loading_game_) {
			city.active_exploiture();
		}		
	}	
	
	// If this is right after loading a game we don't need to fire events and such. It was already done before saving.
	if (!loading_game_) {
		if (turn() != previous_turn_) {
			game_events::fire("turn " + turn_num);
			game_events::fire("new turn");

			recommend();
			if (tent::mode != TOWER_MODE && ally_all_ai()) {
				game_events::show_hero_message(&heros_[hero::number_scout], NULL, _("lips death and teeth cold, all ai sides concluded treaty of alliance."), game_events::INCIDENT_ALLY);

				if (rpg::stratum != hero_stratum_leader) {
					// enter to final battle automaticly
					if (!final_capital().first) {
						artifical* capital_of_human = units_.city_from_cityno(rpg::h->city_);
						artifical* capital_of_ai = decide_ai_capital();

						set_final_capital(capital_of_human, capital_of_ai);
						final_battle(capital_of_human->side(), -1, -1);
					}
				}
			}

			previous_turn_ = turn();
			// turn or new turn may trigger endlevel, but if here will result end directly.
			// check_end_level();
		}

		game_events::fire("side turn");
		game_events::fire("side " + side_num + " turn");
		game_events::fire("side turn " + turn_num);
		game_events::fire("side " + side_num + " turn " + turn_num);

		// first turn should begin to reserach technology.
		if (!current_team.ing_technology()) {
			current_team.select_ing_technology();
			if (current_team.ing_technology()) {
				utils::string_map symbols;
				symbols["begin"] = help::tintegrate::generate_format(current_team.ing_technology()->name(), "blue");
				symbols["side"] = help::tintegrate::generate_format(current_team.name(), "green");
				artifical* city = units_.city_from_cityno(current_team.leader()->city_);
				if (city && tent::mode != TOWER_MODE) {
					game_events::show_hero_message(&heros_[229], city, 
						vgettext("$side begin to research $begin!", symbols), game_events::INCIDENT_TECHNOLOGY);
				}
			}
		}
	}

	if (current_team.is_human()) {
		gui_->set_team(player_number_ - 1);
		gui_->recalculate_minimap();
		gui_->invalidate_all();
		gui_->draw(true,true);
	}

	// We want to work out if units for this player should get healed,
	// and the player should get income now.
	// Healing/income happen if it's not the first turn of processing,
	// or if we are loading a game.
	if (!loading_game_ && turn() > 1) {
		if (player_number_ == rpg::h->side_ + 1 && rpg::forbids) {
			rpg::forbids --;
		}
		uint32_t start = SDL_GetTicks();

		// new turn: 1)team, 2)city, 3)troop/commoner/artifcal
		current_team.new_turn();
		
		std::vector<artifical*> holded_cities = current_team.holded_cities();
		for (std::vector<artifical*>::iterator it = holded_cities.begin(); it != holded_cities.end(); ++ it) {
			artifical& city = **it;
			city.get_experience(increase_xp::turn_ublock(city), 3);
			city.new_turn();
		}

		const std::pair<unit**, size_t> p = current_team.field_troop();
		if (p.second) {
			size_t troops_vsize = p.second;
			if (troops_cache_vsize_ < troops_vsize) {
				free(troops_cache_);
				troops_cache_vsize_ = troops_vsize;
				troops_cache_ = (unit**)malloc(troops_cache_vsize_ * sizeof(unit*));
			}
			memcpy(troops_cache_, p.first, troops_vsize * sizeof(unit*));
			for (size_t i = 0; i < troops_vsize; i ++) {
				unit* u = troops_cache_[i];
				if (u->new_turn()) {
					units_.erase(u);
				} else if (tent::mode == TOWER_MODE && current_team.is_human()) {
					u->set_movement(0);
				}
			}
		}
		
		uint32_t new_turn = SDL_GetTicks();
		posix_print("(new_turn)used time: %u ms\n", new_turn - start);

		// If the expense is less than the number of villages owned,
		// then we don't have to pay anything at all
		int expense = current_team.side_upkeep() - current_team.villages().size();
		if (expense > 0) {
			current_team.spend_gold(expense);
		}

		{
			// cast active tactics before calculate healing.
			// some tactic maybe clear poision state, so that can heal.
			const events::command_disabler disable_commands;

			// cast active tactic
			std::vector<unit*> tactics = current_team.active_tactics();
			for (std::vector<unit*>::const_iterator it = tactics.begin(); it != tactics.end(); ++ it) {
				unit& u = **it;
				hero& h = *u.tactic_hero();
				// why below tow statement need notice order. see remark#31
				do_remove_active_tactic(u, false);
				cast_tactic(teams_, units_, u, h, NULL, false, false);
			}

			uint32_t gold = SDL_GetTicks();
			posix_print("(gold)used time: %u ms\n", gold - new_turn);

			calculate_healing(player_number_, !skip_replay_);
			uint32_t healing = SDL_GetTicks();
			posix_print("(healing)used time: %u ms\n", healing - new_turn);

			reset_resting(units_, player_number_);
			uint32_t resting = SDL_GetTicks();
			posix_print("(resting)used time: %u ms\n", resting - healing);

			road_guarding(units_, teams_, gui_->road_locs(), player_number_);
		}
	}

	if (!loading_game_) {
		game_events::fire("turn refresh");
		game_events::fire("side " + side_num + " turn refresh");
		game_events::fire("turn " + turn_num + " refresh");
		game_events::fire("side " + side_num + " turn " + turn_num + " refresh");
	}

	const time_of_day &tod = tod_manager_.get_time_of_day();

	if (int(team_index) + 1 == first_player_)
		sound::play_sound(tod.sounds, sound::SOUND_SOURCES);

	if (!recorder.is_skipping()){
		::clear_shroud(team_index + 1);
		gui_->invalidate_all();
	}

	if (!recorder.is_skipping() && !skip_replay_ && current_team.get_scroll_to_leader()){
		gui_->scroll_to_leader(units_, player_number_,game_display::ONSCREEN,false);
	}
	loading_game_ = false;
}

//builds the snapshot config from its members and their configs respectively
void play_controller::to_config(config& cfg) const
{
	// config cfg;
	cfg.clear();

	cfg.merge_attributes(level_);

	cfg.remove_attribute("employ_count");
	cfg.remove_attribute("ai_count");
	cfg.remove_attribute("modify_placing");

	std::stringstream strstr;
	strstr.str("");
	for (std::vector<int>::const_iterator it = treasures_.begin(); it != treasures_.end(); ++ it) {
		if (it != treasures_.begin()) {
			strstr << ", ";
		}
		strstr << *it;
	}
	cfg["treasures"] = strstr.str();

	// emploies
	strstr.str("");
	for (std::vector<hero*>::const_iterator it = emploies_.begin(); it != emploies_.end(); ++ it) {
		int number = (*it)->number_;
		if (it != emploies_.begin()) {
			strstr << ", ";
		}
		strstr << number;
	}
	cfg["emploies"] = strstr.str();

	if (final_capital_.first && !cfg.has_attribute("final_capital")) {
		strstr.str("");
		strstr << final_capital_.first->cityno() << "," << (final_capital_.second? final_capital_.second->cityno(): 0);
		cfg["final_capital"] = strstr.str();
	}
	cfg["maximal_defeated_activity"] = game_config::maximal_defeated_activity;
	if (duel_ == NO_DUEL) {
		cfg["duel"] = "no";
	} else if (duel_ == ALWAYS_DUEL) {
		cfg["duel"] = "always";
	} else {
		cfg.remove_attribute("duel");
	}

	cfg["provoke_cache"] = provoke_cache_2_str();
	cfg["tactic_cache"] = tactic_cache::cache_2_str(); 

	if (all_ai_allied_) {
		cfg["ai_allied"] = all_ai_allied_;
	}
	cfg["runtime"] = true;

	unit_segment2* top = (unit_segment2*)unit::savegame_cache_;
	top->count_ = teams_.size();
	int offset = sizeof(unit_segment2);
	
	for (std::vector<team>::const_iterator t = teams_.begin(); t != teams_.end(); ++t) {
		const std::vector<artifical*>& holded_cities = t->holded_cities();
		const std::pair<unit**, size_t> p = t->field_troop();

		unit_segment2* side = (unit_segment2*)(unit::savegame_cache_ + offset);
		side->count_ = holded_cities.size() + p.second;
		side->size_ = offset;
		offset += sizeof(unit_segment2);

		t->write(unit::savegame_cache_ + offset);
		offset += ((team_fields_t*)(unit::savegame_cache_ + offset))->size_;

		// current visible units
		for (std::vector<artifical*>::const_iterator i = holded_cities.begin(); i != holded_cities.end(); ++ i) {
			artifical* city = *i;
			// this city
			// Attention: Config of city must be written prior to other artifical. When loading, any artifical added to city, this city must be evalued.
			city->write(unit::savegame_cache_ + offset);
			unit_fields_t* fields = (unit_fields_t*)(unit::savegame_cache_ + offset);
			offset += fields->size_;
		}
		unit** troops = p.first;
		size_t troops_vsize = p.second;
		for (size_t i = 0; i < troops_vsize; i ++) {
			if (troops[i]->is_artifical()) {
				// field artifcals
				troops[i]->write(unit::savegame_cache_ + offset);
				unit_fields_t* fields = (unit_fields_t*)(unit::savegame_cache_ + offset);
				offset += fields->size_;
			} else {
				// field troops
				troops[i]->write(unit::savegame_cache_ + offset);
				unit_fields_t* fields = (unit_fields_t*)(unit::savegame_cache_ + offset);
				offset += fields->size_;
			}
		}
		side->size_ = offset - side->size_;
	}
	top->size_ = offset;

	cfg.merge_with(tod_manager_.to_config());

	// Write terrain_graphics data in snapshot, too
	BOOST_FOREACH (const config &tg, level_.child_range("terrain_graphics")) {
		cfg.add_child("terrain_graphics", tg);
	}

	//write out the current state of the map
	cfg["map_data"] = map_.write();
}

bool play_controller::scenario_env_changed(const tscenario_env& env) const
{
	if (env.duel != duel_) return true;
	if (env.maximal_defeated_activity != game_config::maximal_defeated_activity) return true;
	return false;
}

void play_controller::erase_treasure(int pos)
{
	treasures_.erase(treasures_.begin() + pos);
}

void play_controller::erase_employ(hero& h)
{
	std::vector<hero*>::iterator find = std::find(emploies_.begin(), emploies_.end(), &h);
	emploies_.erase(find);
}

void play_controller::finish_side_turn()
{
	for (unit_map::iterator uit = units_.begin(); uit != units_.end(); ++uit) {
		if (uit->side() == player_number_)
			uit->end_turn();
	}

	const std::string turn_num = str_cast(turn());
	const std::string side_num = str_cast(player_number_);
	game_events::fire("side turn end");
	game_events::fire("side "+ side_num + " turn end");
	game_events::fire("side turn " + turn_num + " end");
	game_events::fire("side " + side_num + " turn " + turn_num + " end");

	// This implements "delayed map sharing."
	// It is meant as an alternative to shared vision.
	if(current_team().copy_ally_shroud()) {
		gui_->recalculate_minimap();
		gui_->invalidate_all();
	}

	mouse_handler_.deselect_hex();
	n_unit::id_manager::instance().reset_fake();
	game_events::pump();
}

void play_controller::finish_turn()
{
  	const std::string turn_num = str_cast(turn() - 1);
	const std::string side_num = str_cast(player_number_);
	game_events::fire("turn end");
	game_events::fire("turn " + turn_num + " end");

	LOG_NG << "turn event..." << (recorder.is_skipping() ? "skipping" : "no skip") << '\n';
	update_locker lock_display(gui_->video(),recorder.is_skipping());
	gamestate_.get_variable("turn_number") = int(turn());
}

bool play_controller::enemies_visible() const
{
	// If we aren't using fog/shroud, this is easy :)
	if(current_team().uses_fog() == false && current_team().uses_shroud() == false)
		return true;

	// See if any enemies are visible
	for(unit_map::const_iterator u = units_.begin(); u != units_.end(); ++u)
		if (current_team().is_enemy(u->side()) && !gui_->fogged(u->get_location()))
			return true;

	return false;
}

bool play_controller::execute_command(hotkey::HOTKEY_COMMAND command, int index, std::string str)
{
	if (index >= 0) {
		unsigned i = static_cast<unsigned>(index);
		if (i < savenames_.size() && !savenames_[i].empty()) {
			// Load the game by throwing load_game_exception
			throw game::load_game_exception(savenames_[i],false,false);

		}
	}
	return command_executor::execute_command(command, index, str);
}

bool play_controller::can_execute_command(hotkey::HOTKEY_COMMAND command, int index) const
{
	if (index >= 0) {
		unsigned i = static_cast<unsigned>(index);
		if ((i < savenames_.size() && !savenames_[i].empty()) || (i < wml_commands_.size() && wml_commands_[i] != NULL)) {
			return true;
		}
	}
	switch (command) {

	// Commands we can always do:
	case hotkey::HOTKEY_ZOOM_IN:
	case hotkey::HOTKEY_ZOOM_OUT:
	case hotkey::HOTKEY_ZOOM_DEFAULT:
	case hotkey::HOTKEY_FULLSCREEN:
	case hotkey::HOTKEY_SCREENSHOT:
	case hotkey::HOTKEY_MAP_SCREENSHOT:
	case hotkey::HOTKEY_ACCELERATED:
	case hotkey::HOTKEY_TOGGLE_ELLIPSES:
	case hotkey::HOTKEY_TOGGLE_GRID:
	case hotkey::HOTKEY_MOUSE_SCROLL:
	case hotkey::HOTKEY_ANIMATE_MAP:
	case hotkey::HOTKEY_MUTE:
	case hotkey::HOTKEY_PREFERENCES:
	case hotkey::HOTKEY_OBJECTIVES:
	case hotkey::HOTKEY_STATISTICS:
	case hotkey::HOTKEY_QUIT_GAME:
	case hotkey::HOTKEY_CLEAR_MSG:
	case hotkey::HOTKEY_UNIT_DETAIL:
		return true;
	case hotkey::HOTKEY_HELP:
		return game_config::tiny_gui? false: true;

	case hotkey::HOTKEY_RPG:
		return network::nconnections() == 0; // Can only load games if not in a network game

	case hotkey::HOTKEY_CHAT_LOG:
		return network::nconnections() > 0;

	case hotkey::HOTKEY_UNDO:
		return !linger_ && !undo_stack_.empty() && !events::commands_disabled && !browse_;

	default:
		return false;
	}
}

void play_controller::enter_textbox()
{
	if(menu_handler_.get_textbox().active() == false) {
		return;
	}

	const std::string str = menu_handler_.get_textbox().box()->text();
	const unsigned int team_num = player_number_;
	events::mouse_handler& mousehandler = mouse_handler_;

	switch(menu_handler_.get_textbox().mode()) {
	case gui::TEXTBOX_MESSAGE:
		menu_handler_.do_speak();
		menu_handler_.get_textbox().close(*gui_);  //need to close that one after executing do_speak() !
		break;
	default:
		menu_handler_.get_textbox().close(*gui_);
		ERR_DP << "unknown textbox mode\n";
	}

}

void play_controller::tab()
{
	gui::TEXTBOX_MODE mode = menu_handler_.get_textbox().mode();

	std::set<std::string> dictionary;
	switch(mode) {
	case gui::TEXTBOX_MESSAGE:
	{
		BOOST_FOREACH (const team& t, teams_) {
			if(!t.is_empty())
				dictionary.insert(t.current_player());
		}

		// Add observers
		BOOST_FOREACH (const std::string& o, gui_->observers()){
			dictionary.insert(o);
		}
		//Exclude own nick from tab-completion.
		//NOTE why ?
		dictionary.erase(preferences::login());

		break;
	}
	default:
		ERR_DP << "unknown textbox mode\n";
	} //switch(mode)

	menu_handler_.get_textbox().tab(dictionary);
}


team& play_controller::current_team()
{
	assert(player_number_ > 0 && player_number_ <= int(teams_.size()));
	return teams_[player_number_-1];
}

const team& play_controller::current_team() const
{
	assert(player_number_ > 0 && player_number_ <= int(teams_.size()));
	return teams_[player_number_-1];
}

int play_controller::find_human_team_before(const size_t team_num) const
{
	if (team_num > teams_.size())
		return -2;

	int human_side = -2;
	for (int i = team_num-2; i > -1; --i) {
		if (teams_[i].is_human()) {
			human_side = i;
			break;
		}
	}
	if (human_side == -2) {
		for (size_t i = teams_.size()-1; i > team_num-1; --i) {
			if (teams_[i].is_human()) {
				human_side = i;
				break;
			}
		}
	}
	return human_side+1;
}

void play_controller::fill_employ_hero(std::set<int>& candidate, int count, int& random)
{
	// 1/3: wander
	while (count && !candidate.empty()) {
		int pos = random % candidate.size();
		std::set<int>::iterator it = candidate.begin();
		std::advance(it, pos);
		hero& h = heros_[*it];

		if (h.leadership_ >= ftofxp9(85) || h.charm_ >= ftofxp9(85)) {
			h.status_ = hero_status_employable;
			emploies_.push_back(&h);

			candidate.erase(it);
			count --;
		}

		random = random * 1103515245 + 12345;
		random = (static_cast<unsigned>(random / 65536) % 32768);
	}
}

void play_controller::slice_before_scroll() {
	soundsources_manager_->update();
}

events::mouse_handler& play_controller::get_mouse_handler_base() {
	return mouse_handler_;
}

game_display& play_controller::get_display() {
	return *gui_;
}

bool play_controller::have_keyboard_focus()
{
	return !menu_handler_.get_textbox().active();
}

void play_controller::process_focus_keydown_event(const SDL_Event& event)
{
	if(event.key.keysym.sym == SDLK_ESCAPE) {
		menu_handler_.get_textbox().close(*gui_);
	} else if(event.key.keysym.sym == SDLK_TAB) {
		tab();
	} else if(event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER) {
		enter_textbox();
	}
}

void play_controller::process_keydown_event(const SDL_Event& event) {
}

void play_controller::process_keyup_event(const SDL_Event& event) {
	// If the user has pressed 1 through 9, we want to show
	// how far the unit can move in that many turns
	if(event.key.keysym.sym >= '1' && event.key.keysym.sym <= '7') {
		const int new_path_turns = (event.type == SDL_KEYDOWN) ?
		                           event.key.keysym.sym - '1' : 0;

		if(new_path_turns != mouse_handler_.get_path_turns()) {
			mouse_handler_.set_path_turns(new_path_turns);

			const unit_map::iterator u = mouse_handler_.selected_unit();

			if(u != units_.end()) {
				bool teleport = u->get_ability_bool("teleport");

				// if it's not the unit's turn, we reset its moves
				unit_movement_resetter move_reset(*u, u->side() != player_number_);

				mouse_handler_.set_current_paths(pathfind::paths(map_, units_, u->get_location(),
				                       teams_,false,teleport, teams_[gui_->viewing_team()],
				                       mouse_handler_.get_path_turns()));

				gui_->highlight_reach(mouse_handler_.current_paths());
			}
		}
	}
}

void play_controller::post_mouse_press(const SDL_Event& /*event*/)
{
	if (mouse_handler_.get_undo()) {
		mouse_handler_.set_undo(false);
		menu_handler_.undo(player_number_);
	}
}

static void trim_items(std::vector<std::string>& newitems) {
	if (newitems.size() > 5) {
		std::vector<std::string> subitems;
		subitems.push_back(newitems[0]);
		subitems.push_back(newitems[1]);
		subitems.push_back(newitems[newitems.size() / 3]);
		subitems.push_back(newitems[newitems.size() * 2 / 3]);
		subitems.push_back(newitems.back());
		newitems = subitems;
	}
}

void play_controller::expand_autosaves(std::vector<std::string>& items)
{
}

void play_controller::expand_wml_commands(std::vector<std::string>& items)
{
	wml_commands_.clear();
	for (unsigned int i = 0; i < items.size(); ++i) {
		if (items[i] == "wml") {
			items.erase(items.begin() + i);
			std::map<std::string, wml_menu_item*>& gs_wmi = gamestate_.wml_menu_items;
			if(gs_wmi.empty())
				break;
			std::vector<std::string> newitems;

			const map_location& hex = mouse_handler_.get_last_hex();
			gamestate_.get_variable("x1") = hex.x + 1;
			gamestate_.get_variable("y1") = hex.y + 1;
			scoped_xy_unit highlighted_unit("unit", hex.x, hex.y, units_);

			std::map<std::string, wml_menu_item*>::iterator itor;
			for (itor = gs_wmi.begin(); itor != gs_wmi.end()
				&& newitems.size() < MAX_WML_COMMANDS; ++itor) {
				config& show_if = itor->second->show_if;
				config filter_location = itor->second->filter_location;
				if ((show_if.empty()
					|| game_events::conditional_passed(vconfig(show_if)))
				&& (filter_location.empty()
					|| terrain_filter(vconfig(filter_location), units_)(hex))
				&& (!itor->second->needs_select
					|| gamestate_.last_selected.valid()))
				{
					wml_commands_.push_back(itor->second);
					std::string newitem = itor->second->description;

					// Prevent accidental hotkey binding by appending a space
					newitem.push_back(' ');
					newitems.push_back(newitem);
				}
			}
			items.insert(items.begin()+i, newitems.begin(), newitems.end());
			break;
		}
		wml_commands_.push_back(NULL);
	}
}

// @m: memu object taht will be display
// @gui: global game_display reference
void play_controller::show_context_menu(theme::menu* m, display& gui)
{
	uint32_t show_flags = 0, idx_in_u32 = 0, disable_flags = 0;

	const theme::menu* m_adjusted = m;
	if (!m) {
		m_adjusted = gui.get_theme().context_menu("");
	}
	if (!m_adjusted) {
		return;
	}

	const std::vector<std::string>& items = m_adjusted->items();
	hotkey::HOTKEY_COMMAND command;
	std::vector<std::string>::const_iterator i = items.begin();
	while (i != items.end()) {
		command = hotkey::get_hotkey(*i).get_id();
		// Remove commands that can't be executed or don't belong in this type of menu
		if (in_context_menu(command)) {
			show_flags |= 1 << idx_in_u32;
			if (!enable_context_menu(command, mouse_handler_.get_selected_hex())) {
				disable_flags |= 1 << idx_in_u32;
			}
		}
		idx_in_u32 ++;
		++ i;
	}
	if (!show_flags) {
		return;
	}

	gui.hide_context_menu(m_adjusted, false, show_flags, disable_flags);
}

void play_controller::show_menu(const std::vector<std::string>& items_arg, int xloc, int yloc, bool context_menu)
{
	std::vector<std::string> items = items_arg;
	hotkey::HOTKEY_COMMAND command;
	std::vector<std::string>::iterator i = items.begin();
	while(i != items.end()) {
		command = hotkey::get_hotkey(*i).get_id();
		// Remove WML commands if they would not be allowed here
		if(*i == "wml") {
			if(!context_menu || gui_->viewing_team() != gui_->playing_team()
			|| events::commands_disabled || !teams_[gui_->viewing_team()].is_human()) {
				i = items.erase(i);
				continue;
			}
		// Remove commands that can't be executed or don't belong in this type of menu
		} else if(!can_execute_command(command)
		|| (context_menu && !in_context_menu(command))) {
			i = items.erase(i);
			continue;
		}
		++i;
	}

	// Add special non-hotkey items to the menu and remember their indices
	// don't show autosave files
	expand_autosaves(items);
	expand_wml_commands(items);

	if(items.empty())
		return;

	command_executor::show_menu(items, xloc, yloc, context_menu, *gui_);
}

bool play_controller::in_context_menu(hotkey::HOTKEY_COMMAND command) const
{
	// current side must human player
	const team& current_team = teams_[player_number_ - 1];

	if (command != hotkey::HOTKEY_LIST && command != hotkey::HOTKEY_SYSTEM && !replaying_ && !current_team.is_human()) {
		return false;
	}
	if (linger_) {
		return command == hotkey::HOTKEY_LIST || command == hotkey::HOTKEY_SYSTEM;
	}
	if (replaying_ && (command == hotkey::HOTKEY_TECHNOLOGY_TREE || command == hotkey::HOTKEY_FINAL_BATTLE)) {
		// current don't support, remark them.
		return false;
	}

	const artifical* city = NULL;
	const map_location& loc = mouse_handler_.get_selected_hex();
	unit_map::const_iterator itor;
	if (!replaying_) {
		itor = find_visible_unit(loc, current_team);
	}

	switch(command) {
	// Only display these if the mouse is over a castle or keep tile
	case hotkey::HOTKEY_RECRUIT:
	case hotkey::HOTKEY_EXPEDITE:
	case hotkey::HOTKEY_ARMORY:
	case hotkey::HOTKEY_MOVE:
		if (rpging_ || tent::mode == TOWER_MODE) {
			return false;
		}
		if (mouse_handler_.in_multistep_state()) {
			return false;
		}
		// loc is in myself city.
		city = units_.city_from_loc(loc);
		if (itor.valid() && city && (city->side() == player_number_)) {
			return true;
		}
		break;

	case hotkey::HOTKEY_BUILD_M:
		if (current_team.builds().empty()) {
			return false;
		}
	case hotkey::HOTKEY_BUILD:
		if (rpging_) {
			return false;
		}
		if (itor.valid() && !itor->is_artifical() && itor->human()) {
			if (tent::mode == TOWER_MODE || itor->attacks_left()) {
				return true;
			}
		}
		break;

	case hotkey::HOTKEY_EXTRACT:
		if (tent::mode != TOWER_MODE) {
			return false;
		}
		if (mouse_handler_.in_multistep_state()) {
			return false;
		}
		if (itor.valid() && !itor->is_artifical() && (itor->side() == player_number_)) {
			return true;
		}
		break;

	case hotkey::HOTKEY_DEMOLISH:
		if (mouse_handler_.in_multistep_state()) {
			return false;
		}
		if (rpging_) {
			return false;
		}
		if (rpg::stratum == hero_stratum_citizen) {
			return false;
		}
		if (itor.valid() && (tent::mode == TOWER_MODE || itor->is_artifical()) && (itor->side() == player_number_) && !unit_is_city(&*itor)) {
			return true;
		}
		break;

	case hotkey::HOTKEY_INTERIOR:
		if (mouse_handler_.in_multistep_state()) {
			return false;
		}
		if (rpging_) {
			return false;
		}
		return !events::commands_disabled && !itor.valid();

	case hotkey::HOTKEY_TECHNOLOGY_TREE:
		if (mouse_handler_.in_multistep_state()) {
			return false;
		}
		if (rpging_ || tent::mode == TOWER_MODE) {
			return false;
		}
		return !events::commands_disabled && !itor.valid();

	case hotkey::HOTKEY_EMPLOY:
		if (mouse_handler_.in_multistep_state()) {
			return false;
		}
		if (rpging_ || tent::mode != TOWER_MODE) {
			return false;
		}
		return replaying_ || (!events::commands_disabled && !itor.valid());

	case hotkey::HOTKEY_FINAL_BATTLE:
		if (mouse_handler_.in_multistep_state()) {
			return false;
		}
		if (rpging_ || tent::mode == TOWER_MODE) {
			return false;
		}
		return !events::commands_disabled && (teams_.size() >= 3) && !itor.valid() && (human_players_ == 1) && !network::nconnections();

	case hotkey::HOTKEY_LIST:
	case hotkey::HOTKEY_SYSTEM:
		if (mouse_handler_.in_multistep_state()) {
			return false;
		}
		if (rpging_) {
			return false;
		}
		return replaying_ || (!events::commands_disabled && !itor.valid() && (network::nconnections() || current_team.is_human()));

	case hotkey::HOTKEY_RPG_EXCHANGE:
	case hotkey::HOTKEY_RPG_INDEPENDENCE:
		if (rpg::stratum != hero_stratum_mayor || linger_ || replaying_ || events::commands_disabled || network::nconnections()) {
			return false;
		}
	case hotkey::HOTKEY_RPG_DETAIL:
	case hotkey::HOTKEY_RPG_TREASURE:
		if (mouse_handler_.in_multistep_state()) {
			return false;
		}
		if (rpging_) {
			return true;
		}
		break;

	default:
		return true;
	}

	return false;
}

bool play_controller::enable_context_menu(hotkey::HOTKEY_COMMAND command, const map_location& loc) const
{
	const team& current_team = teams_[player_number_ - 1];
	const std::vector<artifical*>& holded_cities = current_team.holded_cities();
	unit_map::const_iterator itor;
	if (!replaying_) {
		itor = find_visible_unit(loc, current_team);
	}
	const artifical* city = units_.city_from_loc(loc);

	switch(command) {
	case hotkey::HOTKEY_EMPLOY:
		return !emploies_.empty();

	case hotkey::HOTKEY_BUILD_M:
		if (tent::mode == TOWER_MODE) {
			if (units_.count(loc, false)) {
				return false;
			}
			const unit_type* wall = unit_types.find_wall();
			if (!wall->terrain_matches(map_.get_terrain(loc))) {
				return false;
			}
			if (!current_team.may_build_count()) {
				return false;
			}
		}
		break;

	case hotkey::HOTKEY_RECRUIT:
		if (rpg::stratum == hero_stratum_citizen) {
			return false;
		} else if (rpg::stratum == hero_stratum_mayor && city->mayor() != rpg::h) {
			return false;
		}
		if (city->fresh_heros().empty()) {
			return false;
		}
		break;
	case hotkey::HOTKEY_EXPEDITE:
		if (city->reside_troops().empty()) {
			return false;
		}
		if (city->is_surrounded()) {
			return false;
		}
		break;
	case hotkey::HOTKEY_ARMORY:
		{
			const std::vector<unit*>& reside_troops = city->reside_troops();
			const std::vector<hero*>& fresh_heros = city->fresh_heros();
			size_t reside_troops_size = reside_troops.size();
			if (!reside_troops_size || (reside_troops_size == 1 && !reside_troops[0]->second().valid() && fresh_heros.empty())) {
				return false;
			}
			for (std::vector<unit*>::const_iterator it = reside_troops.begin(); it != reside_troops.end(); ++ it) {
				const unit& u = **it;
				if (u.human()) {
					return true;
				}
			}
			return false;
		}
	case hotkey::HOTKEY_MOVE:
		if (rpg::stratum == hero_stratum_citizen) {
			if (city->cityno() != rpg::h->city_ || rpg::h->status_ != hero_status_idle) {
				return false;
			}
		} else if (rpg::stratum == hero_stratum_mayor && city->mayor() != rpg::h) {
			return false;
		}
		if (current_team.holded_cities().size() < 2 || city->fresh_heros().empty()) {
			return false;
		}
		break;

	case hotkey::HOTKEY_EXTRACT:
		if (!itor.valid() || itor->is_artifical() || (itor->side() != player_number_) || !itor->second().valid()) {
			return false;
		}
		break;

	case hotkey::HOTKEY_DEMOLISH:
		if (!itor.valid() || itor->side() != player_number_ || unit_is_city(&*itor)) {
			return false;
		}
		if (itor->is_artifical() && tent::mode != TOWER_MODE) {
			const artifical* art = unit_2_artifical(&*itor);
			const map_location* tiles;
			size_t adjacent_size;
			// to mayor, can domolish it that owner his city.
			if (rpg::stratum == hero_stratum_mayor) {
				if (art->type() == unit_types.find_market()) {
					std::map<const map_location, int>::const_iterator it = unit_map::economy_areas_.find(loc);
					if (units_.city_from_cityno(it->second)->mayor() != rpg::h) {
						return false;
					}
				} else if (art->type() == unit_types.find_keep() || art->type() == unit_types.find_wall()) {
					bool found_rpg_city = false;
					tiles = itor->adjacent_2_;
					adjacent_size = itor->adjacent_size_2_;
					for (int i = 0; i != adjacent_size; ++i) {
						unit_map::const_iterator another = units_.find(tiles[i]);
						if (another.valid() && another->is_city()) {
							const artifical* city = unit_2_artifical(&*another);
							if (city->mayor() == rpg::h) {
								found_rpg_city = true;
								break;
							}
						}
					}
					if (!found_rpg_city) {
						return false;
					}
				}
			}
			// there is enemy unit in 2 grids, cannot demolish.
			for (int times = 0; times < 2; times ++) {
				if (times == 0) {
					tiles = art->adjacent_;
					adjacent_size = art->adjacent_size_;
				} else {
					tiles = itor->adjacent_2_;
					adjacent_size = itor->adjacent_size_2_;
				}
				for (int i = 0; i != adjacent_size; ++i) {
					unit_map::const_iterator another = units_.find(tiles[i]);
					if (another.valid() && current_team.is_enemy(another->side())) {
						return false;
					}
					another = units_.find(tiles[i], false);
					if (another.valid() && current_team.is_enemy(another->side())) {
						return false;
					}
				}
			}
			return true;			
		}
		break;
	case hotkey::HOTKEY_RPG_TREASURE:
		for (std::vector<artifical*>::const_iterator it = holded_cities.begin(); it != holded_cities.end(); ++ it) {
			const artifical* city = *it;

			const std::vector<unit*>& resides = city->reside_troops();
			for (std::vector<unit*>::const_iterator it2 = resides.begin(); it2 != resides.end(); ++ it2) {
				unit* u = *it2;
				if (!u->human()) {
					continue;
				}
				return true;
			}

			const std::vector<hero*>& freshes = city->fresh_heros();
			const std::vector<hero*>& finishes = city->finish_heros();
			if (city->mayor() == rpg::h || rpg::stratum == hero_stratum_leader) {
				if (!freshes.empty() || !finishes.empty()) {
					return true;
				}
			}
			if (rpg::stratum == hero_stratum_citizen) {
				if (std::find(freshes.begin(), freshes.end(), rpg::h) != freshes.end()) {
					return true;
				}
				if (std::find(finishes.begin(), finishes.end(), rpg::h) != finishes.end()) {
					return true;
				}
			}
		}
		return false;
	case hotkey::HOTKEY_RPG_EXCHANGE:
	case hotkey::HOTKEY_RPG_INDEPENDENCE:
		if (holded_cities.size() < 2) {
			return false;
		}
		break;

	case hotkey::HOTKEY_BUILD_TACTIC:
		{
			map_location adjs[6];
			get_adjacent_tiles(loc, adjs);
			BOOST_FOREACH (const map_location &adj, adjs) {
				if (!map_.on_board(adj)) continue;
				// Add the tile to be checked if it hasn't already been and
				// isn't being checked.
				if (map_.is_ea(adj)) {
					std::map<const map_location, int>::const_iterator it = unit_map::economy_areas_.find(adj);
					if (it != unit_map::economy_areas_.end()) {
						const artifical& city = *units_.city_from_cityno(it->second);
						if (city.tactic_on_ea()) {
							return false;
						}
					}
				}
			}
		}
		break;
	}	

	return true;
}

void play_controller::refresh_city_buttons(const artifical& city) const
{
	static hotkey::HOTKEY_COMMAND city_buttons[] = {
		hotkey::HOTKEY_RECRUIT, 
		hotkey::HOTKEY_EXPEDITE, 
		hotkey::HOTKEY_ARMORY, 
		hotkey::HOTKEY_MOVE
	};
	static int nb = sizeof(city_buttons) / sizeof(hotkey::HOTKEY_COMMAND);
	for (int i = 0; i < nb; i ++) {
		gui::button* b = gui_->find_button(hotkey::get_hotkey(city_buttons[i]).get_command());
		if (b) {
			b->enable(enable_context_menu(city_buttons[i], city.get_location()));
		}
	}
}

std::string play_controller::get_action_image(hotkey::HOTKEY_COMMAND command, int index) const
{
	if(index >= 0 && index < static_cast<int>(wml_commands_.size())) {
		wml_menu_item* const& wmi = wml_commands_[index];
		if(wmi != NULL) {
			return wmi->image.empty() ? game_config::images::wml_menu : wmi->image;
		}
	}
	return command_executor::get_action_image(command, index);
}

hotkey::ACTION_STATE play_controller::get_action_state(hotkey::HOTKEY_COMMAND command, int /*index*/) const
{
	switch(command) {
	case hotkey::HOTKEY_DELAY_SHROUD:
		return teams_[gui_->viewing_team()].auto_shroud_updates() ? hotkey::ACTION_OFF : hotkey::ACTION_ON;
	default:
		return hotkey::ACTION_STATELESS;
	}
}

namespace {
	static const std::string empty_str = "";
}

const std::string& play_controller::select_victory_music() const
{
	if(victory_music_.empty())
		return empty_str;

	const size_t p = gamestate_.rng().get_next_random() % victory_music_.size();
	assert(p < victory_music_.size());
	return victory_music_[p];
}

const std::string& play_controller::select_defeat_music() const
{
	if(defeat_music_.empty())
		return empty_str;

	const size_t p = gamestate_.rng().get_next_random() % defeat_music_.size();
	assert(p < defeat_music_.size());
	return defeat_music_[p];
}


void play_controller::set_victory_music_list(const std::string& list)
{
	victory_music_ = utils::split(list);
	if(victory_music_.empty())
		victory_music_ = utils::split(game_config::default_victory_music);
}

void play_controller::set_defeat_music_list(const std::string& list)
{
	defeat_music_  = utils::split(list);
	if(defeat_music_.empty())
		defeat_music_ = utils::split(game_config::default_defeat_music);
}

void play_controller::check_victory()
{
	check_end_level();

	// cities_teams saved this teams that holded one or more city
	std::vector<unsigned> cities_teams;

	bool human_defeated = false;

	// Clear villages for teams that have no city && no field troop
	for (std::vector<team>::iterator tm_beg = teams_.begin(), tm = tm_beg, tm_end = teams_.end(); tm != tm_end; ++tm) {
		if (!tm->holded_cities().empty()) {
			// don't conside side that hasn't city
			if (!tm->is_empty()) {
				cities_teams.push_back(tm->side());
			}
		} else if (teams_[tm->side() - 1].is_human()) {
			human_defeated = true;
		}
		const std::pair<unit**, size_t> p = tm->field_troop();
		if (!p.second && tm->holded_cities().empty() && !tm->villages().empty()) {
			tm->clear_villages();
			// invalidate_all() is overkill and expensive but this code is
			// run rarely so do it the expensive way.
			gui_->invalidate_all();
		}
	}
	if (human_defeated) {
		throw end_level_exception(DEFEAT);
	}

	if (final_capital_.first) {
		if (final_capital_.second && teams_[final_capital_.second->side() - 1].is_human()) {
			throw end_level_exception(VICTORY);
		} else if (!teams_[final_capital_.first->side() - 1].is_human()) {
			throw end_level_exception(DEFEAT);
		} 
	}

	bool found_player = false;

	// found_player: 
	//   ture: human控制的阵营已不存在敌对阵营
	//   false: 
	for (size_t n = 0; n != cities_teams.size(); ++n) {
		size_t side = cities_teams[n] - 1;

		for (size_t m = n +1 ; m != cities_teams.size(); ++m) {
			if (teams_[side].is_enemy(cities_teams[m])) {
				return;
			}
		}

		if (teams_[side].is_human()) {
			found_player = true;
		}
	}

	if (found_player) {
		game_events::fire("enemies defeated");
		check_end_level();
	}

	if (!victory_when_enemies_defeated_ && (found_player || is_observer())) {
		// This level has asked not to be ended by this condition.
		return;
	}

	if (non_interactive()) {
		std::cout << "winner: ";
		BOOST_FOREACH (unsigned l, cities_teams) {
			std::string ai = ai::manager::get_active_ai_identifier_for_side(l);
			if (ai.empty()) ai = "default ai";
			std::cout << l << " (using " << ai << ") ";
		}
		std::cout << '\n';
	}

	throw end_level_exception(VICTORY);
}

void play_controller::process_oos(const std::string& msg) const
{
	if (game_config::ignore_replay_errors) return;

	std::stringstream message;
	message << _("The game is out of sync. It might not make much sense to continue. Do you want to save your game?");
	message << "\n\n" << _("Error details:") << "\n\n" << msg;

	config snapshot;
	to_config(snapshot);
	savegame::oos_savegame save(heros_, snapshot);
	save.save_game_interactive(resources::screen->video(), message.str(), gui::YES_NO); // can throw end_level_exception
}

void play_controller::set_final_capital(artifical* human, artifical* ai)
{
	final_capital_.first = human;
	final_capital_.second = ai;
}

bool play_controller::do_recruit(artifical* city, int cost_exponent, bool rpg_mode)
{
	int side_num = city->side();
	const map_location& city_loc = city->get_location();
	team& current_team = teams_[side_num - 1];

	const hero *master, *second, *third;
	const unit_type* ut;
	if (city->fresh_heros().empty()) {
		gui2::show_transient_message(gui_->video(), "", _("There are no hero available to recruit"));
		return false;
	}
	if (city->recruits(-1).empty()) {
		gui2::show_transient_message(gui_->video(), "", _("There are no unit type available to recruit."));
		return false;
	}

	{
		gui2::trecruit dlg(*gui_, teams_, units_, heros_, *city, cost_exponent, rpg_mode);
		try {
			dlg.show(gui_->video());
		} catch(twml_exception& e) {
			e.show(*gui_);
			return false;
		}
		// player may generate troop when select heors, and these heros were changed to hero_status_military status.
		// it is time to be back hero_status_idle.
		std::vector<hero*>& fresh_heros = city->fresh_heros();
		for (std::vector<hero*>::iterator itor = fresh_heros.begin(); itor != fresh_heros.end(); ++ itor) {
			(*itor)->status_ = hero_status_idle;
		}
		if (dlg.get_retval() != gui2::twindow::OK) {
			return false;
		}
		ut = dlg.unit_type_ptr();
		master = dlg.master();
		second = dlg.second();
		third = dlg.third();
		if (!master->valid()) {
			return false;
		}
	}

	if (ut->cost() * cost_exponent / 100 > current_team.gold()) {
		gui2::show_transient_message(gui_->video(), "", _("You don't have enough gold to recruit that unit"));
		return false;
	}
	const events::command_disabler disable_commands;

	bool commercial_changed = false;

	// generated troop: 1)can full movement; 2)can full attack.
	std::vector<const hero*> v;
	v.push_back(&heros_[master->number_]);
	if (second->valid()) {
		v.push_back(&heros_[second->number_]);
	}
	if (third->valid()) {
		v.push_back(&heros_[third->number_]);
	}

	::do_recruit(units_, heros_, teams_, current_team, ut, v, *city, ut->cost() * cost_exponent / 100, true, true);

	menu_handler_.clear_undo_stack(side_num);
	refresh_city_buttons(*city);

	return true;
}

bool play_controller::generate_commoner(artifical& city, const unit_type* ut, bool replay)
{
	VALIDATE(ut->master() != HEROS_INVALID_NUMBER, "play_controller::generate_commoner, desired master is HEROS_INVALID_NUMBER");

	int side_num = city.side();
	const map_location& city_loc = city.get_location();
	team& current_team = teams_[side_num - 1];

	std::vector<const hero*> v;
	v.push_back(&heros_[ut->master()]);
	type_heros_pair pair(ut, v);
	if (!replay) {
		// recorder.add_recruit(ut->id(), city.get_location(), v, 0, false);
	}

	unit* new_unit = new unit(units_, heros_, teams_, pair, city.cityno(), true);
	new_unit->set_movement(new_unit->total_movement());

	current_team.spend_gold(0);
	city.commoner_come_into(new_unit, -1, false);

	menu_handler_.clear_undo_stack(side_num);

	return true;
}

void play_controller::calculate_commoner_gotos(team& current_team, std::vector<std::pair<unit*, int> >& gotos)
{
	gotos.clear();

	const int random = get_random();
	int derived_random;

	// field commoner
	const std::pair<unit**, size_t> p = current_team.field_troop();
	unit** troops = p.first;
	size_t troops_vsize = p.second;
	for (size_t i = 0; i < troops_vsize; i ++) {
		unit& u = *troops[i];
		if (u.is_commoner()) {
			artifical* goto_city = units_.city_from_loc(u.get_goto());
			if (goto_city->side() != current_team.side() ||
				(u.block_turns() >= 1 && !u.get_from().valid())) {
				// goto city is captured, be back!
				u.set_from(goto_city->get_location());
				u.set_goto(units_.city_from_cityno(u.cityno())->get_location());
				u.set_task(unit::TASK_BACK);
				u.set_block_turns(0);
			}
			gotos.push_back(std::make_pair<unit*, int>(&u, -1));
		}
	}

	// reside commoner
	const std::vector<artifical*>& holded_cities = current_team.holded_cities();
	for (std::vector<artifical*>::const_iterator it = holded_cities.begin(); it != holded_cities.end(); ++ it) {
		artifical* city = *it;
		
		// recruit
		std::vector<unit*>& reside_commoners = city->reside_commoners();
		int ratio;

		if (city->police() >= game_config::min_tradable_police) {
			derived_random = random + city->master().number_ + city->hitpoints();

			// recruit probability: 1/10
			ratio = 10;
			int generate_ratio = ratio * (200 - city->generate_speed_) / 100;
			if (!generate_ratio) {
				generate_ratio = 1;
			}
			if (int(reside_commoners.size() + city->field_commoners().size()) < city->max_commoners()) {
				if (!(random % generate_ratio)) {
					if (derived_random & 1) {
						generate_commoner(*city, unit_types.master_type(hero::number_businessman), false);
					} else {
						generate_commoner(*city, unit_types.master_type(hero::number_scholar), false);
					}
				}
			}
		}

		// trade/transfer
		const std::set<int>& roaded_cities = city->roaded_cities();
		std::vector<artifical*> candidate, transferable_cities;
		for (std::set<int>::const_iterator it2 = roaded_cities.begin(); it2 != roaded_cities.end(); ++ it2) {
			artifical* roaded = units_.city_from_cityno(*it2);
			if (roaded->side() == current_team.side() && roaded->police() >= game_config::min_tradable_police) {
				candidate.push_back(roaded);
				if (int(roaded->reside_commoners().size() + roaded->field_commoners().size()) < roaded->max_commoners()) {
					transferable_cities.push_back(roaded);
				}
			}
		}
		if (candidate.empty()) {
			continue;
		}
		if (!reside_commoners.empty()) {
			int index = reside_commoners.size() - 1;
			bool transfered = false, traded = false;
			for (std::vector<unit*>::reverse_iterator it2 = reside_commoners.rbegin(); it2 != reside_commoners.rend(); ++ it2, index --) {
				unit& u = **it2;

				derived_random = random + u.hitpoints() + u.experience();
				// trade probability: 1/6
				ratio = 6;
				int trade_ratio = ratio * (200 - city->trade_speed_) / 100;
				if (!trade_ratio) {
					trade_ratio = 1;
				}

				if (city->police() < game_config::min_tradable_police) {
					// transfer probability: 1/3
					if (!transfered && !(random % 3) && !transferable_cities.empty()) {
						u.set_task(unit::TASK_TRANSFER);
						u.set_goto(transferable_cities[random % transferable_cities.size()]->get_location());
						gotos.push_back(std::make_pair<unit*, int>(city, index));
						// avoid another commoner to transfer.
						transfered = true;
					} else {
						u.set_task(unit::TASK_NONE);
					}

				} else if (!traded && !(derived_random % trade_ratio)) {
					u.set_task(unit::TASK_TRADE);
					u.set_goto(candidate[derived_random % candidate.size()]->get_location());
					gotos.push_back(std::make_pair<unit*, int>(city, index));
					// avoid another commoner to trade.
					traded = true;
				} else {
					u.set_task(unit::TASK_NONE);
				}
			}
		}
	}
}

void play_controller::commoner_back(artifical* entered_city, artifical* belong_city, const map_location& src_loc)
{
	unit& back_commoner = *entered_city->reside_commoners().back();
	const map_location& entered_loc = entered_city->get_location();

	VALIDATE(entered_city != belong_city, "commoner_back, entered_city is same as belong_city!");

	std::vector<map_location> steps;
	const std::vector<map_location>& r = road(entered_city->cityno(), belong_city->cityno());
	std::vector<map_location>::const_iterator stand_it = std::find(r.begin(), r.end(), src_loc);
	bool direct_road = r.front() == entered_loc;
	if (direct_road) {
		std::advance(stand_it, 1);
		std::copy(r.begin(), stand_it, std::back_inserter(steps));
	} else {
		for (std::vector<map_location>::const_reverse_iterator it2 = r.rbegin(); it2 != r.rend(); ++ it2) {
			const map_location& l = *it2;
			steps.push_back(l);
			if (l == src_loc) break;
		}
	}
	unit_display::move_unit(steps, back_commoner, teams_);
	back_commoner.set_movement(0);

	if (!units_.city_from_loc(src_loc)) {
		// end at: not-city
		back_commoner.set_from(entered_city->get_location());
		back_commoner.set_goto(belong_city->get_location());
		back_commoner.set_cityno(belong_city->cityno());
		units_.add(src_loc, &back_commoner);
	} else {
		// end at: city
		belong_city->commoner_come_into(&back_commoner);
	}
	entered_city->commoner_go_out(entered_city->reside_commoners().size() - 1);
}

void play_controller::do_commoner(team& current_team)
{
	std::vector<std::pair<unit*, int> > ping, pang;
	std::vector<std::pair<unit*, int> >* curr = &ping, *background = &pang;
	map_location goto_loc;
	std::pair<unit*, int> param;

	recorder.do_commoner();

	calculate_commoner_gotos(current_team, *curr);

	int times = 0;
	while (++ times <= 2 && !curr->empty()) {
		for (std::vector<std::pair<unit*, int> >::iterator g = curr->begin(); g != curr->end(); ++g) {
			artifical* belong_city;
			const map_location src_loc = g->first->get_location();
			// desired move troop may move into one city. after it, g->first became to access denied.
			// so update goto before move_unit.
			if (g->second >= 0) {
				belong_city = unit_2_artifical(g->first);
				goto_loc = belong_city->reside_commoners()[g->second]->get_goto();
			} else {
				belong_city = units_.city_from_cityno(g->first->cityno());
				goto_loc = g->first->get_goto();
			}
			const map_location arrived_at = move_unit(false, current_team, *g, goto_loc, false);
			if (g->second < 0 && arrived_at == src_loc) {
				const unit& u = *units_.find(src_loc);
				if (u.movement_left()) {
					background->push_back(*g);
				}
			}
		}
		curr->clear();
		if (curr == &ping) {
			curr = &pang;
			background = &ping;
		} else {
			curr = &ping;
			background = &pang;
		}
	}
}

map_location play_controller::move_unit(bool troop, team& current_team, const std::pair<unit*, int>& pair, map_location to, bool dst_must_reachable)
{
	unit* unit_ptr = NULL;
	if (pair.second >= 0) {
		if (troop) {
			unit_ptr = unit_2_artifical(pair.first)->reside_troops()[pair.second];
		} else {
			unit_ptr = unit_2_artifical(pair.first)->reside_commoners()[pair.second];
		}
	} else {
		unit_ptr = pair.first;
	}

	map_location from = unit_ptr->get_location();
	map_location unit_location_ = from;

	if (from != to) {
		// it shouldn't use move_spectator of unit_ptr! when expediate, un will became invalid.
		move_unit_spectator move_spectator(units_);
		std::vector<map_location> steps;

		if (troop) {
			// check_before
			//do an A*-search

			const pathfind::shortest_path_calculator calc(*unit_ptr, current_team, units_, teams_, map_, false, true);
			// double stop_at = dst_must_reachable? 10000.0: calc.getUnitHoldValue() * 3 + 10000.0;
			double stop_at = dst_must_reachable? 10000.0: calc.getUnitHoldValue() + 10000.0;

			//allowed teleports
			std::set<map_location> allowed_teleports;
			pathfind::plain_route route_ = a_star_search(from, to, stop_at, &calc, map_.w(), map_.h(), &allowed_teleports);

			if (route_.steps.empty()) {
				return map_location();
			}
			// modify! last location must no unit on it.
			while (!dst_must_reachable) {
				to = route_.steps.back();
				if (!units_.count(to)) {
					break;
				} else if (artifical* c = units_.city_from_loc(to)) {
					if (c->side() == unit_ptr->side()) { 
						// same side's city. 
						break;
					}
				}
				if (to == from) {
					// A(city)(city), A will enter it
					return map_location();
				}
				route_.steps.pop_back();
			}
			steps = route_.steps;
		} else {
			// cannot use a_star_search, when expeting, returned unit on loc of city is expeting troop! 
			// play_controll::road will mistake, avoid it!
			artifical* start_city;
			artifical* stop_city;
			bool forward = !unit_ptr->get_from().valid();
			if (forward) {
				start_city = units_.city_from_cityno(unit_ptr->cityno());
				stop_city = units_.city_from_loc(to);
			} else {
				start_city = units_.city_from_loc(unit_ptr->get_from());
				stop_city = units_.city_from_cityno(unit_ptr->cityno());
			}
			
			const std::vector<map_location>& r = road(start_city->cityno(), stop_city->cityno());
			std::vector<map_location>::const_iterator from_it = std::find(r.begin(), r.end(), from);
			int index = std::distance(r.begin(), from_it);
			int stop_at;
			bool direct_road = r.front() != to;
			if (direct_road) {
				// maybe expedite, +1
				stop_at = index + unit_ptr->movement_left() + 1;
				if (stop_at >= (int)r.size()) {
					stop_at = (int)r.size() - 1;
				}
				for ( ; index <= stop_at; index ++) {
					steps.push_back(r[index]);
				}
			} else {
				stop_at = 0;
				if (index > unit_ptr->movement_left() + 1) {
					stop_at = index - (unit_ptr->movement_left() + 1);
				}
				for ( ; index >= stop_at; index --) {
					steps.push_back(r[index]);
				}
			}
		}
		// do_execute
		move_spectator.set_unit(units_.find(from));
		
		bool gamestate_changed = false;

		if (!steps.empty()) {
			if (pair.second >= 0) {
				artifical* expediting_city = unit_2_artifical(pair.first);
				// std::vector<unit*>& reside_troops = expediting_city->reside_troops();
				gui_->place_expedite_city(*expediting_city);
				units_.set_expediting(expediting_city, troop, pair.second);
				::move_unit(
					/*move_unit_spectator* move_spectator*/ &move_spectator,
					/*std::vector<map_location> route*/ steps,
					/*replay* move_recorder*/ &recorder,
					/*undo_list* undo_stack*/ NULL,
					/*bool show_move*/ true,
					/*map_location *next_unit*/ NULL,
					/*bool continue_move*/ true, //@todo: 1.7 set to false after implemeting interrupt awareness
					/*bool should_clear_shroud*/ true,
					/*bool is_replay*/ false);
				gui_->remove_expedite_city();
				units_.set_expediting();
				if (!move_spectator.get_unit().valid() || move_spectator.get_unit()->get_location() != from) {
					// move_unit may be not move! reside troop ratain in city.
					if (troop) {
						expediting_city->troop_go_out(units_.last_expedite_index());
					} else {
						expediting_city->commoner_go_out(units_.last_expedite_index());
					}
				} else {
					unit_ptr->remove_movement_ai();
				}

			} else {
				::move_unit(
					/*move_unit_spectator* move_spectator*/ &move_spectator,
					/*std::vector<map_location> route*/ steps,
					/*replay* move_recorder*/ &recorder,
					/*undo_list* undo_stack*/ NULL,
					/*bool show_move*/ true,
					/*map_location *next_unit*/ NULL,
					/*bool continue_move*/ true, //@todo: 1.7 set to false after implemeting interrupt awareness
					/*bool should_clear_shroud*/ true,
					/*bool is_replay*/ false);
			}
			// once unit moved one or more grid, unit_ptr may invalid. below code must not use un!
			if (!move_spectator.get_unit().valid()) {
				// mover is died!
				unit_location_ = map_location();
			} else if (move_spectator.get_ambusher().valid() || !move_spectator.get_seen_enemies().empty() || !move_spectator.get_seen_friends().empty() ) {
				unit_location_ = move_spectator.get_unit()->get_location();
				gamestate_changed = true;
			} else if (move_spectator.get_unit().valid()){
				unit_location_ = move_spectator.get_unit()->get_location();
				const unit& u = *move_spectator.get_unit();
				if (unit_location_ != from || (u.is_commoner() && !u.movement_left() && u.get_from().valid())) {
					gamestate_changed = true;
				}
			}
		}

		if (gamestate_changed) {

			unit_map::iterator un = units_.find(unit_location_);
			if (unit_location_ != from && un->movement_left() > 0) {
				bool remove_movement_ = true;
				bool remove_attacks_ = false;
				if (remove_movement_){
					un->remove_movement_ai();
				}
				if (remove_attacks_){
					un->remove_attacks_ai();
				}
			}
		}
/*
		try {
			if (gamestate_changed) {
				manager::raise_gamestate_changed();
			}
			resources::controller->check_victory();
			resources::controller->check_end_level();
		} catch (...) {
			throw;
		}
*/
		// check_after
		if (unit_location_ != from && unit_location_ != to) {
/*
			// We've been ambushed; find the ambushing unit and attack them.
			adjacent_tiles_array locs;
			get_adjacent_tiles(unit_location_, locs.data());
			for(adjacent_tiles_array::const_iterator adj_i = locs.begin(); adj_i != locs.end(); ++adj_i) {
				const unit_map::const_iterator itor = units_.find(*adj_i);
				if(itor != units_.end() && current_team_.is_enemy(itor->second.side()) &&
				   !itor->second.incapacitated()) {
					// here, only melee weapon is valid, need modify!!
					battle_context bc(units_, unit_location_, *adj_i, -1, -1, aggression_);
					attack_enemy(unit_location_,itor->first,bc.get_attacker_stats().attack_num,bc.get_defender_stats().attack_num);
					break;
				}
			}
*/
		}

		return unit_location_;
	} else {
		return from;
	}
}

void play_controller::rpg_update()
{
	std::string message;
	int incident = game_events::INCIDENT_APPOINT;
	if (rpg::stratum == hero_stratum_citizen) {
		bool control_more = false;
		if (rpg::h->meritorious_ >= 8000) {
			artifical* city = units_.city_from_cityno(rpg::h->city_);

			city->select_mayor(rpg::h);

			// all reside/field troop to human
			std::vector<unit*>& resides = city->reside_troops();
			for (std::vector<unit*>::iterator it = resides.begin(); it != resides.end(); ++ it) {
				unit& u = **it;
				u.set_human(true);
			}
			std::vector<unit*>& fields = city->field_troops();
			for (std::vector<unit*>::iterator it = fields.begin(); it != fields.end(); ++ it) {
				unit& u = **it;
				if (!u.human()) {
					u.set_human(true);
					u.set_goto(map_location());
					gui_->refresh_access_troops(u.side() - 1, game_display::REFRESH_INSERT, &u);
					if (!unit_can_move(u)) {
						gui_->refresh_access_troops(u.side() - 1, game_display::REFRESH_DISABLE, &u);
					}
				}
			}
			// rpg::forbids = 10;
		} else if (rpg::h->meritorious_ >= 5000 && rpg::humans.size() < 4) {
			control_more = true;
		} else if (rpg::h->meritorious_ >= 2500 && rpg::humans.size() < 3) {
			control_more = true;
		} else if (rpg::h->meritorious_ >= 1000 && rpg::humans.size() < 2) {
			control_more = true;
		}
		if (control_more) {
			std::vector<const unit*> units_ptr;
			team& current_team = (*resources::teams)[rpg::h->side_];
			std::vector<artifical*>& holded_cities = current_team.holded_cities();
			for (std::vector<artifical*>::iterator it = holded_cities.begin(); it != holded_cities.end(); ++ it) {
				artifical* city = *it;
				std::vector<unit*>& resides = city->reside_troops();
				for (std::vector<unit*>::iterator it = resides.begin(); it != resides.end(); ++ it) {
					unit& u = **it;
					if (!u.human()) {
						units_ptr.push_back(&u);
					}
				}
			
				std::vector<unit*>& fields = city->field_troops();
				for (std::vector<unit*>::iterator it = fields.begin(); it != fields.end(); ++ it) {
					unit& u = **it;
					if (!u.human()) {
						units_ptr.push_back(&u);
					}
				}
			}

			if (units_ptr.empty()) {
				return;
			}
			message = _("You can control more troop.");
			game_events::show_hero_message(&heros_[229], NULL, message, incident);

			int res = -1;
			std::string name = "input";
			if (!replaying_) {
				gui2::tselect_unit dlg(*gui_, teams_, units_, heros_, units_ptr, gui2::tselect_unit::HUMAN);
				try {
					dlg.show(gui_->video());
				} catch(twml_exception& e) {
					e.show(*gui_);
				}

				res = dlg.troop_index();

				config cfg;
				cfg["value"] = res;
				recorder.user_input(name, cfg);
			} else {
				do_replay_handle(player_number_, name);
				const config *action = get_replay_source().get_next_action();
				if (!action || !*(action = &action->child(name))) {
					replay::process_error("[" + name + "] expected but none found\n");
				}
				res = action->get("value")->to_int(-1);
			}
			unit& u = *(const_cast<unit*>(units_ptr[res]));
			// end this turn
			u.set_movement(0);
			u.set_attacks(0);

			u.set_human(true);
			u.set_goto(map_location());
			if (!units_.city_from_loc(u.get_location())) {
				gui_->refresh_access_troops(u.side() - 1, game_display::REFRESH_INSERT, &u);
				if (!unit_can_move(u)) {
					gui_->refresh_access_troops(u.side() - 1, game_display::REFRESH_DISABLE, &u);
				}
			}
			// rpg::forbids = 10;
		}
	}
}

// return value:
//   true: executed ally operation. so next need do some post operation, for example 
bool play_controller::ally_all_ai(bool force)
{
	if (all_ai_allied_) {
		return false;
	}
	if (network::nconnections() > 0) {
		// in network state, don't ally all ai.
		return false;
	}

	int human_team_index = -1;
	int allied_teams = 0;

	std::vector<artifical*> cities;
	for (size_t side = 0; side != teams_.size(); side ++) {
		if (teams_[side].is_human()) {
			human_team_index = side;
		}
		std::vector<artifical*>& side_cities = teams_[side].holded_cities();
		for (std::vector<artifical*>::iterator itor = side_cities.begin(); itor != side_cities.end(); ++ itor) {
			artifical* city = *itor;
			cities.push_back(city);
		}
	}
	if (human_team_index < 0) {
		return false;
	}
	if (force || teams_[human_team_index].holded_cities().size() * 3 >= cities.size()) {
		// erase all strategy
		for (size_t i = 0; i < teams_.size(); i ++) {
			teams_[i].erase_strategies(false);
		}

		// all ai sides concluded treaty of alliance
		for (int from_side = 0; from_side != teams_.size(); from_side ++) {
			if (from_side == human_team_index || !teams_[from_side].holded_cities().size() || teams_[from_side].is_empty() || !teams_[human_team_index].is_enemy(from_side + 1)) {
				continue;
			}
			for (int to_side = 0; to_side != teams_.size(); to_side ++) {
				if (to_side == human_team_index || to_side == from_side || !teams_[to_side].holded_cities().size() || teams_[to_side].is_empty() || !teams_[human_team_index].is_enemy(to_side + 1)) {
					continue;
				}
				if (teams_[from_side].is_enemy(to_side + 1)) {
					teams_[from_side].set_ally(to_side + 1);
				}
			}
			allied_teams ++;
		}
		all_ai_allied_ = true;
		return allied_teams >= 2? true: false;
	}
	return false;
}

artifical* play_controller::decide_ai_capital() const
{
	artifical* capital_of_ai = NULL;

	int score, best_score = -1;
	std::vector<artifical*> ai_holded_cities;
	for (size_t side = 0; side != teams_.size(); side ++) {
		if (teams_[side].is_human()) {
			continue;
		}
		const std::vector<artifical*>& side_cities = teams_[side].holded_cities();
		for (std::vector<artifical*>::const_iterator itor = side_cities.begin(); itor != side_cities.end(); ++ itor) {
			artifical* city = *itor;
			ai_holded_cities.push_back(city);
			score = city->hitpoints() + (city->reside_troops().size() + city->field_troops().size() + city->fresh_heros().size() + city->finish_heros().size()) * 10;
			if (score > best_score) {
				best_score = score;
				capital_of_ai = city;
			}
		}
	}
	return capital_of_ai;
}

void play_controller::recommend()
{
	std::vector<team>& teams = teams_;

	std::string message;
	artifical* selected_city = NULL;
	hero* selected_hero = NULL;

	std::vector<artifical*> cities, full_cities;
	for (size_t side = 0; side != teams.size(); side ++) {
		std::vector<artifical*>& side_cities = teams[side].holded_cities();
		for (std::vector<artifical*>::iterator itor = side_cities.begin(); itor != side_cities.end(); ++ itor) {
			artifical* city = *itor;
			full_cities.push_back(city);
			if (!city->wander_heros().size()) {
				continue;
			}
			cities.push_back(city);
		}
	}
	if (!cities.size()) {
		return;
	}
	// move max_move_wander_heros wander hero
	int random = 1;
	int hero_index = 0;
	if (tent::mode != TOWER_MODE && full_cities.size() > 1) {
		artifical* from_city = cities[turn() % cities.size()];
		hero* leader = teams[from_city->side() - 1].leader();
		artifical* to_city = full_cities[(turn() + random) % full_cities.size()];
		
		while (to_city == from_city) {
			to_city = full_cities[(turn() + random) % full_cities.size()];
			random ++;
		}
		std::vector<hero*>& from_wander_heros = from_city->wander_heros();
		int max_move_wander_heros = 2;
		for (std::vector<hero*>::iterator itor = from_wander_heros.begin(); hero_index < max_move_wander_heros && itor != from_wander_heros.end();) {
			hero* h = *itor;
			if (from_city->side() == team::empty_side_ || h->loyalty(*leader) < game_config::move_loyalty_threshold) {
				to_city->wander_into(*h, teams[from_city->side() - 1].is_human()? false: true);
				itor = from_wander_heros.erase(itor);
				hero_index ++;
			} else {
				++ itor;
			}
		}
	}
	// recommand wander hero
	if (cities.size() == 1 && !cities[0]->wander_heros().size()) {
		return;
	}

	random = get_random();
	do {
		selected_city = cities[random % cities.size()];
		random ++;
	} while (!selected_city->wander_heros().size());
	std::vector<hero*>& wander_heros = selected_city->wander_heros();
	hero_index = random % wander_heros.size();
	std::vector<hero*>::iterator erase_it = wander_heros.begin();
	std::advance(erase_it, hero_index);

	selected_hero = wander_heros[hero_index];

	if (tent::mode != TOWER_MODE) {
		hero* leader = teams[selected_city->side() - 1].leader();
		if (selected_city->side() == team::empty_side_ || selected_hero->loyalty(*leader) < game_config::wander_loyalty_threshold) {
			artifical* into_city = cities[random % cities.size()];
			into_city->wander_into(*selected_hero, teams[selected_city->side() - 1].is_human()? false: true);
			wander_heros.erase(erase_it);
			return;
		}
	}
	// update heros list in artifical
	selected_hero->status_ = hero_status_idle;
	selected_hero->side_ = selected_city->side() - 1;
	selected_city->fresh_heros().push_back(selected_hero);
	wander_heros.erase(erase_it);

	message = _("Let me join in. I will do my best to maintenance our honor.");

	map_location loc2(MAGIC_HERO, selected_hero->number_);
	game_events::fire("post_recommend", selected_city->get_location(), loc2);

	show_hero_message(selected_hero, selected_city, message, game_events::INCIDENT_RECOMMENDONESELF);
}

bool play_controller::do_ally(bool alignment, int my_side, int to_ally_side, int emissary_number, int target_side, int strategy_index)
{
	if (!alignment) {
		// current not support discard alignment.
		return false;
	}

	hero& emissary = heros_[emissary_number];
	team& my_team = teams_[my_side - 1];
	team& to_ally_team = teams_[to_ally_side - 1];
	team& target_team = teams_[target_side - 1];
	
	std::vector<strategy>& strategies = my_team.strategies();
	strategy& s = strategies[strategy_index];
	artifical* target_city = units_.city_from_cityno(s.target_);

	// display dialog
	utils::string_map symbols;
	symbols["city"] = help::tintegrate::generate_format(target_city->name(), help::tintegrate::object_color);
	symbols["first"] = help::tintegrate::generate_format(my_team.name(), help::tintegrate::hero_color);
	symbols["second"] = help::tintegrate::generate_format(to_ally_team.name(), help::tintegrate::hero_color);
	
	if (s.type_ != strategy::DEFEND || !target_team.is_human() || !to_ally_team.is_enemy(target_side)) {
		// calculate to_ally_team aggreen with whether or not.
		std::vector<mr_data> mrs;
		units_.calculate_mrs_data(mrs, to_ally_side, false);
		if (!mrs[0].enemy_cities.empty() && mrs[0].enemy_cities[0]->side() == my_side) {
			// game_events::show_hero_message(&heros[hero::number_scout], NULL, vgettext("$first wants to ally with $second, but $second refuse.", symbols), game_events::INCIDENT_ALLY);
			return false;
		}
	}

	my_team.set_ally(to_ally_side);
	to_ally_team.set_ally(my_side);

	s.allies_.insert(to_ally_side);

	// emissary from fresh to finish.
	artifical* city = units_.city_from_cityno(emissary.city_);
	city->hero_go_out(emissary);
	city->finish_into(emissary, hero_status_backing);

	std::string message;
	if (s.type_ == strategy::AGGRESS) {
		message = vgettext("In order to aggress upon $city, $first and $second concluded treaty of alliance.", symbols);
	} else if (s.type_ == strategy::DEFEND) {
		message = vgettext("In order to defend $city, $first and $second concluded treaty of alliance.", symbols);
	} else {
		VALIDATE(false, "ally, unknown strategy type.");
	}
	if (my_team.is_human() || to_ally_team.is_human() || target_team.is_human()) {
		game_events::show_hero_message(&heros_[hero::number_scout], NULL, message, game_events::INCIDENT_ALLY);
	}
	return true;
}

void play_controller::construct_road()
{
	VALIDATE(!unit_map::scout_unit_, "play_controller::construct_road, detect unit_map::scout_unit_ isn't null.");
	const unit_type *ut = unit_types.find_scout();
	std::vector<const hero*> scout_heros;
	scout_heros.push_back(&heros_[hero::number_scout]);
	
	// select a valid cityno
	int cityno = -1;
	city_map& citys = units_.get_city_map();
	for (city_map::const_iterator i = citys.begin(); i != citys.end(); ++ i) {
		cityno = i->cityno();
		break;
	}
	if (cityno == -1) {
		throw game::game_error("You must define at least one city.");
	}
	// recruit
	type_heros_pair pair(ut, scout_heros);
	unit_map::scout_unit_ = new unit(units_, heros_, teams_, pair, cityno, false);

	std::multimap<int, int> roads_from_cfg;
	std::vector<std::string> vstr = utils::parenthetical_split(level_["roads"]);
	for (std::vector<std::string>::const_iterator it = vstr.begin(); it != vstr.end(); ++ it) {
		const std::vector<std::string> vstr1 = utils::split(*it);
		if (vstr1.size() == 2) {
			int no1 = lexical_cast_default<int>(vstr1[0]);
			int no2 = lexical_cast_default<int>(vstr1[1]);
			VALIDATE(units_.city_from_cityno(no1), "play_controller::construct_road, invalid cityno " + vstr1[0]);
			VALIDATE(units_.city_from_cityno(no2), "play_controller::construct_road, invalid cityno " + vstr1[1]);
			VALIDATE(no1 != no2, "play_controller::construct_road, invalid road " + *it);
			roads_from_cfg.insert(std::make_pair(no1, no2));
		}
	}

	artifical* a;
	artifical* b;
	std::map<artifical*, std::set<int> > roaded_cities;
	std::map<artifical*, std::set<int> >::iterator find;
	size_t shortest_road = -1;
	std::pair<int, int> shortest_cities;
	for (std::multimap<int, int>::const_iterator it = roads_from_cfg.begin(); it != roads_from_cfg.end(); ++ it) {
		if (it->first < it->second) {
			a = units_.city_from_cityno(it->first);
			b = units_.city_from_cityno(it->second);
		} else {
			a = units_.city_from_cityno(it->second);
			b = units_.city_from_cityno(it->first);
		}
		find = roaded_cities.find(a);
		if (find == roaded_cities.end()) {
			std::set<int> s;
			s.insert(b->cityno());
			roaded_cities.insert(std::make_pair(a, s));
		} else {
			find->second.insert(b->cityno());
		}
		find = roaded_cities.find(b);
		if (find == roaded_cities.end()) {
			std::set<int> s;
			s.insert(a->cityno());
			roaded_cities.insert(std::make_pair(b, s));
		} else {
			find->second.insert(a->cityno());
		}

		const pathfind::emergency_path_calculator calc(*unit_map::scout_unit_, map_);
		double stop_at = 10000.0;
		//allowed teleports
		std::set<map_location> allowed_teleports;
		// NOTICE! although start and end are same, but rank filp, result maybe differenrt! so use cityno to make sure same.
		pathfind::plain_route route = a_star_search(a->get_location(), b->get_location(), stop_at, &calc, map_.w(), map_.h(), &allowed_teleports);
		if (!route.steps.empty()) {
			if (route.steps.size() < shortest_road) {
				shortest_road = route.steps.size();
				shortest_cities = std::make_pair(a->cityno(), b->cityno());
			}
			roads_.insert(std::make_pair(std::make_pair(a->cityno(), b->cityno()), route.steps));
		}
	}
	if (!roads_.empty()) {
		// avoid one trade can city I thoughtout city II.
		const unit_type_data::unit_type_map& types = unit_types.types();
		for (unit_type_data::unit_type_map::const_iterator it = types.begin(); it != types.end(); ++ it) {
			const unit_type& ut = it->second;
			if (hero::is_commoner(ut.master())) {
				if (ut.movement() + 4 >= (int)shortest_road) {
					std::stringstream err;
					utils::string_map symbols;
					symbols["from"] = units_.city_from_cityno(shortest_cities.first)->name();
					symbols["to"] = units_.city_from_cityno(shortest_cities.second)->name();
					err << vgettext("map error, too short road from $from to $to!", symbols);
					VALIDATE(false, err.str());
				}
			}
		}
	
		for (std::map<artifical*, std::set<int> >::const_iterator it = roaded_cities.begin(); it != roaded_cities.end(); ++ it) {
			artifical& city = *it->first;
			city.set_roaded_cities(it->second);
		}

		gui_->construct_road_locs(roads_);
	}
}

const std::vector<map_location>& play_controller::road(int a, int b) const
{
	VALIDATE(a != b, "road, must tow city when get road.");
	std::map<std::pair<int, int>, std::vector<map_location> >::const_iterator find;
	int new_a, new_b;

	if (a < b) {
		new_a = a;
		new_b = b;
	} else {
		new_a = b;
		new_b = a;
	}
	find = roads_.find(std::make_pair(new_a, new_b));
	if (find != roads_.end()) {
		return find->second;
	}
	return null_val::vector_location;
}

const std::vector<map_location>& play_controller::road(const unit& u) const
{
	const artifical* start_city;
	const artifical* stop_city;
	bool forward = !u.get_from().valid();
	if (forward) {
		start_city = units_.city_from_cityno(u.cityno());
		stop_city = units_.city_from_loc(u.get_goto());
	} else {
		start_city = units_.city_from_loc(u.get_from());
		stop_city = units_.city_from_cityno(u.cityno());
	}
	return road(start_city->cityno(), stop_city->cityno());
}

void play_controller::do_build(unit& builder, const unit_type* ut, const map_location& art_loc, int cost)
{
	// below maybe use animation, use disable_commands to avoid re-enter.
	const events::command_disabler disable_commands;
	const map_location& builder_loc = builder.get_location();
	bool replay = cost >= 0;
	team& builder_team = teams_[builder.side() - 1];

	gui_->remove_temporary_unit();
	// 单位站的格子和建造格子肯定相邻,不必移动
	// 生成建筑
	if (cost < 0) {
		int cost_exponent = teams_[builder.side() - 1].cost_exponent();
		cost = ut->cost() * cost_exponent / 100;
		if (tent::mode == TOWER_MODE && builder_team.is_human()) {
			// increase wall's cost.
			cost *= tower_cost_ratio;
		}
	}
	builder_team.spend_gold(cost);

	// find city that owner this economy grid.
	artifical* ownership_city = NULL;
	std::map<const map_location, int>::const_iterator it = unit_map::economy_areas_.find(art_loc);
	if (it != unit_map::economy_areas_.end()) {
		ownership_city = units_.city_from_cityno(it->second);
	} else if (tent::mode != TOWER_MODE && (ut == unit_types.find_keep() || ut == unit_types.find_wall())) {
		ownership_city = find_city_for_wall(units_, art_loc);
	}

	if (!ownership_city) {
		// this artifical isn't in city's economy area.
		ownership_city = units_.city_from_cityno(builder.cityno());
	}

	if (!replay) {
		recorder.add_build(ut, ownership_city->get_location(), builder_loc, art_loc, cost);
		// 取消单位选中
		mouse_handler_.select_hex(map_location(), false);
	}

	// reconstruct artifical that has trait
	std::vector<const hero*> v;
	if (ut->master() != HEROS_INVALID_NUMBER) {
		v.push_back(&heros_[ut->master()]);
	} else {
		v.push_back(&ownership_city->master());
	}
	type_heros_pair pair(ut, v);
	artifical new_art(units_, heros_, teams_, pair, ownership_city->cityno(), true);

	units_.add(art_loc, &new_art);
	
	if (tent::mode != TOWER_MODE) {
		// build hero endes turn.
		builder.set_movement(0);
		builder.set_attacks(0);
		builder.set_goto(map_location());
	}
}

int play_controller::human_players()
{
	return human_players_;
}