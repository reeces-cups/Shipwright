#include "fill.hpp"

#include "custom_messages.hpp"
#include "dungeon.hpp"
#include "../context.h"
#include "item_pool.hpp"
#include "location_access.hpp"
#include "logic.hpp"
#include "random.hpp"
#include "spoiler_log.hpp"
#include "starting_inventory.hpp"
#include "hints.hpp"
#include "hint_list.hpp"
#include "entrance.hpp"
#include "shops.hpp"
//#include "debug.hpp"
#include "soh/Enhancements/randomizer/static_data.h"

#include <vector>
#include <list>
#include <spdlog/spdlog.h>

using namespace CustomMessages;
using namespace Logic;
using namespace Settings;

static bool placementFailure = false;

static void RemoveStartingItemsFromPool() {
  for (RandomizerGet startingItem : StartingInventory) {
    for (size_t i = 0; i < ItemPool.size(); i++) {
      if (startingItem == RG_BIGGORON_SWORD) {
        if (ItemPool[i] == RG_GIANTS_KNIFE || ItemPool[i] == RG_BIGGORON_SWORD) {
          ItemPool[i] = GetJunkItem();
        }
        continue;
      } else if (startingItem == ItemPool[i] || (Rando::StaticData::RetrieveItem(startingItem).IsBottleItem() &&
                                                 Rando::StaticData::RetrieveItem(ItemPool[i]).IsBottleItem())) {
        if (AdditionalHeartContainers > 0 && 
          (startingItem == RG_PIECE_OF_HEART || startingItem == RG_TREASURE_GAME_HEART)) {
          ItemPool[i] = RG_HEART_CONTAINER;
          AdditionalHeartContainers--;
        } else {
          ItemPool[i] = GetJunkItem();
        }
        break;
      }
    }
  }
}

//This function will propogate Time of Day access through the entrance
static bool UpdateToDAccess(Entrance* entrance, SearchMode mode) {

  bool ageTimePropogated = false;

  //propogate childDay, childNight, adultDay, and adultNight separately
  Area* parent = entrance->GetParentRegion();
  Area* connection = entrance->GetConnectedRegion();

  if (!connection->childDay && parent->childDay && entrance->CheckConditionAtAgeTime(Logic::IsChild, AtDay)) {
    connection->childDay = true;
    ageTimePropogated = true;
  }
  if (!connection->childNight && parent->childNight && entrance->CheckConditionAtAgeTime(Logic::IsChild, AtNight)) {
    connection->childNight = true;
    ageTimePropogated = true;
  }
  if (!connection->adultDay && parent->adultDay && entrance->CheckConditionAtAgeTime(IsAdult, AtDay)) {
    connection->adultDay = true;
    ageTimePropogated = true;
  }
  if (!connection->adultNight && parent->adultNight && entrance->CheckConditionAtAgeTime(IsAdult, AtNight)) {
    connection->adultNight = true;
    ageTimePropogated = true;
  }

  //special check for temple of time
  bool propogateTimeTravel = mode != SearchMode::TimePassAccess && mode != SearchMode::TempleOfTimeAccess;
  if (!AreaTable(RR_ROOT)->Adult() && AreaTable(RR_TOT_BEYOND_DOOR_OF_TIME)->Child() && propogateTimeTravel) {
    AreaTable(RR_ROOT)->adultDay   = AreaTable(RR_TOT_BEYOND_DOOR_OF_TIME)->childDay;
    AreaTable(RR_ROOT)->adultNight = AreaTable(RR_TOT_BEYOND_DOOR_OF_TIME)->childNight;
  } else if (!AreaTable(RR_ROOT)->Child() && AreaTable(RR_TOT_BEYOND_DOOR_OF_TIME)->Adult() && propogateTimeTravel){
    AreaTable(RR_ROOT)->childDay   = AreaTable(RR_TOT_BEYOND_DOOR_OF_TIME)->adultDay;
    AreaTable(RR_ROOT)->childNight = AreaTable(RR_TOT_BEYOND_DOOR_OF_TIME)->adultNight;
  }

  return ageTimePropogated;
}

// Various checks that need to pass for the world to be validated as completable
static void ValidateWorldChecks(SearchMode& mode, bool checkPoeCollectorAccess, bool checkOtherEntranceAccess, std::vector<RandomizerRegion>& areaPool) {
  // Condition for validating Temple of Time Access
  if (mode == SearchMode::TempleOfTimeAccess && ((Settings::ResolvedStartingAge == AGE_CHILD && AreaTable(RR_TEMPLE_OF_TIME)->Adult()) || (Settings::ResolvedStartingAge == AGE_ADULT && AreaTable(RR_TEMPLE_OF_TIME)->Child()) || !checkOtherEntranceAccess)) {
    mode = SearchMode::ValidStartingRegion;
  }
  // Condition for validating a valid starting region
  if (mode == SearchMode::ValidStartingRegion) {
    bool childAccess = Settings::ResolvedStartingAge == AGE_CHILD || AreaTable(RR_TOT_BEYOND_DOOR_OF_TIME)->Child();
    bool adultAccess = Settings::ResolvedStartingAge == AGE_ADULT || AreaTable(RR_TOT_BEYOND_DOOR_OF_TIME)->Adult();

    Area* kokiri = AreaTable(RR_KOKIRI_FOREST);
    Area* kakariko = AreaTable(RR_KAKARIKO_VILLAGE);

    if ((childAccess && (kokiri->Child() || kakariko->Child())) ||
        (adultAccess && (kokiri->Adult() || kakariko->Adult())) ||
        !checkOtherEntranceAccess) {
       mode = SearchMode::PoeCollectorAccess;
       ApplyStartingInventory();
       Logic::NoBottles = true;
    }
  }
  // Condition for validating Poe Collector Access
  if (mode == SearchMode::PoeCollectorAccess && (AreaTable(RR_MARKET_GUARD_HOUSE)->Adult() || !checkPoeCollectorAccess)) {
    // Apply all items that are necessary for checking all location access
      std::vector<RandomizerGet> itemsToPlace =
          FilterFromPool(ItemPool, [](const auto i) { return Rando::StaticData::RetrieveItem(i).IsAdvancement(); });
    for (RandomizerGet unplacedItem : itemsToPlace) {
      Rando::StaticData::RetrieveItem(unplacedItem).ApplyEffect();
    }
    // Reset access as the non-starting age
    if (Settings::ResolvedStartingAge == AGE_CHILD) {
      for (RandomizerRegion areaKey : areaPool) {
        AreaTable(areaKey)->adultDay = false;
        AreaTable(areaKey)->adultNight = false;
      }
    } else {
      for (RandomizerRegion areaKey : areaPool) {
        AreaTable(areaKey)->childDay = false;
        AreaTable(areaKey)->childNight = false;
      }
    }
    mode = SearchMode::AllLocationsReachable;
  } else {
    Logic::NoBottles = false;
  }
}

