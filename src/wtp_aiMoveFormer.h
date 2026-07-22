#pragma once

#include <vector>
#include "robin_hood.h"

#include "engine.h"
#include "wtp_ai_game.h"

constexpr int NETWORK_BASE_RANGE = 5;

constexpr int PRESERVED_LAND_ROCKY_TILE_COUNT = 4;
constexpr int BUNKER_ENEMY_BASE_RANGE_MIN = 2;
constexpr int BUNKER_ENEMY_BASE_RANGE_MAX = 5;
constexpr int BUNKER_PLAYER_BASE_RANGE_MIN = 2;
constexpr int BUNKER_PLAYER_BASE_RANGE_MAX = 3;

constexpr double BUNKER_PLACEMENT_COEFFICIENTS[BUNKER_PLAYER_BASE_RANGE_MAX + 1][BUNKER_PLAYER_BASE_RANGE_MAX + 1] =
	{
		{0.00, 0.00, 0.00, 0.00, },
		{0.00, 0.00, 0.00, 0.00, },
		{0.80, 0.80, 0.50, 0.00, },
		{1.00, 1.00, 0.40, 0.25, },
	}
;

struct FactionTerraformingInfo
{
	double averageNormalTerraformingRateMultiplier{};
	double averagePlantFungusTerraformingRateMultiplier{};
	double averageRemoveFungusTerraformingRateMultiplier{};

	double bareLandScore{};
	double bareMineScore{};
	double bareSolarScore{};

	FactionTerraformingInfo() = default;

};

/*
Tile potentially can be terraformed.
*/
struct TileTerraformingInfo
{
	MAP *tile;
	bool landRocky;

	// terraformable tile
	bool availableTerraformingSite = false;
	// terraformable tile for base yield
	bool availableBaseTerraformingSite = false;
	// base works this tile
	bool worked = false;
	// baseId that works this tile
	int workedBaseId = -1;
	// base can work this tile
	bool workable = false;
	// baseIds those can work this tile
	std::vector<int> workableBaseIds;
	// baseIds those are affected by area improvement at this tile (river, condenser, echelon mirror)
	std::vector<int> areaWorkableBaseIds;
	// minimal land rocky tile count
	int landRockyTileCount;

	bool harvested = false;
	bool terraformed = false;
	bool terraformedConventional = false;

	// current terraforming
	robin_hood::unordered_flat_set<FormerItem> terraformingItems;

	// converts former action(s) into map state; operates on whatever MAP is passed - the live tile or a local value
	static void applyTerraforming(MAP *state, FormerItem action);
	static void applyTerraforming(MAP *state, robin_hood::unordered_flat_set<FormerItem> const &actions);

};

struct WorkerGain
{
	double gain;
	MAP *tile;
};
struct BaseTerraformingInfo
{
	std::vector<MAP *> terraformingSites;
	int landRockyTileCount;
	int popSzie;
	int nutrientCost;
	double income;
	double mineralValue;
	double energyValue;
	double economyValue;
	double labsValue;
	std::vector<WorkerGain> workerGains;
	std::vector<ResourceYield> unworkedTileYields;

	double getIntakeGain(ResourceYield const& yield, int economy, int labs) const;

};

// terraforming options

