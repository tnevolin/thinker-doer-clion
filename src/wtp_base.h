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
	std::array<ResourceYield, 21> workTileYields{};
	int mineralMultiplierDenominator{};
	int economyMultiplierDenominator{};
	int psychMultiplierDenominator{};
	int labsMultiplierDenominator{};
	double mineralValue{};
	double energyValue{};
	double economyValue{};
	double psychValue{};
	double labsValue{};
};

struct BaseEnergy
{
	int our_rank;
	std::array<int, MaxPlayerNum> otherFaciontRanks;
	int coeff_psych;
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

struct BaseScore
{
	CitizenAllocation citizenAllocation{};
	int condition{};
	ResourceYield surplus{};
	double gain{};
};

void __cdecl wtp_mod_base_yield();
void mod_base_yield_base_compute(BaseEnergy const &baseEnergy, int energyIntake);
std::array<ResourceYield, 21> getBaseWorkTileYields(int baseId);
int32_t getBaseAvailableWorkTiles(int baseId);
void updateBase(BaseComputeParameterSet const &baseComputeParameterSet, bool compute);
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
int getBaseCondition();
ResourceYield getBaseSurplus();
double getBaseSurplusGain();