//Get the max number of tokens that can possibly be useful
static int GetMaxGSCount() {
  auto ctx = Rando::Context::GetInstance();
  //If bridge or LACS is set to tokens, get how many are required
  int maxBridge = 0;
  int maxLACS = 0;
  if (Settings::Bridge.Is(RAINBOWBRIDGE_TOKENS)) {
    maxBridge = Settings::BridgeTokenCount.Value<uint8_t>();
  }
  if (Settings::GanonsBossKey.Is(GANONSBOSSKEY_LACS_TOKENS)) {
    maxLACS = Settings::LACSTokenCount.Value<uint8_t>();
  }
  maxBridge = std::max(maxBridge, maxLACS);
  //Get the max amount of GS which could be useful from token reward locations
  int maxUseful = 0;
  //If the highest advancement item is a token, we know it is useless since it won't lead to an otherwise useful item
  if (ctx->GetItemLocation(RC_KAK_100_GOLD_SKULLTULA_REWARD)->GetPlacedItem().IsAdvancement() && ctx->GetItemLocation(RC_KAK_100_GOLD_SKULLTULA_REWARD)->GetPlacedItem().GetItemType() != ITEMTYPE_TOKEN) {
    maxUseful = 100;
  } else if (ctx->GetItemLocation(RC_KAK_50_GOLD_SKULLTULA_REWARD)->GetPlacedItem().IsAdvancement() &&
             ctx->GetItemLocation(RC_KAK_50_GOLD_SKULLTULA_REWARD)->GetPlacedItem().GetItemType() != ITEMTYPE_TOKEN) {
      maxUseful = 50;
  } else if (ctx->GetItemLocation(RC_KAK_40_GOLD_SKULLTULA_REWARD)->GetPlacedItem().IsAdvancement() &&
             ctx->GetItemLocation(RC_KAK_40_GOLD_SKULLTULA_REWARD)->GetPlacedItem().GetItemType() != ITEMTYPE_TOKEN) {
      maxUseful = 40;
  } else if (ctx->GetItemLocation(RC_KAK_30_GOLD_SKULLTULA_REWARD)->GetPlacedItem().IsAdvancement() &&
             ctx->GetItemLocation(RC_KAK_30_GOLD_SKULLTULA_REWARD)->GetPlacedItem().GetItemType() != ITEMTYPE_TOKEN) {
      maxUseful = 30;
  } else if (ctx->GetItemLocation(RC_KAK_20_GOLD_SKULLTULA_REWARD)->GetPlacedItem().IsAdvancement() &&
             ctx->GetItemLocation(RC_KAK_20_GOLD_SKULLTULA_REWARD)->GetPlacedItem().GetItemType() != ITEMTYPE_TOKEN) {
      maxUseful = 20;
  } else if (ctx->GetItemLocation(RC_KAK_10_GOLD_SKULLTULA_REWARD)->GetPlacedItem().IsAdvancement() &&
             ctx->GetItemLocation(RC_KAK_10_GOLD_SKULLTULA_REWARD)->GetPlacedItem().GetItemType() != ITEMTYPE_TOKEN) {
      maxUseful = 10;
  }
  //Return max of the two possible reasons tokens could be important, minus the tokens in the starting inventory
  return std::max(maxUseful, maxBridge) - StartingSkulltulaToken.Value<uint8_t>();
}

std::string GetShopItemBaseName(std::string itemName) {
  std::string baseName = itemName.erase(0, 4); //Delete "Buy "
  //Delete amount, if present (so when it looks like Buy Deku Nut (10) remove the (10))
  if (baseName.find("(") != std::string::npos) {
    baseName = baseName.erase(baseName.find("("));
  }
  //Do the same for [] (only applies to red potions, other things with [] also have a ())
  if (baseName.find("[") != std::string::npos) {
    baseName = baseName.erase(baseName.find("["));
  }
  return baseName;
}

std::vector<RandomizerCheck> GetEmptyLocations(std::vector<RandomizerCheck> allowedLocations) {
    auto ctx = Rando::Context::GetInstance();
    return FilterFromPool(allowedLocations, [ctx](const auto loc) {
        return ctx->GetItemLocation(loc)->GetPlacedRandomizerGet() == RG_NONE;
    });
}

std::vector<RandomizerCheck> GetAllEmptyLocations() {
    auto ctx = Rando::Context::GetInstance();
    return FilterFromPool(ctx->allLocations, [ctx](const auto loc) {
        return ctx->GetItemLocation(loc)->GetPlacedRandomizerGet() == RG_NONE;
    });
}

