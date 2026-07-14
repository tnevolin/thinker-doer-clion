#pragma once

#include "main.h"
#include "wtp_mod.h"
#include "robin_hood.h"
#include "wtp_game.h"

extern MAP *baseComputeExcludedTile;

struct BaseComputeParameterSet
{
	bool canGrow{};
	bool canRiot{};
	ResourceYield satelliteYield;
	int32_t availableWorkTiles{};
	std::array<ResourceYield, 21> workTileResourceYields{};
	std::array<int32_t, 21> removeReplacementWorkTiles{};
	std::array<int32_t, 21> addReplacementWorkTiles{};
	int mineralMultiplierNumerator{};
	int economyMultiplierNumerator{};
	int psychMultiplierNumerator{};
	int labsMultiplierNumerator{};
	double mineralValue{};
	double energyValue{};
	double economyValue{};
	double psychValue{};
	double labsValue{};
	int maxDistance{};
	int hqDistance{};
	int efficiencyRating{};
	int fixedTalentTotal{};
	int fixedDroneTotal{};
	int fixedSuperdroneTotal{};
};

struct WorkTile
{
	int number;
	int x;
	int y;
	MAP *tile;
	int nutrient;
	int mineral;
	int energy;
};

struct FarmerAllocation
{
	int workerTileIndex;
	int x;
	int y;
	MAP *tile;
	int nutrient;
	int mineral;
	int energy;
	int economy;
	int psych;
	int labs;
};

struct CitizenAllocation
{
	int32_t worked_tiles;
	int32_t specialist_total;
	int32_t specialist_types[2];
};

struct BaseConditions
{
	bool nutrientShortfall;
	bool mineralShortfall;
	bool rioting;
};

void __cdecl wtp_mod_base_yield();
void populateBaseAvailableWorkTiles(BaseComputeParameterSet &baseComputeParameterSet);
void populateBaseWorkTileYields(BaseComputeParameterSet &baseComputeParameterSet);
void populateAddReplacementWorkTiles(BaseComputeParameterSet &baseComputeParameterSet);
void populateRemoveReplacementWorkTiles(BaseComputeParameterSet &base_compute_parameters);
void updateBase(BaseComputeParameterSet const& parameterSet, bool compute);
void wtp_normalize_happiness(BASE *base, bool subtractSpecialists = false);
void wtp_add_psych_row(BASE *base, int num);
void __cdecl wtp_mod_base_psych(int base_id);
std::vector<int> getAvailableSpecialistTypes(int factionId, int basePopSize);
int getBestSpecialistType(std::vector<int> const &availableSpecialists, double econValue, double labsValue, double psychValue);
bool wtp_base_pop_boom(int base_id);
int wtp_flat_hurry_cost(int base_id, int item_id, int hurry_mins);
int wtp_terraform_eco_damage(int base_id);
int findReplacementSpecialist(int factionId, int specialistId);
void clear_stack(int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int);
int getBasePsychCoefficient(int baseId);
double getBaseTileScore(ResourceYield const &tileYield, BaseComputeParameterSet const &baseComputeParameterSet);
int wtp_mod_energy_intake_lost(int base_id, int energy, int32_t* effic_energy_lost);
int getEnergyLost(int energy, int hqDistance, int maxDistance, int efficiencyRating);
void storeCitizenAllocation(CitizenAllocation &citizenAllocation);
void applyCitizenAllocation(CitizenAllocation &citizenAllocation);
int isBetterBase(BASE const &newBase, BASE const &oldBase);
ResourceYield getBaseSurplus();
double getBaseSurplusGain();
double getBaseSurplusGain(BASE const &base);
std::array<int, MaxSpecialistNum> getSpecialistTypeCounts(int bestSpecialistType);
void updateBaseNutrient();
void updateBaseMineral(BaseComputeParameterSet const &parameterSet);
void updateBaseEnergy(BaseComputeParameterSet const &parameterSet);
void updateBasePsych(BaseComputeParameterSet const &parameterSet);
int getInefficiencyFormulaMaxDistance();
int getInefficiencyFormulaHQDistance(int base_id);
int getInefficiencyFormulaEfficiencyRating(int base_id);
void populateBaseFixedPsychBalance(BaseComputeParameterSet &parameterSet);
char *getBaseAllocationString();
BaseConditions getBaseConditions();

