#pragma GCC diagnostic ignored "-Wshadow"

#include "wtp_aiProduction.h"

#include <cmath>
#include <set>
#include <map>
#include <numeric>
#include <deque>

#include "wtp_autoplay.h"
#include "wtp_terranx.h"
#include "wtp_mod.h"
#include "wtp_game.h"
#include "wtp_aiRoute.h"
#include "wtp_aiMoveFormer.h"
#include "wtp_aiHurry.h"
#include "wtp_aiMoveColony.h"
#include "move.h"

// global statistics

double meanNewBaseGain;
double landColonyGain;

// global parameters

double globalColonyDemand;
double baseColonyDemandMultiplier;

// territory coverage by colonies: all colonies on the map, map tiles not owned by anyone by region, and the average territory of a base

struct RegionTerritory
{
	MAP *tile;
	bool ocean;
	int unownedTileCount;
};

std::vector<int> colonyVehicleIds;
robin_hood::unordered_flat_map<int, RegionTerritory> regionTerritories;
double averageBaseTerritory = 0.0;

int bestFormerChassisIds[3];
int bestFormerSpeeds[3];
robin_hood::unordered_flat_map<int, BaseProductionInfo> baseProductionInfos;
robin_hood::unordered_flat_map<int, double> weakestEnemyBaseProtection;
robin_hood::unordered_flat_set<int> warRegions;

constexpr double LAND_ARTILLERY_SATURATION_RATIO = 0.20;
constexpr double INFANTRY_DEFENSIVE_SATURATION_RATIO = 0.75;

double techStealGain;

// sea transport parameters by sea cluster

robin_hood::unordered_flat_map<int, double> seaTransportDemands;

// military power in count of existing and potential military units cost
robin_hood::unordered_flat_map<int, double> militaryPowers;

int selectedProject = -1;

// currently processing base production demand

std::vector<ProductionDemand> productionDemands;
ProductionDemand *currentBaseProductionDemand;

// ProductionDemand

void ProductionDemand::initialize(int _baseId)
{
	baseId = _baseId;
	base = getBase(_baseId);
	
	baseSeaCluster = getSeaCluster(getBaseMapTile(baseId));
	
	item = -FAC_STOCKPILE_ENERGY;
	priority = 0.0;
	
	baseGain = getBaseGain(baseId);
	baseCitizenGain = baseGain / static_cast<double>(base->pop_size);
	baseWorkerGain = baseGain / static_cast<double>(getBaseWorkerCount(baseId));
	
}

void ProductionDemand::addItemPriority(int item, double priority)
{
	this->itemPriorities[item] = std::max(this->itemPriorities[item], priority);

	if (priority > this->priority)
	{
		this->item = item;
		this->priority = priority;
	}
	
}

/**
Prepares production choices.
*/
void productionStrategy()
{
	Profiling::start("productionStrategy", "strategy");
	
	// set global statistics
	
	Profiling::start("set global statistics", "productionStrategy");
	meanNewBaseGain = getNewBaseGain();
	landColonyGain = getLandColonyGain();
	Profiling::stop("set global statistics");
	
	// base production infos
	
	Profiling::start("base production infos", "productionStrategy");
	for (int baseId : aiData.baseIds)
	{
		baseProductionInfos.emplace(baseId, BaseProductionInfo());
		baseProductionInfos.at(baseId).baseGain = getBaseGain(baseId);
		baseProductionInfos.at(baseId).extraPoliceGains.at(0) = getBasePoliceGain(baseId, false);
		baseProductionInfos.at(baseId).extraPoliceGains.at(1) = getBasePoliceGain(baseId, true);
	}
	Profiling::stop("base production infos");
	
	// evaluate global demands
	// populate global variables
	
	populateFactionProductionData();
	evaluateGlobalColonyDemand();
	populateColonyCoverageData();
	evaluateNavalInvasionTransportDemand();
	evaluateGlobalSeaTransportDemand();
	evaluateGlobalSeaTransportDemand();

	// set production
	
	initializeProductionDemands();
	suggestGlobalProduction();
	suggestBaseProductions();
	applyBaseProductions();
	
	// hurry protective unit production
	
	hurryProtectiveUnit();
	
	Profiling::stop("productionStrategy");
	
}

void populateFactionProductionData()
{
	Profiling::start("populateFactionProductionData", "productionStrategy");
	
	// global parameters
	
	techStealGain = getTechStealGain();
	
	// fomer speed
	
	std::fill(std::begin(bestFormerSpeeds), std::begin(bestFormerSpeeds) + 3, 0);
	std::fill(std::begin(bestFormerChassisIds), std::begin(bestFormerChassisIds) + 3, -1);
	for (int unitId : aiData.formerUnitIds)
	{
		int chassisId = Units[unitId].chassis_id;
		CChassis chassis = Chassis[Units[unitId].chassis_id];
		if (chassis.speed > bestFormerSpeeds[chassis.triad])
		{
			bestFormerChassisIds[chassis.triad] = chassisId;
			bestFormerSpeeds[chassis.triad] = chassis.speed;
		}
	}
	
	// landArtillery and landDefenders saturation
	
	int landOffensiveVehicleCount = 0;
	int landArtilleryVehicleCount = 0;
	
	for (int vehicleId : aiData.combatVehicleIds)
	{
		VEH *vehicle = getVehicle(vehicleId);
		
		// land
		
		if (vehicle->triad() != TRIAD_LAND)
			continue;
		
		// offensive
		
		if (!isOffensiveVehicle(vehicleId))
			continue;
		
		// land offensive count
		
		landOffensiveVehicleCount++;
		
		// artillery
		
		if (!isArtilleryVehicle(vehicleId))
			continue;
		
		// land artillery count
		
		landArtilleryVehicleCount++;
		
	}

	int vehicleCount = 0;
	int infantryDefensiveVehicleCount = 0;
	
	for (int vehicleId : aiData.combatVehicleIds)
	{
		// vehicle count
		
		vehicleCount++;
		
		// infantry defensive
		
		if (!isInfantryDefensiveVehicle(vehicleId))
			continue;
		
		// infantry defensive count
		
		infantryDefensiveVehicleCount++;
		
	}

	Profiling::stop("populateFactionProductionData");
	
}

/**
Evaluates need for new colonies based on current inefficiency.
Every new colony creates more drones and, thus, is less effective in larger empire.
*/
void evaluateGlobalColonyDemand()
{
	Profiling::start("evaluateGlobalColonyDemand", "productionStrategy");
	
	debug("evaluateGlobalColonyDemand - %s\n", MFactions[aiFactionId].noun_faction);
	
	Faction *faction = getFaction(aiFactionId);
	
	// set default global colony demand multiplier
	
	globalColonyDemand = 1.0;
	
	// do not compute if there are no bases to produce anything
	
	if (aiData.baseIds.size() == 0)
	{
		Profiling::stop("evaluateGlobalColonyDemand");
		return;
	}
	
	// --------------------------------------------------
	// inefficiency
	// --------------------------------------------------
	
	// how many b-drones new base generates
	// the engine adds efficiency drones to different bases once the base count is above the base limit L:
	// a new base creates about (2 * N - L) / L of them: 2 per base between L and 2 * L, 4 between 2 * L and 3 * L, etc.
	// so that approaching k * L every base has k - 1 extra drones
	// this is a corrective coefficient: it grows linearly from zero at the limit with the same slope 2 / L
	// and ignores the single drone the engine adds right above the limit
	
	int contentPop, baseLimit;
	mod_psych_check(aiFactionId, &contentPop, &baseLimit);
	int newBaseCount = faction->base_count + 1;
	double newBaseBDrones = (baseLimit > 0 ? 2.0 * static_cast<double>(std::max(0, newBaseCount - baseLimit)) / static_cast<double>(baseLimit) : 0.0);
	
	// averages
	
	int sumPopulation = 0.0;
	double sumPsychMultiplier = 0.0;
	
	for (int baseId : aiData.baseIds)
	{
		BASE *base = getBase(baseId);
		
		sumPopulation += base->pop_size;
		sumPsychMultiplier += getBasePsychMultiplier(baseId);
		
	}
	
	double averagePsychMultiplier = sumPsychMultiplier / static_cast<double>(aiData.baseIds.size());
	double averageBaseSize = static_cast<double>(sumPopulation) / static_cast<double>(aiData.baseIds.size());
	
	// average efficiency loss
	
	double doctorsPerDrone = 1.0 / averagePsychMultiplier;
	double doctorsPerNewBase = doctorsPerDrone * newBaseBDrones;
	globalColonyDemand = 1.0 - doctorsPerNewBase / averageBaseSize;
	
	// reduce colony demand proportionally
	
	
	debug
	(
		"\tglobalColonyDemand=%5.2f"
		" baseLimit=%4d"
		" newBaseBDrones=%5.2f"
		" averagePsychMultiplier=%5.2f"
		" doctorsPerDrone=%5.2f"
		" doctorsPerNewBase=%5.2f"
		"\n"
		, globalColonyDemand
		, baseLimit
		, newBaseBDrones
		, averagePsychMultiplier
		, doctorsPerDrone
		, doctorsPerNewBase
	);
	
	Profiling::stop("evaluateGlobalColonyDemand");

}

/**
Collects data to estimate how much of the remaining territory the colonies on the map are going to cover:
all colonies of all factions, map tiles not owned by anyone and the average territory of a base.
Exact destinations of colonies are not needed for that.
*/
void populateColonyCoverageData()
{
	colonyVehicleIds.clear();

	for (int vehicleId = 0; vehicleId < *VehCount; vehicleId++)
	{
		VEH *vehicle = getVehicle(vehicleId);

		if (vehicle->faction_id <= 0)
			continue;
		if (!isColonyVehicle(vehicleId))
			continue;

		colonyVehicleIds.push_back(vehicleId);

	}

	// remaining territory: tiles not owned by anyone, by region
	// average base territory: owned tiles per base of any faction

	regionTerritories.clear();
	int ownedTileCount = 0;

	for (MAP *tile = *MapTiles; tile < *MapTiles + *MapAreaTiles; tile++)
	{
		if (tile->is_pole_tile())
			continue;

		RegionTerritory &regionTerritory = regionTerritories.emplace(tile->region, RegionTerritory{tile, is_ocean(tile), 0}).first->second;

		if (tile->is_owned())
		{
			ownedTileCount++;
		}
		else
		{
			regionTerritory.unownedTileCount++;
		}

	}

	int baseCount = 0;

	for (int baseId = 0; baseId < *BaseCount; baseId++)
	{
		if (getBase(baseId)->faction_id > 0)
		{
			baseCount++;
		}
	}

	averageBaseTerritory = (baseCount > 0 ? static_cast<double>(ownedTileCount) / static_cast<double>(baseCount) : 0.0);

}

/*
Estimates Thinker's naval invasion troop-lift demand and folds it into the same sea transport
request counters WTP's own colony/former/crawler transit requests use, so
evaluateGlobalSeaTransportDemand() naturally builds more transports when an invasion needs
them, without a second, parallel demand-tracking mechanism.

Deliberately approximate: this is a ballpark headcount, not a precise schedule. It counts the
faction's own land combat/probe vehicles Thinker's own invasion_unit() considers part of the
invasion - the same population combat_move's landing_unit flag marches to the staging point -
rather than only those that have already physically arrived there, so the demand signal has
lead time instead of lagging behind troop movement by the several turns marching takes.

Colony pods are deliberately excluded: WPN_COLONY_MODULE is in MANAGED_UNIT_TYPES, so colony
pod movement is WTP's own moveColonyStrategy, not Thinker's colony_move - their transport
needs already flow through the existing transit request mechanism.

Runs after this turn's movement in the previous turn (production for turn N follows movement
for turn N-1), so Thinker's invasion plan and troop positioning are already settled, not
still being computed, by the time this reads them.
*/
void evaluateNavalInvasionTransportDemand()
{
	Profiling::start("evaluateNavalInvasionTransportDemand", "productionStrategy");

	AIPlans &p = plans[aiFactionId];

	// no active invasion plan

	if (p.naval_start_x < 0)
	{
		Profiling::stop("evaluateNavalInvasionTransportDemand");
		return;
	}

	MAP *navalStartTile = getMapTile(p.naval_start_x, p.naval_start_y);

	if (navalStartTile == nullptr)
	{
		Profiling::stop("evaluateNavalInvasionTransportDemand");
		return;
	}

	int seaCluster = getSeaCluster(navalStartTile);

	for (int vehicleId : aiData.vehicleIds)
	{
		VEH *vehicle = getVehicle(vehicleId);

		if (vehicle->triad() != TRIAD_LAND)
			continue;

		if (!isCombatVehicle(vehicleId) && !isProbeVehicle(vehicleId))
			continue;

		if (!invasion_unit(vehicleId))
			continue;

		aiData.addSeaTransportRequest(seaCluster);

	}

	Profiling::stop("evaluateNavalInvasionTransportDemand");

}

void evaluateGlobalSeaTransportDemand()
{
	Profiling::start("evaluateGlobalSeaTransportDemand", "productionStrategy");

	debug("evaluateGlobalSeaTransportDemand - %s\n", getMFaction(aiFactionId)->noun_faction);
	
	// reset demands
	
	seaTransportDemands.clear();
	
	// demand and best base by cluster
	
	int transportFoilSpeed = getUnitSpeed(aiFactionId, BSC_TRANSPORT_FOIL);
	
	for (robin_hood::pair<int, int> seaTransportRequestCountEntry : aiData.seaTransportRequestCounts)
	{
		int seaCluster = seaTransportRequestCountEntry.first;
		int seaTransportRequestCount = seaTransportRequestCountEntry.second;
		
		// demand
		
		double transportationCoverage = 0.0;
		
		for (int vehicleId : aiData.seaTransportVehicleIds[seaCluster])
		{
			int vehicleSpeed = getVehicleSpeed(vehicleId);
			int remainingCapacity = getTransportRemainingCapacity(vehicleId);
			
			// adjust value for capacity and speed
			
			transportationCoverage += 0.5 * remainingCapacity * (static_cast<double>(vehicleSpeed) / static_cast<double>(transportFoilSpeed));
			
		}
		
		seaTransportDemands[seaCluster] = std::max(0.0, static_cast<double>(seaTransportRequestCount) - transportationCoverage);
		
	}
	
//	if (DEBUG)
//	{
//		for (robin_hood::pair<int, double> seaTransportDemandEntry : seaTransportDemands)
//		{
//			int cluster = seaTransportDemandEntry.first;
//			double demand = seaTransportDemandEntry.second;
//			
//			debug("%2d %5.2f\n", cluster, demand);
//			
//		}
//		
//	}
	
	Profiling::stop("evaluateGlobalSeaTransportDemand");
	
}

void populateWarRegions()
{
	Profiling::start("populateWarRegions", "productionStrategy");

	debug("populateWarRegions - %s\n", getMFaction(aiFactionId)->noun_faction);

	warRegions.clear();

	// iterate bases
	for (int baseId = 0; baseId < *BaseCount; ++baseId)
	{
		BASE &base = Bases[baseId];
		MAP *baseTile = getBaseMapTile(baseId);
		TileInfo &baseTileInfo = aiData.getTileInfo(baseTile);

		// vendetta
		if (!isHostile(aiFactionId, base.faction_id))
			continue;

		if (baseTile->is_land())
		{
			// land region
			warRegions.insert(baseTile->region);
		}
		else
		{
			// sea regions
			warRegions.insert(baseTileInfo.adjacentSeaRegions.begin(), baseTileInfo.adjacentSeaRegions.end());
		}

	}

	Profiling::stop("populateWarRegions");

}

void initializeProductionDemands()
{
	Profiling::start("initializeProductionDemands", "productionStrategy");
	
	productionDemands.clear();
	
	for (int baseId : aiData.baseIds)
	{
		productionDemands.emplace_back();
		productionDemands.back().initialize(baseId);
	}
	
	Profiling::stop("initializeProductionDemands");
	
}

void suggestGlobalProduction()
{
	Profiling::start("suggestGlobalProduction", "productionStrategy");
	
    debug("\nsuggestGlobalProduction - %s\n\n", MFactions[*CurrentFaction].noun_faction);
    
    evaluateHeadquarters();
    evaluateProject();
	
    debug("\n");
	
	Profiling::stop("suggestGlobalProduction");
	
}

void suggestBaseProductions()
{
	Profiling::start("suggestBaseProductions", "productionStrategy");
	
    debug("\nsuggestBaseProductions - %s\n\n", MFactions[*CurrentFaction].noun_faction);
	
    for (ProductionDemand &productionDemand : productionDemands)
	{
		currentBaseProductionDemand = &productionDemand;
		suggestBaseProduction();
	}
	
    debug("\n");
	
	Profiling::stop("suggestBaseProductions");
	
}

/*
Checks whether WTP's production evaluators actually cover this unit type.

Non-combat roles (colony/former/crawler) are identified by their module weapon, not
by being unarmed, so they are listed explicitly in MANAGED_UNIT_TYPES.

Combat-classified units (isCombatUnit: nonzero weapon offense value) are managed by
default, with two carve-outs for weapon types no evaluator actually prices:
- superweapons (planet buster and the other single-strike payloads) - UNMANAGED_COMBAT_WEAPON_TYPES
- probes are only evaluated in their defensive (infantry chassis) role so far;
  offensive probes (any other chassis) are not managed yet
*/
// TODO add supply transport
bool isManagedUnit(int unitId)
{
	// colony / former / transport ship - identified by their module weapon
	// (WPN_TROOP_TRANSPORT is the sea/land transport cargo-capacity module,
	// not to be confused with WPN_SUPPLY_TRANSPORT, the crawler's convoy module)

	static robin_hood::unordered_flat_set<int> const MANAGED_UNIT_TYPES
	{
		WPN_COLONY_MODULE,
		WPN_TERRAFORMING_UNIT,
		WPN_TROOP_TRANSPORT,
//		WPN_SUPPLY_TRANSPORT,
		WPN_PROBE_TEAM,
	}
	;

	if (MANAGED_UNIT_TYPES.count(Units[unitId].weapon_id) != 0)
		return true;

	// superweapon payloads - not yet evaluated
	// (weapon types isCombatUnit() classifies as combat, but that no evaluator prices)

	static robin_hood::unordered_flat_set<int> const UNMANAGED_COMBAT_WEAPON_TYPES
	{
		WPN_PLANET_BUSTER,
		WPN_CONVENTIONAL_PAYLOAD,
		WPN_TECTONIC_PAYLOAD,
		WPN_FUNGAL_PAYLOAD,
	}
	;

	if (UNMANAGED_COMBAT_WEAPON_TYPES.count(Units[unitId].weapon_id) != 0)
		return false;

	// remaining armed combat units

	return isCombatUnit(unitId);

}

