#pragma GCC diagnostic ignored "-Wshadow"

#include <cfloat>
#include <cmath>
#include <vector>

#include "wtp_aiMoveFormer.h"
#include "wtp_aiRoute.h"
#include "wtp_aiMove.h"

// variables

FactionTerraformingInfo factionTerraformingInfo;
std::vector<MAP *> raiseableCoasts;
robin_hood::unordered_flat_set<MAP *> significantTerraformingRequestLocations;

// FORMER_ORDER

FormerOrder::FormerOrder(int _vehicleId)
: vehicleId(_vehicleId)
{}

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

	// initialize data

	setupTerraformingData();

	// populate data

	populateTerraformingData();

	// formers

	cancelRedundantOrders();
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

	// populate base projected sizes

	for (int baseId : aiData.baseIds)
	{
		BaseTerraformingInfo &baseTerraformingInfo = getBaseTerraformingInfo(baseId);

		// 10 turns and mininum 3 pop

		baseTerraformingInfo.projectedPopSize = std::max(3, getBaseProjectedSize(baseId, 10));

	}

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

	// populate base minimal yields

	for (int baseId : aiData.baseIds)
	{
		BaseTerraformingInfo &baseTerraformingInfo = getBaseTerraformingInfo(baseId);

		std::vector<MAP *> baseWorkedTiles = getBaseWorkedTiles(baseId);

		if (baseWorkedTiles.empty())
		{
			baseTerraformingInfo.minimalNutrientYield = 0;
			baseTerraformingInfo.minimalMineralYield = 0;
			baseTerraformingInfo.minimalEnergyYield = 0;
		}
		else
		{
			baseTerraformingInfo.minimalNutrientYield = INT_MAX;
			baseTerraformingInfo.minimalMineralYield = INT_MAX;
			baseTerraformingInfo.minimalEnergyYield = INT_MAX;

			for (MAP *tile : getBaseWorkedTiles(baseId))
			{
				baseTerraformingInfo.minimalNutrientYield = std::min(baseTerraformingInfo.minimalNutrientYield, mod_crop_yield(aiFactionId, baseId, getX(tile), getY(tile), 0));
				baseTerraformingInfo.minimalMineralYield = std::min(baseTerraformingInfo.minimalMineralYield, mod_mine_yield(aiFactionId, baseId, getX(tile), getY(tile), 0));
				baseTerraformingInfo.minimalEnergyYield = std::min(baseTerraformingInfo.minimalEnergyYield, mod_energy_yield(aiFactionId, baseId, getX(tile), getY(tile), 0));
			}

		}

	}

	// store original map states

	for (TileTerraformingInfo &tileTerraformingInfo : tileTerraformingInfos)
	{
		tileTerraformingInfo.storeOriginalMapTile();
	}

	// store effective map states from ongoing terraforming

	for (int vehicleId : aiData.formerVehicleIds)
	{
		VEH &vehicle = Vehs[vehicleId];

		// currently terraforming

		if (!isVehicleTerraforming(vehicleId))
			continue;

		// get order and convert it to former action

		auto action = static_cast<FormerItem>(vehicle.order - ORDER_FARM);

		// store effective map states from ongoing terraforming

		TileTerraformingInfo &tileTerraformingInfo = tileTerraformingInfos[getVehicleMapTileIndex(vehicleId)];
		tileTerraformingInfo.applyTerraforming(action);
		tileTerraformingInfo.storeEffectiveMapTile();
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
		generateConventionalTerraformingRequests(baseId);
	}

	// remove overlapping conventional terraforming requests leaving only the most significant one

	std::sort(terraformingRequests.begin(), terraformingRequests.end());

	robin_hood::unordered_flat_set<MAP *> terraformingRequestLocations;
	for (auto terraformingRequestIterator = terraformingRequests.begin(); terraformingRequestIterator != terraformingRequests.end(); )
	{
		TerraformingRequest &terraformingRequest = *terraformingRequestIterator;

		if (terraformingRequestLocations.find(terraformingRequest.tile) != terraformingRequestLocations.end())
		{
			terraformingRequestIterator = terraformingRequests.erase(terraformingRequestIterator);
		}
		else
		{
			terraformingRequestLocations.insert(terraformingRequest.tile);
			++terraformingRequestIterator;
		}

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

	// sort terraformingRequests

	std::sort(terraformingRequests.begin(), terraformingRequests.end());

	Profiling::stop("generateTerraformingRequests");

}