TERRAFORMING_OPTION const TO_ROCKY_MINE			{"rocky mine"			, false, true , false, true , FORMER_MINE			, {FORMER_MINE}};
TERRAFORMING_OPTION const TO_MINE				{"mine"				, false, false, false, true , FORMER_MINE			, {FORMER_FARM, FORMER_SOIL_ENR, FORMER_MINE}};
TERRAFORMING_OPTION const TO_SOLAR_COLLECTOR	{"solar collector"	, false, false, false, true , FORMER_SOLAR			, {FORMER_FARM, FORMER_SOIL_ENR, FORMER_SOLAR}};
TERRAFORMING_OPTION const TO_CONDENSER			{"condenser"			, false, false, true , true , FORMER_CONDENSER		, {FORMER_FARM, FORMER_SOIL_ENR, FORMER_CONDENSER}};
TERRAFORMING_OPTION const TO_ECHELON_MIRROR		{"echelon mirror"		, false, false, true , true , FORMER_ECH_MIRROR		, {FORMER_FARM, FORMER_SOIL_ENR, FORMER_ECH_MIRROR}};
TERRAFORMING_OPTION const TO_THERMAL_BOREHOLE	{"thermal borehole"	, false, false, false, true , FORMER_THERMAL_BORE	, {FORMER_THERMAL_BORE}};
TERRAFORMING_OPTION const TO_FOREST				{"forest"				, false, false, false, true , FORMER_FOREST			, {FORMER_FOREST}};
TERRAFORMING_OPTION const TO_LAND_FUNGUS		{"land fungus"		, false, false, false, true , FORMER_PLANT_FUNGUS	, {FORMER_PLANT_FUNGUS}};
TERRAFORMING_OPTION const TO_MINING_PLATFORM	{"mining platform"	, true , false, false, true , FORMER_MINE			, {FORMER_MINE}};
TERRAFORMING_OPTION const TO_TIDAL_HARNESS		{"tidal harness"		, true , false, false, true , FORMER_SOLAR			, {FORMER_SOLAR}};
TERRAFORMING_OPTION const TO_SEA_FUNGUS			{"sea fungus"			, true , false, false, true , FORMER_PLANT_FUNGUS	, {FORMER_PLANT_FUNGUS}};
TERRAFORMING_OPTION const TO_AQUIFER			{"aquifer"			, false, false, true , true , FORMER_AQUIFER			, {FORMER_AQUIFER}};
TERRAFORMING_OPTION const TO_RAISE_LAND			{"raise land"			, false, false, true , true , FORMER_RAISE_LAND		, {FORMER_RAISE_LAND}};
TERRAFORMING_OPTION const TO_NETWORK			{"road/tube"			, false, false, false, false, FORMER_ROAD			, {FORMER_ROAD, FORMER_MAGTUBE}};
TERRAFORMING_OPTION const TO_LAND_SENSOR		{"sensor (land)"		, false, false, false, false, FORMER_SENSOR			, {FORMER_SENSOR}};
TERRAFORMING_OPTION const TO_SEA_SENSOR			{"sensor (sea)"		, true , false, false, false, FORMER_SENSOR			, {FORMER_SENSOR}};
TERRAFORMING_OPTION const TO_LAND_BUNKER		{"bunker (land)"		, false, false, false, false, FORMER_BUNKER			, {FORMER_BUNKER}};

// conventional terraforming options

const std::array<const std::vector<TERRAFORMING_OPTION *>, 2> BASE_TERRAFORMING_OPTIONS
{{
	// land
	{
		const_cast<TERRAFORMING_OPTION*>(&TO_ROCKY_MINE),
		const_cast<TERRAFORMING_OPTION*>(&TO_MINE),
		const_cast<TERRAFORMING_OPTION*>(&TO_SOLAR_COLLECTOR),
		const_cast<TERRAFORMING_OPTION*>(&TO_CONDENSER),
		const_cast<TERRAFORMING_OPTION*>(&TO_ECHELON_MIRROR),
		const_cast<TERRAFORMING_OPTION*>(&TO_THERMAL_BOREHOLE),
		const_cast<TERRAFORMING_OPTION*>(&TO_FOREST),
		const_cast<TERRAFORMING_OPTION*>(&TO_LAND_FUNGUS),
	},
	// sea
	{
		const_cast<TERRAFORMING_OPTION*>(&TO_MINING_PLATFORM),
		const_cast<TERRAFORMING_OPTION*>(&TO_TIDAL_HARNESS),
		const_cast<TERRAFORMING_OPTION*>(&TO_SEA_FUNGUS),
	},
}};

struct TerraformingOptionScore
{
	MAP *tile;
	TERRAFORMING_OPTION const *option;
	robin_hood::unordered_flat_set<FormerItem> actions;
	double incomeGain;
	int terraformingTime;

	TerraformingOptionScore(MAP *_tile, TERRAFORMING_OPTION const *_option, robin_hood::unordered_flat_set<FormerItem> const& _actions, double _incomeGain);

};