/*
Checks whether WTP's production evaluators cover this production item, unit or facility.
*/
bool isManaged(int item)
{
	// units
	if (item >= 0)
	{
		return isManagedUnit(item);
	}
	// projects are managed
	if (-item >= SP_ID_First && -item <= SP_ID_Last)
	{
		return true;
	}
	// facilities
	if (-item >= Fac_ID_First && -item <= Fac_All_ID_Last)
	{
		static robin_hood::unordered_flat_set<int> const MANAGED_FACILITIES{
			FAC_HEADQUARTERS,
			FAC_CHILDREN_CRECHE,
			FAC_RECYCLING_TANKS,
			FAC_PERIMETER_DEFENSE,
			FAC_TACHYON_FIELD,
			FAC_RECREATION_COMMONS,
			FAC_ENERGY_BANK,
			FAC_NETWORK_NODE,
			FAC_BIOLOGY_LAB,
			FAC_SKUNKWORKS,
			FAC_HOLOGRAM_THEATRE,
			FAC_PARADISE_GARDEN,
			FAC_TREE_FARM,
			FAC_HYBRID_FOREST,
			FAC_FUSION_LAB,
			FAC_QUANTUM_LAB,
			FAC_RESEARCH_HOSPITAL,
			FAC_NANOHOSPITAL,
			FAC_ROBOTIC_ASSEMBLY_PLANT,
			FAC_NANOREPLICATOR,
			FAC_QUANTUM_CONVERTER,
			FAC_GENEJACK_FACTORY,
			FAC_PUNISHMENT_SPHERE,
			FAC_HAB_COMPLEX,
			FAC_HABITATION_DOME,
			FAC_PRESSURE_DOME,
			FAC_COMMAND_CENTER,
			FAC_NAVAL_YARD,
			FAC_AEROSPACE_COMPLEX,
			FAC_BIOENHANCEMENT_CENTER,
			FAC_CENTAURI_PRESERVE,
			FAC_TEMPLE_OF_PLANET,
			//		FAC_PSI_GATE,
			//		FAC_COVERT_OPS_CENTER,
			//		FAC_BROOD_PIT,
			FAC_AQUAFARM,
			FAC_SUBSEA_TRUNKLINE,
			FAC_THERMOCLINE_TRANSDUCER,
			//		FAC_FLECHETTE_DEFENSE_SYS,
			//		FAC_SUBSPACE_GENERATOR,
			//		FAC_GEOSYNC_SURVEY_POD,
			//		FAC_SKY_HYDRO_LAB,
			//		FAC_NESSUS_MINING_STATION,
			//		FAC_ORBITAL_POWER_TRANS,
			//		FAC_ORBITAL_DEFENSE_POD,
			FAC_STOCKPILE_ENERGY,
		};

		return MANAGED_FACILITIES.count(-item) != 0;
	}

	// anything else is unmanaged by default

	return false;

}

void applyBaseProductions()
{
	Profiling::start("applyBaseProductions", "productionStrategy");
	
    debug("\napplyBaseProductions - %s\n\n", MFactions[*CurrentFaction].noun_faction);
	
    for (ProductionDemand &productionDemand : productionDemands)
	{
		int baseId = productionDemand.baseId;
		BASE* base = productionDemand.base;
		
		debug("applyBaseProduction - %s\n", base->name);
		
		// pre-WTP production choice
		// whatever was already queued for the base during the upkeep phase - vanilla's or
		// Thinker's own choice, depending on which is enabled for this faction

		int currentChoice = base->queue_items[0];
		int choice = currentChoice;
		debug("(%s)\n", prod_name(choice));

    	// determine if currentChoice should be WTP overrided

    	bool override = false;

    	// discard unavailable item
   		if (!isBaseCanBuildItem(baseId, currentChoice))
   		{
   			override = true;
    	}

    	// discard current choice if it takes long production time
    	int currentChoiceBuildTime = getBaseItemBuildTime(baseId, currentChoice, false);
    	if (currentChoiceBuildTime > aiData.developmentScale)
    	{
    		override = true;
    	}

		// determine whether WTP manages this category of choice
		// managed: WTP's own priority list decides, among WTP's own alternatives only
		// not managed: defer unconditionally to the pre-WTP choice - no comparison, since WTP's
		// economical priorities and vanilla's/Thinker's internal scoring are not on a comparable scale

		// ReSharper disable once CppTooWideScopeInitStatement
		if (isManaged(choice))
		{
			override = true;
		}

		if (override)
		{
//			if constexpr (DEBUG)
//			{
//				std::vector<robin_hood::pair<int, double>> sortedItemPriorities(productionDemand.itemPriorities.begin(), productionDemand.itemPriorities.end());
//				std::sort(sortedItemPriorities.begin(), sortedItemPriorities.end(), [](const robin_hood::pair<int, double>& a, const robin_hood::pair<int, double>& b) { return a.second > b.second; });
//
//				for (const auto& itemPriority : sortedItemPriorities)
//				{
//					debug("\t%5.2f %-24s\n", itemPriority.second, prod_name(itemPriority.first));
//				}
//
//			}

			choice = productionDemand.item;

			debug("=> %-25s (WTP) priority=%5.2f\n", prod_name(choice), productionDemand.priority);

		}
		else
		{
			debug("=> %-25s (pre-WTP, unmanaged)\n", prod_name(choice));
		}

		// autoplay journal: the best candidates this base considered

		if (autoplay_journal_wanted())
		{
			std::vector<std::pair<int, double>> candidates;
			for (const robin_hood::pair<int, double> &itemPriority : productionDemand.itemPriorities)
			{
				candidates.emplace_back(itemPriority.first, itemPriority.second);
			}
			std::sort(candidates.begin(), candidates.end(), [](const std::pair<int, double> &a, const std::pair<int, double> &b) { return a.second > b.second; });

			const int count = std::min<int>(6, candidates.size());
			int items[6];
			double priorities[6];
			for (int i = 0; i < count; i++)
			{
				items[i] = candidates[i].first;
				priorities[i] = candidates[i].second;
			}

			autoplay_journal_production(baseId, currentChoice, currentChoiceBuildTime, aiData.developmentScale, override, choice, items, priorities, count);

		}

		// set base production if changed

		if (choice != currentChoice)
		{
			mod_base_change(baseId, choice);
		}

		debug("\n");
		
	}
	
    debug("\n");
	
	Profiling::stop("applyBaseProductions");
	
}

void evaluateHeadquarters()
{
	debug("- evaluateHeadquarters\n");
	
	int facilityId = FAC_HEADQUARTERS;
	
	// currentBudget
	
	int currentBudget = 0;
	
	for (ProductionDemand &otherProductionDemand : productionDemands)
	{
		int otherBaseId = otherProductionDemand.baseId;
		BASE *otherBase = otherProductionDemand.base;
		
		Profiling::start("- evaluateHeadquarters - computeBase");
		computeBase(otherBaseId, false);
		Profiling::stop("- evaluateHeadquarters - computeBase");
		
		int budget = otherBase->economy_total + otherBase->psych_total + otherBase->labs_total;
		currentBudget += budget;
		
	}
	
	// current HQ base
	
	int hqBaseId = -1;
	
	for (int baseId : aiData.baseIds)
	{
		if (isBaseHasFacility(baseId, facilityId))
		{
			hqBaseId = baseId;
			setBaseFacility(baseId, facilityId, false);
		}
		
	}
	
	// best HQ location
	
	ProductionDemand *bestHQProductionDemand = nullptr;
	int bestHQProductionDemandBudget = currentBudget;
	
	for (ProductionDemand &productionDemand : productionDemands)
	{
		int baseId = productionDemand.baseId;
		
		int totalBudget = 0;
		
		setBaseFacility(baseId, facilityId, true);
		
		for (ProductionDemand &otherProductionDemand : productionDemands)
		{
			int otherBaseId = otherProductionDemand.baseId;
			BASE *otherBase = otherProductionDemand.base;
			
			Profiling::start("- evaluateHeadquarters - computeBase");
			computeBase(otherBaseId, false);
			Profiling::stop("- evaluateHeadquarters - computeBase");
			
			int budget = otherBase->economy_total + otherBase->psych_total + otherBase->labs_total;
			totalBudget += budget;
			
		}
		
		setBaseFacility(baseId, facilityId, false);
		
		if (totalBudget > bestHQProductionDemandBudget)
		{
			bestHQProductionDemand = &productionDemand;
			bestHQProductionDemandBudget = totalBudget;
		}
		
	}
	
	// restore HQ
	
	if (hqBaseId != -1)
	{
		setBaseFacility(hqBaseId, facilityId, true);
	}
	
	// no relocation base
	
	if (bestHQProductionDemand == nullptr)
		return;
	
	// add demand
	
	int budgetImprovement = bestHQProductionDemandBudget - currentBudget;
	double income = getResourceScore(0.0, static_cast<double>(budgetImprovement));
	double gain = getGainIncome(income);
	double priority = getItemPriority(-facilityId, gain);
	bestHQProductionDemand->addItemPriority(-facilityId, priority);
	
	debug
	(
		"\t%-25s"
		" priority=%5.2f   |"
		" income=%5.2f"
		" gain=%5.2f"
		"\n"
		, bestHQProductionDemand->base->name
		, priority
		, income
		, gain
	)
	;
	
}

/*
Total power of the attackers expected at the base.
*/
static double getBaseAttackerPower(int baseId)
{
	auto defendDataIterator = aiData.defendLocations.find(getBaseMapTile(baseId));

	return defendDataIterator == aiData.defendLocations.end() ? 0.0 : defendDataIterator->second.getAttackerPower();

}

void evaluateProject()
{
	debug("evaluateProject - %s\n", getMFaction(aiFactionId)->noun_faction);
	
	// find project
	
	int cheapestProjectFacilityId = -1;
	int cheapestProjectFacilityCost = INT_MAX;
	
	for (int projectFacilityId = SP_ID_First; projectFacilityId <= SP_ID_Last; projectFacilityId++)
	{
		// exclude not available project
		
		if (!mod_facility_avail((FacilityId)projectFacilityId, aiFactionId, -1, 0))
			continue;
		
		// project cost
		
		int cost = getFacility(projectFacilityId)->cost;
		
		debug("\t%-30s cost=%3d\n", getFacility(projectFacilityId)->name, getFacility(projectFacilityId)->cost);
		
		// update best
		
		if (cost < cheapestProjectFacilityCost)
		{
			debug("\t\t- best\n");
			cheapestProjectFacilityId = projectFacilityId;
			cheapestProjectFacilityCost = cost;
		}
		
	}
	
	if (cheapestProjectFacilityId == -1)
		return;
	
	int selectedProjectFacilityId = cheapestProjectFacilityId;
	CFacility *selectedProjectFacility = getFacility(selectedProjectFacilityId);
	
	// max threat
	
	double maxThreat = 2.0;
	for (ProductionDemand &productionDemand : productionDemands)
	{
		double threat = getBaseAttackerPower(productionDemand.baseId);
		maxThreat = std::max(maxThreat, threat);
		
	}

	// check project value
	
	ProductionDemand *bestProductionDemand = nullptr;
	double bestBaseValue = 0.0;
	int bestBaseBuildTime = 0;
	
	for (ProductionDemand &productionDemand : productionDemands)
	{
		int baseId = productionDemand.baseId;
		
		// turns to completion
		
		int buildTime = getBaseItemBuildTime(baseId, -selectedProjectFacilityId, false);
		double buildTimeCoefficient = 1.0 / static_cast<double>(std::max(1, buildTime));
		
		// threat coefficient
		
		double threat = getBaseAttackerPower(baseId);
		double threatCoefficient = 1.0 - threat / maxThreat;
		
		// value
		
		double value = buildTimeCoefficient * threatCoefficient;
		
		if (value > bestBaseValue)
		{
			bestProductionDemand = &productionDemand;
			bestBaseValue = value;
			bestBaseBuildTime = buildTime;
		}
		
	}
	
	if (bestProductionDemand == nullptr)
	{
		debug("\tmost suitable base not found\n");
		return;
	}
	
	// add project priority
	
	double buildTimeCoefficient = conf.ai_production_current_project_priority_build_time / static_cast<double>(std::max(1, bestBaseBuildTime));
	double priority = buildTimeCoefficient;
	
	bestProductionDemand->addItemPriority(-selectedProjectFacilityId, priority);
	
	debug
	(
		"\t%-30s %-25s ai_production_current_project_priority_build_time=%5.2f bestBaseBuildTime=%2d priority=%5.2f\n"
		, selectedProjectFacility->name
		, bestProductionDemand->base->name
		, conf.ai_production_current_project_priority_build_time
		, bestBaseBuildTime
		, priority
	);
	
}

void suggestBaseProduction()
{
	Profiling::start("suggestBaseProduction", "suggestBaseProductions");
	
	debug("suggestBaseProduction - %s\n", currentBaseProductionDemand->base->name);
	
	// evaluate economical items
	
	evaluateFacilities();
	evaluateExpansionUnits();
	evaluateTerraformUnits();
	evaluateConvoyUnits();
	
	// evaluate combat units
	
	evaluateDefensiveProbeUnits();
	evaluatePodPoppingUnits();
	evaluateBaseDefenseUnits();
	evaluateBunkerDefenseUnits();
	evaluateTerritoryProtectionUnits();
	evaluateEnemyBaseAssaultUnits();
	
	// evaluate transport
	// should be after unit evaluations to substitute transport as needed
	
	evaluateSeaTransport();
	
	debug("\n");
	
	Profiling::stop("suggestBaseProduction");
	
}

void evaluateFacilities()
{
	Profiling::start("evaluateFacilities", "suggestBaseProduction");
	
	debug("evaluateFacilities\n");
	
	evaluatePressureDome();
	evaluateStockpileEnergy();
	evaluatePsychFacilitiesRemoval();
	evaluatePsychFacilities();
	evaluateRecyclingTanks();
	evaluateIncomeFacilities();
	evaluateMineralMultiplyingFacilities();
	evaluatePopulationLimitFacilities();
	evaluateMilitaryFacilities();
	evaluatePrototypingFacilities();

	Profiling::stop("evaluateFacilities");
	
}

void evaluatePressureDome()
{
	debug("- evaluatePressureDome\n");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	BASE *base = productionDemand.base;
	
	int facilityId = FAC_PRESSURE_DOME;
	
	if (*ClimateFutureChange <= 0 || !is_shore_level(mapsq(base->x, base->y)))
		return;
	
	// fixed extremely high priority
	double priority = 1000.0;
	
	productionDemand.addItemPriority(-facilityId, priority);
	
	debug
	(
		"\t%-32s"
		" priority=%5.2f   |"
		"\n"
		, getFacility(facilityId)->name
		, priority
	)
	;
	
}

void evaluateStockpileEnergy()
{
	debug("- evaluateStockpileEnergy\n");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	BASE *base = productionDemand.base;
	
	int facilityId = FAC_STOCKPILE_ENERGY;
	
	double extraEnergy = 0.5 * (isFactionHasProject(aiFactionId, FAC_PLANETARY_ENERGY_GRID) ? 1.25 : 1.00) * static_cast<double>(base->mineral_surplus);
	double income = getResourceScore(0.0, extraEnergy);
	double gain = getGainBonus(income);
	double cost = static_cast<double>(base->mineral_surplus) / static_cast<double>(mod_cost_factor(base->faction_id, RSC_MINERAL, -1));
	double priority = gain / cost;
	
	productionDemand.addItemPriority(-facilityId, priority);
	
	debug
	(
		"\t%-32s"
		" priority=%5.2f   |"
		" extraEnergy=%5.2f"
		" income=%5.2f"
		" gain=%5.2f"
		" cost=%5.2f"
		"\n"
		, getFacility(facilityId)->name
		, priority
		, extraEnergy
		, income
		, gain
		, cost
	)
	;
	
}

/**
Evaluates base income facilities removal.
*/
void evaluatePsychFacilitiesRemoval()
{
	debug("- evaluatePsychFacilitiesRemoval\n");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	
	// facilityIds
	
	int facilityIds[] = {FAC_PUNISHMENT_SPHERE, FAC_RECREATION_COMMONS, FAC_HOLOGRAM_THEATRE, FAC_PARADISE_GARDEN, };
	
	for (int facilityId : facilityIds)
	{
		// evaluate regular psych facility removal only when PS is present
		
		if (!has_facility(FAC_PUNISHMENT_SPHERE, baseId) && facilityId != FAC_PUNISHMENT_SPHERE)
			continue;
		
		// facility should be present
		
		if (!isBaseHasFacility(baseId, facilityId))
			continue;
		
		// gain
		
		double gain = getFacilityGain(baseId, facilityId, false);
		
		debug
		(
			"\t%-32s"
			" gain=%5.2f"
			"\n"
			, getFacility(facilityId)->name
			, gain
		);
		
		// remove if positive gain
		
		if (gain >= 0)
		{
			setBaseFacility(baseId, facilityId, false);
			debug("\t\tremoved\n");
		}
		
	}
	
}

void evaluatePsychFacilities()
{
	debug("- evaluatePsychFacilities\n");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	
	// facilityIds
	
	const std::vector<FacilityId> facilityIds
	{
		FAC_PUNISHMENT_SPHERE,
		FAC_RECREATION_COMMONS, FAC_HOLOGRAM_THEATRE, FAC_PARADISE_GARDEN,
	};
	
	for (FacilityId facilityId : facilityIds)
	{
		if (!isBaseCanBuildFacility(baseId, facilityId))
			continue;
		
		// gain
		
		double gain = getFacilityGain(baseId, facilityId, true);
		double priority = conf.ai_production_priority_facility_psych * getItemPriority(-facilityId, gain);
		
		// add demand
		
		productionDemand.addItemPriority(-facilityId, priority);
		
		debug
		(
			"\t%-32s"
			" priority=%5.2f"
			" gain=%5.2f"
			" ai_production_priority_facility_psych=%5.2f"
			"\n"
			, getFacility(facilityId)->name
			, priority
			, gain
			, conf.ai_production_priority_facility_psych
		);
		
	}
	
}

