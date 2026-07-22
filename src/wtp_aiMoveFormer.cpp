#pragma GCC diagnostic ignored "-Wshadow"

#include <cfloat>
#include <cmath>
#include <vector>

#include "main.h"
#include "wtp_aiMoveFormer.h"
#include "wtp_aiRoute.h"
#include "wtp_aiMove.h"
#include "wtp_base.h"

// variables

FactionTerraformingInfo factionTerraformingInfo;
std::vector<MAP *> raiseableCoasts;
robin_hood::unordered_flat_set<MAP *> significantTerraformingRequestLocations;

// TerraformingOptionScore

TerraformingOptionScore::TerraformingOptionScore(MAP *_tile, TERRAFORMING_OPTION const *_option, robin_hood::unordered_flat_set<FormerItem> const &_actions, double _incomeGain)
	: tile(_tile), option(_option), actions(_actions), incomeGain(_incomeGain)
{
	this->terraformingTime = 0;
	for (FormerItem action : actions)
	{
		this->terraformingTime += Terraform[action].rate;
	}

}

// FORMER_ORDER

FormerOrder::FormerOrder(int _vehicleId)
: vehicleId(_vehicleId)
{}

// TileTerraformingInfo

// converts a former action into map state. Pure: mutates whatever MAP is pointed to (live tile or local value)
// and never reads or leaves anything in `this` - caller owns save/restore of the live array, if any.
void TileTerraformingInfo::applyTerraforming(MAP *state, FormerItem action)
{
	switch (action)
	{
	case FORMER_LEVEL_TERRAIN:
		{
			if (state->is_land())
			{
				if (state->is_rocky())
				{
					// rocky turns to rolling
					state->val3 &= ~TILE_ROCKY;
					state->val3 |= TILE_ROLLING;
				}
				else if (state->is_rolling())
				{
					// rolling turns to flat
					state->val3 &= ~TILE_ROLLING;
				}
			}
		}
		break;

	case FORMER_AQUIFER:
		{
			state->items |= BIT_RIVER_SRC | BIT_RIVER;
		}
		break;

	default:
		{
			state->items &= ~Terraform[action].bit_incompatible;
			state->items |= Terraform[action].bit;
		}
		break;

	}

}
void TileTerraformingInfo::applyTerraforming(MAP *state, robin_hood::unordered_flat_set<FormerItem> const &actions)
{
	for (FormerItem action : actions)
	{
		applyTerraforming(state, action);
	}
}

// BaseTerraformingInfo

double BaseTerraformingInfo::getIntakeGain(ResourceYield const &yield, int economy, int labs) const
{
	// resource gain

	double income = getResourceScore(this->mineralValue * yield.mineral, this->energyValue * yield.energy + this->economyValue * economy + this-> labsValue * labs);
	double incomeGain = getGainIncome(income);

	// nutrient gain

	double populationGrowthRate = yield.nutrient / static_cast<double>(this->nutrientCost * (this->popSzie + 1)) / static_cast<double>(this->popSzie);
	double incomeGrowth = populationGrowthRate * this->income;
	double incomeGrowthGain = getGainIncomeGrowth(incomeGrowth);

	// combine

	return incomeGain + incomeGrowthGain;

}

// terraforming data

std::vector<TileTerraformingInfo> tileTerraformingInfos;
std::vector<BaseTerraformingInfo> baseTerraformingInfos;

std::vector<MAP *> terraformingSites;

double networkDemand = 0.0;
std::vector<FormerOrder> formerOrders;
robin_hood::unordered_flat_map<int, FormerOrder *> vehicleFormerOrders;
std::vector<TerraformingRequest> terraformingRequests;

// terraforming data operations

// access terraforming data arrays

BaseTerraformingInfo &getBaseTerraformingInfo(int baseId)
{
	assert(baseId >= 0 && baseId < *BaseCount);
	return baseTerraformingInfos.at(baseId);
}

TileTerraformingInfo &getTileTerraformingInfo(MAP* tile)
{
	assert(isOnMap(tile));
	return tileTerraformingInfos.at(tile - *MapTiles);
}

// global variables

double averageVehicleCostPerTileInOpen;
double averageVehicleCostPerTileInBase;

/*
Prepares former orders.
*/
void moveFormerStrategy()
{
	Profiling::start("moveFormerStrategy", "moveStrategy");

	// remove unussed bunkers

	removeUnusedBunkers();

	// reset former orders if improvement is already there

	cancelRedundantOrders();

	// initialize data

	setupTerraformingData();

	// populate data

	populateTerraformingData();

	// formers

	generateTerraformingRequests();
	applyProximityRules();
	removeTerraformedTiles();
	assignFormerOrders();
	setFormerTasks();

	Profiling::stop("moveFormerStrategy");

}

void setupTerraformingData()
{
	Profiling::start("setupTerraformingData", "moveFormerStrategy");

	FactionInfo &aiFactionInfo = aiData.factionInfos.at(aiFactionId);

	// cleanup and setup data

	tileTerraformingInfos.clear();
	tileTerraformingInfos.resize(*MapAreaTiles, {});

	baseTerraformingInfos.clear();
	baseTerraformingInfos.resize(MaxBaseNum, {});

	terraformingSites.clear();

	terraformingRequests.clear();
	formerOrders.clear();
	vehicleFormerOrders.clear();

	// cluster former counts

	aiFactionInfo.airFormerCount = 0;
	aiFactionInfo.seaClusterFormerCounts.clear();
	aiFactionInfo.landTransportedClusterFormerCounts.clear();

	for (int vehicleId : aiData.formerVehicleIds)
	{
		VEH *vehicle = getVehicle(vehicleId);
		MAP *vehicleTile = getVehicleMapTile(vehicleId);
		int triad = vehicle->triad();

		switch (triad)
		{
		case TRIAD_AIR:
			aiFactionInfo.airFormerCount++;
			break;

		case TRIAD_SEA:
			{
				int seaCluster = getSeaCluster(vehicleTile);
				aiFactionInfo.seaClusterFormerCounts[seaCluster]++;
			}
			break;

		case TRIAD_LAND:
			{
				int landTransportedCluster = getLandTransportedCluster(vehicleTile);
				aiFactionInfo.landTransportedClusterFormerCounts[landTransportedCluster]++;
			}
			break;

		default: ;

		}

	}

	// average former terraforming rate

	double factionNormalTerraformingRateMultipier = isFactionHasProject(aiFactionId, FAC_WEATHER_PARADIGM) ? 1.5 : 1.0;
	double factionFungusTerraformingRateMultipier = isFactionHasProject(aiFactionId, FAC_XENOEMPATHY_DOME) ? 2.0 : 1.0;

	double formerCount = 0.0;
	double sumNormalTerraformingRateMultiplier = 0.0;
	double sumPlantFungusTerraformingRateMultiplier = 0.0;
	double sumRemoveFungusTerraformingRateMultiplier = 0.0;

	for (int vehicleId : aiData.formerVehicleIds)
	{
		// count

		formerCount += 1.0;

		// terraforming rate

		double normalTerraformingRateMultiplier = factionNormalTerraformingRateMultipier * (isVehicleHasAbility(vehicleId, ABL_SUPER_TERRAFORMER) ? 2.0 : 1.0);
		double plantFungusTerraformingRateMultiplier = factionFungusTerraformingRateMultipier * (isVehicleHasAbility(vehicleId, ABL_SUPER_TERRAFORMER) ? 2.0 : 1.0);
		double removeFungusTerraformingRateMultiplier = factionFungusTerraformingRateMultipier * (isVehicleHasAbility(vehicleId, ABL_SUPER_TERRAFORMER) ? 2.0 : 1.0) * (isVehicleHasAbility(vehicleId, ABL_FUNGICIDAL) ? 2.0 : 1.0);

		sumNormalTerraformingRateMultiplier += normalTerraformingRateMultiplier;
		sumPlantFungusTerraformingRateMultiplier += plantFungusTerraformingRateMultiplier;
		sumRemoveFungusTerraformingRateMultiplier += removeFungusTerraformingRateMultiplier;

	}

	factionTerraformingInfo.averageNormalTerraformingRateMultiplier = formerCount == 0 ? 1.0 : sumNormalTerraformingRateMultiplier / formerCount;
	factionTerraformingInfo.averagePlantFungusTerraformingRateMultiplier = formerCount == 0 ? 1.0 : sumPlantFungusTerraformingRateMultiplier / formerCount;
	factionTerraformingInfo.averageRemoveFungusTerraformingRateMultiplier = formerCount == 0 ? 1.0 : sumRemoveFungusTerraformingRateMultiplier / formerCount;

	// bareLandScore

	int forestNutrient = ResInfo->forest_sq.nutrient + (has_tech(Facility[FAC_TREE_FARM].preq_tech, aiFactionId) ? 1 : 0) + (has_tech(Facility[FAC_HYBRID_FOREST].preq_tech, aiFactionId) ? 1 : 0);
	int forestMineral = ResInfo->forest_sq.mineral;
	int forestEnergy = ResInfo->forest_sq.energy + (has_tech(Facility[FAC_HYBRID_FOREST].preq_tech, aiFactionId) ? 1 : 0);
	double forestScore = getTerraformingResourceScore(forestNutrient, forestMineral, forestEnergy);

	double fungusScore = 0.0;
	for (MAP *tile = *MapTiles; tile < *MapTiles + *MapAreaTiles; tile++)
	{
		// fungus

		if (!map_has_item(tile, BIT_FUNGUS))
			continue;

		int x = getX(tile);
		int y = getY(tile);

		int fungusNutrient = mod_crop_yield(aiFactionId, -1, x, y, 0);
		int fungusMineral = mod_mine_yield(aiFactionId, -1, x, y, 0);
		int fungusEnergy = mod_energy_yield(aiFactionId, -1, x, y, 0);
		fungusScore = getTerraformingResourceScore(fungusNutrient, fungusMineral, fungusEnergy);

		break;

	}

	factionTerraformingInfo.bareLandScore = std::max(forestScore, fungusScore);

	// bareMineScore

	int mineNutrient = ResInfo->improved_land.nutrient + (has_tech(Terraform[FORMER_SOIL_ENR].preq_tech, aiFactionId) ? 1 : 0) + Rules->nutrient_effect_mine_sq;
	int mineMineral = 1;
	int mineEnergy = 0;

	factionTerraformingInfo.bareMineScore = getTerraformingResourceScore(mineNutrient, mineMineral, mineEnergy);

	// bareSolarScore

	int solarNutrient = ResInfo->improved_land.nutrient + (has_tech(Terraform[FORMER_SOIL_ENR].preq_tech, aiFactionId) ? 1 : 0);
	int solarMineral = 0;
	int solarEnergy = 1 + (has_tech(Terraform[FORMER_ECH_MIRROR].preq_tech, aiFactionId) ? 2 : 0);

	factionTerraformingInfo.bareSolarScore = getTerraformingResourceScore(solarNutrient, solarMineral, solarEnergy);

	Profiling::stop("setupTerraformingData");

}