//This function will return a vector of ItemLocations that are accessible with
//where items have been placed so far within the world. The allowedLocations argument
//specifies the pool of locations that we're trying to search for an accessible location in
std::vector<RandomizerCheck> GetAccessibleLocations(const std::vector<RandomizerCheck>& allowedLocations, SearchMode mode /* = SearchMode::ReachabilitySearch*/, std::string ignore /*= ""*/, bool checkPoeCollectorAccess /*= false*/, bool checkOtherEntranceAccess /*= false*/) {
    auto ctx = Rando::Context::GetInstance();
    std::vector<RandomizerCheck> accessibleLocations;
    // Reset all access to begin a new search
    if (mode < SearchMode::ValidateWorld) {
        ApplyStartingInventory();
  }
  Areas::AccessReset();
  ctx->LocationReset();
  std::vector<RandomizerRegion> areaPool = {RR_ROOT};

  if (mode == SearchMode::ValidateWorld) {
    mode = SearchMode::TimePassAccess;
    AreaTable(RR_ROOT)->childNight = true;
    AreaTable(RR_ROOT)->adultNight = true;
    AreaTable(RR_ROOT)->childDay = true;
    AreaTable(RR_ROOT)->adultDay = true;
    ctx->allLocationsReachable = false;
  }

  //Variables for playthrough
  int gsCount = 0;
  const int maxGsCount = mode == SearchMode::GeneratePlaythrough ? GetMaxGSCount() : 0; //If generating playthrough want the max that's possibly useful, else doesn't matter
  bool bombchusFound = false;
  std::vector<std::string> buyIgnores;

  //Variables for search
  std::vector<Rando::ItemLocation*> newItemLocations;
  bool updatedEvents = false;
  bool ageTimePropogated = false;
  bool firstIteration = true;

  //Variables for Time Pass access
  bool timePassChildDay = false;
  bool timePassChildNight = false;
  bool timePassAdultDay = false;
  bool timePassAdultNight = false;

  // Main access checking loop
  while (newItemLocations.size() > 0 || updatedEvents || ageTimePropogated || firstIteration) {
    firstIteration = false;
    ageTimePropogated = false;
    updatedEvents = false;

    for (Rando::ItemLocation* location : newItemLocations) {
      location->ApplyPlacedItemEffect();
    }
    newItemLocations.clear();

    std::vector<RandomizerCheck> itemSphere;
    std::list<Entrance*> entranceSphere;

    for (size_t i = 0; i < areaPool.size(); i++) {
      Area* area = AreaTable(areaPool[i]);

      if (area->UpdateEvents(mode)){
        updatedEvents = true;
      }

      // If we're checking for TimePass access do that for each area as it's being updated.
      // TimePass Access is satisfied when every AgeTime can reach an area with TimePass
      // without the aid of TimePass. During this mode, TimePass won't update ToD access
      // in any area.
      if (mode == SearchMode::TimePassAccess) {
        if (area->timePass) {
          if (area->childDay) {
            timePassChildDay = true;
          }
          if (area->childNight) {
            timePassChildNight = true;
          }
          if (area->adultDay) {
            timePassAdultDay = true;
          }
          if (area->adultNight) {
            timePassAdultNight = true;
          }
        }
        // Condition for validating that all startring AgeTimes have timepass access
        // Once satisifed, change the mode to begin checking for Temple of Time Access
        if ((timePassChildDay && timePassChildNight && timePassAdultDay && timePassAdultNight) || !checkOtherEntranceAccess) {
          mode = SearchMode::TempleOfTimeAccess;
        }
      }

      //for each exit in this area
      for (auto& exit : area->exits) {

        //Update Time of Day Access for the exit
        if (UpdateToDAccess(&exit, mode)) {
          ageTimePropogated = true;
          ValidateWorldChecks(mode, checkPoeCollectorAccess, checkOtherEntranceAccess, areaPool);
        }

        //If the exit is accessible and hasn't been added yet, add it to the pool
        Area* exitArea = exit.GetConnectedRegion();
        if (!exitArea->addedToPool && exit.ConditionsMet()) {
          exitArea->addedToPool = true;
          areaPool.push_back(exit.GetConnectedRegionKey());
        }

        // Add shuffled entrances to the entrance playthrough
        if (mode == SearchMode::GeneratePlaythrough && exit.IsShuffled() && !exit.IsAddedToPool() && !noRandomEntrances) {
          entranceSphere.push_back(&exit);
          exit.AddToPool();
          // Don't list a two-way coupled entrance from both directions
          if (exit.GetReverse() != nullptr && exit.GetReplacement()->GetReverse() != nullptr && !exit.IsDecoupled()) {
            exit.GetReplacement()->GetReverse()->AddToPool();
          }
        }
      }

      //for each ItemLocation in this area
      if (mode < SearchMode::ValidateWorld) {
        for (size_t k = 0; k < area->locations.size(); k++) {
          LocationAccess& locPair = area->locations[k];
          RandomizerCheck loc = locPair.GetLocation();
          Rando::ItemLocation* location = ctx->GetItemLocation(loc);

          if (!location->IsAddedToPool() && locPair.ConditionsMet()) {

            location->AddToPool();

            if (location->GetPlacedRandomizerGet() == RG_NONE) {
              accessibleLocations.push_back(loc); //Empty location, consider for placement
            } else {
              //If ignore has a value, we want to check if the item location should be considered or not
              //This is necessary due to the below preprocessing for playthrough generation
              if (ignore != "") {
                ItemType type = location->GetPlacedItem().GetItemType();
                std::string itemName(location->GetPlacedItemName().GetEnglish());
                //If we want to ignore tokens, only add if not a token
                if (ignore == "Tokens" && type != ITEMTYPE_TOKEN) {
                  newItemLocations.push_back(location);
                }
                //If we want to ignore bombchus, only add if bombchu is not in the name
                else if (ignore == "Bombchus" && itemName.find("Bombchu") == std::string::npos) {
                  newItemLocations.push_back(location);
                }
                //We want to ignore a specific Buy item name
                else if (ignore != "Tokens" && ignore != "Bombchus") {
                  if ((type == ITEMTYPE_SHOP && ignore != GetShopItemBaseName(itemName)) || type != ITEMTYPE_SHOP) {
                    newItemLocations.push_back(location);
                  }
                }
              }
              //If it doesn't, we can just add the location
              else {
                newItemLocations.push_back(location); //Add item to cache to be considered in logic next iteration
              }
            }

            //Playthrough stuff
            //Generate the playthrough, so we want to add advancement items, unless we know to ignore them
            if (mode == SearchMode::GeneratePlaythrough) {
              //Item is an advancement item, figure out if it should be added to this sphere
              if (!ctx->playthroughBeatable && location->GetPlacedItem().IsAdvancement()) {
                ItemType type = location->GetPlacedItem().GetItemType();
                std::string itemName(location->GetPlacedItemName().GetEnglish());
                bool bombchus = itemName.find("Bombchu") != std::string::npos; //Is a bombchu location

                //Decide whether to exclude this location
                //This preprocessing is done to reduce the amount of searches performed in PareDownPlaythrough
                //Want to exclude:
                //1) Tokens after the last potentially useful one (the last one that gives an advancement item or last for token bridge)
                //2) Bombchus after the first (including buy bombchus)
                //3) Buy items of the same type, after the first (So only see Buy Deku Nut of any amount once)
                bool exclude = true;
                //Exclude tokens after the last possibly useful one
                if (type == ITEMTYPE_TOKEN && gsCount < maxGsCount) {
                  gsCount++;
                  exclude = false;
                }
                //Only print first bombchu location found
                else if (bombchus && !bombchusFound) {
                  bombchusFound = true;
                  exclude = false;
                }
                //Handle buy items
                //If ammo drops are off, don't do this step, since buyable ammo becomes logically important
                else if (AmmoDrops.IsNot(AMMODROPS_NONE) && !(bombchus && bombchusFound) && type == ITEMTYPE_SHOP) {
                  //Only check each buy item once
                  std::string buyItem = GetShopItemBaseName(itemName);
                  //Buy item not in list to ignore, add it to list and write to playthrough
                  if (std::find(buyIgnores.begin(), buyIgnores.end(), buyItem) == buyIgnores.end()) {
                    exclude = false;
                    buyIgnores.push_back(buyItem);
                  }
                }
                //Add all other advancement items
                else if (!bombchus && type != ITEMTYPE_TOKEN && (AmmoDrops.Is(AMMODROPS_NONE) || type != ITEMTYPE_SHOP)) {
                  exclude = false;
                }
                //Has not been excluded, add to playthrough
                if (!exclude) {
                  itemSphere.push_back(loc);
                }
              }
              //Triforce has been found, seed is beatable, nothing else in this or future spheres matters
              else if (location->GetPlacedRandomizerGet() == RG_TRIFORCE) {
                itemSphere.clear();
                itemSphere.push_back(loc);
                ctx->playthroughBeatable = true;
              }
            }
            //All we care about is if the game is beatable, used to pare down playthrough
            else if (location->GetPlacedRandomizerGet() == RG_TRIFORCE && mode == SearchMode::CheckBeatable) {
              ctx->playthroughBeatable = true;
              return {}; //Return early for efficiency
            }
          }
        }
      }
    }

    if (mode == SearchMode::GeneratePlaythrough && itemSphere.size() > 0) {
      ctx->playthroughLocations.push_back(itemSphere);
    }
    if (mode == SearchMode::GeneratePlaythrough && entranceSphere.size() > 0 && !noRandomEntrances) {
      playthroughEntrances.push_back(entranceSphere);
    }
  }

  //Check to see if all locations were reached
  if (mode == SearchMode::AllLocationsReachable) {
    ctx->allLocationsReachable = true;
    for (const RandomizerCheck loc : ctx->allLocations) {
      if (!ctx->GetItemLocation(loc)->IsAddedToPool()) {
          ctx->allLocationsReachable = false;
          auto message = "Location " +
                         Rando::StaticData::GetLocation(ctx->GetItemLocation(loc)->GetRandomizerCheck())->GetName() +
                         " not reachable\n";
          SPDLOG_DEBUG(message);
        #ifndef ENABLE_DEBUG
          break;
        #endif
      }
    }
    return {};
  }

  erase_if(accessibleLocations, [&allowedLocations, ctx](RandomizerCheck loc){
    for (RandomizerCheck allowedLocation : allowedLocations) {
      if (loc == allowedLocation || ctx->GetItemLocation(loc)->GetPlacedRandomizerGet() != RG_NONE) {
        return false;
      }
    }
    return true;
  });
  return accessibleLocations;
}