void evaluateRecyclingTanks()
{
	debug("- evaluateRecyclingTanks\n");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	
	// facilityIds
	
	const std::vector<FacilityId> facilityIds
	{
		FAC_RECYCLING_TANKS,
	};
	
	for (FacilityId facilityId : facilityIds)
	{
		if (!isBaseCanBuildFacility(baseId, facilityId))
			continue;
		
		// gain
		
		double gain = getFacilityGain(baseId, facilityId, true);
		double priority = getItemPriority(-facilityId, gain);
		
		// add demand
		
		productionDemand.addItemPriority(-facilityId, priority);
		
		debug
		(
			"\t%-32s"
			" priority=%5.2f"
			" gain=%5.2f"
			"\n"
			, getFacility(facilityId)->name
			, priority
			, gain
		);
		
	}
	
}

/**
Evaluates base income facilities.
*/
void evaluateIncomeFacilities()
{
	debug("- evaluateIncomeFacilities\n");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	BASE *base = productionDemand.base;
	
	// facilityIds
	
	const std::vector<FacilityId> facilityIds
	{
		FAC_CHILDREN_CRECHE,
		FAC_ENERGY_BANK,
		FAC_NETWORK_NODE,
		FAC_RESEARCH_HOSPITAL, FAC_NANOHOSPITAL,
		FAC_FUSION_LAB, FAC_QUANTUM_LAB,
		FAC_TREE_FARM, FAC_HYBRID_FOREST,
		FAC_AQUAFARM,
		FAC_SUBSEA_TRUNKLINE,
		FAC_THERMOCLINE_TRANSDUCER,
		FAC_BIOLOGY_LAB,
		FAC_BROOD_PIT,
		FAC_CENTAURI_PRESERVE, FAC_TEMPLE_OF_PLANET,
	};
	
	const robin_hood::unordered_flat_map<FacilityId, int> facilityLifecycles
	{
		{FAC_BIOLOGY_LAB, 1},
		{FAC_BROOD_PIT, 3}, // +1 lifecycle and -25% of the cost
		{FAC_CENTAURI_PRESERVE, 1},
		{FAC_TEMPLE_OF_PLANET, 1},
	};
	
	for (FacilityId facilityId : facilityIds)
	{
		if (!isBaseCanBuildFacility(baseId, facilityId))
			continue;
		
		// gain
		
		double facilityGain = getFacilityGain(baseId, facilityId, true);
		
		// moraleIncome
		
		double moraleIncome = 0.0;
		
		robin_hood::unordered_flat_map<FacilityId,int>::const_iterator facilityLifecycleIterator = facilityLifecycles.find(facilityId);
		if (facilityLifecycleIterator != facilityLifecycles.end())
		{
			int lifecycle = facilityLifecycleIterator->second;
			
			double proportionalMineralBonus = getMoraleProportionalMineralBonus({0, 0, 0, lifecycle});
			moraleIncome = getResourceScore(proportionalMineralBonus * static_cast<double>(base->mineral_intake_2), 0.0);
			
		}
		
		double moraleGain = getGainIncome(moraleIncome);
		
		// combined gain
		
		double gain = facilityGain + moraleGain;
		double priority = getItemPriority(-facilityId, gain);
		
		// add demand
		
		productionDemand.addItemPriority(-facilityId, priority);
		
		debug
		(
			"\t%-32s"
			" priority=%5.2f   |"
			" facilityGain=%5.2f"
			" moraleIncome=%5.2f"
			" moraleGain=%5.2f"
			" gain=%5.2f"
			"\n"
			, getFacility(facilityId)->name
			, priority
			, facilityGain
			, moraleIncome
			, moraleGain
			, gain
		);
		
	}
	
}

void evaluateMineralMultiplyingFacilities()
{
	debug("- evaluateMineralMultiplyingFacilities\n");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	BASE *base = productionDemand.base;
	
	// facilityIds
	
	const std::vector<FacilityId> facilityIds
	{
		FAC_GENEJACK_FACTORY, FAC_ROBOTIC_ASSEMBLY_PLANT, FAC_NANOREPLICATOR, FAC_QUANTUM_CONVERTER,
	};
	
	for (FacilityId facilityId : facilityIds)
	{
		if (!isBaseCanBuildFacility(baseId, facilityId))
			continue;
		
		// gain
		
		double extraMineralIntake = 0.5 * static_cast<double>(base->mineral_intake);
		double income = getResourceScore(extraMineralIntake, 0.0);
		double incomeGain = getGainIncome(income);
		
		double populationGrowth = getBasePopulationGrowth(baseId);
		double extraMineralIntakeGrowth = 0.5 * aiData.averageCitizenMineralIntake * populationGrowth;
		double incomeGrowth = getResourceScore(extraMineralIntakeGrowth, 0.0);
		double incomeGrowthGain = getGainIncomeGrowth(incomeGrowth);
		
		// combined income
		
		double upkeep = getResourceScore(0.0, - Facility[facilityId].maint);
		double upkeepGain = getGainIncome(upkeep);
		
		double gain =
			+ incomeGain
			+ incomeGrowthGain
			+ upkeepGain
		;
		double priority = getItemPriority(-facilityId, gain);
		
		// add demand
		
		productionDemand.addItemPriority(-facilityId, priority);
		
		debug
		(
			"\t%-32s"
			" priority=%5.2f   |"
			" extraMineralIntake=%5.2f"
			" income=%5.2f"
			" incomeGain=%5.2f"
			" aiData.averageCitizenMineralIntake=%5.2f"
			" populationGrowth=%5.2f"
			" extraMineralIntakeGrowth=%5.2f"
			" incomeGrowth=%5.2f"
			" incomeGrowthGain=%5.2f"
			" upkeep=%5.2f"
			" upkeepGain=%5.2f"
			" gain=%5.2f"
			"\n"
			, getFacility(facilityId)->name
			, priority
			, extraMineralIntake
			, income
			, incomeGain
			, aiData.averageCitizenMineralIntake
			, populationGrowth
			, extraMineralIntakeGrowth
			, incomeGrowth
			, incomeGrowthGain
			, upkeep
			, upkeepGain
			, gain
		);
		
	}
	
}

void evaluatePopulationLimitFacilities()
{
	debug("- evaluatePopulationLimitFacilities\n");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	BASE *base = productionDemand.base;
	
	// facilityIds
	
	std::vector<FacilityId> facilityIds
	{
		FAC_HAB_COMPLEX, FAC_HABITATION_DOME,
	};
	
	for (FacilityId facilityId : facilityIds)
	{
		if (!isBaseCanBuildFacility(baseId, facilityId))
			continue;
		
		// parameters
		
		int populationLimit = getFacilityPopulationLimit(base->faction_id, facilityId);
		
		if (populationLimit == -1)
		{
			debug("\tnot a population limit facility\n");
			continue;
		}
		
		int growthToLimitTime = getBaseTurnsToPopulation(baseId, populationLimit + 1);
		int facilityBuildTime = getBaseItemBuildTime(baseId, -facilityId, false);
		
		if (growthToLimitTime <= facilityBuildTime)
		{
			// mandatory limit facility order
			
			double priority = INF;
			productionDemand.addItemPriority(-facilityId, priority);
			
			debug
			(
				"\t%-32s"
				" priority=%5.2f   |"
				" populationLimit=%2d"
				" growthToLimitTime=%2d"
				" facilityBuildTime=%2d"
				" - mandatory order"
				"\n"
				, getFacility(facilityId)->name
				, priority
				, populationLimit
				, growthToLimitTime
				, facilityBuildTime
			);
			
			return;
		}
		
		// gain from building a facility
		
		double facilityGain = getFacilityGain(baseId, facilityId, true);
		
		// gain from population limit lifted
		
		double populationLimitIncome = getResourceScore(base->nutrient_surplus, 0, 0);
		double populationLimitUpkeep = getResourceScore(0.0, -Facility[facilityId].maint);
		double populationLimitGain = getGainDelay(getGainIncome(populationLimitIncome + populationLimitUpkeep), growthToLimitTime);
		
		// total
		
		double gain =
			+ facilityGain
			+ populationLimitGain
		;
		double priority = getItemPriority(-facilityId, gain);
		
		// add demand
		
		productionDemand.addItemPriority(-facilityId, priority);
		
		debug
		(
			"\t%-32s"
			" priority=%5.2f   |"
			" populationLimit=%2d"
			" growthToLimitTime=%2d"
			" facilityGain=%5.2f"
			" populationLimitIncome=%5.2f"
			" populationLimitUpkeep=%5.2f"
			" populationLimitGain=%5.2f"
			" gain=%5.2f"
			"\n"
			, getFacility(facilityId)->name
			, priority
			, populationLimit
			, growthToLimitTime
			, facilityGain
			, populationLimitIncome
			, populationLimitUpkeep
			, populationLimitGain
			, gain
		);
		
	}
	
}

void evaluateMilitaryFacilities()
{
	debug("- evaluateMilitaryFacilities\n");
	
	struct MilitaryFacility
	{
		FacilityId facilityId;
		bool morale;
		bool defense;
		std::array<int,4> moraleBonuses;
		std::array<int,4> defenseLevels;
	};
	
	// how much each defense structure level increases defense
	double defenseStructureMultipliers[2] =
	{
		(1.0 + static_cast<double>(conf.facility_defense_value[0]) / 2.0) / getPercentageBonusMultiplier(Rules->combat_bonus_intrinsic_base_def),
		(1.0 + static_cast<double>(conf.facility_defense_value[3]) / 2.0) / (1.0 + static_cast<double>(conf.facility_defense_value[0]) / 2.0),
	};
	
	// how often land/ocean base experiences each triad attack
	double attackTriadWeights[2][4] =
	{
		{0.6, 0.0, 0.2, 0.2, },
		{0.0, 0.6, 0.2, 0.2, },
	};
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	BASE *base = productionDemand.base;
	bool ocean = is_ocean(getBaseMapTile(baseId));
	TileInfo &baseTileInfo = aiData.getBaseTileInfo(baseId);
	
	int const mineralCostFactor = mod_cost_factor(aiFactionId, RSC_MINERAL, -1);
	
	// facilities
	
	std::vector<MilitaryFacility> militaryFacilities
	{
		{FAC_COMMAND_CENTER			, true , false, {2, 0, 0, 0, }, {0, 0, 0, 0, }},
		{FAC_PERIMETER_DEFENSE		, false, true , {0, 0, 0, 0, }, {1, 0, 0, 0, }},
		{FAC_NAVAL_YARD				, true , true , {0, 2, 0, 0, }, {0, 1, 0, 0, }},
		{FAC_AEROSPACE_COMPLEX		, true , true , {0, 0, 2, 0, }, {0, 0, 1, 0, }},
		{FAC_BIOENHANCEMENT_CENTER	, true , false, {2, 2, 2, 1, }, {0, 0, 0, 0, }},
		{FAC_TACHYON_FIELD			, false, true , {0, 0, 0, 0, }, {2, 2, 2, 0, }},
	};
	
	for (MilitaryFacility &militaryFacility : militaryFacilities)
	{
		FacilityId facilityId = militaryFacility.facilityId;
		
		if (!isBaseCanBuildFacility(baseId, facilityId))
			continue;
		
		// morale income
		
		double moraleIncome = 0.0;
		
		if (militaryFacility.morale)
		{
			double proportionalMineralBonus = getMoraleProportionalMineralBonus(militaryFacility.moraleBonuses);
			double mineralBonus = proportionalMineralBonus * static_cast<double>(base->mineral_intake_2);
			moraleIncome = getResourceScore(mineralBonus, 0.0);
		}
		
		// defense income
		
		double defenseBuildCostSave = 0.0;
		double defenseSupportSave = 0.0;
		
		if (militaryFacility.defense)
		{
			// count cost and support for defense vehicles in this base: old and new (with improved defense)
			
			for (int vehicleId : baseTileInfo.playerVehicleIds)
			{
				VEH *vehicle = getVehicle(vehicleId);
				
				int offenseValue = getVehicleOffenseValue(vehicleId);
				int defenseValue = getVehicleDefenseValue(vehicleId);
				
				// conventional defense
				
				if (defenseValue <= 0)
					continue;
				
				// convert psi offense to max possible offense
				
				if (offenseValue < 0)
				{
					offenseValue = aiData.maxConOffenseValue;
				}
				
				// defense reduction
				
				double reduction = 0.0;
				
				for (int triad = 0; triad < 4; triad++)
				{
					double triadWeight = attackTriadWeights[ocean][triad];
					
					if (triadWeight == 0.0)
						continue;
					
					int defenseLevel = militaryFacility.defenseLevels[triad];
					
					if (defenseLevel == 0)
						continue;
					
					double defenseStructureMultiplier = defenseStructureMultipliers[defenseLevel - 1];
					double triadReduction = 1.0 - (static_cast<double>(offenseValue) + static_cast<double>(defenseValue)) / (static_cast<double>(offenseValue) + static_cast<double>(defenseValue) * defenseStructureMultiplier);
					reduction += triadWeight * triadReduction;
					
				}
				
				defenseBuildCostSave += mineralCostFactor * vehicle->cost() * reduction;
				defenseSupportSave += static_cast<double>(getVehicleSupport(vehicleId)) * reduction;
				
			}
			
		}
		
		// combined income and gain
		
		double bonus = defenseBuildCostSave;
		double income = moraleIncome + defenseSupportSave;
		double upkeep = getResourceScore(0, -Facility[facilityId].maint);
		
		double gain =
			+ getGainBonus(bonus)
			+ getGainIncome(income + upkeep)
		;
		
		double priority = getItemPriority(-facilityId, gain);
		
		// add demand
		
		productionDemand.addItemPriority(-facilityId, priority);
		
		debug
		(
			"\t%-32s"
			" priority=%5.2f   |"
			" moraleIncome=%5.2f"
			" defenseBuildCostSave=%5.2f"
			" defenseSupportSave=%5.2f"
			" bonus=%5.2f"
			" income=%5.2f"
			" upkeep=%5.2f"
			" gain=%5.2f"
			"\n"
			, Facility[facilityId].name
			, priority
			, moraleIncome
			, defenseBuildCostSave
			, defenseSupportSave
			, bonus
			, income
			, upkeep
			, gain
		);
		
	}
	
}

void evaluatePrototypingFacilities()
{
	debug("- evaluatePrototypingFacilities\n");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	BASE *base = productionDemand.base;

	FacilityId facilityId = FAC_SKUNKWORKS;
	
	// free prototype faction cannot build Skunkworks
	
	if (isFactionSpecial(aiFactionId, RFLAG_FREEPROTO))
	{
		debug("\tfree prototype faction cannot build Skunkworks\n");
		return;
	}
	
	// facility should be available
	
	if (!isBaseCanBuildFacility(baseId, facilityId))
		return;
	
	// base should be 90% productive
	
	if (base->mineral_intake_2 < aiData.maxMineralIntake2)
	{
		debug("\tweak production\n");
		return;
	}
	
	// existing Skunkworks ratio
	
	int skunkworksCount = 0;
	
	for (int otherBaseId : aiData.baseIds)
	{
		// exclude self
		
		if (otherBaseId == baseId)
			continue;
		
		// has Skunkworks
		
		if (isBaseHasFacility(baseId, facilityId))
		{
			skunkworksCount++;
		}
		
	}
	
	double skunkworksRatio = static_cast<double>(skunkworksCount) / static_cast<double>(aiData.baseIds.size());
	double useMultiplier = std::max(0.0, (0.1 - skunkworksRatio) / 0.1);
	
	if (useMultiplier <= 0.0)
	{
		debug("\tnot needed\n");
		return;
	}
	
	// gain
	
	double gain = 5.0 * useMultiplier;
	double priority = getItemPriority(-facilityId, gain);
	
	
	productionDemand.addItemPriority(-facilityId, priority);
	
	debug
	(
		"\t%-32s"
		" priority=%5.2f   |"
		" skunkworksRatio=%5.2f"
		" useMultiplier=%5.2f"
		" gain=%5.2f"
		"\n"
		, Facility[facilityId].name
		, priority
		, skunkworksRatio
		, useMultiplier
		, gain
	);
	
}

void evaluateDefensiveProbeUnits()
{
	Profiling::start("evaluateDefensiveProbeUnits", "suggestBaseProduction");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	MAP *baseTile = getBaseMapTile(baseId);
	
	debug("evaluateDefensiveProbeUnits\n");
	
	// base new probe morale multiplier
	
	double newProbeMoraleMultiplier = has_facility(FAC_COVERT_OPS_CENTER, baseId) ? 1.25 : 1.00;
	
	// scan combat units for protection
	
	for (int unitId : aiFactionInfo->buildableUnitIds)
	{
		UNIT *unit = getUnit(unitId);
		
		// defensive probe
		
		if (!(isInfantryUnit(unitId) && isProbeUnit(unitId)))
			continue;
		
		// exclude those base cannot produce
		
		if (!isBaseCanBuildUnit(baseId, unitId))
			continue;
		
		// seek for best target base gain
		
		double bestGain = 0.0;
		
		for (int targetBaseId : aiData.baseIds)
		{
			MAP *targetBaseTile = getBaseMapTile(targetBaseId);
			BaseInfo &targetBaseInfo = aiData.getBaseInfo(targetBaseId);
			BaseProbeData &targetBaseProbeData = targetBaseInfo.probeData;
			
			if (targetBaseProbeData.isSatisfied(false))
				continue;
			
			double travelTime = getUnitApproachTime(aiFactionId, unitId, baseTile, targetBaseTile);
			double travelTimeCoefficient = getExponentialCoefficient(conf.ai_base_threat_travel_time_scale, travelTime);
			
			// probe
			
			double combatEffect = newProbeMoraleMultiplier;
			double winningProbability = getWinningProbability(combatEffect);
			double unitProbeGain = techStealGain * winningProbability;
			double probeGain = unitProbeGain * travelTimeCoefficient;
			
			double upkeep = getResourceScore(-getUnitSupport(unitId), 0.0);
			double upkeepGain = getGainIncome(upkeep);
			
			// combined
			
			double gain =
				+ probeGain
				+ upkeepGain
			;
			
			bestGain = std::max(bestGain, gain);
			
			debug
			(
				"\t\t%-32s"
				" %-25s"
				" travelTime=%7.2f"
				" travelTimeCoefficient=%5.2f"
				" combatEffect=%5.2f"
				" winningProbability=%5.2f"
				" unitProbeGain=%5.2f"
				" probeGain=%5.2f"
				" upkeepGain=%5.2f"
				" gain=%7.2f"
				"\n"
				, unit->name
				, Bases[targetBaseId].name
				, travelTime
				, travelTimeCoefficient
				, combatEffect
				, winningProbability
				, unitProbeGain
				, probeGain
				, upkeepGain
				, gain
			);
			
		}
		
		// priority
		
		double rawPriority = getItemPriority(unitId, bestGain);
		double priority =
			conf.ai_production_base_probe_priority
			* rawPriority
		;
		
		// add production
		
		productionDemand.addItemPriority(unitId, priority);
		
		debug
		(
			"\t%-32s"
			" priority=%5.2f   |"
			" ai_production_base_probe_priority=%5.2f"
			" bestGain=%5.2f"
			" rawPriority=%5.2f"
			"\n"
			, Units[unitId].name
			, priority
			, conf.ai_production_base_probe_priority
			, bestGain
			, rawPriority
		);
		
	}
	
	Profiling::stop("evaluateDefensiveProbeUnits");
	
}