/**
Populates global lists before processing faction strategy.
*/
void populateTerraformingData()
{
	debug("populateTerraformingData - %s\n", getMFaction(aiFactionId)->noun_faction);

	Profiling::start("populateTerraformingData", "moveFormerStrategy");

	// set tile and tileInfo

	for (int mapIndex = 0; mapIndex < *MapAreaTiles; mapIndex++)
	{
		MAP *tile = *MapTiles + mapIndex;
		TileTerraformingInfo &tileTerraformingInfo = tileTerraformingInfos.at(mapIndex);

		tileTerraformingInfo.tile = tile;
		tileTerraformingInfo.landRocky = tile->is_land() && tile->is_rocky();

	}

	// populate tileTerraformingInfos
	// populate terraformingSites
	// populate conventionalTerraformingSites

	for (int baseId : aiData.baseIds)
	{
		BASE *base = &(Bases[baseId]);
		BaseTerraformingInfo &baseTerraformingInfo = getBaseTerraformingInfo(baseId);

		// initialize base terraforming info

		baseTerraformingInfo.landRockyTileCount = 0;

		// base radius tiles

		for (MAP *tile : getBaseRadiusTiles(base->x, base->y, false))
		{
			TileTerraformingInfo &tileTerraformingInfo = getTileTerraformingInfo(tile);

			// exclude not valid terraforming site

			if (!isValidTerraformingSite(tile))
				continue;

			tileTerraformingInfo.availableTerraformingSite = true;

			// worked

			if (isBaseWorkedTile(baseId, tile))
			{
				tileTerraformingInfo.worked = true;
				tileTerraformingInfo.workedBaseId = baseId;
			}

			// workable

			if (isWorkableTile(tile))
			{
				tileTerraformingInfo.workable = true;
				tileTerraformingInfo.workableBaseIds.push_back(baseId);
			}

			// conventional terraforming sites

			if (isValidConventionalTerraformingSite(tile))
			{
				tileTerraformingInfo.availableBaseTerraformingSite = true;
			}

			// update base rocky land tile count

			if (!is_ocean(tile))
			{
				if (map_rockiness(tile) == 2)
				{
					baseTerraformingInfo.landRockyTileCount++;
				}

			}

		}

		// base radius adjacent tiles

		for (int index = OFFSET_COUNT_CENTER; index < OFFSET_COUNT_RADIUS_CORNER; index++)
		{
			int x = wrap(base->x + BASE_TILE_OFFSETS[index][0]);
			int y = base->y + BASE_TILE_OFFSETS[index][1];

			if (!isOnMap(x, y))
				continue;

			MAP *tile = getMapTile(x, y);
			TileTerraformingInfo &tileTerraformingInfo = getTileTerraformingInfo(getMapTile(x, y));

			// valid terraforming site

			if (!isValidTerraformingSite(tile))
				continue;

			tileTerraformingInfo.availableTerraformingSite = true;

			// update area workable bases

			tileTerraformingInfo.areaWorkableBaseIds.push_back(baseId);

		}

	}

	// remove unavailable terraforming clusters

	if (!has_tech(Chassis[CHS_GRAVSHIP].preq_tech, aiFactionId))
	{
		// collect available terraforming clusters

		robin_hood::unordered_flat_set<int> availableTerraformingSeaClusters;
		robin_hood::unordered_flat_set<int> availableTerraformingLandTransportedClusters;

		// for formers

		for (int vehicleId : aiData.formerVehicleIds)
		{
			VEH *vehicle = getVehicle(vehicleId);
			MAP *vehicleTile = getVehicleMapTile(vehicleId);

			switch (vehicle->triad())
			{
			case TRIAD_SEA:
				{
					int seaCluster = getSeaCluster(vehicleTile);

					if (seaCluster != -1)
					{
						availableTerraformingSeaClusters.insert(seaCluster);
					}

				}
				break;

			case TRIAD_LAND:
				{
					int landTransportedCluster = getLandTransportedCluster(vehicleTile);

					if (landTransportedCluster != -1)
					{
						availableTerraformingLandTransportedClusters.insert(landTransportedCluster);
					}

				}
				break;

			default: ;

			}

		}

		// for bases

		if (has_tech(Units[BSC_SEA_FORMERS].preq_tech, aiFactionId))
		{
			for (int baseId : aiData.baseIds)
			{
				MAP *baseTile = getBaseMapTile(baseId);

				int seaCluster = getSeaCluster(baseTile);

				if (seaCluster != -1)
				{
					availableTerraformingSeaClusters.insert(seaCluster);
				}

			}

		}

		if (has_tech(Units[BSC_FORMERS].preq_tech, aiFactionId))
		{
			for (int baseId : aiData.baseIds)
			{
				MAP *baseTile = getBaseMapTile(baseId);

				int landTransportedCluster = getLandTransportedCluster(baseTile);

				if (landTransportedCluster != -1)
				{
					availableTerraformingLandTransportedClusters.insert(landTransportedCluster);
				}

			}

		}

		for (MAP *tile = *MapTiles; tile < *MapTiles + *MapAreaTiles; tile++)
		{
			TileTerraformingInfo &tileTerraformingInfo = getTileTerraformingInfo(tile);

			if (is_ocean(tile))
			{
				int seaCluster = getSeaCluster(tile);

				if (seaCluster == -1 || availableTerraformingSeaClusters.find(seaCluster) == availableTerraformingSeaClusters.end())
				{
					tileTerraformingInfo.availableTerraformingSite = false;
					continue;
				}

			}
			else
			{
				int landTransportedCluster = getLandTransportedCluster(tile);

				if (landTransportedCluster == -1 || availableTerraformingLandTransportedClusters.find(landTransportedCluster) == availableTerraformingLandTransportedClusters.end())
				{
					tileTerraformingInfo.availableTerraformingSite = false;
					continue;
				}

			}

		}

	}

//	if (DEBUG)
//	{
//		debug("\tavailableTerraformingSites\n");
//		for (MAP *tile = *MapTiles; tile < *MapTiles + *MapAreaTiles; tile++)
//		{
//			int x = getX(tile);
//			int y = getY(tile);
//			TileTerraformingInfo &tileTerraformingInfo = getTileTerraformingInfo(getMapTile(x, y));
//
//			if (!tileTerraformingInfo.availableTerraformingSite)
//				continue;
//
//			debug("\t\t%s\n", getLocationString(tile));
//
//		}
//
//	}

	// populated terraformed tiles and former orders

	for (int vehicleId : aiData.vehicleIds)
	{
		VEH *vehicle = &Vehs[vehicleId];
		int triad = vehicle->triad();
		int defenseValue = getVehicleDefenseValue(vehicleId);
		MAP *vehicleTile = getVehicleMapTile(vehicleId);
		TileTerraformingInfo &tileTerraformingInfo = tileTerraformingInfos.at(getVehicleMapTileIndex(vehicleId));

		// process supplies and formers

		// supplies
		if (isSupplyVehicle(vehicleId))
		{
			// convoying vehicles
			if (isVehicleConvoying(vehicleId))
			{
				tileTerraformingInfo.harvested = true;
			}

		}
		// formers
		else if (isFormerVehicle(vehicle))
		{
			// terraforming vehicles

			if (isVehicleTerraforming(vehicle))
			{
				// set sync task

				setTask(Task(vehicleId, TT_NONE));

				// terraformed flag

				tileTerraformingInfo.terraformed = true;

				// conventional terraformed flag

				if
				(
					vehicle->order == ORDER_FARM
					||
					vehicle->order == ORDER_SOIL_ENRICHER
					||
					vehicle->order == ORDER_MINE
					||
					vehicle->order == ORDER_SOLAR_COLLECTOR
					||
					vehicle->order == ORDER_CONDENSER
					||
					vehicle->order == ORDER_ECHELON_MIRROR
					||
					vehicle->order == ORDER_THERMAL_BOREHOLE
					||
					vehicle->order == ORDER_PLANT_FOREST
					||
					vehicle->order == ORDER_PLANT_FUNGUS
				)
				{
					tileTerraformingInfo.terraformedConventional = true;
				}

			}
			// available vehicles
			else
			{
				// ignore those in danger zone except land vehicle in ocean

				if (!(triad == TRIAD_LAND && is_ocean(vehicleTile)))
				{
					double vehicleTileDanger = aiData.getTileInfo(getMapTile(vehicle->x, vehicle->y)).hostileDangerZone;

					if (defenseValue > 0 && vehicleTileDanger > defenseValue)
						continue;

				}

				// exclude air terraformers - let AI deal with them itself

				if (veh_triad(vehicleId) == TRIAD_AIR)
					continue;

				// add vehicle

				formerOrders.emplace_back(vehicleId);

			}

		}

	}

	// calculate network demand

	int networkType = (has_tech(Terraform[FORMER_MAGTUBE].preq_tech, aiFactionId) ? BIT_MAGTUBE : BIT_ROAD);

	int eligibleTileCount = 0;
	int coveredTileCount = 0;

	for (int tileIndex = 0; tileIndex < *MapAreaTiles; tileIndex++)
	{
		MAP *tile = *MapTiles + tileIndex;
		TileInfo &tileInfo = aiData.getTileInfo(tileIndex);

		// land

		if (tileInfo.ocean)
			continue;

		// player territory

		if (tile->owner != aiFactionId)
			continue;

		// base radius

		if (!map_has_item(tile, BIT_BASE_RADIUS))
			continue;

		// count network coverage

		eligibleTileCount++;


		if (map_has_item(tile, networkType))
		{
			coveredTileCount++;
		}

	}

	double networkDensity = static_cast<double>(coveredTileCount) / static_cast<double>(eligibleTileCount);

	networkDemand = std::max(0.0, 1.0 - networkDensity / conf.ai_terraforming_networkDensityThreshold);

	// populate terraforming site lists

	for (MAP *tile = *MapTiles; tile < *MapTiles + *MapAreaTiles; tile++)
	{
		TileTerraformingInfo &tileTerraformingInfo = getTileTerraformingInfo(tile);

		if (tileTerraformingInfo.availableTerraformingSite)
		{
			terraformingSites.push_back(tile);
		}

	}

//	if (DEBUG)
//	{
//		debug("\tterraformingSites\n");
//		for (MAP *tile : terraformingSites)
//		{
//			debug("\t\t%s ocean=%d\n", getLocationString(tile), is_ocean(tile));
//		}
//
//	}

	// populate base conventionalTerraformingSites

	for (int baseId : aiData.baseIds)
	{
		MAP *baseTile = getBaseMapTile(baseId);
		BaseTerraformingInfo &baseTerraformingInfo = getBaseTerraformingInfo(baseId);

		for (MAP *baseRadiusTile : getBaseRadiusTiles(baseTile, false))
		{
			TileTerraformingInfo &baseRadiusTileTerraformingInfo = getTileTerraformingInfo(baseRadiusTile);

			// conventional terraforming site

			if (!baseRadiusTileTerraformingInfo.availableBaseTerraformingSite)
				continue;

			// not worked by other base

			if (!(baseRadiusTileTerraformingInfo.workedBaseId == -1 || baseRadiusTileTerraformingInfo.workedBaseId == baseId))
				continue;

			// add to base tiles

			baseTerraformingInfo.terraformingSites.push_back(baseRadiusTile);

		}

	}

	// base gain values

	debug("\tworker gains\n");
	for (int baseId : aiData.baseIds)
	{
		debug("\t\t%s\n", Bases[baseId].name);
		BASE &base = Bases[baseId];
		BaseTerraformingInfo &baseTerraformingInfo = getBaseTerraformingInfo(baseId);
		std::vector<MAP *> baseWorkedTiles = getBaseWorkedTiles(baseId);

		baseTerraformingInfo.popSzie = static_cast<unsigned char>(base.pop_size);
		baseTerraformingInfo.nutrientCost = mod_cost_factor(base.faction_id, RSC_NUTRIENT, baseId);
		baseTerraformingInfo.income = getBaseIncome(baseId, false);
		baseTerraformingInfo.mineralValue = getBaseMineralMultiplier(baseId);
		baseTerraformingInfo.energyValue = getBaseEnergyMultiplier(baseId) * (1.0 - static_cast<double>(wtp_mod_energy_intake_lost(baseId, 100, nullptr)) / 100.0);
		baseTerraformingInfo.economyValue = getBaseEconomyMultiplier(baseId);
		baseTerraformingInfo.labsValue = getBaseLabsMultiplier(baseId);

		debug("\t\t\tfarmer gains\n");
		baseTerraformingInfo.workerGains.reserve(base.pop_size);
		for (MAP *workedTile : baseWorkedTiles)
		{
			ResourceYield workedTileResourceYield = getTileResourceYield(workedTile, baseId);
			double farmerGain = baseTerraformingInfo.getIntakeGain(workedTileResourceYield, 0, 0);
			baseTerraformingInfo.workerGains.push_back({farmerGain, workedTile});
			debug("\t\t\t\t%s %5.2f {%d-%d-%d}\n", getLocationString(workedTile), farmerGain, workedTileResourceYield.nutrient, workedTileResourceYield.mineral, workedTileResourceYield.energy);
		}

		// do not account for extra specialists
		debug("\t\t\tspecialist gains\n");
		for (int specialistIndex = 0; specialistIndex < std::min(MaxBaseSpecNum, base.specialist_total); ++specialistIndex)
		{
			int specialistType = base.specialist_type(specialistIndex);
			CCitizen citizen = Citizen[specialistType];

			// do not remove psych specialist

			if (citizen.psych_bonus > 0)
				continue;

			double specialistGain = baseTerraformingInfo.getIntakeGain({0, 0, 0}, citizen.econ_bonus, citizen.labs_bonus);
			baseTerraformingInfo.workerGains.push_back({specialistGain, nullptr});
			debug("\t\t\t\t%s %5.2f\n", citizen.singular_name, specialistGain);

		}
		// sort gains ascending
		std::sort(baseTerraformingInfo.workerGains.begin(), baseTerraformingInfo.workerGains.end(), [](WorkerGain const &o1, WorkerGain const &o2) { return o1.gain < o2.gain; });

	}

	// base unworked tile yields

	debug("\tunworked tile yields\n");
	for (int baseId : aiData.baseIds)
	{
		debug("\t\t%s\n", Bases[baseId].name);
		BaseTerraformingInfo &baseTerraformingInfo = getBaseTerraformingInfo(baseId);
		std::vector<ResourceYield> &unworkedTileYields = baseTerraformingInfo.unworkedTileYields;

		// process available unworked tiles

		std::vector<MAP *> availableTiles = baseTerraformingInfo.terraformingSites;
		std::vector<MAP *> workedTiles = getBaseWorkedTiles(baseId);
		robin_hood::unordered_flat_set<MAP *> workedTileSet(workedTiles.begin(), workedTiles.end());

		for (MAP *availableTile : availableTiles)
		{
			// unworked

			if (workedTileSet.find(availableTile) == workedTileSet.end())
				continue;

			unworkedTileYields.push_back(getTileResourceYield(availableTile, baseId));

		}

		// remove all yields from there those are equal or inferior to any remaining yield there

		for (auto it = unworkedTileYields.begin(); it != unworkedTileYields.end(); )
		{
			bool remove = false;

			for (auto it2 = unworkedTileYields.begin(); it2 != unworkedTileYields.end(); ++it2)
			{
				if (it == it2)
					continue;

				if (ResourceYield::isEqualOrSuperior(*it2, *it))
				{
					remove = true;
					break;
				}

			}

			if (remove)
			{
				it = unworkedTileYields.erase(it);
			}
			else
			{
				++it;
			}

		}

		if (DEBUG)
		{
			for (ResourceYield unworkedTileYeild : unworkedTileYields)
			{
				debug("\t\t\t{%2d-%2d-%2d}\n", unworkedTileYeild.nutrient, unworkedTileYeild.mineral, unworkedTileYeild.energy);
			}

		}

	}

	// record ongoing terraforming as planned
	// the live map tile is never mutated here - it always reflects true current game state.
	// "planned" work is tracked purely as data (terraformingItems), not as array state.

	for (int vehicleId : aiData.formerVehicleIds)
	{
		VEH &vehicle = Vehs[vehicleId];

		// currently terraforming

		if (!isVehicleTerraforming(vehicleId))
			continue;

		// get order and convert it to former action

		auto action = static_cast<FormerItem>(vehicle.order - ORDER_FARM);

		TileTerraformingInfo &tileTerraformingInfo = tileTerraformingInfos[getVehicleMapTileIndex(vehicleId)];
		tileTerraformingInfo.terraformingItems.insert(action);

	}

	// landRockyTileCount

	for (TileTerraformingInfo &tileTerraformingInfo : tileTerraformingInfos)
	{
		MAP *tile = tileTerraformingInfo.tile;

		tileTerraformingInfo.landRockyTileCount = INT_MAX;

		if (!(tile->is_land() && tile->is_rocky()))
			continue;

		for (int baseId : tileTerraformingInfo.workableBaseIds)
		{
			BaseTerraformingInfo &baseTerraformingInfo = baseTerraformingInfos.at(baseId);

			tileTerraformingInfo.landRockyTileCount = std::min(tileTerraformingInfo.landRockyTileCount, baseTerraformingInfo.landRockyTileCount);

		}

	}

	// sensor coverage parameters

	// baseRadiusTileCount

	int territoryTileCountOpen = 0;
	int territoryTileCountBase = 0;
	for (MAP *tile = *MapTiles; tile < *MapTiles + *MapAreaTiles; tile++)
	{
		if (tile->owner != aiFactionId)
			continue;

		if (tile->is_base_or_bunker())
		{
			territoryTileCountBase++;
		}
		else
		{
			territoryTileCountOpen++;
		}

	}

	// totalVehicleCost

	int totalVehicleCostOpen = 0;
	int totalVehicleCostBase = 0;
	for (int vehicleId : aiData.vehicleIds)
	{
		VEH &vehicle = Vehs[vehicleId];
		MAP *tile = getVehicleMapTile(vehicleId);
		if (tile->owner != aiFactionId)
			continue;

		if (tile->is_base_or_bunker())
		{
			totalVehicleCostBase += vehicle.cost();
		}
		else
		{
			totalVehicleCostOpen += vehicle.cost();
		}

	}

	// averageVehicleCostPerTile

	averageVehicleCostPerTileInOpen = static_cast<double>(totalVehicleCostOpen) / static_cast<double>(territoryTileCountOpen);
	averageVehicleCostPerTileInBase = static_cast<double>(totalVehicleCostBase) / static_cast<double>(territoryTileCountBase);

	Profiling::stop("populateTerraformingData");

}