/**
Generates conventional terraforming request.
*/
void generateConventionalTerraformingRequests(int baseId)
{
	debug("generateConventionalTerraformingRequests - %s\n", Bases[baseId].name);

	for (TileTerraformingInfo &tileTerraformingInfo : tileTerraformingInfos)
	{
		MAP *tile = tileTerraformingInfo.tile;
		TileInfo &tileInfo = aiData.getTileInfo(tileTerraformingInfo.tile);

		// available terraforming site

		if (!tileTerraformingInfo.availableBaseTerraformingSite)
			continue;

		debug("\t%s\n", getLocationString(tile));

		std::vector<TerraformingOptionScore> terraformingOptionScores;
		for (TERRAFORMING_OPTION *option : BASE_TERRAFORMING_OPTIONS[tileInfo.ocean])
		{
			// rocky option requires rocky tile

			if (option->rocky && !tileTerraformingInfo.landRocky)
				continue;

			// required action should be available

			if (!isTerraformingAvailable(tile, option->requiredAction))
				continue;

			// improve tile and collect actions

			std::set<FormerItem> actions;

			// remove fungus if needed

			if (option->requiredAction != FORMER_PLANT_FUNGUS && tile->is_fungus())
			{
				actions.insert(FORMER_REMOVE_FUNGUS);
				tileTerraformingInfo.applyTerraforming(FORMER_REMOVE_FUNGUS);
			}

			// level terrain if needed

			if (tile->is_land_rocky() && !option->rocky)
			{
				actions.insert(FORMER_LEVEL_TERRAIN);
				tileTerraformingInfo.applyTerraforming(FORMER_LEVEL_TERRAIN);
			}

			// apply available actions

			for (FormerItem action : option->actions)
			{
				if (isTerraformingAvailable(tile, action))
				{
					actions.insert(action);
					tileTerraformingInfo.applyTerraforming(action);
				}
			}

			// build road on land

			if (tileInfo.land && !map_has_item(tile, BIT_ROAD))
			{
				actions.insert(FORMER_ROAD);
				tileTerraformingInfo.applyTerraforming(FORMER_ROAD);
			}

			// get original and improved tile

			MAP const &originalTile = tileTerraformingInfo.effectiveTile;
			MAP const improvedTile = *tileTerraformingInfo.tile;

			// calculate option score

			double score = calculateConventionalTerraformingScore(tile, originalTile, improvedTile);
			tileTerraformingInfo.restoreEffectiveMapTile();

			// factor in area effect

			switch (option->requiredAction)
			{
			case FORMER_CONDENSER:
				score += getCondenserGain(tile);
				break;
			case FORMER_ECH_MIRROR:
				score += getEchelonMirrorGain(tile);
				break;
			default: ;
			}

			if (score < 0.0)
				continue;

			// adjust score to preserve land rocky tiles

			if (tileTerraformingInfo.landRocky && !option->rocky && tileTerraformingInfo.landRockyTileCount < PRESERVED_LAND_ROCKY_TILE_COUNT)
			{
				double landRockyPreservationCoefficient = std::min(1.0, static_cast<double>(tileTerraformingInfo.landRockyTileCount) / static_cast<double>(PRESERVED_LAND_ROCKY_TILE_COUNT));
				score *= landRockyPreservationCoefficient; // NOLINT
			}

			// save option score

			terraformingOptionScores.push_back({score, option, actions});

		}

		// nothing found

		if (terraformingOptionScores.empty())
			continue;

		// find best option

		auto iterator = std::max_element(terraformingOptionScores.begin(), terraformingOptionScores.end());
		TerraformingOptionScore bestTerraformingOptionScore = std::move(*iterator);
		terraformingOptionScores.erase(iterator);

		// find second best option score

		double secondBestOptionScore = 0.0;
		if (!terraformingOptionScores.empty())
		{
			secondBestOptionScore = std::max_element(terraformingOptionScores.begin(), terraformingOptionScores.end())->score;
		}

		// compute fitScore and adjusted score

		double fitScoreMultiplier = -0.25 + 1.00 * std::min(0.5, 1.0 - secondBestOptionScore / bestTerraformingOptionScore.score);
		double optionScore = fitScoreMultiplier * bestTerraformingOptionScore.score;

		// insert actions

		insertActionTerraformingRequests(tile, bestTerraformingOptionScore.actions, optionScore);

		if (DEBUG)
		{
			debug("\t%5.2f %-15s\n", optionScore, bestTerraformingOptionScore.option->name);
		}

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

		TerraformingRequest terraformingRequest(bridgeRequest.tile, FORMER_RAISE_LAND, gain);

		terraformingRequests.push_back(terraformingRequest);

	}

}