/*
Simple proportion to current combat units.
*/
void evaluateOffensiveProbeUnits()
{
	Profiling::start("evaluateOffensiveProbeUnits", "suggestBaseProduction");

	debug("evaluateOffensiveProbeUnits\n");

	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	MAP *baseTile = getBaseMapTile(baseId);

	// base new probe morale multiplier
	double newProbeMoraleMultiplier = has_facility(FAC_COVERT_OPS_CENTER, baseId) ? 1.25 : 1.00;

	// scan combat units for protection
	for (int unitId : aiFactionInfo->buildableUnitIds)
	{
		UNIT *unit = getUnit(unitId);

		// defensive probe

		if (!(isInfantryUnit(unitId) && isProbeUnit(unitId)))
			continue;

		// exclude those base cannot produce

		if (!isBaseCanBuildUnit(baseId, unitId))
			continue;

		// seek for best target base gain

		double bestGain = 0.0;

		for (int targetBaseId : aiData.baseIds)
		{
			MAP *targetBaseTile = getBaseMapTile(targetBaseId);
			BaseInfo &targetBaseInfo = aiData.getBaseInfo(targetBaseId);
			BaseProbeData &targetBaseProbeData = targetBaseInfo.probeData;

			if (targetBaseProbeData.isSatisfied(false))
				continue;

			double travelTime = getUnitApproachTime(aiFactionId, unitId, baseTile, targetBaseTile);
			double travelTimeCoefficient = getExponentialCoefficient(conf.ai_base_threat_travel_time_scale, travelTime);

			// probe

			double combatEffect = newProbeMoraleMultiplier;
			double winningProbability = getWinningProbability(combatEffect);
			double unitProbeGain = techStealGain * winningProbability;
			double probeGain = unitProbeGain * travelTimeCoefficient;

			double upkeep = getResourceScore(-getUnitSupport(unitId), 0.0);
			double upkeepGain = getGainIncome(upkeep);

			// combined

			double gain =
				+ probeGain
				+ upkeepGain
			;

			bestGain = std::max(bestGain, gain);

			debug
			(
				"\t\t%-32s"
				" %-25s"
				" travelTime=%7.2f"
				" travelTimeCoefficient=%5.2f"
				" combatEffect=%5.2f"
				" winningProbability=%5.2f"
				" unitProbeGain=%5.2f"
				" probeGain=%5.2f"
				" upkeepGain=%5.2f"
				" gain=%7.2f"
				"\n"
				, unit->name
				, Bases[targetBaseId].name
				, travelTime
				, travelTimeCoefficient
				, combatEffect
				, winningProbability
				, unitProbeGain
				, probeGain
				, upkeepGain
				, gain
			);

		}

		// priority

		double rawPriority = getItemPriority(unitId, bestGain);
		double priority =
			conf.ai_production_base_probe_priority
			* rawPriority
		;

		// add production

		productionDemand.addItemPriority(unitId, priority);

		debug
		(
			"\t%-32s"
			" priority=%5.2f   |"
			" ai_production_base_probe_priority=%5.2f"
			" bestGain=%5.2f"
			" rawPriority=%5.2f"
			"\n"
			, Units[unitId].name
			, priority
			, conf.ai_production_base_probe_priority
			, bestGain
			, rawPriority
		);

	}

	Profiling::stop("evaluateOffensiveProbeUnits");

}

void evaluateExpansionUnits()
{
	Profiling::start("evaluateExpansionUnits", "suggestBaseProduction");
	
	debug("evaluateExpansionUnits\n");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	BASE *base = productionDemand.base;
	MAP *baseTile = getBaseMapTile(baseId);
	
	// verify base can build a colony
	
	if (!canBaseProduceColony(baseId))
	{
		debug("\tbase cannot build a colony\n");
		Profiling::stop("evaluateExpansionUnits");
		return;
	}
	
	// population limit coefficient
	// a base that cannot grow loses little by building a colony:
	// 2.0 at the population limit, 0.25 less for each citizen below it, not below 1.0
	// no limit (both population facilities built) gives 1.0
	// it applies only while the base cannot build the facility that raises the limit:
	// otherwise the base would keep building colonies at the limit instead of the facility that lets it grow
	
	int populationFacilityId = (!isBaseHasFacility(baseId, FAC_HAB_COMPLEX) ? FAC_HAB_COMPLEX : (!isBaseHasFacility(baseId, FAC_HABITATION_DOME) ? FAC_HABITATION_DOME : -1));
	bool populationFacilityAvailable = (populationFacilityId != -1 && isBaseCanBuildFacility(baseId, static_cast<FacilityId>(populationFacilityId)));
	
	double populationHeadroom = static_cast<double>(getBasePopulationLimit(baseId)) - static_cast<double>(base->pop_size);
	double populationLimitCoefficient = (populationFacilityAvailable ? 1.0 : std::clamp(2.0 - 0.25 * populationHeadroom, 1.0, 2.0));
	
	// citizen loss gain
	
	double citizenIncome = getBaseCitizenIncome(baseId);
	double citizenLossGain = 0.75 * getGainIncome(-citizenIncome);
	
	// process colony units
	
	for (int unitId : aiData.colonyUnitIds)
	{
		UNIT *unit = getUnit(unitId);
		int triad = unit->triad();
		
		// base cannot build sea colony
		
		if (triad == TRIAD_SEA && !isBaseAccessesWater(baseId))
			continue;
			
		// base adjacent sea region area is too small
		
		if (triad == TRIAD_SEA)
		{
			int seaCluster = getSeaCluster(baseTile);
			int seaClusterArea = getSeaClusterArea(seaCluster);
			
			if (seaClusterArea <= 10)
				continue;
			
		}
			
//		debug("\t[%3d] %-32s\n", unitId, unit->name);
		
		// iterate available build sites
		
		MAP *bestBuildSite = nullptr;
		double bestBuildSiteGain = 0.0;

		for (MAP *tile : availableBuildSites)
		{
			TileExpansionInfo &expansionTileInfo = getTileExpansionInfo(tile);
			bool ocean = aiData.getTileInfo(tile).ocean;
			
			// exclude unavailable build sites
			
			if (aiData.production.unavailableBuildSites.count(tile) != 0)
				continue;
			
			// matching realm
			
			if ((triad == TRIAD_LAND && ocean) || (triad == TRIAD_SEA && !ocean))
				continue;
				
			// check unit can reach destination
			
			if (!isUnitDestinationReachable(unitId, baseTile, tile))
				continue;
			
			// travel time

			double travelTime = getUnitApproachTime(aiFactionId, unitId, baseTile, tile);
			
			// yield score
			
			double buildSiteBaseGain = expansionTileInfo.buildSiteBaseGain;
			
			// build gain
			
			double buildSiteBuildGain = getGainDelay(buildSiteBaseGain, travelTime);
			
			// upkeep
			
			double upkeepGain = getGainTimeInterval(getGainIncome(getResourceScore(-getBaseNextUnitSupport(baseId, unitId), 0)), 0, travelTime);
			
			// combine
			
			double buildSiteGain = buildSiteBuildGain + upkeepGain;
			
			// update best
			
			if (buildSiteGain > bestBuildSiteGain)
			{
				bestBuildSite = tile;
				bestBuildSiteGain = buildSiteGain;
			}
			
//			debug
//			(
//				"\t\t%s"
//				" travelTime=%7.2f"
//				" buildSiteBaseGain=%5.2f"
//				" buildSiteBuildGain=%5.2f"
//				"\n"
//				, getLocationString(tile)
//				, travelTime
//				, buildSiteBaseGain
//				, buildSiteBuildGain
//			);
			
		}
		
		// territory coverage coefficient
		// the colonies of all factions heading to the same remaining territory cover a part of it, each with the average base territory
		// the more of the territory they cover, the less a new colony is worth:
		// no effect at or below the "full" coverage, the expansion stops at or above the "stop" coverage, linear between

		int colonyCount = 0;

		for (int colonyVehicleId : colonyVehicleIds)
		{
			if ((getUnit(getVehicle(colonyVehicleId)->unit_id)->triad() == TRIAD_SEA) != (triad == TRIAD_SEA))
				continue;
			if (!isUnitDestinationReachable(unitId, baseTile, getVehicleMapTile(colonyVehicleId)))
				continue;

			colonyCount++;

		}

		int remainingTileCount = 0;

		for (auto const &regionTerritoryEntry : regionTerritories)
		{
			RegionTerritory const &regionTerritory = regionTerritoryEntry.second;

			if (regionTerritory.ocean != (triad == TRIAD_SEA))
				continue;
			if (!isUnitDestinationReachable(unitId, baseTile, regionTerritory.tile))
				continue;

			remainingTileCount += regionTerritory.unownedTileCount;

		}

		double territoryCoverage = (remainingTileCount > 0 ? static_cast<double>(colonyCount) * averageBaseTerritory / static_cast<double>(remainingTileCount) : 1.0);
		double coverageFull = conf.ai_production_expansion_coverage_full;
		double coverageStop = conf.ai_production_expansion_coverage_stop;
		double territoryCoverageCoefficient = (coverageStop > coverageFull ? std::clamp((coverageStop - territoryCoverage) / (coverageStop - coverageFull), 0.0, 1.0) : 1.0);

		// combine with citizen loss gain

		double gain = bestBuildSiteGain + citizenLossGain;

		// priority

		double rawPriority = getItemPriority(unitId, gain);
		double priority =
			conf.ai_production_expansion_priority
			* globalColonyDemand
			* populationLimitCoefficient
			* territoryCoverageCoefficient
			* rawPriority
		;
		
		// set base demand
		
		productionDemand.addItemPriority(unitId, priority);
		

		
		debug
		(
			"\t%-32s"
			" priority=%5.2f   |"
			" citizenLossGain=%5.2f"
			" bestBuildSite=%s"
			" bestBuildSiteGain=%5.2f"
			" gain=%5.2f"
			" ai_production_expansion_priority=%5.2f"
			" globalColonyDemand=%5.2f"
			" populationLimitCoefficient=%5.2f"
			" territoryCoverageCoefficient=%5.2f"
			" territoryCoverage=%5.2f"
			" colonyCount=%d"
			" remainingTileCount=%d"
			" rawPriority=%5.2f"
			"\n"
			, unit->name
			, priority
			, citizenLossGain
			, getLocationString(bestBuildSite)
			, bestBuildSiteGain
			, gain
			, conf.ai_production_expansion_priority
			, globalColonyDemand
			, populationLimitCoefficient
			, territoryCoverageCoefficient
			, territoryCoverage
			, colonyCount
			, remainingTileCount
			, rawPriority
		);
		
	}
	
	Profiling::stop("evaluateExpansionUnits");
	
}

void evaluateTerraformUnits()
{
	struct TerraformingGain
	{
		double time;
		double income;
		double gain;
	};

	Profiling::start("evaluateTerraformUnits", "suggestBaseProduction");
	
	debug("evaluateTerraformUnits\n");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	MAP *baseTile = getBaseMapTile(baseId);
	int baseSeaCluster = getBaseSeaCluster(baseTile);
	
	// supported formers
	
	int supportedFormerCounts[3] = {0, 0, 0};
	for (int vehicleId : aiData.formerVehicleIds)
	{
		VEH *vehicle = getVehicle(vehicleId);
		int triad = vehicle->triad();
		
		if (vehicle->home_base_id != baseId)
			continue;
		
		supportedFormerCounts[triad]++;
		
	}
	
	// existing formers
	
	int existingFormerCounts[3] = {0, 0, 0};
	for (int vehicleId : aiData.formerVehicleIds)
	{
		MAP *vehicleTile = getVehicleMapTile(vehicleId);
		int chassisId = Units[Vehs[vehicleId].unit_id].chassis_id;
		CChassis &chassis = Chassis[chassisId];
		int triad = chassis.triad;
		int speed = chassis.speed;
		
		// same cluster
		
		switch (triad)
		{
		case TRIAD_AIR:
			if (!isSameAirCluster(chassisId, speed, baseTile, vehicleTile))
				continue;
			break;
		case TRIAD_SEA:
			if (!isSameSeaCluster(baseSeaCluster, vehicleTile))
				continue;
			break;
		case TRIAD_LAND:
			if (!isSameLandTransportedCluster(baseTile, vehicleTile))
				continue;
			break;
		}
		
		existingFormerCounts[triad]++;
		
	}
	
	// extra former gain
	
	double extraFormerGains[3] = {0.0, 0.0, 0.0};
	for (Triad triad : {TRIAD_AIR, TRIAD_SEA, TRIAD_LAND})
	{
		// for sea former base should have access to water
		
		if (triad == TRIAD_SEA && !(isBaseAccessesWater(baseId) && baseSeaCluster >= 0))
			continue;
		
		// variable set
		
		if (bestFormerChassisIds[triad] == -1 || bestFormerSpeeds[triad] == 0)
			continue;
		
		// average travel time and terraforming gains
		
		HarmonicSummaryStatistics distanceHarmonicSummary;
		std::vector<TerraformingGain> terraformingGains;
		for (TerraformingRequest &formerRequest : aiData.production.terraformingRequests)
		{
			TileInfo &tileInfo = aiData.getTileInfo(formerRequest.tile);
			
			// sensible terraforming time
			
			if (formerRequest.terraformingTime <= 0.0)
				continue;
			
			// compatible surface
			
			if ((triad == TRIAD_LAND && !tileInfo.land) || (triad == TRIAD_SEA && !tileInfo.ocean))
				continue;
			
			// same cluster
			
			switch (triad)
			{
			case TRIAD_AIR:
				if (!isSameAirCluster(bestFormerChassisIds[TRIAD_AIR], bestFormerSpeeds[TRIAD_AIR], baseTile, formerRequest.tile))
					continue;
				break;
			case TRIAD_SEA:
				if (!isSameSeaCluster(baseSeaCluster, formerRequest.tile))
					continue;
				break;
			case TRIAD_LAND:
				if (!isSameLandTransportedCluster(baseTile, formerRequest.tile))
					continue;
				break;
			}
			
			// distance
			
			double distance = static_cast<double>(getRange(baseTile, formerRequest.tile));
			distanceHarmonicSummary.add(distance);
			
			// terraforming gain
			
			terraformingGains.push_back({static_cast<double>(formerRequest.terraformingTime), formerRequest.incomeGain, 0.0});
			
		}
		double averageDistance = distanceHarmonicSummary.getHarmonicMean();
		double averateTravelTime = averageDistance / bestFormerSpeeds[triad];
		
		// update terraforming gains
		
		for (TerraformingGain &terraformingGain : terraformingGains)
		{
			terraformingGain.time += averateTravelTime;
			terraformingGain.gain = terraformingGain.income / terraformingGain.time;
		}
		
		// sort terraforming gains
		
		std::sort(terraformingGains.begin(), terraformingGains.end(), [](TerraformingGain const &a, TerraformingGain const &b){ return a.gain > b.gain; });
		
		// extra former gain
		
		double existingFormerCount = static_cast<double>(existingFormerCounts[triad]);
		double existingFormerAccumulatedTime = 0.0;
		double existingFormerTotalGain = 0.0;
		double extraFormerCount = existingFormerCount + 1.0;
		double extraFormerAccumulatedTime = 0.0;
		double extraFormerTotalGain = 0.0;
		for (TerraformingGain &terraformingGain : terraformingGains)
		{
			if (existingFormerCount > 0.0)
			{
				existingFormerAccumulatedTime += terraformingGain.time / existingFormerCount;
				existingFormerTotalGain += getGainDelay(getGainIncome(terraformingGain.income), existingFormerAccumulatedTime);
			}
			extraFormerAccumulatedTime += terraformingGain.time / extraFormerCount;
			extraFormerTotalGain += getGainDelay(getGainIncome(terraformingGain.income), extraFormerAccumulatedTime);
//debug
//(
//	"\tterraformingGain.time=%5.2f"
//	" terraformingGain.income=%5.2f"
//	" existingFormerAccumulatedTime=%5.2f"
//	" existingFormerGain=%5.2f"
//	" extraFormerAccumulatedTime=%5.2f"
//	" extraFormerGain=%5.2f"
//	" delta=%5.2f"
//	"\n"
//	, terraformingGain.time
//	, terraformingGain.income
//	, existingFormerAccumulatedTime
//	, getGainDelay(getGainIncome(terraformingGain.income), existingFormerAccumulatedTime)
//	, extraFormerAccumulatedTime
//	, getGainDelay(getGainIncome(terraformingGain.income), extraFormerAccumulatedTime)
//	, getGainDelay(getGainIncome(terraformingGain.income), extraFormerAccumulatedTime) - getGainDelay(getGainIncome(terraformingGain.income), existingFormerAccumulatedTime)
//);
		}
		extraFormerGains[triad] = extraFormerTotalGain - existingFormerTotalGain;
		
	}
debug("extraFormerGains= %5.2f %5.2f %5.2f\n", extraFormerGains[0], extraFormerGains[1], extraFormerGains[2]);
	
	// process available former units
	
	for (int unitId : aiData.formerUnitIds)
	{
		UNIT &unit = Units[unitId];
		int triad = unit.triad();
		
		// best unit speed
		
		if (unit.chassis_id != bestFormerChassisIds[triad])
			continue;
		
		// for sea former base should have access to water
		
		if (triad == TRIAD_SEA && !(isBaseAccessesWater(baseId) && baseSeaCluster >= 0))
			continue;
		
		// shoud have some gain
		
		if (extraFormerGains[triad] == 0.0)
			continue;
		
		// early base restriction
		// no more than one former of each triad
		
		if (*CurrentTurn < 40 && supportedFormerCounts[triad] >= 1)
			continue;
		
		// unit priority
		
		double unitPriorityCoefficient = getUnitPriorityCoefficient(baseId, unitId);
		
		if (unitPriorityCoefficient <= 0.0)
			continue;
		
		// terraforming gain
		
		double terraformingGain = extraFormerGains[triad];
		
		// upkeep gain
		
		double upkeepIncome = getResourceScore(-static_cast<double>(getUnitSupport(unitId)), 0.0);
		double upkeepGain = getGainIncome(upkeepIncome);
		
		// gain
		
		double gain = terraformingGain + upkeepGain;
		
		// priority
		
		double improvementPriority = 0.0;
		switch (triad)
		{
		case TRIAD_AIR:
			improvementPriority = 0.5 * (conf.ai_production_improvement_priority_sea + conf.ai_production_improvement_priority_land);
			break;
		case TRIAD_SEA:
			improvementPriority = conf.ai_production_improvement_priority_sea;
			break;
		case TRIAD_LAND:
			improvementPriority = conf.ai_production_improvement_priority_land;
			break;
		}
		
		double rawPriority = getItemPriority(unitId, gain);
		double priority =
			improvementPriority
			* unitPriorityCoefficient
			* rawPriority
		;
		
		productionDemand.addItemPriority(unitId, priority);
		
		debug
		(
			"\t%-32s"
			" priority=%5.2f   |"
			" terraformingGain=%5.2f"
			" upkeepGain=%5.2f"
			" gain=%5.2f"
			" rawPriority=%5.2f"
			" improvementPriority=%5.2f"
			" unitPriorityCoefficient=%5.2f"
			"\n"
			, unit.name
			, priority
			, terraformingGain
			, upkeepGain
			, gain
			, rawPriority
			, improvementPriority
			, unitPriorityCoefficient
		);
		
	}
	
	Profiling::stop("evaluateTerraformUnits");
	
}