static void GeneratePlaythrough() {
    auto ctx = Rando::Context::GetInstance();
    ctx->playthroughBeatable = false;
    LogicReset();
    GetAccessibleLocations(ctx->allLocations, SearchMode::GeneratePlaythrough);
}

//Remove unnecessary items from playthrough by removing their location, and checking if game is still beatable
//To reduce searches, some preprocessing is done in playthrough generation to avoid adding obviously unnecessary items
static void PareDownPlaythrough() {
  auto ctx = Rando::Context::GetInstance();
  std::vector<RandomizerCheck> toAddBackItem;
  //Start at sphere before Ganon's and count down
  for (int i = ctx->playthroughLocations.size() - 2; i >= 0; i--) {
    //Check each item location in sphere
    std::vector<int> erasableIndices;
    std::vector<RandomizerCheck> sphere = ctx->playthroughLocations.at(i);
    for (int j = sphere.size() - 1; j >= 0; j--) {
      RandomizerCheck loc = sphere.at(j);
      RandomizerGet copy = ctx->GetItemLocation(loc)->GetPlacedRandomizerGet(); //Copy out item
      ctx->GetItemLocation(loc)->SetPlacedItem(RG_NONE); //Write in empty item
      ctx->playthroughBeatable = false;
      LogicReset();

      std::string ignore = "";
      if (Rando::StaticData::RetrieveItem(copy).GetItemType() == ITEMTYPE_TOKEN) {
        ignore = "Tokens";
      }
      else if (Rando::StaticData::RetrieveItem(copy).GetName().GetEnglish().find("Bombchu") != std::string::npos) {
        ignore = "Bombchus";
      }
      else if (Rando::StaticData::RetrieveItem(copy).GetItemType() == ITEMTYPE_SHOP) {
        ignore = GetShopItemBaseName(Rando::StaticData::RetrieveItem(copy).GetName().GetEnglish());
      }

      GetAccessibleLocations(ctx->allLocations, SearchMode::CheckBeatable, ignore); //Check if game is still beatable

      //Playthrough is still beatable without this item, therefore it can be removed from playthrough section.
      if (ctx->playthroughBeatable) {
        // Uncomment to print playthrough deletion log in citra
        //  std::string itemname(Rando::StaticData::RetrieveItem(copy).GetName().GetEnglish());
        //  std::string locationname(GetLocation(loc)->GetName());
        //  std::string removallog = itemname + " at " + locationname + " removed from playthrough";
        //  CitraPrint(removallog);
        ctx->playthroughLocations[i].erase(ctx->playthroughLocations[i].begin() + j);
        ctx->GetItemLocation(loc)->SetDelayedItem(copy); //Game is still beatable, don't add back until later
        toAddBackItem.push_back(loc);
      }
      else {
        ctx->GetItemLocation(loc)->SetPlacedItem(copy); //Immediately put item back so game is beatable again
      }
    }
  }

  //Some spheres may now be empty, remove these
  for (int i = ctx->playthroughLocations.size() - 2; i >= 0; i--) {
    if (ctx->playthroughLocations.at(i).size() == 0) {
      ctx->playthroughLocations.erase(ctx->playthroughLocations.begin() + i);
    }
  }

  //Now we can add back items which were removed previously
  for (RandomizerCheck loc : toAddBackItem) {
    ctx->GetItemLocation(loc)->SaveDelayedItem();
  }
}

//Very similar to PareDownPlaythrough except it creates the list of Way of the Hero items
//Way of the Hero items are more specific than playthrough items in that they are items which *must*
// be obtained to logically be able to complete the seed, rather than playthrough items which
// are just possible items you *can* collect to complete the seed.
static void CalculateWotH() {
  auto ctx = Rando::Context::GetInstance();
  //First copy locations from the 2-dimensional playthroughLocations into the 1-dimensional wothLocations
  //size - 1 so Triforce is not counted
  for (size_t i = 0; i < ctx->playthroughLocations.size() - 1; i++) {
    for (size_t j = 0; j < ctx->playthroughLocations[i].size(); j++) {
      if (ctx->GetItemLocation(ctx->playthroughLocations[i][j])->IsHintable()) {
        ctx->wothLocations.push_back(ctx->playthroughLocations[i][j]);
      }
    }
  }

  //Now go through and check each location, seeing if it is strictly necessary for game completion
  for (int i = ctx->wothLocations.size() - 1; i >= 0; i--) {
    RandomizerCheck loc = ctx->wothLocations[i];
    RandomizerGet copy = ctx->GetItemLocation(loc)->GetPlacedRandomizerGet(); //Copy out item
    ctx->GetItemLocation(loc)->SetPlacedItem(RG_NONE); //Write in empty item
    ctx->playthroughBeatable = false;
    LogicReset();
    GetAccessibleLocations(ctx->allLocations, SearchMode::CheckBeatable); //Check if game is still beatable
    ctx->GetItemLocation(loc)->SetPlacedItem(copy); //Immediately put item back
    //If removing this item and no other item caused the game to become unbeatable, then it is strictly necessary, so keep it
    //Else, delete from wothLocations
    if (ctx->playthroughBeatable) {
      ctx->wothLocations.erase(ctx->wothLocations.begin() + i);
    }
  }

  ctx->playthroughBeatable = true;
  LogicReset();
  GetAccessibleLocations(ctx->allLocations);
}

//Will place things completely randomly, no logic checks are performed
static void FastFill(std::vector<RandomizerGet> items, std::vector<RandomizerCheck> locations, bool endOnItemsEmpty = false) {
  auto ctx = Rando::Context::GetInstance();
  //Loop until locations are empty, or also end if items are empty and the parameters specify to end then
  while (!locations.empty() && (!endOnItemsEmpty || !items.empty())) {
    if (items.empty() && !endOnItemsEmpty) {
      items.push_back(GetJunkItem());
    }

    RandomizerCheck loc = RandomElement(locations, true);
    ctx->GetItemLocation(loc)->SetAsHintable();
    ctx->PlaceItemInLocation(loc, RandomElement(items, true));
  }
}

