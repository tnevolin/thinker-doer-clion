#pragma once

#include "robin_hood.h"

#include "main.h"
#include "engine.h"

const int GAME_DURATION = 350;
const double MAX_THREAT_TURNS = 10.0;
const double ARTILLERY_OFFENSIVE_VALUE_KOEFFICIENT = 0.5;
const int LAST_TURN = 400;
const int BASE_FUTURE_SPAN = 100;
// psych 50% multiplier bonus to base resource growth (more workers)
const double PSYCH_MULTIPLIER_RESOURCE_GROWTH_INCREASE = 0.25;
double const MIN_COMBAT_DEMAND_SUPERIORITY = 2.0;
double const MAX_COMBAT_DEMAND_SUPERIORITY = 3.0;
int const TARGET_ENEMY_BASE_RANGE = 5;

struct BaseProductionInfo
{
	double baseGain;
	std::array<double, 2> extraPoliceGains;
};

struct ProductionDemand
{
	int baseId;
	BASE *base;
	int baseSeaCluster;
	
	int item;
	double priority;
	robin_hood::unordered_flat_map<int, double> itemPriorities;
	
	double baseGain;
	double baseCitizenGain;
	double baseWorkerGain;
	
	void initialize(int baseId);
	void addItemPriority(int item, double priority);
	
};

struct PRODUCTION_CHOICE
{
	int item;
	bool urgent;
};

struct BaseMetric
{
	int nutrient = 0;
	int mineral = 0;
	int economy = 0;
	int psych = 0;
	int labs = 0;
};

struct Interval
{
	int min = 0;
	int max = 0;
	Interval(int _min, int _max): min{_min}, max{_max} {}
};

struct SurfacePodData
{
	int scanRange = 0;
	int tileCount = 0;
	int podCount = 0;
	int scoutCount = 0;
	double averagePodDistance = 0.0;
	double totalConsumptionRate = 0.0;
	double factionConsumptionRate = 0.0;
	double factionConsumptionGain = 0.0;
	
};

// production strategy

void productionStrategy();
void populateFactionProductionData();
void evaluateGlobalColonyDemand();
void evaluateGlobalSeaTransportDemand();
void initializeProductionDemands();
void suggestGlobalProduction();
void suggestBaseProductions();
void applyBaseProductions();
bool isManaged(int item);
bool isManagedUnit(int unitId);

// global item selection

void evaluateHeadquarters();
void evaluateProject();

// base item selection

void suggestBaseProduction();

void evaluateFacilities();
void evaluatePressureDome();
void evaluateStockpileEnergy();
void evaluatePsychFacilitiesRemoval();
void evaluatePsychFacilities();
void evaluateRecyclingTanks();
void evaluateIncomeFacilities();
void evaluateMineralMultiplyingFacilities();
void evaluatePopulationLimitFacilities();
void evaluateMilitaryFacilities();
void evaluatePrototypingFacilities();

void evaluateDefensiveProbeUnits();
void evaluateExpansionUnits();
void evaluateTerraformUnits();
void evaluateConvoyUnits();
void evaluatePodPoppingUnits();
void evaluateBaseDefenseUnits();
void evaluateBunkerDefenseUnits();
void evaluateTerritoryProtectionUnits();
void evaluateEnemyBaseAssaultUnits();
void evaluateSeaTransport();
int findScoutUnit(int triad);
bool canBaseProduceColony(int baseId);
std::vector<int> getRegionBases(int factionId, int region);
int getRegionBasesMaxMineralSurplus(int factionId, int region);
int getRegionBasesMaxPopulationSize(int factionId, int region);
int calculateUnitTypeCount(int baseId, int weaponType, int triad, int excludedBaseId);
bool isMilitaryItem(int item);
bool isBaseCanBuildUnit(int baseId, int unitId);
bool isBaseCanBuildFacility(int baseId, FacilityId facilityId);
int getFirstAvailableFacility(int baseId, std::vector<FacilityId> facilityIds);
double getUnitPriorityCoefficient(int baseId, int unitId);
int findInfantryPoliceUnit(bool first);
void hurryProtectiveUnit();

//=======================================================
// estimated income functions
//=======================================================

double getFacilityGain(int baseId, int facilityId, bool build);
double getDevelopmentScore(int turn);
double getDevelopmentScoreGrowth(int turn);
double getDevelopmentScale();
double getRawProductionPriority(double gain, double cost);
double getProductionPriority(double gain, double cost);
double getProductionConstantIncomeGain(double score, double buildTime, double bonusDelay);
double getProductionLinearIncomeGain(double score, double buildTime, double bonusDelay);
double getMoraleProportionalMineralBonus(std::array<int,4> levels);
BaseMetric getBaseMetric(int baseId);
double getDemand(double required, double provided);

//=======================================================
// statistical estimates
//=======================================================

double getFactionStatisticalMineralIntake2(double turn);
double getFactionStatisticalMineralIntake2Growth(double turn);
double getFactionStatisticalBudgetIntake2(double turn);
double getFactionStatisticalBudgetIntake2Growth(double turn);
double getBaseStatisticalSize(double age);
double getBaseStatisticalMineralIntake(double age);
double getBaseStatisticalMineralIntake2(double age);
double getBaseStatisticalMineralMultiplier(double age);
double getBaseStatisticalBudgetIntake(double age);
double getBaseStatisticalBudgetIntake2(double age);
double getBaseStatisticalBudgetMultiplier(double age);
double getFlatMineralIntakeGain(int delay, double value);
double getFlatBudgetIntakeGain(int delay, double value);
double getBasePopulationGain(int baseId,  Interval &baseSizeInterval);
double getBasePopulationGrowthGain(int baseId, int delay, double proportionalGrowthIncrease);
double getBaseProportionalMineralIntakeGain(int baseId, int delay, double proportion);
double getBaseProportionalBudgetIntakeGain(int baseId, int delay, double proportion);
double getBaseProportionalMineralIntake2Gain(int baseId, int delay, double proportion);
double getBaseProportionalBudgetIntake2Gain(int baseId, int delay, double proportion);

//=======================================================
// production item helper functions
//=======================================================

int getBasePoliceExtraCapacity(int baseId);
double getItemPriority(int item, double gain);
double getBaseColonyUnitGain(int baseId, int unitId, double travelTime, double buildSiteScore);
double getBasePoliceGain(int baseId, bool police2x);
double getTechStealGain();