void evaluateConvoyUnits()
{
	Profiling::start("evaluateConvoyUnits", "suggestBaseProduction");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	MAP *baseTile = getBaseMapTile(baseId);
	
	debug("evaluateConvoyUnits\n");
	
	// process available supply units
	
	for (int unitId : aiFactionInfo->buildableUnitIds)
	{
		UNIT *unit = getUnit(unitId);
		int triad = unit->triad();
		
		// crawler
		
		if (!isSupplyUnit(unitId))
			continue;
		
		// for sea crawler base should have access to water
		
		if (triad == TRIAD_SEA && !isBaseAccessesWater(baseId))
			continue;
		
		// unit priority
		
		double unitPriorityCoefficient = getUnitPriorityCoefficient(baseId, unitId);
		
		if (unitPriorityCoefficient <= 0.0)
			continue;
		
		// iterate convoy requests
		
		double bestGain = 0.0;
		for (ConvoyRequest &convoyRequest : aiData.production.convoyRequests)
		{
			// matching surface
			
			if ((is_ocean(convoyRequest.tile) && triad == TRIAD_LAND) || (!is_ocean(convoyRequest.tile) && triad == TRIAD_SEA))
				continue;
			
			// reachable
			
			if (!isUnitDestinationReachable(unitId, baseTile, convoyRequest.tile))
				continue;
			
			// travelTime
			
			double travelTime = getUnitApproachTime(aiFactionId, unitId, baseTile, convoyRequest.tile);
			if (travelTime == INF)
				continue;
			
			// gain
			
			double gain = getGainDelay(convoyRequest.gain, travelTime);
			
			bestGain = std::max(bestGain, gain);
			
		}
		
		// gain
		
		double incomeGain = bestGain;
		double upkeepGain = getGainIncome(getResourceScore(-getUnitSupport(unitId), 0.0));
		double gain = incomeGain + upkeepGain;
		
		// priority
		
		double priority = unitPriorityCoefficient * getItemPriority(unitId, gain);;
		
		productionDemand.addItemPriority(unitId, priority);
		
		debug
		(
			"\t%-32s"
			" priority=%5.2f   |"
			" unitPriorityCoefficient=%5.2f"
			" incomeGain=%5.2f"
			" upkeepGain=%5.2f"
			" gain=%5.2f"
			"\n"
			, unit->name
			, priority
			, unitPriorityCoefficient
			, incomeGain
			, upkeepGain
			, gain
		);
		
	}
	
	Profiling::stop("evaluateConvoyUnits");
	
}

/*
Evaluates the production of the units that pop pods.
The demand is the pods left without a vehicle by the pod assignment, so the vehicles already available are accounted for.
A unit serves the pods near the base that it reaches without transportation.
The gain is the discounted pod bonuses the unit collects popping them one by one, like the former production queue.
*/
void evaluatePodPoppingUnits()
{
	Profiling::start("evaluatePodPoppingUnits", "suggestBaseProduction");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	MAP *baseTile = getBaseMapTile(baseId);
	
	debug("evaluatePodPoppingUnits\n");

	// proximity of the pods by the unit triad: land, sea
	
	constexpr std::array<int, 2> SCAN_RANGES {10, 20};
	
	// scan combat units
	
	for (int unitId : aiData.combatUnitIds)
	{
		UNIT *unit = getUnit(unitId);
		int triad = unit->triad();

		// producible

		if (!isBaseCanBuildUnit(baseId, unitId))
			continue;

		// unit priority

		double unitPriorityCoefficient = getUnitPriorityCoefficient(baseId, unitId);
		if (unitPriorityCoefficient <= 0.0)
			continue;

		// pop popping unit

		if (!isPodPoppingUnit(unitId))
			continue;

		// surface triad
		
		if (triad == TRIAD_AIR)
			continue;
		
		// base needs access to water to collect sea pods
		
		if (triad == TRIAD_SEA && !isBaseAccessesWater(baseId))
			continue;
		
		// fastest triad chassis
		
		if (unit->speed() < aiFactionInfo->fastestTriadChassisIds.at(triad))
			continue;

		// speed
		
		int unitSpeed = getUnitSpeed(aiFactionId, unitId);
		if (unitSpeed <= 0)
			continue;

		// pod requests near the base that the unit reaches without transportation

		std::vector<std::pair<double, double>> podTravelTimeGains;

		for (PodRequest const &podRequest : aiData.production.podRequests)
		{
			// matching surface
			
			if (aiData.getTileInfo(podRequest.tile).ocean != (triad == TRIAD_SEA))
				continue;
			
			// proximity
			
			if (getRange(baseTile, podRequest.tile) > SCAN_RANGES.at(triad))
				continue;
			
			// same cluster
			
			if ((triad == TRIAD_LAND && !isSameLandCluster(baseTile, podRequest.tile)) || (triad == TRIAD_SEA && !isSameSeaCluster(baseTile, podRequest.tile)))
				continue;
			
			// travel time
			
			double travelTime = getUnitApproachTime(aiFactionId, unitId, baseTile, podRequest.tile);
			if (travelTime == INF)
				continue;
			
			podTravelTimeGains.push_back({travelTime, podRequest.gain});

		}

		// there are pods to pop

		if (podTravelTimeGains.empty())
			continue;

		// the unit pops the pods one by one, the closest first
		// every pod pop is followed by the average recovery time: a slow unit cannot disengage from wildlife and is damaged more
		// the gain is how much of the pod bonuses the unit collects before they lose value to the delay
		// the pods already served by the available vehicles are not in the requests

		std::sort(podTravelTimeGains.begin(), podTravelTimeGains.end());

		double recoveryTime = unitSpeed == 1 ? 2.0 : 1.0;
		double minTravelTime = podTravelTimeGains.front().first;
		double podPoppingIncomeGain = 0.0;

		for (size_t index = 0; index < podTravelTimeGains.size(); index++)
		{
			double delay = podTravelTimeGains.at(index).first + static_cast<double>(index) * recoveryTime;
			podPoppingIncomeGain += getGainDelay(podTravelTimeGains.at(index).second, delay);
		}

		// upkeep
		
		double upkeepGain = getGainIncome(getResourceScore(-getUnitSupport(unitId), 0.0));
		
		// combined
		
		double gain =
			+ podPoppingIncomeGain
			+ upkeepGain
		;
		
		// priority
		
		double rawPriority = getItemPriority(unitId, gain);
		double priority =
			conf.ai_production_pod_popping_priority
			* unitPriorityCoefficient
			* rawPriority
		;
		
		// add production
		
		productionDemand.addItemPriority(unitId, priority);
		
		debug
		(
			"\t%-32s"
			" priority=%5.2f   |"
			" conf.ai_production_pod_popping_priority=%5.2f"
			" unitPriorityCoefficient=%5.2f"
			" minTravelTime=%5.2f"
			" podPoppingIncomeGain=%5.2f"
			" upkeepGain=%5.2f"
			" gain=%5.2f"
			" rawPriority=%5.2f"
			"\n"
			, Units[unitId].name
			, priority
			, conf.ai_production_pod_popping_priority
			, unitPriorityCoefficient
			, minTravelTime
			, podPoppingIncomeGain
			, upkeepGain
			, gain
			, rawPriority
		);
		
	}
	
	Profiling::stop("evaluatePodPoppingUnits");
	
}

/*
Units the base can produce to defend locations:
- infantry defenders with the best armor or psi
- land artillery and interceptors with the best weapon or psi
*/
static std::vector<int> getDefenseCandidateUnitIds(int baseId)
{
	std::vector<int> candidateUnitIds;
	
	for (int unitId : aiData.combatUnitIds)
	{
		UNIT *unit = getUnit(unitId);
		int offenseValue = unit->offense_value();
		int defenseValue = unit->defense_value();
		
		if (!isDefenseCandidateUnit(unitId))
			continue;
		
		bool bestArmor = (defenseValue < 0 || defenseValue >= aiFactionInfo->maxConDefenseValue);
		bool bestWeapon = (offenseValue < 0 || offenseValue >= aiFactionInfo->maxConOffenseValue);
		
		// armor matters for infantry, weapon matters for artillery and interceptors
		
		bool best = (isInfantryDefensiveUnit(unitId) ? bestArmor : bestWeapon);
		
		if (!best)
			continue;
		
		if (!isBaseCanBuildUnit(baseId, unitId))
			continue;
		
		candidateUnitIds.push_back(unitId);
		
	}
	
	return candidateUnitIds;
	
}

/*
Defended location a unit built at the base can protect: the unit can reach it.
*/
static bool isDefenseLocationServed(int baseId, int unitId, MAP const *tile)
{
	return getUnitApproachTime(aiFactionId, unitId, getBaseMapTile(baseId), tile) != INF;
}

/*
Efficiency of the unit built at the base at the defended location.
The unit is built with the morale of the base.
*/
static double getDefenseUnitEfficiency(DefendData &defendData, int baseId, int unitId)
{
	return defendData.getUnitEfficiency(aiFactionId, unitId, aiData.getBaseInfo(baseId).moraleMultipliers.at(getUnit(unitId)->triad()));
}

/*
Best efficiency of the candidate units built at the base over all defended locations the base can protect.
Efficiencies are divided by it: the best unit at the best location gets 1.0, the others get less.
So the absolute efficiency does not change the appeal of defenders, and the difference between units and between locations is kept.
*/
static double getBestDefenseEfficiency(int baseId, std::vector<int> const &candidateUnitIds)
{
	double bestEfficiency = 0.0;
	
	for (auto &[tile, defendData] : aiData.defendLocations)
	{
		for (int unitId : candidateUnitIds)
		{
			if (!isDefenseLocationServed(baseId, unitId, tile))
				continue;
			
			bestEfficiency = std::max(bestEfficiency, getDefenseUnitEfficiency(defendData, baseId, unitId));
		}
		
	}
	
	return bestEfficiency;
	
}

void evaluateBaseDefenseUnits()
{
	Profiling::start("evaluateBaseDefenseUnits", "suggestBaseProduction");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	MAP *baseTile = getBaseMapTile(baseId);

	debug("evaluateBaseDefenseUnits\n");
	
	// candidate units: best armor or psi defenders the base can produce
	
	std::vector<int> candidateUnitIds = getDefenseCandidateUnitIds(baseId);
	
	double bestEfficiency = getBestDefenseEfficiency(baseId, candidateUnitIds);
	
	// scan candidate units for protection
	
	for (int unitId : candidateUnitIds)
	{
		UNIT *unit = getUnit(unitId);
		

		// seek for best target base gain
		
		double bestGain = 0.0;
		
		for (int targetBaseId : aiData.baseIds)
		{
			MAP *targetBaseTile = getBaseMapTile(targetBaseId);
			BaseInfo &targetBaseInfo = aiData.baseInfos.at(targetBaseId);
			BasePoliceData &targetBasePoliceData = targetBaseInfo.policeData;
			auto targetDefendDataIterator = aiData.defendLocations.find(targetBaseTile);
			if (targetDefendDataIterator == aiData.defendLocations.end())
				continue;
			DefendData &targetDefendData = targetDefendDataIterator->second;
			
			double travelTime = getUnitApproachTime(aiFactionId, unitId, baseTile, targetBaseTile);
			double travelTimeCoefficient = getExponentialCoefficient(conf.ai_base_threat_travel_time_scale, travelTime);
			
			// police
			
			double unitPoliceGain = targetBasePoliceData.isSufficientForType(isPolice2xUnit(unitId, aiFactionId)) ? 0.0 : targetBasePoliceData.getUnitPoliceGain(unitId, aiFactionId);
			double policeGain = conf.ai_production_priority_police * getGainDelay(unitPoliceGain, travelTime);
			
			// protection
			
			// efficiency of the unit at the location relative to the best of all candidate units at all locations the base can protect:
			// the unit types and the locations are compared with each other, the appeal of defenders does not depend on the absolute efficiency
			double unitRelativeEfficiency = (isDefenseLocationServed(baseId, unitId, targetBaseTile) && bestEfficiency > 0.0 ? getDefenseUnitEfficiency(targetDefendData, baseId, unitId) / bestEfficiency : 0.0);
			double unitProtectionGain = targetDefendData.targetGain * unitRelativeEfficiency;
			double protectionGain = unitProtectionGain * travelTimeCoefficient;

			double upkeep = getResourceScore(-getBaseNextUnitSupport(baseId, unitId), 0);
			double upkeepGain = getGainIncome(upkeep);

			// combined

			double gain =
				+ policeGain
				+ protectionGain
				+ upkeepGain
			;
			
			bestGain = std::max(bestGain, gain);
			
			debug
			(
				"\t\t%-32s"
				" %-25s"
				" travelTime=%7.2f"
				" travelTimeCoefficient=%5.2f"
				" ai_production_priority_police=%5.2f"
				" unitPoliceGain=%5.2f"
				" policeGain=%5.2f"
				" ai_production_base_protection_priority=%5.2f"
				" unitRelativeEfficiency=%5.2f"
				" unitProtectionGain=%5.2f"
				" protectionGain=%5.2f"
				" upkeepGain=%5.2f"
				" gain=%7.2f"
				"\n"
				, unit->name
				, Bases[targetBaseId].name
				, travelTime
				, travelTimeCoefficient
				, conf.ai_production_priority_police
				, unitPoliceGain
				, policeGain
				, conf.ai_production_base_protection_priority
				, unitRelativeEfficiency
				, unitProtectionGain
				, protectionGain
				, upkeepGain
				, gain
			);
			
		}
		
		// priority
		
		double rawPriority = getItemPriority(unitId, bestGain);
		double priority =
			conf.ai_production_base_protection_priority
			* rawPriority
		;
		
		// add production
		
		productionDemand.addItemPriority(unitId, priority);
		
		debug
		(
			"\t%-32s"
			" priority=%5.2f   |"
			" ai_production_base_protection_priority=%5.2f"
			" bestGain=%5.2f"
			" rawPriority=%5.2f"
			"\n"
			, Units[unitId].name
			, priority
			, conf.ai_production_base_protection_priority
			, bestGain
			, rawPriority
		);
		
	}
	
	Profiling::stop("evaluateBaseDefenseUnits");
	
}

void evaluateBunkerDefenseUnits()
{
	Profiling::start("evaluateBunkerDefenseUnits", "suggestBaseProduction");
	
	debug("evaluateBunkerDefenseUnits\n");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	MAP *baseTile = getBaseMapTile(baseId);

	// scan candidate units for protection
	
	std::vector<int> candidateUnitIds = getDefenseCandidateUnitIds(baseId);
	
	double bestEfficiency = getBestDefenseEfficiency(baseId, candidateUnitIds);
	
	for (int unitId : candidateUnitIds)
	{
		

		// seek for best target base gain
		
		double bestGain = 0.0;
		
		for (robin_hood::pair<MAP const*, BunkerInfo> &bunkerInfoEntry : aiData.bunkerInfos)
		{
			MAP const *targetBunkerTile = bunkerInfoEntry.first;
			auto targetDefendDataIterator = aiData.defendLocations.find(targetBunkerTile);
			if (targetDefendDataIterator == aiData.defendLocations.end())
				continue;
			DefendData &targetDefendData = targetDefendDataIterator->second;
			
			double travelTime = getUnitApproachTime(aiFactionId, unitId, baseTile, targetBunkerTile);
			if (travelTime == INF)
				continue;
			double travelTimeCoefficient = getExponentialCoefficient(conf.ai_base_threat_travel_time_scale, travelTime);
			
			// protection

			// efficiency of the unit at the location relative to the best of all candidate units at all locations the base can protect
			double unitRelativeEfficiency = (isDefenseLocationServed(baseId, unitId, targetBunkerTile) && bestEfficiency > 0.0 ? getDefenseUnitEfficiency(targetDefendData, baseId, unitId) / bestEfficiency : 0.0);
			double unitProtectionGain = targetDefendData.targetGain * unitRelativeEfficiency;
			double protectionGain = unitProtectionGain * travelTimeCoefficient;
			
			double upkeep = getResourceScore(-getBaseNextUnitSupport(baseId, unitId), 0);
			double upkeepGain = getGainIncome(upkeep);
			
			// combined
			
			double gain =
				+ protectionGain
				+ upkeepGain
			;
			
			bestGain = std::max(bestGain, gain);
			
//			debug
//			(
//				"\t\t%-32s"
//				" %-25s"
//				" travelTime=%7.2f"
//				" travelTimeCoefficient=%5.2f"
//				" ai_production_priority_police=%5.2f"
//				" unitPoliceGain=%5.2f"
//				" policeGain=%5.2f"
//				" ai_production_base_protection_priority=%5.2f"
//				" combatEffect=%5.2f"
//				" survivalEffect=%5.2f"
//				" unitProtectionGain=%5.2f"
//				" protectionGain=%5.2f"
//				" upkeepGain=%5.2f"
//				" gain=%7.2f"
//				"\n"
//				, unit->name
//				, Bases[targetBaseId].name
//				, travelTime
//				, travelTimeCoefficient
//				, conf.ai_production_priority_police
//				, unitPoliceGain
//				, policeGain
//				, conf.ai_production_base_protection_priority
//				, combatEffect
//				, survivalEffect
//				, unitProtectionGain
//				, protectionGain
//				, upkeepGain
//				, gain
//			);
			
		}
		
		// priority
		
		double rawPriority = getItemPriority(unitId, bestGain);
		double priority =
			conf.ai_production_base_protection_priority
			* rawPriority
		;
		
		// add production
		
		productionDemand.addItemPriority(unitId, priority);
		
		debug
		(
			"\t%-32s"
			" priority=%5.2f   |"
			" ai_production_base_protection_priority=%5.2f"
			" bestGain=%5.2f"
			" rawPriority=%5.2f"
			"\n"
			, Units[unitId].name
			, priority
			, conf.ai_production_base_protection_priority
			, bestGain
			, rawPriority
		);
		
	}
	
	Profiling::stop("evaluateBunkerDefenseUnits");
	
}