/*
Generate request for aquifer.
*/
void generateAquiferTerraformingRequest(MAP *tile)
{
	// compute gain

	double gain = getAquiferGain(tile);

	// store terraformingRequest

	terraformingRequests.emplace_back(tile, FORMER_AQUIFER, gain);

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

	if (!isTerraformingAvailable(tile, action))
		return;

	// compute gain

	double gain = getRaiseLandGain(tile);

	terraformingRequests.emplace_back(tile, action, gain);

}

/**
Generates request for network (road/tube).
*/
void generateNetworkTerraformingRequest(MAP *tile)
{
	TileTerraformingInfo &tileTerraformingInfo = getTileTerraformingInfo(tile);

	// land

	if (!tile->is_land())
		return;

	// action: road or magtube

	FormerItem action;
	if (isTerraformingAvailable(tile, FORMER_ROAD))
	{
		action = FORMER_ROAD;
	}
	else if (isTerraformingAvailable(tile, FORMER_MAGTUBE))
	{
		action = FORMER_MAGTUBE;
	}
	else
	{
		return;
	}

	// generate terraforming changes

	tileTerraformingInfo.applyTerraforming(action);
	MAP originalTile = tileTerraformingInfo.effectiveTile;
	MAP improvedTile = *tile;
	double gain = getNetworkGain(tile, originalTile, improvedTile);
	tileTerraformingInfo.restoreEffectiveMapTile();

	// collect actions

	std::set<FormerItem> actions;
	actions.insert(action);

	// remove fungus if needed

	if (tile->is_fungus() && !has_tech(Rules->tech_preq_build_road_fungus, aiFactionId))
	{
		actions.insert(FORMER_REMOVE_FUNGUS);
	}

	insertActionTerraformingRequests(tile, actions, gain);

}

/**
Generate request for sensor.
*/
void generateSensorTerraformingRequest(MAP *tile)
{
	FormerItem action = FORMER_SENSOR;

	// available

	if (!isTerraformingAvailable(tile, action))
		return;

	// generate terraforming changes

	double gain = getSensorGain(tile);

	// collect actions

	std::set<FormerItem> actions;
	actions.insert(action);

	// remove fungus if needed

	if (tile->is_fungus() && !has_tech(Rules->tech_preq_build_road_fungus, aiFactionId))
	{
		actions.insert(FORMER_REMOVE_FUNGUS);
	}

	insertActionTerraformingRequests(tile, actions, gain);

}

/**
Generate request for bunker.
*/
void generateBunkerTerraformingRequest(MAP *tile)
{
	FormerItem action = FORMER_BUNKER;

	// available

	if (!isTerraformingAvailable(tile, action))
		return;

	// generate terraforming changes

	double gain = getBunkerGain(tile);

	// collect actions

	std::set<FormerItem> actions;
	actions.insert(action);

	// remove fungus if needed

	if (tile->is_fungus() && !has_tech(Rules->tech_preq_build_road_fungus, aiFactionId))
	{
		actions.insert(FORMER_REMOVE_FUNGUS);
	}

	insertActionTerraformingRequests(tile, actions, gain);

}

