push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_productionsite_type {
   name = "barbarians_gamekeepers_hut",
   -- TRANSLATORS: This is a building name used in lists of buildings
   descname = pgettext("barbarians_building", "Gamekeeper’s Hut"),
   icon = dirname .. "menu.png",
   size = "small",

   buildcost = {
      log = 4,
      granite = 1
   },
   return_on_dismantle = {
      log = 1,
      granite = 1
   },

   animation_directory = dirname,
   animations = {
      unoccupied = {
         hotspot = { 44, 43 },
      },
   },

   spritesheets = {
      build = {
         frames = 4,
         rows = 2,
         columns = 2,
         hotspot = { 44, 43 }
      },
      idle = {
         frames = 20,
         rows = 5,
         columns = 4,
         hotspot = { 44, 43 }
      },
   },

   aihints = {
      prohibited_till = 510,
      basic_amount = 1
   },

   working_positions = {
      barbarians_gamekeeper = 1
   },

   programs = {
      main = {
         -- TRANSLATORS: Completed/Skipped/Did not start working because ...
         descname = _("working"),
         actions = {
            -- time of worker: 11.2-25.6 sec, min+max average 18.4 sec
            -- min. time total: 11.2 + 53 = 64.2 sec
            -- max. time total: 25.6 + 53 = 78.6 sec
            -- avg. time total: 18.4 + 53 = 71.4 sec
            "callworker=release",
            "sleep=duration:53s"
         }
      },
   },
}

pop_textdomain()