void evaluateTerritoryProtectionUnits()
{
	Profiling::start("evaluateTerritoryProtectionUnits", "suggestBaseProduction");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	MAP *baseTile = getBaseMapTile(baseId);
	
	debug("evaluateTerritoryProtectionUnits\n");
	
	// scan combat units for protection
	
	for (int unitId : aiData.combatUnitIds)
	{
		UNIT *unit = getUnit(unitId);
//		int offenseValue = unit->offense_value();
//		int defenseValue = unit->defense_value();
		
//		// best weapon and armor or psi
//		
//		if (!((offenseValue < 0 || offenseValue >= aiFactionInfo->maxConOffenseValue) && (defenseValue < 0 || defenseValue >= std::min(aiFactionInfo->maxConOffenseValue, aiFactionInfo->maxConDefenseValue))))
//			continue;
//		
//		// fastest conventional chassis
//		
//		if (offenseValue > 0 && unit->chassis_id != aiFactionInfo->fastestTriadChassisIds.at(unit->triad()))
//			continue;
//		
		// exclude those base cannot produce
		
		if (!isBaseCanBuildUnit(baseId, unitId))
			continue;
		
		// unit priority
		
		double unitPriorityCoefficient = getUnitPriorityCoefficient(baseId, unitId);
		
		if (unitPriorityCoefficient <= 0.0)
			continue;
		
		// seek for best unsatisfied target stack gain
		
		double bestGain = 0.0;
		
		for (EnemyStackInfo *enemyStackInfo : aiData.production.untargetedEnemyStackInfos)
		{
			// enemy
			
			if (!enemyStackInfo->hostile)
				continue;
			
			// not sufficiently targeted
			
			if (!enemyStackInfo->isSufficient(false))
				continue;
			
			// ship and artillery do not attack tower
			
			if (isArtilleryUnit(unitId) && enemyStackInfo->alienFungalTower)
				continue;
			
			// get attack position
			
			MapDoubleValue position(nullptr, INF);
			bool direct = true;

			if (position.tile == nullptr && enemyStackInfo->isUnitCanMeleeAttackStack(unitId))
			{
				position = getMeleeAttackPosition(unitId, baseTile, enemyStackInfo->tile);
				direct = true;
			}
			if (position.tile == nullptr && enemyStackInfo->isUnitCanArtilleryAttackStack(unitId))
			{
				position = getArtilleryAttackPosition(unitId, baseTile, enemyStackInfo->tile);
				direct = enemyStackInfo->artillery;
			}
			
			if (position.tile == nullptr)
				continue;
			
			// travel time
			
			double travelTime = position.value;
//			double travelTimeCoefficient = getExponentialCoefficient(conf.ai_combat_travel_time_scale, travelTime);
			
			// gain
			
			double combatEffect;
			double combatValue;

			if (direct)
			{
				combatEffect = enemyStackInfo->getUnitDirectEffect(unitId);
				double winningProbability = getWinningProbability(combatEffect);
				double defenderDestructionGain = winningProbability * enemyStackInfo->destructionGain;
				double attackerDestructionGain = (1.0 - winningProbability) * aiData.unitDestructionGains.at(unitId);
				combatValue = defenderDestructionGain - attackerDestructionGain;
			}
			else
			{
				combatEffect = enemyStackInfo->getUnitBombardmentEffect(unitId);
				double defenderDestructionGain = combatEffect * enemyStackInfo->destructionGain;
				double attackerDestructionGain = 0.0;
				combatValue = defenderDestructionGain - attackerDestructionGain;
			}

			double attackGain = getGainDelay(getGainBonus(combatValue), travelTime);
			
			double upkeep = getResourceScore(-getUnitSupport(unitId), 0);
			double upkeepGain = getGainIncome(upkeep);
			
			double gain =
				+ attackGain
				+ upkeepGain
			;
			
			bestGain = std::max(bestGain, gain);
			
			debug
			(
				"\t\t%-32s"
				" -> %s"
				" travelTime=%7.2f"
				" combatEffect=%5.2f"
				" attackGain=%5.2f"
				" upkeepGain=%5.2f"
				" gain=%7.2f"
				"\n"
				, unit->name
				, getLocationString(enemyStackInfo->tile)
				, travelTime
				, combatEffect
				, attackGain
				, upkeepGain
				, gain
			);
			
		}
		
		// priority
		
		double rawPriority = getItemPriority(unitId, bestGain);
		double priority =
			conf.ai_production_base_protection_priority
			* unitPriorityCoefficient
			* rawPriority
		;
		
		// add production
		
		productionDemand.addItemPriority(unitId, priority);
		
		debug
		(
			"\t%-32s"
			" priority=%5.2f   |"
			" unitPriorityCoefficient=%5.2f"
			" bestGain=%5.2f"
			" rawPriority=%5.2f"
			"\n"
			, Units[unitId].name
			, priority
			, unitPriorityCoefficient
			, bestGain
			, rawPriority
		);
		
	}
	
	Profiling::stop("evaluateTerritoryProtectionUnits");
	
}

void evaluateEnemyBaseAssaultUnits()
{
	Profiling::start("evaluateEnemyBaseAssaultUnits", "suggestBaseProduction");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	MAP *baseTile = getBaseMapTile(baseId);
	
	debug("evaluateEnemyBaseAssaultUnits\n");
	
	// scan combat units
	
	for (int unitId : aiData.combatUnitIds)
	{
		UNIT *unit = getUnit(unitId);
		int offenseValue = unit->offense_value();
		int defenseValue = unit->defense_value();
		
		// best weapon and armor or psi
		
		if (!((offenseValue < 0 || offenseValue >= aiFactionInfo->maxConOffenseValue) && (defenseValue < 0 || defenseValue >= std::min(aiFactionInfo->maxConOffenseValue, aiFactionInfo->maxConDefenseValue))))
			continue;
		
		// exclude those base cannot produce
		
		if (!isBaseCanBuildUnit(baseId, unitId))
			continue;
		
		// unit priority
		
		double unitPriorityCoefficient = getUnitPriorityCoefficient(baseId, unitId);
		
		if (unitPriorityCoefficient <= 0.0)
		{
			Profiling::stop("evaluateEnemyBaseAssaultUnits");
			return;
		}
		
		debug("\t%-32s\n", unit->name);

		// iterate potential enemy bases
		
		double bestGain = 0.0;
		
		for (int enemyBaseId = 0; enemyBaseId < *BaseCount; enemyBaseId++)
		{
			BASE *enemyBase = getBase(enemyBaseId);
			MAP *enemyBaseTile = getBaseMapTile(enemyBaseId);
			BaseInfo &enemyBaseInfo = aiData.getBaseInfo(enemyBaseId);
			
			// enemy base stack
			
			if (!aiData.isEnemyStackAt(enemyBaseTile))
				continue;

			// exclude player base or friendly base
			
			if (isFriendly(aiFactionId, enemyBase->faction_id))
				continue;
			
			// travel time
			
			double travelTime = getUnitApproachTime(aiFactionId, unitId, baseTile, enemyBaseTile);
			
			if (travelTime == INF)
				continue;
			
			// proportional base capture value
			
			double unitContribution = enemyBaseInfo.combatData.getAssailantUnitContribution(aiFactionId, unitId);
			
			if (unitContribution <= 0.0)
				continue;
			
			double unitEnemyBaseCaptureGain  = enemyBaseInfo.captureGain * unitContribution;
			double incomeGain = getGainDelay(unitEnemyBaseCaptureGain, travelTime);
			
			// range coefficient
			
			double rangeCoefficient = enemyBaseInfo.closestPlayerBaseRange <= TARGET_ENEMY_BASE_RANGE ? 1.0 : static_cast<double>(TARGET_ENEMY_BASE_RANGE) / static_cast<double>(enemyBaseInfo.closestPlayerBaseRange);
			double captureGain = rangeCoefficient * incomeGain;
			
			// upkeep gain
			
			double upkeepGain = getGainTimeInterval(getGainIncome(getResourceScore(-getUnitSupport(unitId), 0)), 0.0, travelTime);
			
			// combined gain
			
			double gain =
				+ captureGain
				+ upkeepGain
			;
			
			bestGain = std::max(bestGain, gain);
			
//			debug
//			(
//				"\t\t%-25s"
//				" travelTime=%7.2f"
//				" assaultEffect=%5.2f"
//				" productionRatio=%5.2f"
//				" costRatio=%5.2f"
//				" adjustedEffect=%5.2f"
//				" adjustedSuperiority=%5.2f"
//				" enemyBaseCaptureGain=%5.2f"
//				" adjustedEnemyBaseCaptureGain=%5.2f"
//				" incomeGain=%5.2f"
//				" closestPlayerBaseRange=%2d"
//				" rangeCoefficient=%5.2f"
//				" captureGain=%5.2f"
//				" upkeepGain=%5.2f"
//				" gain=%5.2f"
//				"\n"
//				, enemyBase->name
//				, travelTime
//				, assaultEffect
//				, productionRatio
//				, costRatio
//				, adjustedEffect
//				, adjustedSuperiority
//				, enemyBaseCaptureGain
//				, adjustedEnemyBaseCaptureGain
//				, incomeGain
//				, enemyBaseInfo.closestPlayerBaseRange
//				, rangeCoefficient
//				, captureGain
//				, upkeepGain
//				, gain
//			);
			
		}
		
		// priority
		
		double rawPriority = getItemPriority(unitId, bestGain);
		double priority =
			unitPriorityCoefficient
			* rawPriority
		;
		
		// add production
		
		productionDemand.addItemPriority(unitId, priority);
		
//		debug
//		(
//			"\t%-32s"
//			" priority=%5.2f   |"
//			" unitPriorityCoefficient=%5.2f"
//			" landArtillerySaturationCoefficient=%5.2f"
//			" bestGain=%5.2f"
//			" rawPriority=%5.2f"
//			"\n"
//			, Units[unitId].name
//			, priority
//			, unitPriorityCoefficient
//			, landArtillerySaturationCoefficient
//			, bestGain
//			, rawPriority
//		);
		
	}
	
	Profiling::stop("evaluateEnemyBaseAssaultUnits");
	
}

void evaluateSeaTransport()
{
	Profiling::start("evaluateSeaTransport", "suggestBaseProduction");
	
	debug("evaluateSeaTransport\n");
	
	ProductionDemand &productionDemand = *currentBaseProductionDemand;
	int baseId = productionDemand.baseId;
	int baseSeaCluster = productionDemand.baseSeaCluster;
	
	// base should have access to water
	
	if (baseSeaCluster == -1)
	{
		Profiling::stop("evaluateSeaTransport");
		return;
	}
	
	// find transport unit
	
	int unitId = aiFactionInfo->bestSeaTransportUnitId;
	
	if (unitId == -1)
	{
		debug("\tno seaTransport unit\n");
		Profiling::stop("evaluateSeaTransport");
		return;
	}
	
	double seaTransportDemand = seaTransportDemands[baseSeaCluster];
	
	if (seaTransportDemand <= 0.0)
	{
		debug("seaTransportDemand <= 0.0\n");
		Profiling::stop("evaluateSeaTransport");
		return;
	}
	
	// transport priority is proportional to the demand: the more cargo waits, the more urgent the transport is

	double unitPriorityCoefficient = getUnitPriorityCoefficient(baseId, unitId);
	double priority =
		conf.ai_production_transport_priority
		* seaTransportDemand
		* unitPriorityCoefficient
	;

	productionDemand.addItemPriority(unitId, priority);

	debug
	(
		"\t%-32s"
		" priority=%5.2f   |"
		" seaTransportDemand=%5.2f"
		" conf.ai_production_transport_priority=%5.2f"
		" unitPriorityCoefficient=%5.2f"
		"\n"
		, getUnit(unitId)->name
		, priority
		, seaTransportDemand
		, conf.ai_production_transport_priority
		, unitPriorityCoefficient
	);

	Profiling::stop("evaluateSeaTransport");

}

int findScoutUnit(int triad)
{
	int bestUnitId = -1;
	double bestProportionalUnitChassisSpeed = -1;
	int bestUnitChassisSpeed = -1;

	for (int unitId : aiData.combatUnitIds)
	{
		UNIT *unit = &(Units[unitId]);

		// given triad only

		if (unit_triad(unitId) != triad)
			continue;

		// calculate speed and proportional speed

		int unitChassisSpeed = unit_chassis_speed(unitId);
		double proportionalUnitChassisSpeed = static_cast<double>(unit_chassis_speed(unitId)) / static_cast<double>(unit->cost);

		// find fastest/cheapest unit

		if
		(
			bestUnitId == -1
			||
			proportionalUnitChassisSpeed > bestProportionalUnitChassisSpeed
			||
			(proportionalUnitChassisSpeed == bestProportionalUnitChassisSpeed && unitChassisSpeed > bestUnitChassisSpeed)
		)
		{
			bestUnitId = unitId;
			bestProportionalUnitChassisSpeed = proportionalUnitChassisSpeed;
			bestUnitChassisSpeed = unitChassisSpeed;
		}

	}

	return bestUnitId;

}

/**
Checks if base has enough population and minerals to issue a colony by the time it is built.
*/
bool canBaseProduceColony(int baseId)
{
	BASE *base = &(Bases[baseId]);
	
	if (base->mineral_surplus <= 1)
		return false;
	
	// projectedTime
	
	int buildTime = getBaseItemBuildTime(baseId, BSC_COLONY_POD, true);
	int projectedSize = getBaseProjectedSize(baseId, buildTime);
	
	if (projectedSize == 1)
		return false;
	
	// set projected
	
	base->pop_size = projectedSize - 1;
	Profiling::start("- canBaseProduceColony - computeBase");
	computeBase(baseId, true);
	Profiling::stop("- canBaseProduceColony - computeBase");
	
	int projectedMineralSurplus = base->mineral_surplus;
	
	// restore
	
	aiData.resetBase(baseId);
	
	// check surplus is not below zero
	
	if (projectedMineralSurplus < 0)
		return false;
	
	// all checks passed
	
	return true;
	
}

int getRegionBasesMaxMineralSurplus(int factionId, int region)
{
	int maxMineralSurplus = 0;

	for (int baseId : getRegionBases(factionId, region))
	{
		BASE *base = &(Bases[baseId]);

		maxMineralSurplus = std::max(maxMineralSurplus, base->mineral_surplus);

	}

	return maxMineralSurplus;

}

int getRegionBasesMaxPopulationSize(int factionId, int region)
{
	int maxPopulationSize = 0;

	for (int baseId : getRegionBases(factionId, region))
	{
		BASE *base = &(Bases[baseId]);

		maxPopulationSize = std::max(maxPopulationSize, static_cast<int>(base->pop_size));

	}

	return maxPopulationSize;

}

/*
Calculates number of units with specific weaponType: existing and building.
Only corresponding triad units are counted.
*/
int calculateUnitTypeCount(int factionId, int weaponType, int triad, int excludedBaseId)
{
	int unitTypeCount = 0;

	for (int vehicleId = 0; vehicleId < *VehCount; vehicleId++)
	{
		VEH *vehicle = getVehicle(vehicleId);

		if (vehicle->faction_id != factionId)
			continue;

		if (Units[vehicle->unit_id].weapon_id != weaponType)
			continue;

		if ((triad != -1) && (vehicle->triad() != triad))
			continue;

		unitTypeCount++;

	}

	for (int baseId = 0; baseId < *BaseCount; baseId++)
	{
		// exclude this base

		if (baseId == excludedBaseId)
			continue;

		BASE *base = &(Bases[baseId]);
		int item = base->queue_items[0];

		if (base->faction_id != factionId)
			continue;

		if (item < 0)
			continue;

		if (Units[item].weapon_id != weaponType)
			continue;

		if (unit_triad(item) != triad)
			continue;

		unitTypeCount++;

	}

	return unitTypeCount;

}

/*
Checks for military purpose unit/structure.
*/
bool isMilitaryItem(int item)
{
	if (item >= 0)
	{
		UNIT *unit = &(Units[item]);
		int weaponId = unit->weapon_id;

		return
			(isCombatUnit(item) && !isScoutUnit(item))
			||
			weaponId == WPN_PROBE_TEAM
			||
			weaponId == WPN_CONVENTIONAL_PAYLOAD
			||
			weaponId == WPN_PLANET_BUSTER
		;

	}
	else
	{
		return false;
	}

}

/*
Returns first available but unbuilt facility from list.
*/
int getFirstAvailableFacility(int baseId, const std::vector<FacilityId> &facilityIds)
{
	for (FacilityId facilityId : facilityIds)
	{
		if (isBaseCanBuildFacility(baseId, facilityId))
		{
			return facilityId;
		}
		
	}
	
	// nothing is available
	
	return -1;
	
}

/**
Calculates unit priority reduction based on existing support.
*/
double getUnitPriorityCoefficient(int baseId, int unitId)
{
	constexpr double maxSupportRatio = 0.5;
	BASE *base = &Bases[baseId];
	
	// at max
	if (*VehCount >= MaxVehNum)
		return 0.0;
	
	// baseNextUnitSupport
	int nextUnitSupport = getBaseNextUnitSupport(baseId, unitId);
	if (nextUnitSupport == 0)
		return 1.0;
	
	// unit intake support ratio
	int nextUnitTotalSupport = base->mineral_intake_2 - base->mineral_surplus + nextUnitSupport;
	double nextUnitTotalSupportRatio = static_cast<double>(nextUnitTotalSupport) / static_cast<double>(base->mineral_intake_2);

	if (nextUnitTotalSupportRatio >= maxSupportRatio)
		return 0.0;
	
	return 1.0 - nextUnitTotalSupportRatio / maxSupportRatio;
	
}