/*
Checks and removes redundant orders.
*/
void cancelRedundantOrders()
{
	Profiling::start("cancelRedundantOrders", "moveFormerStrategy");

	for (int id : aiData.formerVehicleIds)
	{
		if (isVehicleTerrafomingOrderCompleted(id))
		{
			setVehicleOrder(id, ORDER_NONE);
		}

	}

	Profiling::stop("cancelRedundantOrders");

}

void generateTerraformingRequests()
{
	Profiling::start("generateTerraformingRequests", "moveFormerStrategy");

	// conventional requests

	debug("CONVENTIONAL\n");

	significantTerraformingRequestLocations.clear();

	for (int baseId : aiData.baseIds)
	{
		generateBaseConventionalTerraformingRequests(baseId);
	}

	// aquifer

	debug("AQUIFER\n");

	for (MAP *tile : terraformingSites)
	{
		generateAquiferTerraformingRequest(tile);
	}

	// raise workable tiles to increase energy output

	debug("RAISE LAND TERRAFORMING_REQUESTS\n");

	for (MAP *tile : terraformingSites)
	{
		generateRaiseLandTerraformingRequest(tile);
	}

	// network

	debug("NETWORK TERRAFORMING_REQUESTS\n");

	for (MAP *tile : terraformingSites)
	{
		// land

		if (is_ocean(tile))
			continue;

		generateNetworkTerraformingRequest(tile);

	}

	// sensors

	debug("SENSOR TERRAFORMING_REQUESTS\n");

	for (MAP *tile : terraformingSites)
	{
		generateSensorTerraformingRequest(tile);
	}

	// bunkers

	debug("BUNKER TERRAFORMING_REQUESTS\n");

	for (MAP *tile : terraformingSites)
	{
		// land

		if (is_ocean(tile))
			continue;

		generateBunkerTerraformingRequest(tile);

	}

	// land bridges

	if (*CurrentTurn > 20)
	{
		debug("LAND BRIDGE TERRAFORMING_REQUESTS\n");
		generateLandBridgeTerraformingRequests();
	}

	// sort terraformingRequests by formerGain descending

	std::stable_sort(terraformingRequests.begin(), terraformingRequests.end(), [](TerraformingRequest const &a, TerraformingRequest const &b){ return a.formerGain > b.formerGain; });

	// TODO remove requests for same option/action in same tile. Also remove less priotiosed incompatible requests those would cancel previous requests or be cancelled by previous requests.

	Profiling::stop("generateTerraformingRequests");

}