/*
| The algorithm places items in the world in reverse.
| This means we first assume we have every item in the item pool and
| remove an item and try to place it somewhere that is still reachable
| This method helps distribution of items locked behind many requirements.
| - OoT Randomizer
*/
static void AssumedFill(const std::vector<RandomizerGet>& items, const std::vector<RandomizerCheck>& allowedLocations,
                        bool setLocationsAsHintable = false) {
    auto ctx = Rando::Context::GetInstance();
    if (items.size() > allowedLocations.size()) {
        printf("\x1b[2;2HERROR: MORE ITEMS THAN LOCATIONS IN GIVEN LISTS");
        SPDLOG_DEBUG("Items:\n");
        // NOLINTNEXTLINE(clang-diagnostic-unused-variable)
        for (const RandomizerGet item : items) {
            SPDLOG_DEBUG("\t");
            SPDLOG_DEBUG(Rando::StaticData::RetrieveItem(item).GetName().GetEnglish());
            SPDLOG_DEBUG("\n");
        }
        SPDLOG_DEBUG("\nAllowed Locations:\n");
        // NOLINTNEXTLINE(clang-diagnostic-unused-variable)
        for (const RandomizerCheck loc : allowedLocations) {
            SPDLOG_DEBUG("\t");
            SPDLOG_DEBUG(Rando::StaticData::GetLocation(loc)->GetName());
            SPDLOG_DEBUG("\n");
        }
        placementFailure = true;
        return;
    }

    if (Settings::Logic.Is(LOGIC_NONE)) {
        FastFill(items, GetEmptyLocations(allowedLocations), true);
        return;
    }

    // keep retrying to place everything until it works or takes too long
    int retries = 10;
    bool unsuccessfulPlacement = false;
    std::vector<RandomizerCheck> attemptedLocations;
    do {
        retries--;
        if (retries <= 0) {
            placementFailure = true;
            return;
        }
        unsuccessfulPlacement = false;
        std::vector<RandomizerGet> itemsToPlace = items;

        // copy all not yet placed advancement items so that we can apply their effects for the fill algorithm
        std::vector<RandomizerGet> itemsToNotPlace =
            FilterFromPool(ItemPool, [](const auto i) { return Rando::StaticData::RetrieveItem(i).IsAdvancement(); });

        // shuffle the order of items to place
        Shuffle(itemsToPlace);
        while (!itemsToPlace.empty()) {
            RandomizerGet item = std::move(itemsToPlace.back());
            Rando::StaticData::RetrieveItem(item).SetAsPlaythrough();
            itemsToPlace.pop_back();

            // assume we have all unplaced items
            LogicReset();
            for (RandomizerGet unplacedItem : itemsToPlace) {
                Rando::StaticData::RetrieveItem(unplacedItem).ApplyEffect();
            }
            for (RandomizerGet unplacedItem : itemsToNotPlace) {
                Rando::StaticData::RetrieveItem(unplacedItem).ApplyEffect();
            }

            // get all accessible locations that are allowed
            const std::vector<RandomizerCheck> accessibleLocations = GetAccessibleLocations(allowedLocations);

            // retry if there are no more locations to place items
            if (accessibleLocations.empty()) {

                SPDLOG_DEBUG("\nCANNOT PLACE ");
                SPDLOG_DEBUG(Rando::StaticData::RetrieveItem(item).GetName().GetEnglish());
                SPDLOG_DEBUG(". TRYING AGAIN...\n");

#ifdef ENABLE_DEBUG
                Areas::DumpWorldGraph(Rando::StaticData::RetrieveItem(item).GetName().GetEnglish());
                PlacementLog_Write();
#endif

                // reset any locations that got an item
                for (RandomizerCheck loc : attemptedLocations) {
                    ctx->GetItemLocation(loc)->SetPlacedItem(RG_NONE);
                    //itemsPlaced--;
                }
                attemptedLocations.clear();

                unsuccessfulPlacement = true;
                break;
            }

            // place the item within one of the allowed locations
            RandomizerCheck selectedLocation = RandomElement(accessibleLocations);
            ctx->PlaceItemInLocation(selectedLocation, item);
            attemptedLocations.push_back(selectedLocation);

            // This tells us the location went through the randomization algorithm
            // to distinguish it from locations which did not or that the player already
            // knows
            if (setLocationsAsHintable) {
                ctx->GetItemLocation(selectedLocation)->SetAsHintable();
            }

            // If ALR is off, then we check beatability after placing the item.
            // If the game is beatable, then we can stop placing items with logic.
            if (!LocationsReachable) {
                ctx->playthroughBeatable = false;
                LogicReset();
                GetAccessibleLocations(ctx->allLocations, SearchMode::CheckBeatable);
                if (ctx->playthroughBeatable) {
                    SPDLOG_DEBUG("Game beatable, now placing items randomly. " + std::to_string(itemsToPlace.size()) +
                                " major items remaining.\n\n");
                    FastFill(itemsToPlace, GetEmptyLocations(allowedLocations), true);
                    return;
                }
            }
        }
    } while (unsuccessfulPlacement);
}

//This function will specifically randomize dungeon rewards for the End of Dungeons
//setting, or randomize one dungeon reward to Link's Pocket if that setting is on
static void RandomizeDungeonRewards() {
  auto ctx = Rando::Context::GetInstance();
  //quest item bit mask of each stone/medallion for the savefile
  static constexpr std::array<uint32_t, 9> bitMaskTable = {
    0x00040000, //Kokiri Emerald
    0x00080000, //Goron Ruby
    0x00100000, //Zora Sapphire
    0x00000001, //Forest Medallion
    0x00000002, //Fire Medallion
    0x00000004, //Water Medallion
    0x00000008, //Spirit Medallion
    0x00000010, //Shadow Medallion
    0x00000020, //Light Medallion
  };
  int baseOffset = Rando::StaticData::RetrieveItem(RG_KOKIRI_EMERALD).GetItemID();

  //End of Dungeons includes Link's Pocket
  if (ShuffleRewards.Is(REWARDSHUFFLE_END_OF_DUNGEON)) {
    //get stones and medallions
    std::vector<RandomizerGet> rewards = FilterAndEraseFromPool(ItemPool, [](const auto i) {return Rando::StaticData::RetrieveItem(i).GetItemType() == ITEMTYPE_DUNGEONREWARD;});

    // If there are less than 9 dungeon rewards, prioritize the actual dungeons
    // for placement instead of Link's Pocket
    if (rewards.size() < 9) {
      ctx->PlaceItemInLocation(RC_LINKS_POCKET, RG_GREEN_RUPEE);
    }

    if (Settings::Logic.Is(LOGIC_VANILLA)) { //Place dungeon rewards in vanilla locations
      for (RandomizerCheck loc : Rando::StaticData::dungeonRewardLocations) {
        ctx->GetItemLocation(loc)->PlaceVanillaItem();
      }
    } else { //Randomize dungeon rewards with assumed fill
      AssumedFill(rewards, Rando::StaticData::dungeonRewardLocations);
    }

    for (size_t i = 0; i < Rando::StaticData::dungeonRewardLocations.size(); i++) {
      const auto index = ctx->GetItemLocation(Rando::StaticData::dungeonRewardLocations[i])->GetPlacedItem().GetItemID() - baseOffset;
      rDungeonRewardOverrides[i] = index;

      //set the player's dungeon reward on file creation instead of pushing it to them at the start.
      //This is done mainly because players are already familiar with seeing their dungeon reward
      //before opening up their file
      if (i == Rando::StaticData::dungeonRewardLocations.size()-1) {
        LinksPocketRewardBitMask = bitMaskTable[index];
      }
    }
  } else if (LinksPocketItem.Is(LINKSPOCKETITEM_DUNGEON_REWARD)) {
    //get 1 stone/medallion
    std::vector<RandomizerGet> rewards = FilterFromPool(
        ItemPool, [](const auto i) { return Rando::StaticData::RetrieveItem(i).GetItemType() == ITEMTYPE_DUNGEONREWARD; });
    // If there are no remaining stones/medallions, then Link's pocket won't get one
    if (rewards.empty()) {
      ctx->PlaceItemInLocation(RC_LINKS_POCKET, RG_GREEN_RUPEE);
      return;
    }
    RandomizerGet startingReward = RandomElement(rewards, true);

    LinksPocketRewardBitMask = bitMaskTable[Rando::StaticData::RetrieveItem(startingReward).GetItemID() - baseOffset];
    ctx->PlaceItemInLocation(RC_LINKS_POCKET, startingReward);
    //erase the stone/medallion from the Item Pool
    FilterAndEraseFromPool(ItemPool, [startingReward](const RandomizerGet i) {return i == startingReward;});
  }
}