int findInfantryPoliceUnit(bool first)
{
    int bestUnitId = -1;
    double bestUnitEffectiveness = 0.0;

    // search among police2x

    for (int unitId : aiData.combatUnitIds)
	{
        UNIT *unit = &Units[unitId];
        int defenseValue = getUnitDefenseValue(unitId);

        // infantry police2x

        if (!isInfantryDefensivePolice2xUnit(unitId, aiFactionId))
			continue;

		// calculate effectiveness

		double benefit = 1.0 + (first ? 0.0 : (static_cast<double>(defenseValue - 1) / static_cast<double>(aiData.maxConDefenseValue)) / 2.0);
		double cost = static_cast<double>(unit->cost);

		if (!isUnitHasAbility(unitId, ABL_CLEAN_REACTOR))
		{
			cost += 8.0;
		}

		if (isUnitHasAbility(unitId, ABL_TRANCE))
		{
			benefit *= getPercentageBonusMultiplier(Rules->combat_bonus_trance_vs_psi / 2);
		}

		double effectiveness = (cost <= 0.0 ? 0.0 : benefit / cost);

		if (effectiveness > bestUnitEffectiveness)
		{
			bestUnitId = unitId;
			bestUnitEffectiveness = effectiveness;
		}

	}

	if (bestUnitId != -1)
	{
		return bestUnitId;
	}

	// search among others

    for (int unitId : aiData.combatUnitIds)
	{
        UNIT *unit = &Units[unitId];
        int defenseValue = getUnitDefenseValue(unitId);

        // infantry

        if (!isInfantryDefensiveUnit(unitId))
			continue;

		// calculate effectiveness

		double benefit = 1.0 + (first ? 0.0 : (static_cast<double>(defenseValue - 1) / static_cast<double>(aiData.maxConDefenseValue)) / 2.0);
		double cost = static_cast<double>(unit->cost);

		if (!isUnitHasAbility(unitId, ABL_CLEAN_REACTOR))
		{
			cost += 8.0;
		}

		if (isUnitHasAbility(unitId, ABL_TRANCE))
		{
			benefit *= getPercentageBonusMultiplier(Rules->combat_bonus_trance_vs_psi / 2);
		}

		double effectiveness = (cost <= 0.0 ? 0.0 : benefit / cost);

		if (effectiveness > bestUnitEffectiveness)
		{
			bestUnitId = unitId;
			bestUnitEffectiveness = effectiveness;
		}

	}

	return bestUnitId;

}

// assigned defenders that arrive later than this (turns) do not stop the rush of a protective unit
constexpr double HURRY_DEFENDER_ARRIVAL_TIME = 2.0;

void hurryProtectiveUnit()
{
	Profiling::start("hurryProtectiveUnit", "productionStrategy");
	
	debug("hurryProtectiveUnit - %s\n", getMFaction(aiFactionId)->noun_faction);

	// the base with the highest value at risk among those that build a defender for insufficiently defended location
	// defenders still far away do not count as defending: the base stays unprotected until they arrive
	int mostVulnerableBaseId = -1;
	double mostVulnerableBaseGain = 0.0;

	for (int baseId : aiData.baseIds)
	{
		debug("\t%-25s\n", getBase(baseId)->name);

		int item = getBaseBuildingItem(baseId);

		debug("\t\titem=%3d\n", item);

		if (item < 0 || !isDefenseCandidateUnit(item))
		{
			debug("\t\tnot infantry devensive\n");
			continue;
		}

		auto defendDataIterator = aiData.defendLocations.find(getBaseMapTile(baseId));
		if (defendDataIterator == aiData.defendLocations.end())
			continue;

		DefendData &defendData = defendDataIterator->second;

		// sufficiently defended soon
		if (defendData.isSufficientWithin(HURRY_DEFENDER_ARRIVAL_TIME))
			continue;

		debug("\t\ttargetGain=%5.2f\n", defendData.targetGain);

		if (defendData.targetGain > mostVulnerableBaseGain)
		{
			mostVulnerableBaseId = baseId;
			mostVulnerableBaseGain = defendData.targetGain;
		}

	}

	if (mostVulnerableBaseId == -1)
	{
		Profiling::stop("hurryProtectiveUnit");
		return;
	}

	debug
	(
		"\tmostVulnerableBase=%-25s"
		" mostVulnerableBaseGain=%5.2f"
		"\n"
		, getBase(mostVulnerableBaseId)->name
		, mostVulnerableBaseGain
	);

	int item = getBaseBuildingItem(mostVulnerableBaseId);

	if (item < 0 || !isDefenseCandidateUnit(item))
	{
		debug("\t\tnot infantry devensive\n");
		Profiling::stop("hurryProtectiveUnit");
		return;
	}

	// hurry production

	int spendPool = getFaction(aiFactionId)->energy_credits;

	hurryProductionPartially(mostVulnerableBaseId, spendPool);

	debug("\t%-25s spendPool=%4d\n", getBase(mostVulnerableBaseId)->name, spendPool);

	Profiling::stop("hurryProtectiveUnit");
	
}

//=======================================================
// estimated income functions
//=======================================================

/**
Computes facility gain by comparing to base state with and without it.
1. immediate resouce income improvement
2. income growth due to increase in nutrient surplus
3. implicit income due to reduction in eco-damage
*/
double getFacilityGain(int baseId, int facilityId, bool build)
{
	BASE *base = getBase(baseId);
	
	if (build)
	{
		// cannot build
		
		if (!isBaseFacilityAvailable(baseId, facilityId))
		{
			return 0.0;
		}
		
	}
	else
	{
		// cannot remove
		
		if (!isBaseHasFacility(baseId, facilityId))
		{
			return 0.0;
		}
		
	}
	
	// compute old income
	
	double oldPopulationGrowth = getBasePopulationGrowth(baseId);
	double oldIncome = getBaseIncome(baseId);
	int oldEcoDamage = std::min(100, base->eco_damage);
	
	// compute new income
	
	setBaseFacility(baseId, facilityId, build);
	Profiling::start("- getFacilityGain - computeBase");
	computeBase(baseId, true);
	Profiling::stop("- getFacilityGain - computeBase");
	
	double newPopulationGrowth = getBasePopulationGrowth(baseId);
	double newIncome = getBaseIncome(baseId);
	int newEcoDamage = std::min(100, base->eco_damage);
	
	// restore base
	
	aiData.resetBase(baseId);
	
	// estimate eco damage effect
	// assume eco damage is the chance of number of workers to lose half or their productivity
	
	double ecoDamageIncome = 0.0;
	
	if (newEcoDamage != oldEcoDamage)
	{
		// fungal pop increase
		
		int ecoDamage = newEcoDamage - oldEcoDamage;
		double fungalPops = static_cast<double>(ecoDamage) / 100.0;
		double fungalPopTiles = fungalPops * static_cast<double>(getFaction(aiFactionId)->clean_minerals_modifier) / 3.0;
		
		// fungal pop gain
		
		ecoDamageIncome = - 0.5 * fungalPopTiles * getBaseIncome(baseId) / static_cast<double>(base->pop_size);
		
	}
	
	// gain
	
	double income = (newIncome - oldIncome) + ecoDamageIncome;
	double incomeGain = getGainIncome(income);
	
	double populationGrowthIncrease = newPopulationGrowth - oldPopulationGrowth;
	double incomeGrowth = aiData.averageCitizenResourceIncome * populationGrowthIncrease;
	double incomeGrowthGain = getGainIncomeGrowth(incomeGrowth);
	
	double gain = incomeGain + incomeGrowthGain;
	
	double maintenanceIncome = (build ? +1 : -1) * getResourceScore(0.0, -Facility[facilityId].maint);
	double maintenanceIncomeGain = getGainIncome(maintenanceIncome);
	gain += maintenanceIncomeGain;
	
	// return combined gain
	
	return gain;
	
}

/**
Computes statistical average faction development score at given point in time.
*/
double getDevelopmentScore(int turn)
{
	double mineralIntake2 = getFactionStatisticalMineralIntake2(turn);
	double budgetIntake2 = getFactionStatisticalBudgetIntake2(turn);
	return getResourceScore(mineralIntake2, budgetIntake2);
}

/**
Computes statistical average faction development score growth.
*/
double getDevelopmentScoreGrowth(int turn)
{
	double mineralIntake2Growth = getFactionStatisticalMineralIntake2Growth(turn);
	double budgetIntake2Growth = getFactionStatisticalBudgetIntake2Growth(turn);
	return getResourceScore(mineralIntake2Growth, budgetIntake2Growth);
}

/**
Computes development scale at current point in time.
Approximate time when faction power growth e times.
*/
double getDevelopmentScale()
{
	return conf.ai_characteristic_time_a + conf.ai_characteristic_time_b * *CurrentTurn;
}

/*
Evaluates gain production priority based on cost.
*/
double getRawProductionPriority(double gain, double cost)
{
	assert(cost > 0.0);
	return gain / cost;
}

/*
Normalizes production priority by normal raw production priority.
*/
double getProductionPriority(double gain, double cost)
{
	return getRawProductionPriority(gain, cost) / landColonyGain;
}

/*
Computes base production constant income gain.
*/
double getProductionConstantIncomeGain(double score, double buildTime, double bonusDelay)
{
	return
		score
		* aiData.developmentScale
		* getBuildTimeEffectCoefficient(buildTime) * getBonusDelayEffectCoefficient(bonusDelay)
	;
}

/*
Computes base production linear income gain.
*/
double getProductionLinearIncomeGain(double score, double buildTime, double bonusDelay)
{
	return
		score
		* aiData.developmentScale * aiData.developmentScale
		* getBuildTimeEffectCoefficient(buildTime) * getBonusDelayEffectCoefficient(bonusDelay)
	;
}

/*
Estimates morale/lifecycle effect.
Returns proportional mineral bonus for average faction base.
*/
double getMoraleProportionalMineralBonus(std::array<int,4> levels)
{
	// summarize costs and supports

	int totalUnitWeightedCount = 0;
	int totalUnitWeightedCost = 0;

	for (int vehicleId : aiData.combatVehicleIds)
	{
		VEH *vehicle = getVehicle(vehicleId);
		int triad = vehicle->triad();
		int unitCost = getVehicleUnitCost(vehicleId);
		bool native = isNativeVehicle(vehicleId);

		// type

		int type = (native ? 3 : triad);

		// accumulate weighted values

		totalUnitWeightedCount += 1 * levels[type];
		totalUnitWeightedCost += unitCost * levels[type];

	}

	// reduction in build cost and support
	// assuming every 20-th combat unit is dying every turn

	double mineralReduction = 0.125 * (totalUnitWeightedCount + 0.05 * totalUnitWeightedCost);

	// proportional bonus on average base production

	return (aiData.totalMineralIntake2 <= 0 ? 0.0 : std::min(1.0, mineralReduction / static_cast<double>(aiData.totalMineralIntake2)));

}

double getDemand(double required, double provided)
{
	double demand;

	if (required <= 0.0)
	{
		demand = 0.0;
	}
	else if (provided >= required)
	{
		demand = 0.0;
	}
	else
	{
		demand = 1.0 - provided / required;
	}

	return demand;

}

//=======================================================
// statistical estimates
//=======================================================

/**
Computes faction statistical mineral intake2.
*/
double getFactionStatisticalMineralIntake2(double turn)
{
	return
		+ conf.ai_faction_mineral_a0
		+ conf.ai_faction_mineral_a1 * turn
		+ conf.ai_faction_mineral_a2 * turn * turn
	;
}

/**
Computes faction statistical mineral intake2 growth.
*/
double getFactionStatisticalMineralIntake2Growth(double turn)
{
	return
		+ conf.ai_faction_mineral_a1
		+ conf.ai_faction_mineral_a2 * 2 * turn
	;
}

/**
Computes faction statistical budget intake2.
*/
double getFactionStatisticalBudgetIntake2(double turn)
{
	return
		+ conf.ai_faction_budget_a0
		+ conf.ai_faction_budget_a1 * turn
		+ conf.ai_faction_budget_a2 * turn * turn
		+ conf.ai_faction_budget_a3 * turn * turn * turn
	;
}

/**
Computes faction statistical budget intake2 growth.
*/
double getFactionStatisticalBudgetIntake2Growth(double turn)
{
	return
		+ conf.ai_faction_budget_a1
		+ conf.ai_faction_budget_a2 * 2 * turn
		+ conf.ai_faction_budget_a3 * 3 * turn * turn
	;
}

/*
Returns base statistical size with age.
*/
double getBaseStatisticalSize(double age)
{
	double value =
		+ conf.ai_base_size_a * log(age - conf.ai_base_size_b)
		+ conf.ai_base_size_c
	;

	return std::max(1.0, value);

}

/*
Returns base statistical mineral intake.
*/
double getBaseStatisticalMineralIntake(double age)
{
	double value =
		+ conf.ai_base_mineral_intake_a * log(age - conf.ai_base_mineral_intake_b)
		+ conf.ai_base_mineral_intake_c
	;

	return std::max(1.0, value);

}

/*
Returns base statistical mineral intake2.
*/
double getBaseStatisticalMineralIntake2(double age)
{
	double value =
		+ conf.ai_base_mineral_intake2_a * age
		+ conf.ai_base_mineral_intake2_b
	;

	return std::max(1.0, value);

}

/*
Returns base statistical mineral multiplier.
*/
double getBaseStatisticalMineralMultiplier(double age)
{
	double value =
		+ conf.ai_base_mineral_multiplier_a * age * age
		+ conf.ai_base_mineral_multiplier_b * age
		+ conf.ai_base_mineral_multiplier_c
	;

	return std::max(1.0, value);

}

/*
Reages base statistical budget intake.
*/
double getBaseStatisticalBudgetIntake(double age)
{
	double value =
		+ conf.ai_base_budget_intake_a * age
		+ conf.ai_base_budget_intake_b
	;

	return std::max(1.0, value);

}

/*
Reages base statistical budget intake2.
*/
double getBaseStatisticalBudgetIntake2(double age)
{
	double value =
		+ conf.ai_base_budget_intake2_a * age * age
		+ conf.ai_base_budget_intake2_b * age
		+ conf.ai_base_budget_intake2_c
	;

	return std::max(1.0, value);

}

/*
Reages base statistical budget multiplier.
*/
double getBaseStatisticalBudgetMultiplier(double age)
{
	double value =
		+ conf.ai_base_budget_multiplier_a * age * age
		+ conf.ai_base_budget_multiplier_b * age
		+ conf.ai_base_budget_multiplier_c
	;

	return std::max(1.0, value);

}

/*
Estimates flat mineral intake gain.
*/
double getFlatMineralIntakeGain(int delay, double value)
{
	assert(delay >= 1);

	int initialTurn = *CurrentTurn + delay;
	double currentFactionStatisticalMineralIntake2 = getFactionStatisticalMineralIntake2(*CurrentTurn);

	// summarize gain

	double gain = 0.0;

	for (int futureTurn = initialTurn; futureTurn <= LAST_TURN; futureTurn++)
	{
		double futureFactionStatisticalMineralIntake2 = getFactionStatisticalMineralIntake2(futureTurn);
		double futureMineralDecayCoefficient = currentFactionStatisticalMineralIntake2 / futureFactionStatisticalMineralIntake2;

		double futureMineralWeight = value * futureMineralDecayCoefficient;

		gain += getResourceScore(futureMineralWeight, 0);

	}

	return gain;

}

/*
Estimates flat budget intake gain.
*/
double getFlatBudgetIntakeGain(int delay, double value)
{
	assert(delay >= 1);

	int initialTurn = *CurrentTurn + delay;
	double currentFactionStatisticalBudgetIntake2 = getFactionStatisticalBudgetIntake2(*CurrentTurn);

	// summarize gain

	double gain = 0.0;

	for (int futureTurn = initialTurn; futureTurn <= LAST_TURN; futureTurn++)
	{
		double futureFactionStatisticalBudgetIntake2 = getFactionStatisticalBudgetIntake2(futureTurn);
		double futureBudgetDecayCoefficient = currentFactionStatisticalBudgetIntake2 / futureFactionStatisticalBudgetIntake2;

		double futureBudgetWeight = value * futureBudgetDecayCoefficient;

		gain += getResourceScore(futureBudgetWeight, 0);

	}

	return gain;

}