/**
Generates conventional terraforming request.
*/
void generateBaseConventionalTerraformingRequests(int baseId)
{
	debug("generateConventionalTerraformingRequests - %s\n", Bases[baseId].name);

	BaseTerraformingInfo &baseTerraformingInfo = getBaseTerraformingInfo(baseId);

	std::vector<TerraformingOptionScore> baseTerraformingOptionScores;
	for (MAP *tile : baseTerraformingInfo.terraformingSites)
	{
		TileInfo &tileInfo = aiData.getTileInfo(tile);
		TileTerraformingInfo &tileTerraformingInfo = getTileTerraformingInfo(tile);

		debug("\t%s\n", getLocationString(tile));

		std::vector<TerraformingOptionScore> terraformingOptionScores;
		for (TERRAFORMING_OPTION *option : BASE_TERRAFORMING_OPTIONS[tileInfo.ocean])
		{
			// rocky option requires rocky tile

			if (option->rocky && !tileTerraformingInfo.landRocky)
				continue;

			// 1. collect option actions not yet present on the tile (road is always attempted alongside the option)
			// isTerraformingAvailable validates each action's full prerequisite chain (see its own recursive check),
			// so anything collected here is guaranteed completable on this tile - no separate validation pass needed

			robin_hood::unordered_flat_set<FormerItem> actions;

			if (isTerraformingAvailable(tile, FORMER_ROAD, false))
			{
				actions.insert(FORMER_ROAD);
			}

			for (FormerItem action : option->actions)
			{
				if (isTerraformingAvailable(tile, action, false))
				{
					actions.insert(action);
				}
			}

			if (actions.empty())
				continue;

			// 2. add prerequisite actions needed to complete the collected actions (e.g. remove fungus before road)
			// 3. no separate check needed - step 1 already confirmed the full prerequisite chain is available

			robin_hood::unordered_flat_set<FormerItem> unavailableActions = addPrerequisites(tile, actions);

			// 4. generate resulting tile and 5. compute its yield
			// mutate the live tile directly, read through it, then restore - never left mutated

			MAP savedTile = *tile;
			TileTerraformingInfo::applyTerraforming(tile, actions);
			ResourceYield improvedTileYield = getTileResourceYield(tile, baseId);
			*tile = savedTile;

			// discard option if it is no better than already unworked tile yield

			bool noBetter = false;
			for (ResourceYield unworkedTileYield : baseTerraformingInfo.unworkedTileYields)
			{
				if (ResourceYield::isEqualOrInferior(improvedTileYield, unworkedTileYield))
				{
					noBetter = true;
					break;
				}
			}
			if (noBetter)
				continue;

			// calculate option score

			double improvedTileGain = computeWorkerGain(baseId, improvedTileYield);

			// factor in area effect

			switch (option->requiredAction)
			{
			case FORMER_CONDENSER:
				improvedTileGain += getCondenserGain(tile);
				break;
			case FORMER_ECH_MIRROR:
				improvedTileGain += getEchelonMirrorGain(tile);
				break;
			default: ;
			}

			if (improvedTileGain < 0.0)
				continue;

			// factor average gain delay

			int terraformingTime = 0;
			for (FormerItem action : actions)
			{
				terraformingTime += Terraform[action].rate;
			}
			double averageTerraformingTime = static_cast<double>(terraformingTime) / static_cast<double>(actions.size());
			improvedTileGain = getGainDelay(improvedTileGain, averageTerraformingTime);

			// adjust score to preserve land rocky tiles

			if (tileTerraformingInfo.landRocky && !option->rocky && tileTerraformingInfo.landRockyTileCount < PRESERVED_LAND_ROCKY_TILE_COUNT)
			{
				double landRockyPreservationCoefficient = std::min(1.0, static_cast<double>(tileTerraformingInfo.landRockyTileCount) / static_cast<double>(PRESERVED_LAND_ROCKY_TILE_COUNT));
				improvedTileGain *= landRockyPreservationCoefficient; // NOLINT
			}

			// penalize improvement destruction

			uint32_t incompatibleFormerItemBits = 0;
			for (FormerItem const &actionFormerItem : actions)
			{
				CTerraform actionTerraform = Terraform[actionFormerItem];
				incompatibleFormerItemBits |= actionTerraform.bit_incompatible;
			}

			// remove built items as they could be incompatible with themselves and is going to be built anyway
			for (FormerItem const &actionFormerItem : actions)
			{
				CTerraform actionTerraform = Terraform[actionFormerItem];
				incompatibleFormerItemBits &= ~actionTerraform.bit;
			}

			// compute total built items terraforming rate
			int builtItemTotalTerraformingRate = 0;
			for (FormerItem const &actionFormerItem : actions)
			{
				CTerraform builtItemTerraform = Terraform[actionFormerItem];
				builtItemTotalTerraformingRate += builtItemTerraform.rate;
			}

			// compute total destroyed items terraforming rate
			int destroyedItemTotalTerraformingRate = 0;
			for (FormerItem const &destroyedFormerItem : {FORMER_FARM, FORMER_SOIL_ENR, FORMER_MINE, FORMER_SOLAR, FORMER_FOREST, FORMER_CONDENSER, FORMER_ECH_MIRROR, FORMER_THERMAL_BORE, })
			{
				CTerraform destroyedItemTerraform = Terraform[destroyedFormerItem];

				// destroyed improvement must exist and be incompatible
				if (map_has_item(tile, destroyedItemTerraform.bit) && (incompatibleFormerItemBits & destroyedItemTerraform.bit) != 0)
				{
					destroyedItemTotalTerraformingRate += destroyedItemTerraform.rate;
				}

			}

			// update gain with preservation coefficient
			double preservationCoefficient = static_cast<double>(builtItemTotalTerraformingRate) / static_cast<double>(builtItemTotalTerraformingRate + destroyedItemTotalTerraformingRate);
			improvedTileGain *= preservationCoefficient; // NOLINT(clang-diagnostic-unused-variable)
			debug("\t\t%5.2f %-16s improvedTileYield={%d-%d-%d} preservationCoefficient=%5.2f\n", improvedTileGain, option->name, improvedTileYield.nutrient, improvedTileYield.mineral, improvedTileYield.energy, preservationCoefficient);

			// 6. drop actions still blocked on a prerequisite - only what's actually doable now goes into the request

			for (FormerItem unavailableAction : unavailableActions)
			{
				actions.erase(unavailableAction);
			}

			// 7. record the final currently-available actions and their gain

			terraformingOptionScores.push_back({tile, option, actions, improvedTileGain});

		}

		// nothing found

		if (terraformingOptionScores.empty())
			continue;

		// find best option

		auto iterator = std::max_element(terraformingOptionScores.begin(), terraformingOptionScores.end(), [](TerraformingOptionScore const &a, TerraformingOptionScore const &b) { return a.incomeGain < b.incomeGain; });
		TerraformingOptionScore bestTerraformingOptionScore = std::move(*iterator);
		terraformingOptionScores.erase(iterator);

		// compute fitScore and adjusted score for flat output improvements

		if (bestTerraformingOptionScore.option->requiredAction == FORMER_FOREST || bestTerraformingOptionScore.option->requiredAction == FORMER_THERMAL_BORE)
		{
			// find second best option gain

			double secondBestOptionGain = 0.0;
			if (!terraformingOptionScores.empty())
			{
				secondBestOptionGain = std::max_element(terraformingOptionScores.begin(), terraformingOptionScores.end(), [](TerraformingOptionScore const &a, TerraformingOptionScore const &b) { return a.incomeGain < b.incomeGain; })->incomeGain;
			}

			// compute fitScore and adjusted score
			// 0.75 when best option is same as second best option
			// 1.25 when best option is 2x better than second best option

			double fitScoreMultiplier = 1.00 + (-0.25 + (0.25 + 0.25) * 2.00 * std::min(0.5, 1.0 - secondBestOptionGain / bestTerraformingOptionScore.incomeGain));
			bestTerraformingOptionScore.incomeGain *= fitScoreMultiplier;

		}

		// store baseTerraformingOptionScore

		baseTerraformingOptionScores.push_back(bestTerraformingOptionScore);
		debug("\t\t%5.2f %s\n", bestTerraformingOptionScore.incomeGain, bestTerraformingOptionScore.option->name);

	}

	// sort baseTerraformingOptionScores by incomeGain descending

	std::sort(baseTerraformingOptionScores.begin(), baseTerraformingOptionScores.end(), [](const TerraformingOptionScore &a, const TerraformingOptionScore &b) { return a.incomeGain > b.incomeGain; });

	// match them with the worst worker to get the improvement gain

	debug("\treplacement gain\n");
	std::vector<bool> matched(baseTerraformingInfo.workerGains.size(), false);
	for (TerraformingOptionScore &baseTerraformingOptionScore : baseTerraformingOptionScores)
	{
		int matchedIndex = -1;

		// 1. Find workerGains that has same tile as current baseTerraformingOptionScore.

		for (int i = 0; i < static_cast<int>(baseTerraformingInfo.workerGains.size()); i++)
		{
			if (!matched[i] && baseTerraformingInfo.workerGains[i].tile == baseTerraformingOptionScore.tile)
			{
				matchedIndex = i;
				break;
			}
		}

		// 2. If none found, then pick not yet matched workerGains with the minimal gain.

		if (matchedIndex == -1)
		{
			for (int i = 0; i < static_cast<int>(baseTerraformingInfo.workerGains.size()); i++)
			{
				if (!matched[i])
				{
					matchedIndex = i;
					break;
				}
			}
		}

		// no available worker found - exit

		if (matchedIndex == -1)
			break;

		matched[matchedIndex] = true;
		baseTerraformingOptionScore.incomeGain -= baseTerraformingInfo.workerGains[matchedIndex].gain;
		debug("\t\t%5.2f %s %-16s matchedIndex=%2d replacementGain=%5.2f\n", baseTerraformingOptionScore.incomeGain, getLocationString(baseTerraformingOptionScore.tile), baseTerraformingOptionScore.option->name, matchedIndex, baseTerraformingOptionScore.incomeGain);

		// gain is not positive - exit

		if (baseTerraformingOptionScore.incomeGain <= 0)
			break;

		insertActionTerraformingRequests(baseTerraformingOptionScore.tile, baseTerraformingOptionScore.option, baseTerraformingOptionScore.actions, baseTerraformingOptionScore.incomeGain);

	}

}

void generateLandBridgeTerraformingRequests()
{
	debug("generateLandBridgeTerraformingRequests\n");

	robin_hood::unordered_flat_map<int, MapIntValue> bridgeRequests;

	for (MAP *tile : raiseableCoasts)
	{
		int bridgeLandCluster = -1;
		int bridgeRange = -1;

		for (MAP *rangeTile : getRangeTiles(tile, 6, false))
		{
			bool rangeTileOcean = is_ocean(rangeTile);
			int rangeTileLandCluster = getLandCluster(rangeTile);

			// land

			if (rangeTileOcean)
				continue;

			// different cluster

			if (isSameLandCluster(tile, rangeTile))
				continue;

			// lock the minRange

			bridgeLandCluster = rangeTileLandCluster;
			bridgeRange = getRange(tile, rangeTile);
			break;

		}

		if (bridgeLandCluster == -1)
			continue;

		if (bridgeRequests.count(bridgeLandCluster) == 0)
		{
			MapIntValue mapValue(nullptr, INT_MAX);
			bridgeRequests.insert({bridgeLandCluster, mapValue});
		}

		if (bridgeRange < bridgeRequests.at(bridgeLandCluster).value)
		{
			bridgeRequests.at(bridgeLandCluster).tile = tile;
			bridgeRequests.at(bridgeLandCluster).value = bridgeRange;
		}

	}

	for (robin_hood::pair<int, MapIntValue> &bridgeRequestEntry : bridgeRequests)
	{
		MapIntValue bridgeRequest = bridgeRequestEntry.second;

		// score

		double gain =
			conf.ai_terraforming_landBridgeValue
			* getExponentialCoefficient(conf.ai_terraforming_landBridgeRangeScale, bridgeRequest.value - 2)
		;

		debug
		(
			"\t%s"
			" gain=%5.2f"
			" conf.ai_terraforming_landBridgeValue=%5.2f"
			" conf.ai_terraforming_landBridgeRangeScale=%5.2f"
			" bridgeRequest.value=%d"
			"\n"
			, getLocationString(bridgeRequest.tile)
			, gain
			, conf.ai_terraforming_landBridgeValue
			, conf.ai_terraforming_landBridgeRangeScale
			, bridgeRequest.value
		);

		TerraformingRequest terraformingRequest(bridgeRequest.tile, &TO_RAISE_LAND, FORMER_RAISE_LAND, gain);

		terraformingRequests.push_back(terraformingRequest);

	}

}