//Fills any locations excluded by the player with junk items so that advancement items
//can't be placed there.
static void FillExcludedLocations() {
  auto ctx = Rando::Context::GetInstance();
  //Only fill in excluded locations that don't already have something and are forbidden
  std::vector<RandomizerCheck> excludedLocations = FilterFromPool(ctx->allLocations, [ctx](const auto loc){
    return ctx->GetItemLocation(loc)->IsExcluded();
  });

  for (RandomizerCheck loc : excludedLocations) {
    PlaceJunkInExcludedLocation(loc);
  }
}

//Function to handle the Own Dungeon setting
static void RandomizeOwnDungeon(const Dungeon::DungeonInfo* dungeon) {
  auto ctx = Rando::Context::GetInstance();
  std::vector<RandomizerGet> dungeonItems;

  // Search and filter for locations that match the hint region of the dungeon
  // This accounts for boss room shuffle so that own dungeon items can be placed
  // in the shuffled boss room
  std::vector<RandomizerCheck> dungeonLocations = FilterFromPool(ctx->allLocations, [dungeon, ctx](const auto loc) {
    return GetHintRegionHintKey(ctx->GetItemLocation(loc)->GetParentRegionKey()) == dungeon->GetHintKey();
  });

  //filter out locations that may be required to have songs placed at them
  dungeonLocations = FilterFromPool(dungeonLocations, [](const auto loc){
    if (ShuffleSongs.Is(SONGSHUFFLE_SONG_LOCATIONS)) {
      return !(Rando::StaticData::GetLocation(loc)->IsCategory(Category::cSong));
    }
    if (ShuffleSongs.Is(SONGSHUFFLE_DUNGEON_REWARDS)) {
      return !(Rando::StaticData::GetLocation(loc)->IsCategory(Category::cSongDungeonReward));
    }
    return true;
  });

  //Add specific items that need be randomized within this dungeon
  if (Keysanity.Is(KEYSANITY_OWN_DUNGEON) && dungeon->GetSmallKey() != RG_NONE) {
    std::vector<RandomizerGet> dungeonSmallKeys = FilterAndEraseFromPool(ItemPool, [dungeon](const RandomizerGet i){ return (i == dungeon->GetSmallKey()) || (i == dungeon->GetKeyRing());});
    AddElementsToPool(dungeonItems, dungeonSmallKeys);
  }

  if ((BossKeysanity.Is(BOSSKEYSANITY_OWN_DUNGEON) && dungeon->GetBossKey() != RG_GANONS_CASTLE_BOSS_KEY) ||
      (GanonsBossKey.Is(GANONSBOSSKEY_OWN_DUNGEON) && dungeon->GetBossKey() == RG_GANONS_CASTLE_BOSS_KEY)) {
        auto dungeonBossKey = FilterAndEraseFromPool(ItemPool, [dungeon](const RandomizerGet i){ return i == dungeon->GetBossKey();});
        AddElementsToPool(dungeonItems, dungeonBossKey);
  }

  //randomize boss key and small keys together for even distribution
  AssumedFill(dungeonItems, dungeonLocations);

  //randomize map and compass separately since they're not progressive
  if (MapsAndCompasses.Is(MAPSANDCOMPASSES_OWN_DUNGEON) && dungeon->GetMap() != RG_NONE && dungeon->GetCompass() != RG_NONE) {
    auto dungeonMapAndCompass = FilterAndEraseFromPool(ItemPool, [dungeon](const RandomizerGet i){ return i == dungeon->GetMap() || i == dungeon->GetCompass();});
    AssumedFill(dungeonMapAndCompass, dungeonLocations);
  }
}

/*Randomize items restricted to a certain set of locations.
  The fill order of location groups is as follows:
    - Own Dungeon
    - Any Dungeon
    - Overworld
  Small Keys, Gerudo Keys, Boss Keys, Ganon's Boss Key, and/or dungeon rewards
  will be randomized together if they have the same setting. Maps and Compasses
  are randomized separately once the dungeon advancement items have all been placed.*/