/*
Removes terraforming requests violating proximity rules.
*/
void applyProximityRules()
{
	Profiling::start("applyProximityRules", "moveFormerStrategy");

	// apply proximity rules

	for (auto terraformingRequestsIterator = terraformingRequests.begin(); terraformingRequestsIterator != terraformingRequests.end(); )
	{
		TerraformingRequest const &terraformingRequest = *terraformingRequestsIterator;

		if (isProximityRuleSatisfied(terraformingRequest.tile, terraformingRequest.action))
		{
			// rule is satisfied
			// advance iterator
			++terraformingRequestsIterator;
		}
		else
		{
			// rule is broken
			// remove request
			terraformingRequestsIterator = terraformingRequests.erase(terraformingRequestsIterator);
		}

	}

	Profiling::stop("applyProximityRules");

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
		TileTerraformingInfo &rangeTileTerraformingInfo = getTileTerraformingInfo(rangeTile);

		if (map_has_item(&rangeTileTerraformingInfo.originalTile, proximityRule.item))
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
Removes terraforming requests in terraformed tiles.
*/
void removeTerraformedTiles()
{
	Profiling::start("removeTerraformedTiles", "moveFormerStrategy");

	for
	(
		auto terraformingRequestsIterator = terraformingRequests.begin();
		terraformingRequestsIterator != terraformingRequests.end();
	)
	{
		TerraformingRequest &terraformingRequest = *terraformingRequestsIterator;
		TileTerraformingInfo &terraformingRequestTileInfo = getTileTerraformingInfo(terraformingRequest.tile);

		if (terraformingRequestTileInfo.terraformed)
		{
			terraformingRequestsIterator = terraformingRequests.erase(terraformingRequestsIterator);
		}
		else
		{
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
 * 4. update effectiveTile and terraformingItems
 * 5. apply proximity rule to remove similar requests
 */
void assignFormerOrders()
{
	debug("assignFormerOrders - %s\n", MFactions[aiFactionId].noun_faction);

	Profiling::start("assignFormerOrders", "moveFormerStrategy");

	// distribute orders

	aiData.production.terraformingRequests.clear();
	for (TerraformingRequest const &terraformingRequest : terraformingRequests)
	{
		MAP *tile = terraformingRequest.tile;
		FormerItem action = terraformingRequest.action;

		debug("%s %s incomeGain=%5.2f\n", getLocationString(tile), Terraform[action].name, terraformingRequest.incomeGain);

		// verify action complies with proximity rules

		if (!isProximityRuleSatisfied(tile, action))
			continue;

		// find former with the best gainGrowth

		FormerOrder *bestFormerOrder = nullptr;
		double bestFormerOrderGain = 0.0;

		for (FormerOrder &formerOrder : formerOrders)
		{
			int vehicleId = formerOrder.vehicleId;
			VEH *vehicle = getVehicle(vehicleId);
			int triad = vehicle->triad();
			MAP *vehicleTile = getVehicleMapTile(vehicleId);

			// skip assigned

			if (formerOrder.tile != nullptr)
				continue;

			// corresponding triad

			if ((triad == TRIAD_LAND && !tile->is_land()) || (triad == TRIAD_SEA && !tile->is_sea()))
				continue;

			// same cluster

			if ((triad == TRIAD_SEA && !isSameSeaCluster(vehicleTile, tile)) || (triad == TRIAD_LAND && !isSameLandTransportedCluster(vehicleTile, tile)))
				continue;

			// reachable

			if (!isVehicleDestinationReachable(vehicleId, tile))
				continue;

			// travelTime

			double travelTime = getVehicleTravelTime(vehicleId, tile);
			if (travelTime == INF)
				continue;

			// terraformingTime

			int terraformingTime = getTerraformingTime(vehicleId, tile, action);

			// total time

			double totalTime = conf.ai_terraforming_travel_time_multiplier * travelTime + static_cast<double>(terraformingTime);

			// estimate former incomeGain for this improvement

			double gain = getGainIncomeGrowth(terraformingRequest.incomeGain / totalTime);

			// update best

			bool best = false;
			if (gain > bestFormerOrderGain)
			{
				bestFormerOrder = &formerOrder;
				bestFormerOrderGain = gain;
				best = true;
			}

			debug("\t[%4d] %s travelTime=%5.2f terraformingTime=%2d gain=%5.2f\n %s", vehicleId, getLocationString({vehicle->x, vehicle->y}), travelTime, terraformingTime, gain, best ? "- best" : "");

		}

		// not found

		if (bestFormerOrder == nullptr)
		{
			// store unprocessed terraformingRequest
			aiData.production.terraformingRequests.push_back(terraformingRequest);
			continue;
		}

		// assign order

		bestFormerOrder->tile = tile;
		bestFormerOrder->action = action;
		terraformingRequest.assigned = true;

		// update tile terraforming

		TileTerraformingInfo &tileTerraformingInfo = getTileTerraformingInfo(tile);
		tileTerraformingInfo.applyTerraforming(action);
		tileTerraformingInfo.terraformingItems.insert(action);

	}

	Profiling::stop("assignFormerOrders");

}

void setFormerTasks()
{
	Profiling::start("setFormerTasks", "moveFormerStrategy");

	debug("setFormerTasks - %s\n", MFactions[aiFactionId].noun_faction);

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
			transitVehicle(Task(formerOrder.vehicleId, TT_TERRAFORM, formerOrder.tile, nullptr, -1, formerOrder.action));
			debug("\t[%4d] %s->%s %2d\n", formerOrder.vehicleId, getLocationString(getVehicleMapTile(formerOrder.vehicleId)), getLocationString(formerOrder.tile), formerOrder.action);
		}

	}

	Profiling::stop("setFormerTasks");

}

/**
Selects best terraforming option around given base and calculates its terraforming score.
*/
double calculateConventionalTerraformingScore(MAP *tile, MAP const &originalTile, MAP const &improvedTile)
{
	assert(isOnMap(tile));
	
	TileTerraformingInfo &tileTerraformingInfo = getTileTerraformingInfo(tile);
	
	// collect affected bases
	
	std::set<int> affectedBaseIds;

	if (tileTerraformingInfo.worked && tileTerraformingInfo.workedBaseId != -1)
	{
		affectedBaseIds.insert(tileTerraformingInfo.workedBaseId);
	}
	else if (tileTerraformingInfo.workable)
	{
		affectedBaseIds.insert(tileTerraformingInfo.workableBaseIds.begin(), tileTerraformingInfo.workableBaseIds.end());
	}

	double bestGain = 0.0;
	
	for (int baseId : affectedBaseIds)
	{
		double improvementGain = computeBaseTileImprovementGain(baseId, tile, originalTile, improvedTile);
		bestGain = std::max(bestGain, improvementGain);
	}

//	debug
//	(
//		"\t\t\t%-20s %d-%d-%d"
//		" terraformingTime=%5.2f"
//		" improvementIncome=%5.2f"
//		" fitnessScore=%5.2f"
//		" income=%5.2f"
//		" gain=%5.2f"
//		"\n"
//		, option->name
//		, terraformingRequest.yield.nutrient, terraformingRequest.yield.mineral, terraformingRequest.yield.energy
//		, terraformingTime
//		, improvementIncome
//		, fitnessScore
//		, income
//		, gain
//	)
//	;

	return bestGain;

}

/**
Computes base tile improvement surplus effect.
*/

double computeBaseTileImprovementGain(int baseId, MAP *tile, MAP const &originalTile, MAP const &improvedTile)
{
	// current gain



	bool worked = false;

	Profiling::start("- computeBaseTileImprovementGain - computeBase");
	computeBase(baseId, true);
	Profiling::stop("- computeBaseTileImprovementGain - computeBase");

	// old intake

	Resource oldResourceIntake2 = getBaseResourceIntake2(baseId);

	// apply improvement

	*tile = improvedTile;
	Profiling::start("- computeBaseTileImprovementGain - computeBase");
	computeBase(baseId, true);
	Profiling::stop("- computeBaseTileImprovementGain - computeBase");

	// verify square is worked by this base

	worked = isBaseWorkedTile(baseId, tile);

	// new intake

	Resource newResourceIntake2 = getBaseResourceIntake2(baseId);

	// restore map and base

	*tile = originalTile;
	aiData.resetBase(baseId);

	// accumulate improvementGain

	double improvementGain = getBaseImprovementGain(baseId, oldResourceIntake2, newResourceIntake2);

	// discard not worked tile

	if (!worked)
		return 0.0;

	// return improvement income

	return improvementGain;

}

/*
Determines whether terraforming is already completed in this tile.
*/
bool isTerraformingCompleted(MAP const *tile, int action)
{
	return
		// items appeared
		(tile->items & Terraform[action].bit) == Terraform[action].bit
		&&
		// items removed
		(tile->items & Terraform[action].bit_incompatible) == 0
	;
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

// determines whether terraforming can be done in this square with removing fungus and leveling terrain if needed
bool isTerraformingAvailable(MAP *tile, FormerItem action)
{
	assert(isOnMap(tile));

	TileTerraformingInfo const &tileTerraformingInfo = getTileTerraformingInfo(tile);
	bool ocean = is_ocean(tile);
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

	// action is available only when enabled and researched

	if (!has_terra(action, ocean, aiFactionId))
		return false;

	// ocean improvements in deep ocean are available for aquatic faction with Adv. Ecological Engineering only

	if (oceanDeep)
	{
		if (!(aquaticFaction && has_tech(TECH_EcoEng2, aiFactionId)))
			return false;
	}

	// building improvement in fungus requires either fungus removal or ability to build in fungus

	if (tile->is_fungus() && action != FORMER_REMOVE_FUNGUS && !has_terra(FORMER_REMOVE_FUNGUS, ocean, aiFactionId) && !Rules->tech_preq_improv_fungus)
		return false;

	// certain improvements require flatter terrain

	if (tile->is_land_rocky() && !has_terra(FORMER_LEVEL_TERRAIN, false, aiFactionId))
	{
		// improvements requiring flatter terrain cannot be built on rocky terrain
		switch (action)
		{
		case FORMER_FARM:
		case FORMER_SOIL_ENR:
		case FORMER_FOREST:
			return false;
		default: ;
		}
	}

	// do not allow terraforming incompatible with ongoing terraforming

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

// teraforming destroys improvements except fungus
bool isTerraformingDestructive(MAP *tile, FormerItem action)
{
	return (tile->items & Terraform[action].bit_incompatible & ~BIT_FUNGUS) != 0;
}

/*
Determines whether fungus need to be removed before terraforming.
*/
bool isRemoveFungusRequired(int action)
{
	// no need to remove fungus for planting fungus

	if (action == FORMER_PLANT_FUNGUS)
		return false;

	// always remove fungus for basic improvements even with fungus improvement technology

	if (action >= FORMER_FARM && action <= FORMER_SOLAR)
		return true;

	// no need to remove fungus for road with fungus road technology

	if (action == FORMER_ROAD && has_tech(Rules->tech_preq_build_road_fungus, aiFactionId))
		return false;

	// for everything else remove fungus only without fungus improvement technology

	return !has_tech(Rules->tech_preq_improv_fungus, aiFactionId);

}

/*
Determines whether rocky terrain need to be leveled before terraforming.
*/
bool isLevelTerrainRequired(bool ocean, int action)
{
	return !ocean && (action == FORMER_FARM || action == FORMER_SOIL_ENR || action == FORMER_FOREST);

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

/**
Calculates combined weighted resource score taking base additional demand into account.
*/
double calculateBaseResourceScore(int baseId, int currentMineralIntake2, int currentNutrientSurplus, int currentMineralSurplus, int currentEnergySurplus, int improvedNutrientSurplus, int improvedMineralSurplus, int improvedEnergySurplus)
{
	BASE *base = &(Bases[baseId]);

	// improvedNutrientSurplus <= 0 is unacceptable

	if (improvedNutrientSurplus <= 0)
		return 0.0;

	// calculate nutrient and mineral extra score

	double nutrientCostMultiplier = 1.0;
	double mineralCostMultiplier = 1.0;
	double energyCostMultiplier = 1.0;

	// multipliers are for bases size 2 and above

	if (base->pop_size >= 2)
	{
		// calculate nutrient cost multiplier

		double nutrientThreshold = conf.ai_terraforming_baseNutrientThresholdRatio * static_cast<double>(currentNutrientSurplus);

		if (currentNutrientSurplus < nutrientThreshold)
		{
			double proportion = 1.0 - static_cast<double>(currentNutrientSurplus) / nutrientThreshold;
			nutrientCostMultiplier += (conf.ai_terraforming_baseNutrientCostMultiplier - 1.0) * proportion;
		}

		// calculate mineral cost multiplier

		double mineralThreshold = conf.ai_terraforming_baseMineralThresholdRatio * static_cast<double>(currentNutrientSurplus);

		if (currentMineralIntake2 < mineralThreshold)
		{
			double proportion = 1.0 - static_cast<double>(currentMineralIntake2) / mineralThreshold;
			mineralCostMultiplier += (conf.ai_terraforming_baseMineralCostMultiplier - 1.0) * proportion;
		}

		// calculate energy cost multiplier

		if (aiData.grossIncome > 0 && aiData.netIncome > 0)
		{
			double maxNetIncome = 0.5 * static_cast<double>(aiData.grossIncome);
			double minNetIncome = 0.1 * maxNetIncome;
			energyCostMultiplier = maxNetIncome / std::min(maxNetIncome, std::max(minNetIncome, static_cast<double>(aiData.netIncome)));
		}

	}

	// compute final score

	return
		getTerraformingResourceScore
		(
			nutrientCostMultiplier * (improvedNutrientSurplus - currentNutrientSurplus),
			mineralCostMultiplier * (improvedMineralSurplus - currentMineralSurplus),
			energyCostMultiplier * (improvedEnergySurplus - currentEnergySurplus)
		)
	;

}

/*
Applies improvement and computes its maximal yield score for this base.
*/
double computeBaseImprovementYieldScore(int baseId, MAP *tile, MAP *currentMapState, MAP *improvedMapState)
{
	assert(isOnMap(tile));

	BASE *base = &(Bases[baseId]);
	int x = getX(tile);
	int y = getY(tile);

	// apply improved state

	*tile = *improvedMapState;

	// record yield

	int nutrient = mod_crop_yield(base->faction_id, baseId, x, y, 0);
	int mineral = mod_mine_yield(base->faction_id, baseId, x, y, 0);
	int energy = mod_energy_yield(base->faction_id, baseId, x, y, 0);

	// restore original state

	*tile = *currentMapState;

	// compute yield score

	return getTerraformingResourceScore(nutrient, mineral, energy);

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

double getTerraformingResourceScore(ResourceYield const &yield)
{
	return getTerraformingResourceScore(yield.nutrient, yield.mineral, yield.energy);
}

double getTerraformingGain(double income, double terraformingTime)
{
	return getGainDelay(getGainIncome(income), terraformingTime);
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

	int terraformingTime = action_terraform(vehicleId, action, 0);

	// restore vehicle coordinates

	vehicle.x = static_cast<int16_t>(vehicleX);
	vehicle.y = static_cast<int16_t>(vehicleY);

	// return terraforming time

	return terraformingTime;

}

void insertActionTerraformingRequests(MAP *tile, std::set<FormerItem> const &actions, double gain)
{
	// positive gain

	if (gain <= 0.0)
		return;

	// gather total terraforming rate

	int totalTerraformingRate = 0;

	for (FormerItem action : actions)
	{
		totalTerraformingRate += Terraform[action].rate;
	}

	if (totalTerraformingRate == 0)
	{
		debug("ERROR: no terraforming rate for actions");
		return;
	}

	// generate requests with proportional gain

	for (FormerItem action : actions)
	{
		// do not insert improvement if we cannot build it in fungus

		if (tile->is_fungus() && action != FORMER_REMOVE_FUNGUS && isRemoveFungusRequired(action))
			continue;

		// do not insert improvement if we cannot build it in land rocky

		if (tile->is_land_rocky() && action != FORMER_LEVEL_TERRAIN && isLevelTerrainRequired(tile->is_sea(), action))
			continue;

		double actionGain = gain * static_cast<double>(Terraform[action].rate) / static_cast<double>(totalTerraformingRate);
		terraformingRequests.emplace_back(tile, action, actionGain);

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

	if (!isTerraformingAvailable(tile, action))
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

		if (getRange(x, y, base.x, base.y) > 5)
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
			double travelTimeImprovement = originalTravelTimes.at(orgBaseId).at(dstBaseId) - improvedTravelTimes.at(orgBaseId).at(dstBaseId);

			if (travelTimeImprovement > 0.0)
			{
				totalTravelTimeImprovement += travelTimeImprovement;
			}

		}

	}

	// halve travel time improvement to account for bidirectional travel

	totalTravelTimeImprovement /= 2.0;

	// compute network gain

	double resourceScore = getResourceScore(conf.ai_terraforming_networkValueIncomeImprovement, 0.0);
	double improvementIncomeGrowh = resourceScore * totalTravelTimeImprovement / conf.ai_terraforming_networkValueTravelTimeDenominator;
	double networkGain = getGainIncomeGrowth(improvementIncomeGrowh);

	return networkGain;

}

robin_hood::unordered_flat_map<int, robin_hood::unordered_flat_map<int, double>> getCrossBaseTravelTimes(std::set<int> const& baseIds)
{
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

					// within 5 tiles

					if (getRange(orgBaseTile, adjacentTile) > 5)
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