/*
Generate request for aquifer.
*/
void generateAquiferTerraformingRequest(MAP *tile)
{
	FormerItem action = FORMER_AQUIFER;

	if (!isTerraformingAvailable(tile, action, false))
		return;

	// compute gain

	double gain = getAquiferGain(tile);

	// store terraformingRequest

	terraformingRequests.emplace_back(tile, nullptr, action, gain);

}

/*
Generate request to raise energy collection.
*/
void generateRaiseLandTerraformingRequest(MAP *tile)
{
	FormerItem action = FORMER_RAISE_LAND;

	// land

	if (!tile->is_land())
		return;

	// available action

	if (!isTerraformingAvailable(tile, action, false))
		return;

	// compute gain

	double gain = getRaiseLandGain(tile);

	terraformingRequests.emplace_back(tile, nullptr, action, gain);

}

/**
Generates request for network (road/tube).
*/
void generateNetworkTerraformingRequest(MAP *tile)
{
	robin_hood::unordered_flat_set<FormerItem> actions;

	// action: road or magtube

	if (isTerraformingAvailable(tile, FORMER_ROAD, false))
	{
		actions.insert(FORMER_ROAD);
	}
	else if (isTerraformingAvailable(tile, FORMER_MAGTUBE, false))
	{
		actions.insert(FORMER_MAGTUBE);
	}
	else
	{
		return;
	}

	// add prerequisites and store currently unavailable actions
	robin_hood::unordered_flat_set<FormerItem> unavailableActions = addPrerequisites(tile, actions);

	// generate terraforming changes

	MAP originalState = *tile;
	TileTerraformingInfo::applyTerraforming(tile, actions);
	MAP improvedState = *tile;
	*tile = originalState;
	double gain = getNetworkGain(tile, originalState, improvedState);

	// remove currently unavailable actions

	for (FormerItem unavailableAction : unavailableActions)
	{
		actions.erase(unavailableAction);
	}

	insertActionTerraformingRequests(tile, nullptr, actions, gain);

}

/**
Generate request for sensor.
*/
void generateSensorTerraformingRequest(MAP *tile)
{
	FormerItem action = FORMER_SENSOR;

	// available

	if (!isTerraformingAvailable(tile, action, false))
		return;

	robin_hood::unordered_flat_set<FormerItem> actions;
	actions.insert(action);

	// add prerequisites and store currently unavailable actions
	robin_hood::unordered_flat_set<FormerItem> unavailableActions = addPrerequisites(tile, actions);

	// generate terraforming changes

	double gain = getSensorGain(tile);

	// remove currently unavailable actions

	for (FormerItem unavailableAction : unavailableActions)
	{
		actions.erase(unavailableAction);
	}

	insertActionTerraformingRequests(tile, nullptr, actions, gain);

}

/**
Generate request for bunker.
*/
void generateBunkerTerraformingRequest(MAP *tile)
{
	FormerItem action = FORMER_BUNKER;

	// available

	if (!isTerraformingAvailable(tile, action, false))
		return;

	robin_hood::unordered_flat_set<FormerItem> actions;
	actions.insert(action);

	// add prerequisites and store currently unavailable actions
	robin_hood::unordered_flat_set<FormerItem> unavailableActions = addPrerequisites(tile, actions);

	// generate terraforming changes

	double gain = getBunkerGain(tile);

	// remove currently unavailable actions

	for (FormerItem unavailableAction : unavailableActions)
	{
		actions.erase(unavailableAction);
	}

	insertActionTerraformingRequests(tile, nullptr, actions, gain);

}

/*
Removes terraforming requests violating proximity rules.
*/
void applyProximityRules()
{
	// Proximity rules are now handled during assignment phase to ensure consistency
	// and to avoid premature filtering of high-priority requests.
}

// verifies action complies with proximity rules
bool isProximityRuleSatisfied(MAP *tile, FormerItem action)
{
	// return true if proximity rule not defined

	if (PROXIMITY_RULES.find(action) == PROXIMITY_RULES.end())
		return true;

	ProximityRule const &proximityRule = PROXIMITY_RULES.at(action);

	// check existing improvements

	for (MAP *rangeTile : getRangeTiles(tile, proximityRule.existingDistance, true))
	{
		if (map_has_item(rangeTile, proximityRule.item))
		{
			return false;
		}

	}

	// check building improvements

	for (MAP *rangeTile : getRangeTiles(tile, proximityRule.buildingDistance, true))
	{
		TileTerraformingInfo const &rangeTileTerraformingInfo = getTileTerraformingInfo(rangeTile);

		if (rangeTileTerraformingInfo.terraformingItems.find(action) != rangeTileTerraformingInfo.terraformingItems.end())
		{
			return false;
		}

	}

	// validated

	return true;

}

/*
Removes terraforming requests for action that has already been terraformed or appears higher in the list.
*/
void removeTerraformedTiles()
{
	Profiling::start("removeTerraformedTiles", "moveFormerStrategy");

	robin_hood::unordered_flat_map<MAP *, robin_hood::unordered_flat_set<FormerItem>> terraformedActions;

	for
	(
		auto terraformingRequestsIterator = terraformingRequests.begin();
		terraformingRequestsIterator != terraformingRequests.end();
	)
	{
		TerraformingRequest &terraformingRequest = *terraformingRequestsIterator;
		TileTerraformingInfo &terraformingRequestTileInfo = getTileTerraformingInfo(terraformingRequest.tile);

		if
		(
			terraformingRequestTileInfo.terraformingItems.find(terraformingRequest.action) != terraformingRequestTileInfo.terraformingItems.end()
			||
			(terraformedActions.find(terraformingRequestTileInfo.tile) != terraformedActions.end() && terraformedActions.at(terraformingRequestTileInfo.tile).find(terraformingRequest.action) != terraformedActions.at(terraformingRequestTileInfo.tile).end())
		)
		{
			terraformingRequestsIterator = terraformingRequests.erase(terraformingRequestsIterator);
		}
		else
		{
			terraformedActions[terraformingRequestTileInfo.tile].insert(terraformingRequest.action);
			++terraformingRequestsIterator;
		}

	}

	Profiling::stop("removeTerraformedTiles");

//	if (DEBUG)
//	{
//		debug("terraforming requests - %s\n", aiMFaction->noun_faction);
//		for (TERRAFORMING_REQUEST &terraformingRequest : terraformingRequests)
//		{
//			debug
//			(
//				"\t%s"
//				" %-20s"
//				" %6.2f"
//				" income=%5.2f time=%5.2f"
//				"\n"
//				, getLocationString(terraformingRequest.tile)
//				, terraformingRequest.option->name
//				, terraformingRequest.gain
//				, terraformingRequest.income
//				, terraformingRequest.terraformingTime
//			);
//		}
//
//	}

}

/*
 * 1. select terraformingRequest with the highest incomeGain
 * 2. find a former with the highest former incomeGainGrowth (incomeGain / totalTime) for this request
 * 3. assign this former request
 * 4. update terraformingItems
 * 5. apply proximity rule to remove similar requests
 */