// prohibits building improvements too close to existing or building same improvement
struct ProximityRule
{
	MapItem item;
	// cannot build within this range of same existing improvement
	int existingDistance;
	// cannot build within this range of same building improvement
	int buildingDistance;
};
robin_hood::unordered_flat_map<int, ProximityRule> const PROXIMITY_RULES =
{
	{FORMER_CONDENSER		, {BIT_CONDENSER	, 0, 1}},
	{FORMER_ECH_MIRROR	, {BIT_ECH_MIRROR	, 0, 1}},
	{FORMER_THERMAL_BORE	, {BIT_THERMAL_BORE	, 1, 1}},
	{FORMER_AQUIFER		, {BIT_RIVER		, 1, 3}},
	{FORMER_RAISE_LAND	, {BIT_MONOLITH		, 0, 1}}, // use dummy BIT_MONOLITH
	{FORMER_SENSOR		, {BIT_SENSOR		, 2, 2}},
	{FORMER_BUNKER		, {BIT_BUNKER		, 2, 2}},
};

struct FormerOrder
{
	int vehicleId;
	MAP *tile = nullptr;
	int action = -1;

	FormerOrder(int _vehicleId);

};

struct TerraformingRequestAssignment
{
	FormerOrder *formerOrder;
	double travelTime;
};

struct TerraformingImprovement
{
	MAP *tile;
	TERRAFORMING_OPTION const *option;
	int action = -1;
	int nutrient;
	int mineral;
	int energy;
	double score = 0.0;
};

struct TERRAFORMING_SCORE
{
	MAP *tile;
	TERRAFORMING_OPTION const *option;

	int action = -1;
	double score = 0.0;
	double terraformingTime = 0.0;

	TERRAFORMING_SCORE(MAP *_tile, TERRAFORMING_OPTION const *_option)
	: tile{_tile}, option{_option}
	{}

};

// access terraforming data arrays
TileTerraformingInfo &getTileTerraformingInfo(MAP* tile);
BaseTerraformingInfo &getBaseTerraformingInfo(int baseId);

void moveFormerStrategy();
// terraforming data operations
void setupTerraformingData();
void populateTerraformingData();
void cancelRedundantOrders();
void generateTerraformingRequests();
void generateBaseConventionalTerraformingRequests(int baseId);
void generateLandBridgeTerraformingRequests();
void generateAquiferTerraformingRequest(MAP *tile);
void generateRaiseLandTerraformingRequest(MAP *tile);
void generateNetworkTerraformingRequest(MAP *tile);
void generateSensorTerraformingRequest(MAP *tile);
void generateBunkerTerraformingRequest(MAP *tile);
void applyProximityRules();
bool isProximityRuleSatisfied(MAP *tile, FormerItem action);
void removeTerraformedTiles();
void assignFormerOrders();
void setFormerTasks();
double computeWorkerGain(int baseId, ResourceYield const& tileYield);
bool isVehicleTerrafomingOrderCompleted(int vehicleId);
bool isTerraformingAvailable(MAP *tile, FormerItem action, bool immediatelyBuildable);
robin_hood::unordered_flat_set<FormerItem> getTerraformingPrerequisites(MAP *tile, FormerItem action);
bool isTerraformingDestructive(MAP *tile, FormerItem action);
bool isRaiseLandSafe(MAP *tile);
bool isWorkableTile(MAP *tile);
bool isValidConventionalTerraformingSite(MAP *tile);
bool isValidTerraformingSite(MAP *tile);
double getTerraformingResourceScore(double nutrient, double mineral, double energy);
double getTerraformingResourceScore(ResourceYield const &resourceYield);
void removeUnusedBunkers();
int getTerraformingTime(int vehicleId, MAP *tile, FormerItem action);
void insertActionTerraformingRequests(MAP *tile, TERRAFORMING_OPTION const *option, robin_hood::unordered_flat_set<FormerItem> const& actions, double gain);
double getCondenserGain(MAP *tile);
double getEchelonMirrorGain(MAP *tile);
double getAquiferGain(MAP *tile);
double getRaiseLandGain(MAP *tile);
double getNetworkGain(MAP *tile, MAP const &originalTile, MAP const &improvedTile);
robin_hood::unordered_flat_map<int, robin_hood::unordered_flat_map<int, double>> getCrossBaseTravelTimes(std::set<int> const& baseIds);
double getSensorGain(MAP *tile);
double getBunkerGain(MAP *tile);
double getBunkerPlacementCoefficient(MAP *bunkerTile, MAP *baseTile);
bool isCompatibleTerraforming(FormerItem ongoingAction, FormerItem action);
robin_hood::unordered_flat_set<FormerItem> addPrerequisites(MAP *tile, robin_hood::unordered_flat_set<FormerItem> &actions);