/*
Estimates base extra population gain.
Contributing population: (minSize, maxSize].
*/
double getBasePopulationGain(int baseId,  Interval &baseSizeInterval)
{
	trace("getBasePopulationGain\n");

	BASE *base = getBase(baseId);

	int initialTurn = *CurrentTurn + 1;
	int baseAge = getBaseAge(baseId);
	double currentFactionStatisticalMineralIntake2 = getFactionStatisticalMineralIntake2(*CurrentTurn);
	double currentFactionStatisticalBudgetIntake2 = getFactionStatisticalBudgetIntake2(*CurrentTurn);

	// base proportional coefficients

	int baseSize = base->pop_size;
	double baseStatisticalSize = getBaseStatisticalSize(baseAge);
	double baseSizeProportionalCoefficient = static_cast<double>(baseSize) / baseStatisticalSize;

	int baseMineralIntake2 = base->mineral_intake_2;
	double baseStatisticalMineralIntake2 = getBaseStatisticalMineralIntake2(baseAge);
	double baseMineralProportionalCoefficient = static_cast<double>(baseMineralIntake2) / baseStatisticalMineralIntake2;

	int baseBudgetIntake2 = base->economy_total + base->psych_total + base->labs_total;
	double baseStatisticalBudgetIntake2 = getBaseStatisticalBudgetIntake2(baseAge);
	double baseBudgetProportionalCoefficient = static_cast<double>(baseBudgetIntake2) / baseStatisticalBudgetIntake2;

	if constexpr (TRACE)
	{
		debug
		(
			"\tbaseId=%3d"
			" baseSizeInterval.min=%2d"
			" baseSizeInterval.max=%2d"
			" *CurrentTurn=%3d"
			" initialTurn=%3d"
			" baseAge=%3d"
			" currentFactionStatisticalMineralIntake2=%7.2f"
			" currentFactionStatisticalBudgetIntake2=%7.2f"
			" baseSize=%2d"
			" baseStatisticalSize=%5.2f"
			" baseSizeProportionalCoefficient=%5.2f"
			" baseMineralIntake2=%2d"
			" baseStatisticalMineralIntake2=%5.2f"
			" baseMineralProportionalCoefficient=%5.2f"
			" baseBudgetIntake2=%2d"
			" baseStatisticalBudgetIntake2=%5.2f"
			" baseBudgetProportionalCoefficient=%5.2f"
			"\n"
			, baseId
			, baseSizeInterval.min
			, baseSizeInterval.max
			, *CurrentTurn
			, initialTurn
			, baseAge
			, currentFactionStatisticalMineralIntake2
			, currentFactionStatisticalBudgetIntake2
			, baseSize
			, baseStatisticalSize
			, baseSizeProportionalCoefficient
			, baseMineralIntake2
			, baseStatisticalMineralIntake2
			, baseMineralProportionalCoefficient
			, baseBudgetIntake2
			, baseStatisticalBudgetIntake2
			, baseBudgetProportionalCoefficient
		);
	}

	// summarize gain

	double gain = 0.0;

	for (int futureTurn = initialTurn; futureTurn <= LAST_TURN; futureTurn++)
	{
		int futureBaseAge = baseAge + (futureTurn - initialTurn);

		double futureFactionStatisticalMineralIntake2 = getFactionStatisticalMineralIntake2(futureTurn);
		double futureFactionStatisticalBudgetIntake2 = getFactionStatisticalBudgetIntake2(futureTurn);

		double futureMineralDecayCoefficient = currentFactionStatisticalMineralIntake2 / futureFactionStatisticalMineralIntake2;
		double futureBudgetDecayCoefficient = currentFactionStatisticalBudgetIntake2 / futureFactionStatisticalBudgetIntake2;

		double futureBaseSize = getBaseStatisticalSize(futureBaseAge) * baseSizeProportionalCoefficient;

		if (futureBaseSize <= baseSizeInterval.min)
			continue;

		double populationProportion = (std::min(futureBaseSize, static_cast<double>(baseSizeInterval.max)) - static_cast<double>(baseSizeInterval.min)) / futureBaseSize;

		double futureBaseMineralIntake2 = getBaseStatisticalMineralIntake2(futureBaseAge) * baseMineralProportionalCoefficient;
		double futureBaseBudgetIntake2 = getBaseStatisticalBudgetIntake2(futureBaseAge) * baseBudgetProportionalCoefficient;

		double futureBaseMineralIntake2ExtraPopulation = futureBaseMineralIntake2 * populationProportion;
		double futureBaseBudgetIntake2ExtraPopulation = futureBaseBudgetIntake2 * populationProportion;

		double futureBaseMineralWeight = futureBaseMineralIntake2ExtraPopulation * futureMineralDecayCoefficient;
		double futureBaseBudgetWeight = futureBaseBudgetIntake2ExtraPopulation * futureBudgetDecayCoefficient;

		double turnGain = getResourceScore(futureBaseMineralWeight, futureBaseBudgetWeight);
		gain += turnGain;

		if constexpr (TRACE)
		{
			debug
			(
				"\t\tfutureTurn=%3d"
				" futureBaseAge=%3d"
				" futureFactionStatisticalMineralIntake2=%7.2f"
				" futureFactionStatisticalBudgetIntake2=%7.2f"
				" futureMineralDecayCoefficient=%5.2f"
				" futureBudgetDecayCoefficient=%5.2f"
				" futureBaseSize=%5.2f"
				" populationProportion=%5.2f"
				" futureBaseMineralIntake2=%7.2f"
				" futureBaseBudgetIntake2=%7.2f"
				" futureBaseMineralIntake2ExtraPopulation=%7.2f"
				" futureBaseBudgetIntake2ExtraPopulation=%7.2f"
				" futureBaseMineralWeight=%7.2f"
				" futureBaseBudgetWeight=%7.2f"
				" turnGain=%7.2f"
				" gain=%7.2f"
				"\n"
				, futureTurn
				, futureBaseAge
				, futureFactionStatisticalMineralIntake2
				, futureFactionStatisticalBudgetIntake2
				, futureMineralDecayCoefficient
				, futureBudgetDecayCoefficient
				, futureBaseSize
				, populationProportion
				, futureBaseMineralIntake2
				, futureBaseBudgetIntake2
				, futureBaseMineralIntake2ExtraPopulation
				, futureBaseBudgetIntake2ExtraPopulation
				, futureBaseMineralWeight
				, futureBaseBudgetWeight
				, turnGain
				, gain
			);
		}

	}

	if constexpr (TRACE)
	{
		debug
		(
			"\tgain=%7.2f"
			"\n"
			, gain
		);
	}

	return gain;

}

/*
Estimates base extra population gain.
Contributing population: (minSize, maxSize].
*/
double getBasePopulationGrowthGain(int baseId, int delay, double proportionalGrowthIncrease)
{
	assert(baseId >= 0 && baseId < *BaseCount);
	assert(delay >= 1);

	BASE *base = getBase(baseId);
	int baseAge = getBaseAge(baseId);
	double currentFactionStatisticalMineralIntake2 = getFactionStatisticalMineralIntake2(*CurrentTurn);
	double currentFactionStatisticalBudgetIntake2 = getFactionStatisticalBudgetIntake2(*CurrentTurn);
	int initialTurn = *CurrentTurn + delay;

	// base proportional coefficients

	int baseMineralIntake2 = base->mineral_intake_2;
	double baseStatisticalMineralIntake2 = getBaseStatisticalMineralIntake2(baseId);
	double baseMineralProportionalCoefficient = static_cast<double>(baseMineralIntake2) / baseStatisticalMineralIntake2;

	int baseBudgetIntake2 = base->economy_total + base->psych_total + base->labs_total;
	double baseStatisticalBudgetIntake2 = getBaseStatisticalBudgetIntake2(baseId);
	double baseBudgetProportionalCoefficient = static_cast<double>(baseBudgetIntake2) / baseStatisticalBudgetIntake2;

	// summarize gain

	double gain = 0.0;

	for (int futureTurn = initialTurn; futureTurn <= LAST_TURN; futureTurn++)
	{
		double futureBaseAge1 = static_cast<double>(baseAge + delay) + static_cast<double>(futureTurn - initialTurn);
		double futureBaseAge2 = static_cast<double>(baseAge + delay) + static_cast<double>(futureTurn - initialTurn) * proportionalGrowthIncrease;

		double futureFactionStatisticalMineralIntake2 = getFactionStatisticalMineralIntake2(futureTurn);
		double futureFactionStatisticalBudgetIntake2 = getFactionStatisticalBudgetIntake2(futureTurn);

		double futureMineralDecayCoefficient = currentFactionStatisticalMineralIntake2 / futureFactionStatisticalMineralIntake2;
		double futureBudgetDecayCoefficient = currentFactionStatisticalBudgetIntake2 / futureFactionStatisticalBudgetIntake2;

		double futureBaseMineralIntake2Difference =
			+ getBaseStatisticalMineralIntake2(futureBaseAge2) * baseMineralProportionalCoefficient
			- getBaseStatisticalMineralIntake2(futureBaseAge1) * baseMineralProportionalCoefficient
		;
		double futureBaseBudgetIntake2Difference =
			+ getBaseStatisticalBudgetIntake2(futureBaseAge2) * baseBudgetProportionalCoefficient
			- getBaseStatisticalBudgetIntake2(futureBaseAge1) * baseBudgetProportionalCoefficient
		;

		double futureBaseMineralIntake2ExtraPopulation = futureBaseMineralIntake2Difference;
		double futureBaseBudgetIntake2ExtraPopulation = futureBaseBudgetIntake2Difference;

		double futureBaseMineralWeight = futureBaseMineralIntake2ExtraPopulation * futureMineralDecayCoefficient;
		double futureBaseBudgetWeight = futureBaseBudgetIntake2ExtraPopulation * futureBudgetDecayCoefficient;

		gain += getResourceScore(futureBaseMineralWeight, futureBaseBudgetWeight);

	}

	return gain;

}

/*
Estimates proportional mineral intake gain.
*/
double getBaseProportionalMineralIntakeGain(int baseId, int delay, double proportion)
{
	assert(baseId >= 0 && baseId < *BaseCount);
	assert(delay >= 1);

	BASE *base = getBase(baseId);

	int initialTurn = *CurrentTurn + delay;
	int baseAge = getBaseAge(baseId);
	double currentFactionStatisticalMineralIntake2 = getFactionStatisticalMineralIntake2(*CurrentTurn);

	// base proportional coefficients

	int baseMineralIntake = base->mineral_intake;
	double baseStatisticalMineralIntake = getBaseStatisticalMineralIntake(baseId);
	double baseMineralProportionalCoefficient = static_cast<double>(baseMineralIntake) / baseStatisticalMineralIntake;

	// summarize gain

	double gain = 0.0;

	for (int futureTurn = initialTurn; futureTurn <= LAST_TURN; futureTurn++)
	{
		int futureBaseAge = baseAge + (futureTurn - initialTurn);

		double futureFactionStatisticalMineralIntake2 = getFactionStatisticalMineralIntake2(futureTurn);
		double futureMineralDecayCoefficient = currentFactionStatisticalMineralIntake2 / futureFactionStatisticalMineralIntake2;

		double futureBaseMineralIntake = getBaseStatisticalMineralIntake(futureBaseAge) * baseMineralProportionalCoefficient;
		double futureBaseMineralIntakeProportion = futureBaseMineralIntake * proportion;
		double futureBaseMineralWeight = futureBaseMineralIntakeProportion * futureMineralDecayCoefficient;

		gain += getResourceScore(futureBaseMineralWeight, 0);

	}

	return gain;

}

/*
Estimates proportional budget intake gain.
*/
double getBaseProportionalBudgetIntakeGain(int baseId, int delay, double proportion)
{
	assert(baseId >= 0 && baseId < *BaseCount);
	assert(delay >= 1);

	int initialTurn = *CurrentTurn + delay;
	int baseAge = getBaseAge(baseId);
	double currentFactionStatisticalBudgetIntake2 = getFactionStatisticalBudgetIntake2(*CurrentTurn);

	// base proportional coefficients

	Budget baseBudgetIntakeByType = getBaseBudgetIntake(baseId);
	int baseBudgetIntake = baseBudgetIntakeByType.economy + baseBudgetIntakeByType.psych + baseBudgetIntakeByType.labs;
	double baseStatisticalBudgetIntake = getBaseStatisticalBudgetIntake(baseId);
	double baseBudgetProportionalCoefficient = static_cast<double>(baseBudgetIntake) / baseStatisticalBudgetIntake;

	// summarize gain

	double gain = 0.0;

	for (int futureTurn = initialTurn; futureTurn <= LAST_TURN; futureTurn++)
	{
		int futureBaseAge = baseAge + (futureTurn - initialTurn);

		double futureFactionStatisticalBudgetIntake2 = getFactionStatisticalBudgetIntake2(futureTurn);
		double futureBudgetDecayCoefficient = currentFactionStatisticalBudgetIntake2 / futureFactionStatisticalBudgetIntake2;

		double futureBaseBudgetIntake = getBaseStatisticalBudgetIntake(futureBaseAge) * baseBudgetProportionalCoefficient;
		double futureBaseBudgetIntakeProportion = futureBaseBudgetIntake * proportion;
		double futureBaseBudgetWeight = futureBaseBudgetIntakeProportion * futureBudgetDecayCoefficient;

		gain += getResourceScore(0, futureBaseBudgetWeight);

	}

	return gain;

}

/*
Estimates proportional mineral intake gain.
*/
double getBaseProportionalMineralIntake2Gain(int baseId, int delay, double proportion)
{
	assert(baseId >= 0 && baseId < *BaseCount);
	assert(delay >= 1);

	BASE *base = getBase(baseId);

	int initialTurn = *CurrentTurn + delay;
	int baseAge = getBaseAge(baseId);
	double currentFactionStatisticalMineralIntake2 = getFactionStatisticalMineralIntake2(*CurrentTurn);

	// base proportional coefficients

	int baseMineralIntake2 = base->mineral_intake_2;
	double baseStatisticalMineralIntake2 = getBaseStatisticalMineralIntake2(baseId);
	double baseMineralProportionalCoefficient = static_cast<double>(baseMineralIntake2) / baseStatisticalMineralIntake2;

	// summarize gain

	double gain = 0.0;

	for (int futureTurn = initialTurn; futureTurn <= LAST_TURN; futureTurn++)
	{
		int futureBaseAge = baseAge + (futureTurn - initialTurn);

		double futureFactionStatisticalMineralIntake2 = getFactionStatisticalMineralIntake2(futureTurn);
		double futureMineralDecayCoefficient = currentFactionStatisticalMineralIntake2 / futureFactionStatisticalMineralIntake2;

		double futureBaseMineralIntake2 = getBaseStatisticalMineralIntake2(futureBaseAge) * baseMineralProportionalCoefficient;
		double futureBaseMineralIntake2Proportion = futureBaseMineralIntake2 * proportion;
		double futureBaseMineralWeight = futureBaseMineralIntake2Proportion * futureMineralDecayCoefficient;

		gain += getResourceScore(futureBaseMineralWeight, 0);

	}

	return gain;

}

/*
Estimates proportional budget intake gain.
*/
double getBaseProportionalBudgetIntake2Gain(int baseId, int delay, double proportion)
{
	assert(baseId >= 0 && baseId < *BaseCount);
	assert(delay >= 1);

	BASE *base = getBase(baseId);

	int initialTurn = *CurrentTurn + delay;
	int baseAge = getBaseAge(baseId);
	double currentFactionStatisticalBudgetIntake2 = getFactionStatisticalBudgetIntake2(*CurrentTurn);

	// base proportional coefficients

	int baseBudgetIntake2 = base->economy_total + base->psych_total + base->labs_total;
	double baseStatisticalBudgetIntake2 = getBaseStatisticalBudgetIntake2(baseId);
	double baseBudgetProportionalCoefficient = static_cast<double>(baseBudgetIntake2) / baseStatisticalBudgetIntake2;

	// summarize gain

	double gain = 0.0;

	for (int futureTurn = initialTurn; futureTurn <= LAST_TURN; futureTurn++)
	{
		int futureBaseAge = baseAge + (futureTurn - initialTurn);

		double futureFactionStatisticalBudgetIntake2 = getFactionStatisticalBudgetIntake2(futureTurn);
		double futureBudgetDecayCoefficient = currentFactionStatisticalBudgetIntake2 / futureFactionStatisticalBudgetIntake2;

		double futureBaseBudgetIntake2 = getBaseStatisticalBudgetIntake2(futureBaseAge) * baseBudgetProportionalCoefficient;
		double futureBaseBudgetIntake2Proportion = futureBaseBudgetIntake2 * proportion;
		double futureBaseBudgetWeight = futureBaseBudgetIntake2Proportion * futureBudgetDecayCoefficient;

		gain += getResourceScore(0, futureBaseBudgetWeight);

	}

	return gain;

}

//=======================================================
// production item helper functions
//=======================================================

/*
Calculates number of extra police units can be added to the base.
*/
int getBasePoliceExtraCapacity(int baseId)
{
	TileInfo &baseTileInfo = aiData.getBaseTileInfo(baseId);
	
	// get base police allowed
	
	int policeAllowed = getBaseAllowedPolice(baseId);
	
	// no police allowed
	
	if (policeAllowed == 0)
		return 0;
		
	// count number of police units in base
	
	int policePresent = 0;
	
	for (int vehicleId : baseTileInfo.playerVehicleIds)
	{
		// count only police2x if available
		
		if (aiData.police2xUnitAvailable && !isInfantryDefensivePolice2xVehicle(vehicleId))
			continue;
			
		// count police
		
		policePresent++;
		
	}
	
	return std::max(0, policeAllowed - policePresent);
	
}

double getItemPriority(int item, double gain)
{
	double cost = item >= 0 ? Units[item].cost : Facility[-item].cost;
	return gain / cost;
}

double getBaseColonyUnitGain(int baseId, int unitId, double travelTime, double buildSiteScore)
{
	// depopulation
	
	double depopulationIncome = -getBaseCitizenIncome(baseId);
	double depopulationGain = getGainIncome(depopulationIncome);
	
	// support
	
	double supportIncome = getResourceScore(-getUnitSupport(unitId), 0);
	double supportGain = getGainTimeInterval(supportIncome, 0, travelTime);
	
	// new base
	
	double delayedMeanNewBaseGain = getGainDelay(meanNewBaseGain, travelTime);
	double newBaseGain = delayedMeanNewBaseGain * buildSiteScore;
	
	// summarize
	
	return depopulationGain + supportGain + newBaseGain;
	
}

/**
Computes estimated police provided gain.
*/
double getBasePoliceGain(int baseId, bool police2x)
{
	// police allowed
	
	int policeAllowed = getBaseAllowedPolice(baseId);
	
	if (policeAllowed <= 0)
		return 0.0;
	
	// police power
	
	int policePower = getBasePolicePower(baseId, police2x);
	
	// basic base gain
	
	Resource oldBaseIntake2 = getBaseResourceIntake2(baseId);
	double oldBaseGain = getBaseGain(baseId, oldBaseIntake2);
	
	// extra worker base gain
	
	Resource newBaseIntake2 =
		Resource::combine
		(
			oldBaseIntake2,
			{
				aiData.averageWorkerNutrientIntake2,
				aiData.averageWorkerMineralIntake2,
				aiData.averageWorkerEnergyIntake2,
 			}
		)
	;
	double newBaseGain = getBaseGain(baseId, newBaseIntake2);
	
	// extra worker gain
	
	double extraWorkerGain = std::max(0.0, newBaseGain - oldBaseGain);
	
	// police gain
	
	double policeGain = static_cast<double>(policePower) * extraWorkerGain;
	return policeGain;
	
}

double getTechStealGain()
{
	int maxLevelTechId = -1;
	int maxTechLevel = 0;
	for (int techId = 0; techId < TECH_TranT; techId++)
	{
		if (!has_tech(techId, aiFactionId))
			continue;
		
		int techLevel = wtp_tech_level(techId);
		
		if (techLevel > maxTechLevel)
		{
			maxLevelTechId = techId;
			maxTechLevel = std::max(maxTechLevel, techLevel);
		}
		
	}
	
	if (maxLevelTechId == -1)
		return 0.0;
	
	double techCost = wtp_tech_cost(aiFactionId, maxLevelTechId);
	double gain = getGainBonus(techCost);
	
	// not our gain but other faction gain
	// divided by remaining faction count
	
	return gain / 6.0;
	
}