static void RandomizeDungeonItems() {
  auto ctx = Rando::Context::GetInstance();
  using namespace Dungeon;

  //Get Any Dungeon and Overworld group locations
  std::vector<RandomizerCheck> anyDungeonLocations = FilterFromPool(ctx->allLocations, [](const auto loc){return Rando::StaticData::GetLocation(loc)->IsDungeon();});
  //overworldLocations defined in item_location.cpp

  //Create Any Dungeon and Overworld item pools
  std::vector<RandomizerGet> anyDungeonItems;
  std::vector<RandomizerGet> overworldItems;

  for (auto dungeon : dungeonList) {
    if (Keysanity.Is(KEYSANITY_ANY_DUNGEON)) {
      auto dungeonKeys = FilterAndEraseFromPool(ItemPool, [dungeon](const RandomizerGet i){return (i == dungeon->GetSmallKey()) || (i == dungeon->GetKeyRing());});
      AddElementsToPool(anyDungeonItems, dungeonKeys);
    } else if (Keysanity.Is(KEYSANITY_OVERWORLD)) {
      auto dungeonKeys = FilterAndEraseFromPool(ItemPool, [dungeon](const RandomizerGet i){return (i == dungeon->GetSmallKey()) || (i == dungeon->GetKeyRing());});
      AddElementsToPool(overworldItems, dungeonKeys);
    }

    if (BossKeysanity.Is(BOSSKEYSANITY_ANY_DUNGEON) && dungeon->GetBossKey() != RG_GANONS_CASTLE_BOSS_KEY) {
      auto bossKey = FilterAndEraseFromPool(ItemPool, [dungeon](const RandomizerGet i){return i == dungeon->GetBossKey();});
      AddElementsToPool(anyDungeonItems, bossKey);
    } else if (BossKeysanity.Is(BOSSKEYSANITY_OVERWORLD) && dungeon->GetBossKey() != RG_GANONS_CASTLE_BOSS_KEY) {
      auto bossKey = FilterAndEraseFromPool(ItemPool, [dungeon](const RandomizerGet i){return i == dungeon->GetBossKey();});
      AddElementsToPool(overworldItems, bossKey);
    }

    if (GanonsBossKey.Is(GANONSBOSSKEY_ANY_DUNGEON)) {
      auto ganonBossKey = FilterAndEraseFromPool(ItemPool, [](const auto i){return i == RG_GANONS_CASTLE_BOSS_KEY;});
      AddElementsToPool(anyDungeonItems, ganonBossKey);
    } else if (GanonsBossKey.Is(GANONSBOSSKEY_OVERWORLD)) {
        auto ganonBossKey = FilterAndEraseFromPool(ItemPool, [](const auto i) { return i == RG_GANONS_CASTLE_BOSS_KEY; });
      AddElementsToPool(overworldItems, ganonBossKey);
    }
  }

  if (GerudoKeys.Is(GERUDOKEYS_ANY_DUNGEON)) {
      auto gerudoKeys = FilterAndEraseFromPool(ItemPool, [](const auto i) { return i == RG_GERUDO_FORTRESS_SMALL_KEY; });
    AddElementsToPool(anyDungeonItems, gerudoKeys);
  } else if (GerudoKeys.Is(GERUDOKEYS_OVERWORLD)) {
      auto gerudoKeys = FilterAndEraseFromPool(ItemPool, [](const auto i) { return i == RG_GERUDO_FORTRESS_SMALL_KEY; });
    AddElementsToPool(overworldItems, gerudoKeys);
  }

  if (ShuffleRewards.Is(REWARDSHUFFLE_ANY_DUNGEON)) {
      auto rewards = FilterAndEraseFromPool(
          ItemPool, [](const auto i) { return Rando::StaticData::RetrieveItem(i).GetItemType() == ITEMTYPE_DUNGEONREWARD; });
    AddElementsToPool(anyDungeonItems, rewards);
  } else if (ShuffleRewards.Is(REWARDSHUFFLE_OVERWORLD)) {
      auto rewards = FilterAndEraseFromPool(
          ItemPool, [](const auto i) { return Rando::StaticData::RetrieveItem(i).GetItemType() == ITEMTYPE_DUNGEONREWARD; });
    AddElementsToPool(overworldItems, rewards);
  }

  //Randomize Any Dungeon and Overworld pools
  AssumedFill(anyDungeonItems, anyDungeonLocations, true);
  AssumedFill(overworldItems, Rando::StaticData::overworldLocations, true);

  //Randomize maps and compasses after since they're not advancement items
  for (auto dungeon : dungeonList) {
    if (MapsAndCompasses.Is(MAPSANDCOMPASSES_ANY_DUNGEON)) {
      auto mapAndCompassItems = FilterAndEraseFromPool(ItemPool, [dungeon](const RandomizerGet i){return i == dungeon->GetMap() || i == dungeon->GetCompass();});
      AssumedFill(mapAndCompassItems, anyDungeonLocations, true);
    } else if (MapsAndCompasses.Is(MAPSANDCOMPASSES_OVERWORLD)) {
      auto mapAndCompassItems = FilterAndEraseFromPool(ItemPool, [dungeon](const RandomizerGet i){return i == dungeon->GetMap() || i == dungeon->GetCompass();});
      AssumedFill(mapAndCompassItems, Rando::StaticData::overworldLocations, true);
    }
  }
}

static void RandomizeLinksPocket() {
  auto ctx = Rando::Context::GetInstance();
  if (LinksPocketItem.Is(LINKSPOCKETITEM_ADVANCEMENT)) {
   //Get all the advancement items                                                                                                     don't include tokens
      std::vector<RandomizerGet> advancementItems = FilterAndEraseFromPool(ItemPool, [](const auto i) {
          return Rando::StaticData::RetrieveItem(i).IsAdvancement() && Rando::StaticData::RetrieveItem(i).GetItemType() != ITEMTYPE_TOKEN;
      });
   //select a random one
   RandomizerGet startingItem = RandomElement(advancementItems, true);
   //add the others back
   AddElementsToPool(ItemPool, advancementItems);

   ctx->PlaceItemInLocation(RC_LINKS_POCKET, startingItem);
 } else if (LinksPocketItem.Is(LINKSPOCKETITEM_NOTHING)) {
   ctx->PlaceItemInLocation(RC_LINKS_POCKET, RG_GREEN_RUPEE);
 }
}

void VanillaFill() {
  auto ctx = Rando::Context::GetInstance();
  //Perform minimum needed initialization
  AreaTable_Init();
  ctx->GenerateLocationPool();
  GenerateItemPool();
  GenerateStartingInventory();
  //Place vanilla item in each location
  RandomizeDungeonRewards();
  for (RandomizerCheck loc : ctx->allLocations) {
    ctx->GetItemLocation(loc)->PlaceVanillaItem();
  }
  //If necessary, handle ER stuff
  if (ShuffleEntrances) {
    printf("\x1b[7;10HShuffling Entrances...");
    ShuffleAllEntrances();
    printf("\x1b[7;32HDone");
  }
  //Finish up
  ctx->CreateItemOverrides();
  CreateEntranceOverrides();
  CreateWarpSongTexts();
}

void ClearProgress() {
  printf("\x1b[7;32H    "); // Done
  printf("\x1b[8;10H                    "); // Placing Items...Done
  printf("\x1b[9;10H                              "); // Calculating Playthrough...Done
  printf("\x1b[10;10H                     "); // Creating Hints...Done
  printf("\x1b[11;10H                                  "); // Writing Spoiler Log...Done
}