void assignFormerOrders()
{
	Profiling::start("assignFormerOrders", "moveFormerStrategy");

	debug("assignFormerOrders - %s\n", MFactions[aiFactionId].noun_faction);

	// distribute orders

	aiData.production.terraformingRequests.clear();

	// set of assigned tile and action pairs
	std::set<std::pair<MAP *, FormerItem>> assignedRequests;

	// track formers working on each request for cooperation
	struct ScoredFormer { FormerOrder* order; double gain; double travelTime; int terraformingTime; };
	robin_hood::unordered_flat_map<MAP *, robin_hood::unordered_flat_map<FormerItem, std::vector<ScoredFormer>>> requestAssignments;

	// Loop until no more formers can be assigned
	while (true)
	{
		ScoredFormer bestCombination = {nullptr, -1.0, 0.0, 0};
		TerraformingRequest* bestRequest = nullptr;

		for (TerraformingRequest &terraformingRequest : terraformingRequests)
		{
			MAP *tile = terraformingRequest.tile;
			FormerItem action = terraformingRequest.action;

			// verify action complies with proximity rules
			if (!isProximityRuleSatisfied(tile, action))
				continue;

			// find unassigned formers that can work on this request
			for (FormerOrder &formerOrder : formerOrders)
			{
				// skip already assigned in this turn
				if (formerOrder.tile != nullptr)
					continue;

				int vehicleId = formerOrder.vehicleId;
				VEH *vehicle = getVehicle(vehicleId);
				int triad = vehicle->triad();
				MAP *vehicleTile = getVehicleMapTile(vehicleId);

				// corresponding triad
				if ((triad == TRIAD_LAND && !tile->is_land()) || (triad == TRIAD_SEA && !tile->is_sea()))
					continue;

				// same cluster
				if ((triad == TRIAD_SEA && !isSameSeaCluster(vehicleTile, tile)) || (triad == TRIAD_LAND && !isSameLandTransportedCluster(vehicleTile, tile)))
					continue;

				// reachable
				if (!isVehicleDestinationReachable(vehicleId, tile))
					continue;

				double travelTime = getVehicleTravelTime(vehicleId, tile);
				if (travelTime == INF)
					continue;

				int terraformingTime = getTerraformingTime(vehicleId, tile, action);
				
				bool eligible = false;
				auto& cooperatingFormers = requestAssignments[tile][action];
				if (cooperatingFormers.empty())
				{
					// primary former
					eligible = true;
				}
				else
				{
					double workLeftAtArrival = 0.0;
					// Cooperation: check if secondary former spends at least 4 turns
					double workDoneBeforeArrival = 0.0;
					for (ScoredFormer const &cooperatingFormer : cooperatingFormers)
					{
						double prevWorkWindow = std::max(0.0, travelTime - cooperatingFormer.travelTime);
						workDoneBeforeArrival += 1.0 / static_cast<double>(cooperatingFormer.terraformingTime) * prevWorkWindow;
					}
					workLeftAtArrival = 1.0 - workDoneBeforeArrival;

					if (workLeftAtArrival > 0.0)
					{
						double candidateFormerWorkTime = 0.0;
						double combinedTerraformingRate = 1.0 / static_cast<double>(terraformingTime);
						for (ScoredFormer const &cooperatingFormer : cooperatingFormers)
						{
							combinedTerraformingRate += 1.0 / static_cast<double>(cooperatingFormer.terraformingTime);
						}
						candidateFormerWorkTime = workLeftAtArrival / combinedTerraformingRate;

						if (candidateFormerWorkTime >= 4.0)
						{
							// secondary former
							eligible = true;
						}
					}
				}
				if (!eligible)
					continue;

				// former gain

				double totalTime = conf.ai_terraforming_travel_time_multiplier * travelTime + static_cast<double>(terraformingTime);
				double gain = getGainIncomeGrowth(terraformingRequest.incomeGain / totalTime);

				// update best combination

				if (gain > bestCombination.gain)
				{
					bestCombination = {&formerOrder, gain, travelTime, terraformingTime};
					bestRequest = &terraformingRequest;
				}

			}

		}

		if (bestCombination.order == nullptr || bestRequest == nullptr)
			break;

		// assign the best combination

		MAP* tile = bestRequest->tile;
		FormerItem action = bestRequest->action;
		
		bestCombination.order->tile = tile;
		bestCombination.order->action = action;

		auto &cooperatingFormers = requestAssignments[tile][action];
		if (cooperatingFormers.empty())
		{
			// primary former assignment

			bestRequest->assigned = true;
			assignedRequests.insert({tile, action});

			// primary former effect - record as planned; live tile is never mutated here

			TileTerraformingInfo &tileTerraformingInfo = getTileTerraformingInfo(tile);
			tileTerraformingInfo.terraformingItems.insert(action);

			debug("\t%5.2f / %2d -> %5.2f %s %-16s\n", bestRequest->incomeGain, bestRequest->terraformingTime, bestRequest->formerGain, getLocationString(tile), Terraform[action].name);
			debug("\t\t[%4d] (PRIM) travelTime=%5.2f terraformingTime=%2d gain=%5.2f\n", bestCombination.order->vehicleId, bestCombination.travelTime, bestCombination.terraformingTime, bestCombination.gain);

		}
		else
		{
			// secondary former assignment
			// Recalculate candidateFormerWorkTime for the debug message (or store it in ScoredFormer)

			double workDoneBeforeArrival = 0.0;
			for (ScoredFormer const &cooperatingFormer : cooperatingFormers)
			{
				double prevWorkWindow = std::max(0.0, bestCombination.travelTime - cooperatingFormer.travelTime);
				workDoneBeforeArrival += 1.0 / static_cast<double>(cooperatingFormer.terraformingTime) * prevWorkWindow;
			}
			double workLeftAtArrival = 1.0 - workDoneBeforeArrival;
			double combinedTerraformingRate = 1.0 / static_cast<double>(bestCombination.terraformingTime);
			for (ScoredFormer const &cooperatingFormer : cooperatingFormers)
			{
				combinedTerraformingRate += 1.0 / static_cast<double>(cooperatingFormer.terraformingTime);
			}
			double candidateFormerWorkTime = workLeftAtArrival / combinedTerraformingRate;

			debug("\t\t[%4d] (COOP) travelTime=%5.2f workLeftAtArrival=%5.2f candidateWorkTime=%5.2f gain=%5.2f\n", bestCombination.order->vehicleId, bestCombination.travelTime, workLeftAtArrival, candidateFormerWorkTime, bestCombination.gain);

		}
		
		cooperatingFormers.push_back(bestCombination);
		// Sort cooperating formers by travel time so that the work calculation in subsequent iterations (if any) is correct.
		// Actually, the current logic for work calculation assumes they are ordered or at least handles it.
		// The current code used: `for (ScoredFormer const &cooperatingFormer : cooperatingFormers) { ... candidateFormer.travelTime - cooperatingFormer.travelTime ... }`
		// It seems it assumes candidate is arriving later than all previous ones.
		// Since we pick the best gain, it's not guaranteed they are picked in order of travel time.
		// However, a former with much longer travel time will have lower gain.
		// Let's sort them just in case.
		std::sort(cooperatingFormers.begin(), cooperatingFormers.end(), [](const ScoredFormer& a, const ScoredFormer& b) { return a.travelTime < b.travelTime; });

	}

	// add unassigned requests

	debug("unassigned requests\n");
	for (TerraformingRequest &terraformingRequest : terraformingRequests)
	{
		if (!terraformingRequest.assigned)
		{
			aiData.production.terraformingRequests.push_back(terraformingRequest);
			debug("\t%5.2f / %2d -> %5.2f %s %-16s\n", terraformingRequest.incomeGain, terraformingRequest.terraformingTime, terraformingRequest.formerGain, getLocationString(terraformingRequest.tile), Terraform[terraformingRequest.action].name);
		}

	}

	Profiling::stop("assignFormerOrders");

}

void setFormerTasks()
{
	debug("setFormerTasks - %s\n", MFactions[aiFactionId].noun_faction);

	// set all former tasks to none when there are no bases

	if (aiData.baseIds.empty())
	{
		for (FormerOrder &formerOrder : formerOrders)
		{
			setTask(Task(formerOrder.vehicleId, TT_NONE));
		}
		return;
	}

	Profiling::start("setFormerTasks", "moveFormerStrategy");

	// iterate former orders

	for (FormerOrder &formerOrder : formerOrders)
	{
		if (formerOrder.tile == nullptr || formerOrder.action == -1)
		{
			int vehicleId = formerOrder.vehicleId;
			VEH *vehicle = getVehicle(vehicleId);

			// kill supported formers without orders after first few turns

			if (*CurrentTurn > 5 && vehicle->home_base_id >= 0)
			{
				setTask(Task(vehicleId, TT_KILL));
			}

		}
		else
		{
			transitVehicle(Task(formerOrder.vehicleId, TT_TERRAFORM, formerOrder.tile, formerOrder.action));
			debug("\t[%4d] %s->%s %2d\n", formerOrder.vehicleId, getLocationString(getVehicleMapTile(formerOrder.vehicleId)), getLocationString(formerOrder.tile), formerOrder.action);
		}

	}

	Profiling::stop("setFormerTasks");

}

/**
Computes base tile improvement surplus effect.
*/

double computeWorkerGain(int baseId, ResourceYield const &tileYield)
{
	BaseTerraformingInfo &baseTerraformingInfo = getBaseTerraformingInfo(baseId);
	return baseTerraformingInfo.getIntakeGain(tileYield, 0, 0);
}

/*
Determines whether vehicle order is a terraforming order and is already completed in this tile.
*/
bool isVehicleTerrafomingOrderCompleted(int vehicleId)
{
	VEH *vehicle = getVehicle(vehicleId);

	if (!(vehicle->order >= ORDER_FARM && vehicle->order <= ORDER_PLACE_MONOLITH))
		return false;

	MAP *tile = getMapTile(vehicle->x, vehicle->y);

	return (tile->items & Terraform[vehicle->order - ORDER_FARM].bit);

}

// determines whether terraforming can be done in this square with all available prerequisites
// requireImmediatelyBuildable - true: action must be doable right now, with no prerequisite still pending
//                               false: action may still need a prerequisite completed first, and that's fine
bool isTerraformingAvailable(MAP *tile, FormerItem action, bool immediatelyBuildable)
{
	assert(isOnMap(tile));

	TileTerraformingInfo const &tileTerraformingInfo = getTileTerraformingInfo(tile);
	bool oceanDeep = is_ocean_deep(tile);
	bool aquaticFaction = MFactions[aiFactionId].rule_flags & RFLAG_AQUATIC;

	// terraforming is not discovered or disabled

	if (!has_terra(action, tile->is_sea(), aiFactionId))
		return false;

	// terraforming is not available at base square

	if (tile->is_base())
		return false;

	// action is not available if already there

	if (map_has_item(tile, Terraform[action].bit))
		return false;

	// ocean improvements in deep ocean are available for aquatic faction with Adv. Ecological Engineering only

	if (oceanDeep)
	{
		if (!(aquaticFaction && has_tech(TECH_EcoEng2, aiFactionId)))
			return false;
	}

	// check prerequisites
	// a prerequisite must itself be fully available (tech, proximity, compatibility, its own prerequisites, ...)
	// - not just researched - or the action can never actually be completed on this tile

	robin_hood::unordered_flat_set<FormerItem> prerequisites = getTerraformingPrerequisites(tile, action);

	if (!prerequisites.empty())
	{
		if (immediatelyBuildable)
			return false;

		for (FormerItem prerequisite : prerequisites)
		{
			if (!isTerraformingAvailable(tile, prerequisite, false))
				return false;
		}
	}

	// do not allow terraforming destroying ongoing terraforming

	bool compatible = true;
	for (FormerItem const &terraformingItem : tileTerraformingInfo.terraformingItems)
	{
		if (!isCompatibleTerraforming(terraformingItem, action))
		{
			compatible = false;
			break;
		}
	}
	if (!compatible)
		return false;

	// proximity rule

	if (!isProximityRuleSatisfied(tile, action))
		return false;

	// special cases

	switch (action)
	{
	case FORMER_REMOVE_FUNGUS:
		// removing fungus requires fungus
		if (!tile->is_fungus())
			return  false;
		break;
	case FORMER_RAISE_LAND:
		// cannot raise beyond limit
		if (map_level(tile) == (is_ocean(tile) ? ALT_OCEAN_SHELF : ALT_THREE_ABOVE_SEA))
			return false;
		// raising land should not disturb other faction bases
		if (!isRaiseLandSafe(tile))
			return false;
		break;
	case FORMER_LOWER_LAND:
		// not implemented
		break;
	case FORMER_SENSOR:
		// sensor should not destroy existing improvements
		if (isTerraformingDestructive(tile, action))
			return false;
		break;
	default: ;
	}

	return true;

}

// returns terraforming prerequisites
robin_hood::unordered_flat_set<FormerItem> getTerraformingPrerequisites(MAP *tile, FormerItem action)
{
	assert(isOnMap(tile));

	robin_hood::unordered_flat_set<FormerItem> prerequisites;

	// building road in fungus requires fungus removal if no fungus road build discovered

	if
	(
		tile->is_fungus()
		&&
		(action == FORMER_ROAD || action == FORMER_MAGTUBE)
		&&
		!has_tech(Rules->tech_preq_build_road_fungus, aiFactionId)
	)
	{
		prerequisites.insert(FORMER_REMOVE_FUNGUS);
	}

	// building improvement in fungus requires fungus removal if no fungus improvement built discovered

	if
	(
		tile->is_fungus()
		&&
		action != FORMER_REMOVE_FUNGUS && action != FORMER_ROAD && action != FORMER_MAGTUBE
		&&
		!has_tech(Rules->tech_preq_improv_fungus, aiFactionId)
	)
	{
		prerequisites.insert(FORMER_REMOVE_FUNGUS);
	}

	// certain improvements require flatter terrain

	if
	(
		tile->is_land_rocky()
		&&
		(action == FORMER_FARM || action == FORMER_SOIL_ENR || action == FORMER_FOREST)
	)
	{
		prerequisites.insert(FORMER_LEVEL_TERRAIN);
	}

	// soil enricher requires farm

	if (action == FORMER_SOIL_ENR)
	{
		prerequisites.insert(FORMER_FARM);
	}

	return prerequisites;

}

