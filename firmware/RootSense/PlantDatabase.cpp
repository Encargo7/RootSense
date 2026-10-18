#include "PlantDatabase.h"

#include <string.h>

// To add your own plant, copy a line from the group it behaves most like and
// change the key (unique, lowercase, max 15 characters), name and numbers.
// Rule: waterBelowPct < targetPct < soggyAbovePct.
static const PlantProfile PLANTS[] = {
  // key             name                              group                 below target soggy  tip
  {"generic",       "Generic houseplant",             "General",              35,  75,  100, "Water when the top few cm of soil are dry."},

  {"cactus",        "Cactus",                         "Succulents & cacti",   10,  60,   85, "Let it dry out completely between waterings."},
  {"succulent",     "Succulent (echeveria etc.)",     "Succulents & cacti",   15,  65,   90, "Soak and dry: water deeply, then wait until dry."},
  {"aloe",          "Aloe vera",                      "Succulents & cacti",   15,  65,   90, "Very drought tolerant; soggy soil causes rot."},
  {"jade",          "Jade plant",                     "Succulents & cacti",   15,  65,   90, "Wrinkled leaves = thirsty, mushy leaves = too wet."},
  {"snake",         "Snake plant",                    "Succulents & cacti",   15,  60,   90, "Thrives on neglect; water even less in winter."},
  {"zz",            "ZZ plant",                       "Succulents & cacti",   15,  60,   90, "Stores water in its roots; err on the dry side."},

  {"hoya",          "Hoya (wax plant)",               "Tropical foliage",     25,  65,   95, "Semi-succulent: let the soil mostly dry out."},
  {"pothos",        "Pothos",                         "Tropical foliage",     35,  75,  100, "Drooping leaves mean it is thirsty."},
  {"monstera",      "Monstera",                       "Tropical foliage",     35,  75,  100, "Water when the top 5 cm are dry."},
  {"philodendron",  "Philodendron",                   "Tropical foliage",     35,  75,  100, "Yellow lower leaves often mean too much water."},
  {"spider",        "Spider plant",                   "Tropical foliage",     35,  75,  100, "Brown tips can come from tap-water salts."},
  {"rubber",        "Rubber plant (Ficus elastica)",  "Tropical foliage",     30,  70,   95, "Needs less water in winter."},
  {"fiddle",        "Fiddle-leaf fig",                "Tropical foliage",     35,  70,   95, "Hates both drought and soggy roots: be consistent."},
  {"dracaena",      "Dracaena",                       "Tropical foliage",     30,  70,   95, "Let the top of the soil dry out."},
  {"aglaonema",     "Chinese evergreen",              "Tropical foliage",     35,  75,  100, "Keep lightly moist; tolerates low light."},
  {"palm",          "Parlor palm",                    "Tropical foliage",     40,  80,  100, "Likes even moisture but never standing water."},

  {"peacelily",     "Peace lily",                     "Moisture lovers",      45,  85,  105, "Wilts dramatically when dry, recovers quickly."},
  {"fern",          "Fern (Boston, maidenhair)",      "Moisture lovers",      55,  90,  110, "Never let it dry out completely."},
  {"calathea",      "Calathea / prayer plant",        "Moisture lovers",      50,  85,  105, "Curling leaves mean it is thirsty."},
  {"africanviolet", "African violet",                 "Moisture lovers",      45,  80,  100, "Water the soil, not the leaves."},

  {"basil",         "Basil",                          "Herbs",                45,  80,  100, "Wilts quickly when dry."},
  {"mint",          "Mint",                           "Herbs",                55,  90,  110, "Loves moisture."},
  {"parsley",       "Parsley",                        "Herbs",                45,  80,  100, "Keep evenly moist."},
  {"cilantro",      "Cilantro",                       "Herbs",                45,  80,  100, "Stress from drought makes it flower sooner."},
  {"rosemary",      "Rosemary",                       "Herbs",                25,  65,   90, "Mediterranean: likes to dry out between waterings."},
  {"thyme",         "Thyme",                          "Herbs",                20,  60,   90, "Mediterranean: keep it on the dry side."},
  {"lavender",      "Lavender",                       "Herbs",                20,  60,   85, "Hates wet roots; needs fast-draining soil."},

  {"tomato",        "Tomato",                         "Vegetables & fruit",   50,  85,  105, "Uneven watering can make the fruit crack."},
  {"pepper",        "Pepper / chili",                 "Vegetables & fruit",   45,  80,  100, "Let the top dry slightly between waterings."},
  {"lettuce",       "Lettuce / leafy greens",         "Vegetables & fruit",   55,  90,  105, "Shallow roots: keep the top of the soil moist."},
  {"strawberry",    "Strawberry",                     "Vegetables & fruit",   50,  85,  105, "Keep moist, especially while fruiting."},
};

static const size_t PLANT_COUNT = sizeof(PLANTS) / sizeof(PLANTS[0]);

namespace plants {

size_t count() { return PLANT_COUNT; }

const PlantProfile& at(size_t index) { return PLANTS[index < PLANT_COUNT ? index : 0]; }

const PlantProfile* find(const char* key) {
  if (key == nullptr) return nullptr;
  for (size_t i = 0; i < PLANT_COUNT; i++) {
    if (strcmp(PLANTS[i].key, key) == 0) return &PLANTS[i];
  }
  return nullptr;
}

const PlantProfile& fallback() { return PLANTS[0]; }

}  // namespace plants