int Fill() {
  auto ctx = Rando::Context::GetInstance();
  int retries = 0;
  while(retries < 5) {
    placementFailure = false;
    //showItemProgress = false;
    ctx->playthroughLocations.clear();
    playthroughEntrances.clear();
    ctx->wothLocations.clear();
    AreaTable_Init(); //Reset the world graph to intialize the proper locations
    ctx->ItemReset(); //Reset shops incase of shopsanity random
    ctx->GenerateLocationPool();
    GenerateItemPool();
    GenerateStartingInventory();
    RemoveStartingItemsFromPool();
    FillExcludedLocations();

    //Temporarily add shop items to the ItemPool so that entrance randomization
    //can validate the world using deku/hylian shields
    AddElementsToPool(ItemPool, GetMinVanillaShopItems(32)); //assume worst case shopsanity 4
    if (ShuffleEntrances) {
      printf("\x1b[7;10HShuffling Entrances");
      if (ShuffleAllEntrances() == ENTRANCE_SHUFFLE_FAILURE) {
        retries++;
        ClearProgress();
        continue;
      }
      printf("\x1b[7;32HDone");
    }
    //erase temporary shop items
    FilterAndEraseFromPool(ItemPool, [](const auto item) { return Rando::StaticData::RetrieveItem(item).GetItemType() == ITEMTYPE_SHOP; });

    //ctx->showItemProgress = true;
    //Place shop items first, since a buy shield is needed to place a dungeon reward on Gohma due to access
    NonShopItems = {};
    if (Shopsanity.Is(SHOPSANITY_OFF)) {
      PlaceVanillaShopItems(); //Place vanilla shop items in vanilla location
    } else {
      int total_replaced = 0;
      if (Shopsanity.IsNot(SHOPSANITY_ZERO)) { //Shopsanity 1-4, random
        //Initialize NonShopItems
        ItemAndPrice init;
        init.Name = Text{"No Item", "Sin objeto", "Pas d'objet"};
        init.Price = -1;
        init.Repurchaseable = false;
        NonShopItems.assign(32, init);
        //Indices from OoTR. So shopsanity one will overwrite 7, three will overwrite 7, 5, 8, etc.
        const std::array<int, 4> indices = {7, 5, 8, 6};
        //Overwrite appropriate number of shop items
        for (size_t i = 0; i < Rando::StaticData::shopLocationLists.size(); i++) {
          int num_to_replace = GetShopsanityReplaceAmount(); //1-4 shop items will be overwritten, depending on settings
          total_replaced += num_to_replace;
          for (int j = 0; j < num_to_replace; j++) {
            int itemindex = indices[j];
            int shopsanityPrice = GetRandomShopPrice();
            NonShopItems[TransformShopIndex(i * 8 + itemindex - 1)].Price =
                shopsanityPrice; // Set price to be retrieved by the patch and textboxes
            ctx->GetItemLocation(Rando::StaticData::shopLocationLists[i][itemindex - 1])->SetShopsanityPrice(shopsanityPrice);
          }
        }
      }
      //Get all locations and items that don't have a shopsanity price attached
      std::vector<RandomizerCheck> shopLocations = {};
      //Get as many vanilla shop items as the total number of shop items minus the number of replaced items
      //So shopsanity 0 will get all 64 vanilla items, shopsanity 4 will get 32, etc.
      std::vector<RandomizerGet> shopItems = GetMinVanillaShopItems(total_replaced);

      for (size_t i = 0; i < Rando::StaticData::shopLocationLists.size(); i++) {
        for (size_t j = 0; j < Rando::StaticData::shopLocationLists[i].size(); j++) {
          RandomizerCheck loc = Rando::StaticData::shopLocationLists[i][j];
          if (!(ctx->GetItemLocation(loc)->HasShopsanityPrice())) {
            shopLocations.push_back(loc);
          }
        }
      }
      //Place the shop items which will still be at shop locations
      AssumedFill(shopItems, shopLocations);
    }

    //Place dungeon rewards
    RandomizeDungeonRewards();

    //Place dungeon items restricted to their Own Dungeon
    for (auto dungeon : Dungeon::dungeonList) {
      RandomizeOwnDungeon(dungeon);
    }

    //Then Place songs if song shuffle is set to specific locations
    if (ShuffleSongs.IsNot(SONGSHUFFLE_ANYWHERE)) {

      //Get each song
      std::vector<RandomizerGet> songs = FilterAndEraseFromPool(
          ItemPool, [](const auto i) { return Rando::StaticData::RetrieveItem(i).GetItemType() == ITEMTYPE_SONG; });

      //Get each song location
      std::vector<RandomizerCheck> songLocations;
      if (ShuffleSongs.Is(SONGSHUFFLE_SONG_LOCATIONS)) {
          songLocations = FilterFromPool(
              ctx->allLocations, [](const auto loc) { return Rando::StaticData::GetLocation(loc)->IsCategory(Category::cSong); });

      } else if (ShuffleSongs.Is(SONGSHUFFLE_DUNGEON_REWARDS)) {
          songLocations = FilterFromPool(ctx->allLocations, [](const auto loc) {
              return Rando::StaticData::GetLocation(loc)->IsCategory(Category::cSongDungeonReward);
          });
      }

      AssumedFill(songs, songLocations, true);
    }

    //Then place dungeon items that are assigned to restrictive location pools
    RandomizeDungeonItems();

    //Then place Link's Pocket Item if it has to be an advancement item
    RandomizeLinksPocket();
    //Then place the rest of the advancement items
    std::vector<RandomizerGet> remainingAdvancementItems =
        FilterAndEraseFromPool(ItemPool, [](const auto i) { return Rando::StaticData::RetrieveItem(i).IsAdvancement(); });
    AssumedFill(remainingAdvancementItems, ctx->allLocations, true);

    //Fast fill for the rest of the pool
    std::vector<RandomizerGet> remainingPool = FilterAndEraseFromPool(ItemPool, [](const auto i) { return true; });
    FastFill(remainingPool, GetAllEmptyLocations(), false);

    //Add prices for scrubsanity, this is unique to SoH because we write/read scrub prices to/from the spoilerfile.
    if (Scrubsanity.Is(SCRUBSANITY_AFFORDABLE)) {
      for (size_t i = 0; i < Rando::StaticData::scrubLocations.size(); i++) {
        ctx->GetItemLocation(Rando::StaticData::scrubLocations[i])->SetScrubsanityPrice(10);
      }
    } else if (Scrubsanity.Is(SCRUBSANITY_RANDOM_PRICES)) {
      for (size_t i = 0; i < Rando::StaticData::scrubLocations.size(); i++) {
        int randomPrice = GetRandomScrubPrice();
        ctx->GetItemLocation(Rando::StaticData::scrubLocations[i])->SetScrubsanityPrice(randomPrice);
      }
    }

    GeneratePlaythrough();
    //Successful placement, produced beatable result
    if(ctx->playthroughBeatable && !placementFailure) {
      printf("Done");
      printf("\x1b[9;10HCalculating Playthrough...");
      PareDownPlaythrough();
      CalculateWotH();
      printf("Done");
      ctx->CreateItemOverrides();
      CreateEntranceOverrides();

      //funny ganon line
      Text ganonText = RandomElement(GetHintCategory(HintCategory::GanonLine)).GetText();
      CreateMessageFromTextObject(0x70CB, 0, 2, 3, AddColorsAndFormat(ganonText));
      SetGanonText(ganonText);
      
      CreateAllHints();
      CreateWarpSongTexts();
      return 1;
    }
    //Unsuccessful placement
    if(retries < 4) {
      SPDLOG_DEBUG("\nGOT STUCK. RETRYING...\n");
      Areas::ResetAllLocations();
      LogicReset();
      ClearProgress();
    }
    retries++;
  }
  //All retries failed
  return -1;
}