// teraforming destroys improvements except fungus
bool isTerraformingDestructive(MAP *tile, FormerItem action)
{
	return (tile->items & Terraform[action].bit_incompatible & ~BIT_FUNGUS) != 0;
}

/*
Determines whether raising land does not disturb others.
*/
bool isRaiseLandSafe(MAP *raisedTile)
{
	Faction *faction = &(Factions[aiFactionId]);

	// determine affected range

	int maxRange = is_ocean(raisedTile) ? 0 : map_elevation(raisedTile) + 1;

	// check tiles in range

	for (MAP *tile : getRangeTiles(raisedTile, maxRange, true))
	{
		// check if we have treaty/pact with owner

		if
		(
			(tile->owner != aiFactionId)
			&&
			(faction->diplo_status[tile->owner] & (DIPLO_TRUCE | DIPLO_TREATY | DIPLO_PACT))
		)
		{
			return false;
		}

	}

	return true;

}

/*
Checks that base can yield from this site.
*/
bool isWorkableTile(MAP *tile)
{
	if (tile == nullptr)
		return false;

	// exclude volcano mouth

	if ((tile->landmarks & LM_VOLCANO) && (tile->code_at() == 0))
		return false;

	// exclude claimed territory

	if (!(tile->owner == -1 || tile->owner == aiFactionId))
		return false;

	// exclude base

	if (map_has_item(tile, BIT_BASE_IN_TILE))
		return false;

	// exclude not base radius

	if (!map_has_item(tile, BIT_BASE_RADIUS))
		return false;

	// all conditions met

	return true;

}

/*
Checks that conventional improvement can be done at this site and base can work it.
conventional improvement = any yield improvement except river
*/
bool isValidConventionalTerraformingSite(MAP *tile)
{
	if (tile == nullptr)
		return false;

	// exclude not workable sites

	if (!isWorkableTile(tile))
		return false;

	// exclude monolith
	// it cannot be improved for yield

	if (map_has_item(tile, BIT_MONOLITH))
		return false;

	// all conditions met

	return true;

}

bool isValidTerraformingSite(MAP *tile)
{
	TileInfo &tileInfo = aiData.getTileInfo(tile);

	if (tile == nullptr)
		return false;

	// exclude volcano mouth

	if ((tile->landmarks & LM_VOLCANO) && (tile->code_at() == 0))
		return false;

	// exclude claimed territory

	if (!(tile->owner == -1 || tile->owner == aiFactionId))
		return false;

	// exclude base

	if (map_has_item(tile, BIT_BASE_IN_TILE))
		return false;

	// exclude blocked locations

	if (tileInfo.blocks.at(aiFactionId))
		return false;

	// all conditions met

	return true;

}

double getTerraformingResourceScore(double nutrient, double mineral, double energy)
{
	return
		+ conf.ai_terraforming_nutrientWeight * nutrient
		+ conf.ai_terraforming_mineralWeight * mineral
		+ conf.ai_terraforming_energyWeight * energy
	;
}

double getTerraformingResourceScore(ResourceYield const &resourceYield)
{
	return getTerraformingResourceScore(resourceYield.nutrient, resourceYield.mineral, resourceYield.energy);
}

void removeUnusedBunkers()
{
	debug("removeUnusedBunkers - %s\n", MFactions[aiFactionId].noun_faction);

	robin_hood::unordered_flat_set<MAP *> unusedBunkers;
	for (robin_hood::pair<MAP *, BunkerInfo> &bunkerInfoEntry : aiData.bunkerInfos)
	{
		MAP *bunkerTile = bunkerInfoEntry.first;

		if (getBunkerGain(bunkerTile) <= 0.0)
		{
			unusedBunkers.insert(bunkerTile);
		}

	}

	for (MAP *unusedBunkerTile : unusedBunkers)
	{
		unusedBunkerTile->items &= (~BIT_BUNKER);
		aiData.getTileInfo(unusedBunkerTile).bunker = false;
		aiData.bunkerInfos.erase(unusedBunkerTile);
		debug("\t%s\n", getLocationString(unusedBunkerTile));
	}

}

/**
Calculates terraforming time.
*/
int getTerraformingTime(int vehicleId, MAP *tile, FormerItem action)
{
	assert(vehicleId >= 0 && vehicleId <= *VehCount);
	assert(isOnMap(tile));
	assert(action >= FORMER_FARM && action <= FORMER_MONOLITH);

	VEH &vehicle = Vehs[vehicleId];
	int x = getX(tile);
	int y = getY(tile);
	int vehicleX = vehicle.x;
	int vehicleY = vehicle.y;

	// temporarily set vehicle coordinates to the target location

	vehicle.x = static_cast<int16_t>(x);
	vehicle.y = static_cast<int16_t>(y);

	// compute terraforming time
	// original action_terraform code gives one less turn for some reason

	int terraformingTime = action_terraform(vehicleId, action, 0) + 1;

	// restore vehicle coordinates

	vehicle.x = static_cast<int16_t>(vehicleX);
	vehicle.y = static_cast<int16_t>(vehicleY);

	// return terraforming time

	return terraformingTime;

}

void insertActionTerraformingRequests(MAP *tile, TERRAFORMING_OPTION const *option, robin_hood::unordered_flat_set<FormerItem> const& actions, double gain)
{
	// positive gain

	if (gain <= 0.0)
		return;

	// generate requests with distributed gain

	for (FormerItem action : actions)
	{
		double incomeGain = gain / static_cast<double>(actions.size());
		terraformingRequests.emplace_back(tile, option, action, incomeGain);
	}

}

double getCondenserGain(MAP *tile)
{
	double improvementEffect = 0.0;

	for (MAP *areaTile : getRangeTiles(tile, 1, true))
	{
		TileTerraformingInfo &areaTileTerraformingInfo = getTileTerraformingInfo(areaTile);

		// not rainy tile

		if (areaTile->is_rainy())
			continue;

		// worked tile

		if (!areaTileTerraformingInfo.worked)
			continue;

		int baseId = areaTileTerraformingInfo.workedBaseId;

		// old intake

		Resource oldResourceIntake2 = getBaseResourceIntake2(baseId);

		// new intake

		Resource newResourceIntake2 = oldResourceIntake2;
		newResourceIntake2.nutrient++;

		// accumulate improvementGain

		double improvementGain = getBaseImprovementGain(baseId, oldResourceIntake2, newResourceIntake2);
		improvementEffect += improvementGain;

	}

	return improvementEffect;

}

double getEchelonMirrorGain(MAP *tile)
{
	double improvementEffect = 0.0;

	for (MAP *areaTile : getRangeTiles(tile, 1, false))
	{
		TileTerraformingInfo &areaTileTerraformingInfo = getTileTerraformingInfo(areaTile);

		// solar collection in tile

		if (!map_has_item(areaTile, BIT_SOLAR))
			continue;

		// worked tile

		if (!areaTileTerraformingInfo.worked)
			continue;

		int baseId = areaTileTerraformingInfo.workedBaseId;

		// old intake

		Resource oldResourceIntake2 = getBaseResourceIntake2(baseId);

		// new intake

		Resource newResourceIntake2 = oldResourceIntake2;
		newResourceIntake2.energy += conf.echelon_mirror_bonus;

		// accumulate improvementGain

		double improvementGain = getBaseImprovementGain(baseId, oldResourceIntake2, newResourceIntake2);
		improvementEffect += improvementGain;

	}

	return improvementEffect;

}

double getAquiferGain(MAP *tile)
{
	double improvementEffect = 0.0;

	for (int radius = 0; radius <= 3; radius++)
	{
		int tileCount = 0;
		double radiusImprovementEffect = 0.0;

		for (MAP *radiusTile : getSquareBlockRadiusTiles(tile, radius, radius))
		{
			TileTerraformingInfo &radiusTileTerraformingInfo = getTileTerraformingInfo(radiusTile);

			// land only

			if (!radiusTile->is_land())
				continue;

			// not yet river

			if (map_has_item(radiusTile, BIT_RIVER))
				continue;

			// river can possibly extend here

			tileCount++;

			// worked

			if (!radiusTileTerraformingInfo.worked)
				continue;

			int baseId = radiusTileTerraformingInfo.workedBaseId;

			// old intake

			Resource oldResourceIntake2 = getBaseResourceIntake2(baseId);

			// new intake

			Resource newResourceIntake2 = oldResourceIntake2;
			newResourceIntake2.energy++;

			// accumulate improvementGain

			double improvementGain = getBaseImprovementGain(baseId, oldResourceIntake2, newResourceIntake2);
			radiusImprovementEffect += improvementGain;

		}

		// average radius improvement gain

		double averageRadiusImprovementEffect = tileCount <= 0 ? 0.0 : radiusImprovementEffect / tileCount;

		// accumulate improvement gain

		improvementEffect += averageRadiusImprovementEffect;

	}

	// extra bonus
	// river is compatible with any other improvement
	// river is a road

	improvementEffect *= 2.0;

	return improvementEffect;

}

/*
Raise land calculations.
*/
double getRaiseLandGain(MAP *tile)
{
	assert(isOnMap(tile));

	int x = getX(tile);
	int y = getY(tile);
	FormerItem action = FORMER_RAISE_LAND;
	int elevation = tile->elevation();

	// land only

	if (!tile->is_land())
		return 0.0;

	// available action

	if (!isTerraformingAvailable(tile, action, false))
		return 0.0;

	// not max elevation

	if (elevation >= ALT_THREE_ABOVE_SEA)
		return 0.0;

	// enough money

	int cost = terraform_cost(x, y, aiFactionId);
	if (cost > Factions[aiFactionId].energy_credits / 10)
		return 0.0;

	// new elevation

	int newElevation = elevation + 1;

	// compute improvement effect

	double improvementEffect = 0.0;

	for (MAP *rangeTile : getRangeTiles(tile, newElevation, true))
	{
		TileTerraformingInfo &rangeTileTerraformingInfo = getTileTerraformingInfo(rangeTile);

		// land only

		if (!rangeTile->is_land())
			continue;

		// solar collector or echelong mirror

		if (!map_has_item(tile, BIT_SOLAR | BIT_ECH_MIRROR))
			continue;

		// worked

		if (!rangeTileTerraformingInfo.worked)
			continue;

		int baseId = rangeTileTerraformingInfo.workedBaseId;

		// old intake

		Resource oldResourceIntake2 = getBaseResourceIntake2(baseId);

		// new intake

		Resource newResourceIntake2 = oldResourceIntake2;
		newResourceIntake2.energy++;

		// accumulate improvementGain

		double improvementGain = getBaseImprovementGain(baseId, oldResourceIntake2, newResourceIntake2);
		improvementEffect += improvementGain;

	}

	return improvementEffect;

}

double getNetworkGain(MAP *tile, MAP const &originalTile, MAP const &improvedTile)
{
	assert(isOnMap(tile));

	int x = getX(tile);
	int y = getY(tile);

	// find land bases in 5 range

	std::set<int> baseIds;

	for (int baseId : aiData.baseIds)
	{
		BASE &base = Bases[baseId];
		MAP *baseTile = getBaseMapTile(baseId);

		if (!baseTile->is_land())
			continue;

		if (getRange(x, y, base.x, base.y) > NETWORK_BASE_RANGE)
			continue;

		baseIds.insert(baseId);

	}

	// find paired bases lowest former travel times (original and improved)

	*tile = originalTile;
	robin_hood::unordered_flat_map<int, robin_hood::unordered_flat_map<int, double>> originalTravelTimes = getCrossBaseTravelTimes(baseIds);
	*tile = improvedTile;
	robin_hood::unordered_flat_map<int, robin_hood::unordered_flat_map<int, double>> improvedTravelTimes = getCrossBaseTravelTimes(baseIds);
	*tile = originalTile;

	// find travel time improvement

	double totalTravelTimeImprovement = 0.0;

	for (int orgBaseId : baseIds)
	{
		for (int dstBaseId : baseIds)
		{
			// not same

			if (dstBaseId == orgBaseId)
				continue;

			if
			(
				originalTravelTimes.find(orgBaseId) != originalTravelTimes.end() && originalTravelTimes.at(orgBaseId).find(dstBaseId) != originalTravelTimes.at(orgBaseId).end()
				&&
				improvedTravelTimes.find(orgBaseId) != improvedTravelTimes.end() && improvedTravelTimes.at(orgBaseId).find(dstBaseId) != improvedTravelTimes.at(orgBaseId).end()
			)
			{
				double travelTimeImprovement = originalTravelTimes.at(orgBaseId).at(dstBaseId) - improvedTravelTimes.at(orgBaseId).at(dstBaseId);
				if (travelTimeImprovement > 0.0)
				{
					totalTravelTimeImprovement += travelTimeImprovement;
				}
			}

		}

	}

	// halve travel time improvement since it was counted twice in both directions

	totalTravelTimeImprovement /= 2.0;

	// compute network gain

	double resourceScore = getResourceScore(conf.ai_terraforming_networkValueIncomeImprovement, 0.0);
	double improvementIncomeGrowh = resourceScore * totalTravelTimeImprovement / conf.ai_terraforming_networkValueTravelTimeDenominator;
	double networkGain = getGainIncome(improvementIncomeGrowh);

	return networkGain;

}

robin_hood::unordered_flat_map<int, robin_hood::unordered_flat_map<int, double>> getCrossBaseTravelTimes(std::set<int> const& baseIds)
{
	debug("getCrossBaseTravelTimes\n");

	// set tile -> base mapping

	robin_hood::unordered_flat_map<MAP *, int> tileBaseId;
	for (int baseId : baseIds)
	{
		MAP *baseTile = getBaseMapTile(baseId);
		tileBaseId[baseTile] = baseId;
	}

	robin_hood::unordered_flat_map<int, robin_hood::unordered_flat_map<int, double>> travelTimes;

	robin_hood::unordered_map<MAP *, int> travelCosts;
	robin_hood::unordered_set<MAP *> openNodes;
	robin_hood::unordered_set<MAP *> newOpenNodes;

	for (int orgBaseId : baseIds)
	{
		MAP *orgBaseTile = getBaseMapTile(orgBaseId);

		// land

		if (!orgBaseTile->is_land())
			continue;

		// create this base travelTime entry

		travelTimes.emplace(orgBaseId, robin_hood::unordered_flat_map<int, double>());

		// reset iteration variables

		travelCosts.clear();
		openNodes.clear();
		newOpenNodes.clear();

		// set travel origin

		travelCosts.emplace(orgBaseTile, 0);
		openNodes.insert(orgBaseTile);

		// expand travel network

		while (!openNodes.empty())
		{
			for (MAP *currentTile : openNodes)
			{
				int currentTileX = getX(currentTile);
				int currentTileY = getY(currentTile);

				for (MAP *adjacentTile : getAdjacentTiles(currentTile))
				{
					int adjacentTileX = getX(adjacentTile);
					int adjacentTileY = getY(adjacentTile);

					// land

					if (!adjacentTile->is_land())
						continue;

					// within 9 tiles

					if (getRange(orgBaseTile, adjacentTile) > 2 * NETWORK_BASE_RANGE + 1)
						continue;

					// hexCost

					int hexCost = mod_hex_cost(BSC_FORMERS, aiFactionId, currentTileX, currentTileY, adjacentTileX, adjacentTileY, 1);

					// compute travelCost

					int travelCost = travelCosts.at(currentTile) + hexCost;

					// update travelCost

					if (travelCosts.find(adjacentTile) == travelCosts.end() || travelCost < travelCosts.at(adjacentTile))
					{
						travelCosts[adjacentTile] = travelCost;
						newOpenNodes.insert(adjacentTile);
					}

				}

			}

			// swap open nodes

			openNodes.clear();
			openNodes.swap(newOpenNodes);

		}

		// populate cross base travel times

		for (int dstBaseId : baseIds)
		{
			MAP *dstBaseTile = getBaseMapTile(dstBaseId);

			if (travelCosts.find(dstBaseTile) == travelCosts.end())
				continue;

			double travelTime = static_cast<double>(travelCosts.at(dstBaseTile)) / static_cast<double>(Rules->move_rate_roads);
			travelTimes.at(orgBaseId).emplace(dstBaseId, travelTime);

		}

	}

	return travelTimes;

}

double getSensorGain(MAP *tile)
{
	assert(isOnMap(tile));

	TileInfo &tileInfo = aiData.getTileInfo(tile);

	// compute sensor coverage
	// already covered tiles give 1/4 additional bonus comparing to newly covered tiles

	double coveredVehicleCost = 0.0;
	for (TileInfo *rangeTileInfo : tileInfo.range2CenterTileInfos)
	{
		coveredVehicleCost += (rangeTileInfo->sensorCoverages.at(aiFactionId) ? 0.25 : 1.00) * (rangeTileInfo->tile->is_base_or_bunker() ? averageVehicleCostPerTileInBase : averageVehicleCostPerTileInOpen);
	}

	// sensor coverage saves 0.25 of vehicle cost

	double coveredVehicleCostReduction = 0.25 * coveredVehicleCost;

	// sensor gain

	double gain = getGainBonus(coveredVehicleCostReduction);

	return gain;

}

double getBunkerGain(MAP *tile)
{
	assert(isOnMap(tile));

	// land

	if (!tile->is_land())
		return 0.0;

	// not adjacent to base or bunker

	bool adjacentToBaseOrBunker = false;
	for (MAP *adjacentTile : getAdjacentTiles(tile))
	{
		if (adjacentTile->is_base_or_bunker())
			adjacentToBaseOrBunker = true;
	}
	if (adjacentToBaseOrBunker)
		return 0.0;

	// same land region unfriendly base range

	int closestEnemyBaseRange = INT_MAX;
	for (int baseId = 0; baseId < *BaseCount; baseId++)
	{
		BASE &base = Bases[baseId];
		MAP *baseTile = getBaseMapTile(baseId);

		// same land region

		if (baseTile->region != tile->region)
			continue;

		// unfriendly

		if (!isUnfriendly(aiFactionId, base.faction_id))
			continue;

		int range = getRange(tile, baseTile);
		closestEnemyBaseRange = std::min(closestEnemyBaseRange, range);

	}

	// not adjacent to enemy base

	if (closestEnemyBaseRange <= 1)
		return 0.0;

	// enemyBaseRangeCoefficient

	double enemyBaseRangeCoefficient = closestEnemyBaseRange <= BUNKER_ENEMY_BASE_RANGE_MIN ? 1.0 : static_cast<double>(BUNKER_ENEMY_BASE_RANGE_MAX - closestEnemyBaseRange) / static_cast<double>(BUNKER_ENEMY_BASE_RANGE_MAX - BUNKER_ENEMY_BASE_RANGE_MIN);

	// same land region player base placement coefficient

	double placementCoefficient = 0.0;
	for (int baseId = 0; baseId < *BaseCount; baseId++)
	{
		BASE &base = Bases[baseId];
		MAP *baseTile = getBaseMapTile(baseId);

		// same land region

		if (baseTile->region != tile->region)
			continue;

		// player

		if (base.faction_id != aiFactionId)
			continue;

		int range = getRange(tile, baseTile);
		if (range > BUNKER_PLAYER_BASE_RANGE_MAX)
			continue;

		placementCoefficient += getBunkerPlacementCoefficient(tile, baseTile);

	}
	for (MAP *otherTile = *MapTiles; otherTile < *MapTiles + *MapAreaTiles; otherTile++)
	{
		// same land region

		if (otherTile->region != tile->region)
			continue;

		// player

		if (otherTile->owner != aiFactionId)
			continue;

		// bunker

		if (!otherTile->is_bunker())
			continue;

		int range = getRange(tile, otherTile);
		if (range > BUNKER_PLAYER_BASE_RANGE_MAX)
			continue;

		placementCoefficient += getBunkerPlacementCoefficient(tile, otherTile);

	}

	return conf.ai_terraforming_bunkerValue * enemyBaseRangeCoefficient * placementCoefficient * aiData.factionInfos.at(aiFactionId).averageBaseGain;

}

double getBunkerPlacementCoefficient(MAP *bunkerTile, MAP *baseTile)
{
	assert(isOnMap(bunkerTile));
	assert(isOnMap(baseTile));

	int bunkerTileX = getX(bunkerTile);
	int bunkerTileY = getY(bunkerTile);
	int baseTileX = getX(baseTile);
	int baseTileY = getY(baseTile);

	int dx = std::abs(bunkerTileX - baseTileX);
	int dy = std::abs(bunkerTileY - baseTileY);
	if (!map_is_flat() && dx > *MapHalfX) { dx = *MapAreaX - dx; }
	int range = (dx + dy) / 2;
	int shift = std::abs(dx - dy) / 2;

	if (range < BUNKER_ENEMY_BASE_RANGE_MIN || range > BUNKER_PLAYER_BASE_RANGE_MAX)
		return 0.00;

	return BUNKER_PLACEMENT_COEFFICIENTS[range][shift];

}

bool isCompatibleTerraforming(FormerItem ongoingAction, FormerItem action)
{
	return (Terraform[ongoingAction].bit & Terraform[action].bit_incompatible) == 0;
}

// adds prerequisites to action set and returns currently unavailable actions
robin_hood::unordered_flat_set<FormerItem> addPrerequisites(MAP *tile, robin_hood::unordered_flat_set<FormerItem> &actions)
{
	robin_hood::unordered_flat_set<FormerItem> prerequisites;
	robin_hood::unordered_flat_set<FormerItem> unavailableActions;
	for (FormerItem action : actions)
	{
		robin_hood::unordered_flat_set<FormerItem> actionPrerequisites = getTerraformingPrerequisites(tile, action);
		prerequisites.insert(actionPrerequisites.begin(), actionPrerequisites.end());
		if (!actionPrerequisites.empty())
		{
			unavailableActions.insert(action);
		}
	}
	actions.insert(prerequisites.begin(), prerequisites.end());

	return unavailableActions;

}

