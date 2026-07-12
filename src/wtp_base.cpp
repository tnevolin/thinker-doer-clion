
#include "wtp_base.h"

#include "wtp_game.h"

// [WTP]
// temporarily disabled worker locations
// that is used to emulate convoyed locations
MAP *baseComputeExcludedTile = nullptr;

/*
Optimizes mod_base_yield.
*/
void __cdecl wtp_mod_base_yield()
{
	Profiling::start("~ wtp_mod_base_yield");
	
	BASE* base = *CurrentBase;
	int base_id = *CurrentBaseID;
	int faction_id = static_cast<unsigned char>(base->faction_id);
	Faction* faction = &Factions[base->faction_id];
	bool can_grow = base_unused_space(base_id) > 0;
	bool can_riot = base_can_riot(base_id, true);
	
	// static computation parameters

	BaseComputeParameterSet baseComputeParameterSet{};
	baseComputeParameterSet.canGrow = can_grow;
	baseComputeParameterSet.canRiot = can_riot;
	
	// satellite bonuses
	
	satellite_bonus(base_id, &baseComputeParameterSet.satelliteYield.nutrient, &baseComputeParameterSet.satelliteYield.mineral, &baseComputeParameterSet.satelliteYield.energy);

	// base energy parameters
	
	BaseEnergy baseEnergy{};
    baseEnergy.our_rank = own_base_rank(base_id);
	for (int otherFactionId = 1; otherFactionId < MaxPlayerNum; otherFactionId++)
	{
		if (otherFactionId == faction_id)
			continue;
		if (is_alien(otherFactionId) || Factions[otherFactionId].base_count == 0 || Factions[otherFactionId].sanction_turns > 0 || !has_treaty(faction_id, otherFactionId, DIPLO_TREATY))
			continue;
		baseEnergy.otherFaciontRanks.at(otherFactionId) = mod_base_rank(otherFactionId, baseEnergy.our_rank);
	}
	baseEnergy.coeff_psych = getBasePsychCoefficient(base_id);
	
	// reallocate existing workers if managed or requested
	
	uint32_t gov = base->gov_config();
	// manage workers when requested by player click on base tile (all worked tiles are cleared) or governor is active and manages citizens
	// exclude scenario editor mode when player can manually place workers in any base
	bool is_visible = base_id == *BaseUpkeepDrawID && Win_is_visible(BaseWin);
    bool manage_workers =
		// base tile click (all worker tiles are cleared)
		base->worked_tiles == 0
		||
		(
			// governor is active
			(gov & GOV_ACTIVE)
			&&
			// governor manages citizens
			(gov & GOV_MANAGE_CITIZENS)
			&&
			// NOT in scenario editor mode
			!(is_visible && (*GameState & STATE_SCENARIO_EDITOR) && (*GameState & STATE_OMNISCIENT_VIEW))
		);
//    // modified to not override user choice with open window
//    bool pre_upkeep = *BaseUpkeepState != 2 || (base_id == *BaseUpkeepDrawID && Win_is_visible(BaseWin));
//    bool pre_upkeep = *BaseUpkeepState == 1;
	
	// energy conversion and scoring constants
	
	int SE_effic = base->SE_effic(SE_Pending);
	int alloc_econ = 10 - faction->SE_alloc_labs - faction->SE_alloc_psych;
	int alloc_labs = faction->SE_alloc_labs;
	int allocationPenalty = clamp(4 - SE_effic, 0, 8) * (2 * (abs(alloc_econ - alloc_labs) / 2));

	double const mineralValue = getBaseMineralMultiplier(base_id);
	double const efficiency = static_cast<double>(16 - wtp_mod_energy_intake_lost(base_id, 16, nullptr)) / 16.0;
	double const econValue = allocationPenalty == 0 ? 1.0 : 1.0 - static_cast<double>((alloc_econ > alloc_labs ? 1 : 2) * allocationPenalty) / 100.0;
	double const labsValue = allocationPenalty == 0 ? 1.0 : 1.0 - static_cast<double>((alloc_labs > alloc_econ ? 1 : 2) * allocationPenalty) / 100.0 / (has_facility(FAC_PUNISHMENT_SPHERE, base_id) ? 2.0 : 1.0);
	constexpr double psychValue = 1.0;
	double const energyValue = efficiency * (econValue * static_cast<double>(alloc_econ) / 10.0 + labsValue * static_cast<double>(alloc_labs) / 10.0);

	baseComputeParameterSet.mineralValue = mineralValue;
	baseComputeParameterSet.energyValue = energyValue;
	baseComputeParameterSet.economyValue = econValue;
	baseComputeParameterSet.psychValue = psychValue;
	baseComputeParameterSet.labsValue = labsValue;

	// specialists

	int bestSpecialistType = mod_best_specialist();
	std::vector<int> availableSpecialistTypes = getAvailableSpecialistTypes(faction_id, base->pop_size);
	int psychSpecialistType = getBestSpecialistType(availableSpecialistTypes, 0, 0, psychValue);
	int adavancedSpecialistType = getBestSpecialistType(availableSpecialistTypes, econValue, labsValue, 0);
	
	// replace incorrect specialists
	
	for (int i = 0; i < base->specialist_total; i++)
	{
		int spc_id = base->specialist_type(i);
		CCitizen &citizen = Citizen[spc_id];
		
		// update obsolete
		
		if (has_tech(citizen.obsol_tech, faction_id))
		{
			base->specialist_modify(i, findReplacementSpecialist(faction_id, spc_id));
		}
		
		// not uncovered or not allowed
		
		if (!has_tech(citizen.preq_tech, faction_id) || (citizen.psych_bonus == 0 && base->pop_size < Rules->min_base_size_specialists))
		{
			base->specialist_modify(i, psychSpecialistType);
		}
		
	}
	
	// base state
	
	base->state_flags &= ~static_cast<int32_t>(BSTATE_UNK_8000);

	// precompute work tile yields

	baseComputeParameterSet.workTileYields = getBaseWorkTileYields(base_id);

	// available work tiles
	
	int32_t availableWorkTtileBits = getBaseAvailableWorkTiles(base_id);
	
	// remove workers from unavailable locations
	// always allocate base square (it is farmed but not by worker)

	base->worked_tiles &= availableWorkTtileBits | 1;
	
	// get current allocations

	int fixed_farmer_total = __builtin_popcount(base->worked_tiles & ~1);
	int fixed_specialist_total = base->specialist_total;

	// reset workers if required or incorrect

	if (manage_workers || fixed_farmer_total < 0 || fixed_farmer_total > base->pop_size || fixed_specialist_total < 0 || fixed_specialist_total > base->pop_size || fixed_farmer_total + fixed_specialist_total > base->pop_size)
	{
		base->worked_tiles = 1;
		base->specialist_total = 0;
		base->specialist_adjust = 0;
	}

	// preserve fixed allocations

	int unallocatedWorkers = static_cast<unsigned char>(base->pop_size) - __builtin_popcount(base->worked_tiles & ~1) - base->specialist_total;
	int32_t fixedWorkedTileBits = base->worked_tiles;
	std::array<int, MaxSpecialistNum> fixedSpecialistCounts{};
	for (int specialistIndex = 0; specialistIndex < base->specialist_total; ++specialistIndex)
	{
		fixedSpecialistCounts.at(base->specialist_type(specialistIndex))++;
	}

	// reset intake
	// ? maybe not needed
	
	assert(reinterpret_cast<int>(&base->nutrient_intake) + 96 == reinterpret_cast<int>(&base->autoforward_land_base_id));
	memset(&base->nutrient_intake, 0, 96);
	
	if (unallocatedWorkers > 0 || manage_workers)
	{
		// select best tiles for unallocated workers
		
		int workers = unallocatedWorkers;
		while (workers > 0)
		{
			int32_t bestWorkTileBit = 0;
			double bestWorkTileScore = 0.0;
			
			for (int workTileNumber = 1; workTileNumber < 21; workTileNumber++)
			{
				int32_t workTileBit = 1 << workTileNumber;
				ResourceYield const &workTileYield = baseComputeParameterSet.workTileYields.at(workTileNumber);

				// available

				if ((availableWorkTtileBits & workTileBit) == 0)
					continue;

				// not occupied by fixed worker

				if ((fixedWorkedTileBits & workTileBit) != 0)
					continue;

				// not worked
				
				if ((base->worked_tiles & workTileBit) != 0)
					continue;
				
				double score = getBaseTileScore(workTileYield, baseComputeParameterSet);

				if (score > bestWorkTileScore)
				{
					bestWorkTileBit = workTileBit;
					bestWorkTileScore = score;
				}
				
			}
			
			if (bestWorkTileBit == 0)
				break;
			
			base->worked_tiles |= bestWorkTileBit;
			workers--;

		}
			
		// convert unallocated workers to best adavanced specialists
		
		for (; workers > 0; workers--)
		{
			base->specialist_modify(base->specialist_total, adavancedSpecialistType);
			base->specialist_total++;
		}

	}
	
	// fix rioting in base upkeep states 0,1
	// state 0 happens during human interactions
	if (unallocatedWorkers > 0 || manage_workers)
//	if ((unallocatedWorkers != 0 || manage_workers) && *BaseUpkeepState != 2)
	{
		// initial base score variables

		updateBase(baseComputeParameterSet, true);
		int bestBaseCondition = getBaseCondition();
		ResourceYield bestBaseSurplus = getBaseSurplus();
		double bestBaseSurplusGain = getBaseSurplusGain();

		// improvement cycle

		while (true)
		{
			bool changed = false;
			CitizenAllocation bestCitizenAllocation{};

			CitizenAllocation currentCitizenAllocation{};
			storeCitizenAllocation(currentCitizenAllocation);

			// reallocate farmer

			for (int oldWorkTileNumber = 1; oldWorkTileNumber < 21; oldWorkTileNumber++)
			{
				int32_t oldWorkTileBit = 1 << oldWorkTileNumber;
				ResourceYield &oldWorkTileYield = baseComputeParameterSet.workTileYields.at(oldWorkTileNumber);

				// worked

				if ((base->worked_tiles & oldWorkTileBit) == 0)
					continue;

				// search for new location

				for (int newWorkTileNumber = 1; newWorkTileNumber < 21; newWorkTileNumber++)
				{
					int32_t newWorkTileBit = 1 << newWorkTileNumber;
					ResourceYield &newWorkTileYield = baseComputeParameterSet.workTileYields.at(newWorkTileNumber);

					// available

					if ((availableWorkTtileBits & newWorkTileBit) == 0)
						continue;

					// not worked

					if ((base->worked_tiles & newWorkTileBit) != 0)
						continue;

					// not equal or inferior

					if (ResourceYield::isEqualOrInferior(newWorkTileYield, oldWorkTileYield))
						continue;

					// reallocate farmer

					base->worked_tiles &= ~oldWorkTileBit;
					base->worked_tiles |= newWorkTileBit;

					// update base

					updateBase(baseComputeParameterSet, true);

					// update best placement

					int baseCondition = getBaseCondition();
					if (baseCondition >= bestBaseCondition)
					{
						ResourceYield baseSurplus = getBaseSurplus();
						if (!ResourceYield::isEqualOrInferior(baseSurplus, bestBaseSurplus))
						{
							double baseSurplusGain = getBaseSurplusGain();
							if (baseSurplusGain > bestBaseSurplusGain)
							{
								storeCitizenAllocation(bestCitizenAllocation);
								bestBaseCondition = baseCondition;
								bestBaseSurplus = baseSurplus;
								bestBaseSurplusGain = baseSurplusGain;
								changed = true;
							}
						}
					}

					// restore allocaton

					applyCitizenAllocation(currentCitizenAllocation);

				}

				// convert to specialist

				for (int specialistType : availableSpecialistTypes)
				{
					// remove farmer
					// add specialist

					base->worked_tiles &= ~oldWorkTileBit;
					base->specialist_add(specialistType);

					// update base

					updateBase(baseComputeParameterSet, true);

					// update best placement

					int baseCondition = getBaseCondition();
					if (baseCondition >= bestBaseCondition)
					{
						ResourceYield baseSurplus = getBaseSurplus();
						if (!ResourceYield::isEqualOrInferior(baseSurplus, bestBaseSurplus))
						{
							double baseSurplusGain = getBaseSurplusGain();
							if (baseSurplusGain > bestBaseSurplusGain)
							{
								storeCitizenAllocation(bestCitizenAllocation);
								bestBaseCondition = baseCondition;
								bestBaseSurplus = baseSurplus;
								bestBaseSurplusGain = baseSurplusGain;
								changed = true;
							}
						}
					}

					// restore allocaton

					applyCitizenAllocation(currentCitizenAllocation);

				}

			}

			// reallocate specialist

			robin_hood::unordered_set<int> processedSpecialists;

			for (int specialistIndex = 0; specialistIndex < base->specialist_total; specialistIndex++)
			{
				int oldSpecialistType = base->specialist_type(specialistIndex);

				// not yet processed

				if (processedSpecialists.find(oldSpecialistType) != processedSpecialists.end())
					continue;

				processedSpecialists.insert(oldSpecialistType);

				// convert to farmer

				for (int newWorkTileNumber = 1; newWorkTileNumber < 21; newWorkTileNumber++)
				{
					int32_t newWorkTileBit = 1 << newWorkTileNumber;

					// available

					if ((availableWorkTtileBits & newWorkTileBit) == 0)
						continue;

					// not worked

					if ((base->worked_tiles & newWorkTileBit) != 0)
						continue;

					// remove specialist
					// allocate farmer

					base->specialist_remove(specialistIndex, bestSpecialistType);
					base->worked_tiles |= newWorkTileBit;

					// update base

					updateBase(baseComputeParameterSet, true);

					// update best placement

					int baseCondition = getBaseCondition();
					if (baseCondition >= bestBaseCondition)
					{
						ResourceYield baseSurplus = getBaseSurplus();
						if (!ResourceYield::isEqualOrInferior(baseSurplus, bestBaseSurplus))
						{
							double baseSurplusGain = getBaseSurplusGain();
							if (baseSurplusGain > bestBaseSurplusGain)
							{
								storeCitizenAllocation(bestCitizenAllocation);
								bestBaseCondition = baseCondition;
								bestBaseSurplus = baseSurplus;
								bestBaseSurplusGain = baseSurplusGain;
								changed = true;
							}
						}
					}

					// restore allocaton

					applyCitizenAllocation(currentCitizenAllocation);

				}

				// automatic default specialist is not convertable to other type

				if (specialistIndex >= MaxBaseSpecNum)
					continue;

				// convert to other specialist type

				for (int newSpecialistType : availableSpecialistTypes)
				{
					// change specialist type

					base->specialist_modify(specialistIndex, newSpecialistType);

					// update base

					updateBase(baseComputeParameterSet, true);

					// update best placement

					int baseCondition = getBaseCondition();
					if (baseCondition >= bestBaseCondition)
					{
						ResourceYield baseSurplus = getBaseSurplus();
						if (!ResourceYield::isEqualOrInferior(baseSurplus, bestBaseSurplus))
						{
							double baseSurplusGain = getBaseSurplusGain();
							if (baseSurplusGain > bestBaseSurplusGain)
							{
								storeCitizenAllocation(bestCitizenAllocation);
								bestBaseCondition = baseCondition;
								bestBaseSurplus = baseSurplus;
								bestBaseSurplusGain = baseSurplusGain;
								changed = true;
							}
						}
					}

					// restore allocaton

					applyCitizenAllocation(currentCitizenAllocation);

				}

			}

			// exit if not changed

			if (changed)
			{
				applyCitizenAllocation(bestCitizenAllocation);
			}
			else
			{
				break;
			}

		}

		// // fix rioting
		//
		// mod_base_yield_base_compute(baseEnergy, energyIntake);
		// if (can_riot && base->drone_riots() && choiceCount > 0)
		// {
		// 	// compute base
		//
  //           base_update_reset(base, Ns, Ms, Es, workableTiles, workableTtileCount);
  //           int nutrientSurplus = nutrientIntake + nutrientAddition - nutrientConsumption;
  //           int mineralSurplus = (mineralIntake + mineralAddition) * (mineralOutputModifier + 2) / 2 - mineralConsumption;
		// 	mod_base_yield_base_compute(baseEnergy, energyIntake);
		//
		// 	// turn specialists to psych until no drone riot
		//
		// 	for (int specialistNumber = 0; base->drone_riots() && specialistNumber < base->specialist_total; specialistNumber++)
		// 	{
		// 		base->specialist_modify(specialistNumber, psychSpecialistType);
		// 		mod_base_yield_base_compute(baseEnergy, energyIntake);
		// 	}
		//
		// 	// turn workers to psych
		//
		// 	while (base->drone_riots() && choiceCount > 0)
		// 	{
		// 		// find a worker that can be removed while keeping nutrition and support
		//
		// 		int removeChoiceIndex = -1;
		// 		for (int i = choiceCount - 1; i >= 0; i--)
		// 		{
		// 			if (nutrientSurplus - choices.at(i).nutrient >= 0 && mineralSurplus - choices.at(i).mineral >= 0)
		// 			{
		// 				removeChoiceIndex = i;
		// 				break;
		// 			}
		//
		// 		}
		//
		// 		if (removeChoiceIndex == -1)
		// 			break;
		//
		// 		// remove worker and recompute base
		//
		// 		TileValue tileValue = choices.at(removeChoiceIndex);
		// 		std::move(choices.begin() + removeChoiceIndex + 1, choices.end(), choices.begin() + removeChoiceIndex);
		// 		choiceCount--;
  //
		// 		nutrientIntake -= tileValue.nutrient;
		// 		mineralIntake -= tileValue.mineral;
		// 		energyIntake -= tileValue.energy;
		//
		// 		base->worked_tiles &= ~(1 << tileValue.i);
		// 		base->specialist_modify(base->specialist_total++, psychSpecialistType);
		// 		base->specialist_adjust++;
		//
		// 		base_update_reset(base, Ns, Ms, Es, workableTiles, workableTtileCount);
		// 		nutrientSurplus = nutrientIntake + nutrientAddition - nutrientConsumption;
		// 		mineralSurplus = (mineralIntake + mineralAddition) * (mineralOutputModifier + 2) / 2 - mineralConsumption;
		// 		mod_base_yield_base_compute(baseEnergy, energyIntake);
		//
		// 	};
		//
		// }
		//
	}
//	else if (*BaseUpkeepState == 2 && unallocatedWorkers != 0)
//	{
//		// remove excessive choices
//		
//		choiceCount = std::min(choiceCount, (size_t)base->pop_size);
//		
//		// set workers 
//		
//		base->specialist_total = base->pop_size - choiceCount;
//		base->worked_tiles = 1;
//		for (size_t choiceNumber = 0; choiceNumber < choiceCount; choiceNumber++)
//		{
//			TileValue &tileValue = choices.at(choiceNumber);
//			base->worked_tiles |= (1 << tileValue.i);
//		}
//		
//	}
	
	// recompute base

	// enclosing method computes
	updateBase(baseComputeParameterSet, false);

	base->state_flags &= ~static_cast<int32_t>(BSTATE_UNK_100);
	base->eco_damage = terraform_eco_damage(base_id);
	
	Profiling::stop("~ wtp_mod_base_yield");
	
}

/*
Minimal and fast base computation for base_yeild.
*/
void mod_base_yield_base_compute(BaseEnergy const &baseEnergy, int energyIntake)
{
	Profiling::start("~ mod_base_yield_base_energy");
	
	BASE* base = *CurrentBase;
	Faction &faction = Factions[base->faction_id];
	int base_id = *CurrentBaseID;
	int faction_id = static_cast<unsigned char>(base->faction_id);
	int commerce = 0;
	int energygrid = 0;
	
    base->energy_intake = energyIntake;
    base->energy_intake_2 = energyIntake;
    base->energy_intake_2 += BaseResourceConvoyTo[RSC_ENERGY];
    base->energy_consumption = BaseResourceConvoyFrom[RSC_ENERGY];
	
	if (faction.sanction_turns == 0)
	{
		if (is_alien(faction_id))
		{
			energygrid = energy_grid_output(base_id);
		}
		else
		{
			for (int otherFactionId = 1; otherFactionId < MaxPlayerNum; otherFactionId++)
			{
				if (otherFactionId == faction_id)
					continue;
				
				if (is_alien(otherFactionId) || Factions[otherFactionId].base_count == 0 || Factions[otherFactionId].sanction_turns > 0 || !has_treaty(faction_id, otherFactionId, DIPLO_TREATY))
					continue;
				
				int their_rank = baseEnergy.otherFaciontRanks.at(otherFactionId);
				if (their_rank < 0)
					continue;
				
				Faction const &otherFaction = Factions[otherFactionId];
				
				BaseCommerceImport[otherFactionId] = 0;
				BaseCommerceExport[otherFactionId] = 0;
				
				int tech_count = (*TechCommerceCount + 1);
				int base_value = (base->energy_intake + Bases[their_rank].energy_intake + 7) / 8;
				if (global_trade_pact())
				{
					base_value *= 2;
				}
				int commerce_import = (base_value * (faction.tech_commerce_bonus + 1) + tech_count / 2) / tech_count;
				int commerce_export = (base_value * (otherFaction.tech_commerce_bonus + 1) + tech_count / 2) / tech_count;
				if (!has_pact(faction_id, otherFactionId))
				{
					commerce_import /= 2;
					commerce_export /= 2;
				}
				if (*GovernorFaction == faction_id)
				{
					commerce_import++;
				}
				if (*GovernorFaction == otherFactionId)
				{
					commerce_export++;
				}
				commerce += commerce_import;
				
				BaseCommerceImport[otherFactionId] = commerce_import;
				BaseCommerceExport[otherFactionId] = commerce_export;
				
			}
			
		}
		
	}
	base->energy_intake_2 += commerce;
	base->energy_intake_2 += energygrid;
	
	base->energy_inefficiency = wtp_mod_energy_intake_lost(base_id, base->energy_intake_2 - base->energy_consumption, (*BaseUpkeepState == 1 ? faction.unk_43 : nullptr));
	base->energy_surplus = base->energy_intake_2 - base->energy_consumption - base->energy_inefficiency;
	
	if (*BaseUpkeepState == 1)
	{
		faction.energy_surplus_total += clamp(base->energy_surplus, 0, 99999);
	}
	
	// Non-multiplied energy intake is always limited to this range
	int total_energy = clamp(base->energy_surplus, 0, 9999);
	
	int val_psych = (total_energy * faction.SE_alloc_psych + 4) / 10;
	base->psych_total = max(0, min(val_psych, total_energy));
	
	for (int specialistNumber = 0; specialistNumber < base->specialist_total; specialistNumber++)
	{
		int citizenId = specialistNumber < MaxBaseSpecNum ? clamp(base->specialist_type(specialistNumber), 0, MaxSpecialistNum - 1) : mod_best_specialist();
		CCitizen &citizen = Citizen[citizenId];
		
		if (has_tech(citizen.obsol_tech, faction_id))
		{
			for (int otherCitizenId = 0; otherCitizenId < MaxSpecialistNum; otherCitizenId++)
			{
				CCitizen &otherCitizen = Citizen[otherCitizenId];
				
				if (has_tech(otherCitizen.preq_tech, faction_id) && !has_tech(otherCitizen.obsol_tech, faction_id) && otherCitizen.econ_bonus >= citizen.econ_bonus && otherCitizen.psych_bonus >= citizen.psych_bonus && otherCitizen.labs_bonus >= citizen.labs_bonus)
				{
					citizenId = otherCitizenId;
					break; // Original game picks first here regardless of other choices
				}
			}
		}
		
		base->specialist_modify(specialistNumber, citizenId);
		base->psych_total += Citizen[citizenId].psych_bonus;
		
	}

	int coeff_psych = baseEnergy.coeff_psych;
	base->psych_total = (coeff_psych * base->psych_total + 3) / 4;
	
	if (conf.base_psych) {
		mod_base_psych(base_id);
	} else {
		base_psych();
	}
	
	Profiling::stop("~ mod_base_yield_base_energy");
	
}

// Prepcomputes base tile yields.
std::array<ResourceYield, 21> getBaseWorkTileYields(int baseId)
{
	BASE &base = Bases[baseId];
	int factionId = base.factionId();

	std::array<ResourceYield, 21> workTileYields;

	for (int workTileIndex = 0; workTileIndex < 21; workTileIndex++)
	{
		int x = wrap(base.x + TableOffsetX[workTileIndex]);
		int y = base.y + TableOffsetY[workTileIndex];

		ResourceYield &workTileYield = workTileYields.at(workTileIndex);

		workTileYield.nutrient = mod_crop_yield(factionId, baseId, x, y, 0);
		workTileYield.mineral = mod_mine_yield(factionId, baseId, x, y, 0);
		workTileYield.energy = mod_energy_yield(factionId, baseId, x, y, 0);

	}

	return workTileYields;

}

/*
Populates avaiable worker tiles in base catchment area around the base and returns available tile count.
An optimized version of Thinker base_radius function.
*/
int32_t getBaseAvailableWorkTiles(int baseId)
{
	BASE* base = &Bases[baseId];
	int factionId = static_cast<unsigned char>(base->faction_id);
	bool has_map = Factions[factionId].player_flags & PFLAG_MAP_REVEALED;
	int32_t availableWorkTiles = 0;
	
	// populate unavailable tiles
	
	robin_hood::unordered_flat_set<MAP *> vehicleTiles;
	robin_hood::unordered_flat_set<MAP *> otherBaseWorkedTiles;
	
	if (baseComputeExcludedTile != nullptr)
	{
		vehicleTiles.insert(baseComputeExcludedTile);
	}
	
	for (int vehicleId = 0; vehicleId < *VehCount; vehicleId++)
	{
		VEH *vehicle = &Vehs[vehicleId];
		
		// within 2 range
		
		int dy = abs(base->y - vehicle->y);
		if (dy > 2)
			continue;
		int dx = abs(base->x - vehicle->x);
		if (!map_is_flat() && dx > *MapHalfX)
		{
			dx = *MapAreaX - dx;
		}
		if (dx > 2)
			continue;
		
		// vehicle disabling farming
		
		if (vehicle->order == ORDER_CONVOY || (vehicle->faction_id != factionId && vehicle->is_visible(factionId) && !has_treaty(factionId, vehicle->faction_id, DIPLO_TREATY|DIPLO_PACT)))
		{
			MAP *vehicleTile = mapsq(vehicle->x, vehicle->y);
			vehicleTiles.insert(vehicleTile);
		}
		
	}
	
	// Fix: In rare cases bases might have incorrect worked tiles set on foreign territory.
	// To avoid reserving these tiles the function checks actual territory ownership.
	for (int otherBaseIndex = 0; otherBaseIndex < *BaseCount; otherBaseIndex++)
	{
		BASE* otherBase = &Bases[otherBaseIndex];
		
		// other base
		
		if (otherBaseIndex == baseId)
			continue;
		
		// in range
		
		if (map_range(base->x, base->y, otherBase->x, otherBase->y) > 4)
			continue;
		
		for (int workerIndex = 0; workerIndex < 21; workerIndex++)
		{
			// worked tile
			
			if ((otherBase->worked_tiles & (1 << workerIndex)) == 0)
				continue;
			
			int workedTileX = wrap(otherBase->x + TableOffsetX[workerIndex]);
			int workedTileY = otherBase->y + TableOffsetY[workerIndex];
			
			// in range
			
			if (map_range(base->x, base->y, workedTileX, workedTileY) > 2)
				continue;
			
			MAP* otherBaseWorkedTile = mapsq(workedTileX, workedTileY);
			
			// valid location
			
			if (workerIndex == 0 || otherBaseWorkedTile->owner < 0 || otherBaseWorkedTile->owner == otherBase->faction_id || mod_whose_territory(otherBase->faction_id, workedTileX, workedTileY, nullptr, 0) < 0)
			{
				otherBaseWorkedTiles.insert(otherBaseWorkedTile);
			}
			
		}
		
	}
	
	// tiles outside of base catchment area
	
	std::fill(BaseTileFlags + 21, BaseTileFlags + 25, BR_NOT_AVAILABLE);
	
	// tiles in the base catchment area
	
	for (int workTileNumber = 0; workTileNumber < 21; workTileNumber++)
	{
		int x = wrap(base->x + TableOffsetX[workTileNumber]);
		int y = base->y + TableOffsetY[workTileNumber];
		MAP* workTile = mapsq(x, y);
		
		// on map
		
		if (workTile == nullptr)
		{
			BaseTileFlags[workTileNumber] = BR_NOT_VISIBLE;
			continue;
		}
		
		if (workTileNumber == 0)
		{
			BaseTileFlags[0] = BR_BASE_IN_TILE;
		}
		else
		{
			// visible
			
			if (!has_map && !workTile->is_visible(factionId))
			{
				BaseTileFlags[workTileNumber] = BR_NOT_VISIBLE;
				continue;
			}
			
			BaseTileFlags[workTileNumber] = 0;
			
			if (workTile->is_base())
			{
				BaseTileFlags[workTileNumber] |= BR_BASE_IN_TILE;
			}
			
			if (vehicleTiles.find(workTile) != vehicleTiles.end())
			{
				BaseTileFlags[workTileNumber] |= BR_VEH_IN_TILE;
			}
			
			if (workTile->owner >= 0 && mod_whose_territory(factionId, x, y, nullptr, 0) != factionId)
			{
				BaseTileFlags[workTileNumber] |= BR_FOREIGN_TILE;
			}
			else if (otherBaseWorkedTiles.find(workTile) != otherBaseWorkedTiles.end())
			{
				BaseTileFlags[workTileNumber] |= BR_WORKER_ACTIVE;
			}
			
		}
		
		bool valid = (BaseTileFlags[workTileNumber] == 0);
		if (workTileNumber == 0 || valid)
		{
			availableWorkTiles |= 1 << workTileNumber;
		}
		
	}
	
	return availableWorkTiles;
	
}

// updates base
void updateBase(BaseComputeParameterSet const &baseComputeParameterSet, bool compute)
{
	BASE &base = **CurrentBase;

	int farmedTileCount = __builtin_popcount(base.worked_tiles);
	assert(__builtin_popcount(base.worked_tiles) + base.specialist_total == base.pop_size + 1);

	// reset base intake
	
	assert(reinterpret_cast<int>(&base.nutrient_intake) + 96 == reinterpret_cast<int>(&base.autoforward_land_base_id));
    memset(&base.nutrient_intake, 0, 96);
    
    // add satellite intake
    
    base.nutrient_intake = baseComputeParameterSet.satelliteYield.nutrient;
    base.mineral_intake = baseComputeParameterSet.satelliteYield.mineral;
    base.energy_intake = baseComputeParameterSet.satelliteYield.energy;
    
    // update farmer yield
    
	for (int workTileNumber = 0; workTileNumber < 21; workTileNumber++)
	{
		int32_t workTileBit = 1 << workTileNumber;

		// worked

		if ((base.worked_tiles & workTileBit) == 0)
			continue;

		base.nutrient_intake += baseComputeParameterSet.workTileYields.at(workTileNumber).nutrient;
		base.mineral_intake += baseComputeParameterSet.workTileYields.at(workTileNumber).mineral;
		base.energy_intake += baseComputeParameterSet.workTileYields.at(workTileNumber).energy;

	}
	
	base.nutrient_intake_2 = base.nutrient_intake;
	base.mineral_intake_2 = base.mineral_intake;
	base.energy_intake_2 = base.energy_intake;
	base.unused_intake_2 = base.unused_intake;

	if (compute)
	{
		mod_base_nutrient();
		mod_base_minerals();
		mod_base_energy();
	}
	
}

/*
[WTP]
Ensures normal distribution of talents, drones, superdrones.
- 0 <= talents <= worker_count
- 0 <= drones + superdrones <= 2 * worker_count
- superdrones <= drones
- talents + drones <= worker_count
*/
void wtp_normalize_happiness(BASE *base, bool subtractSpecialists)
{
	int const worker_count = base->pop_size - (subtractSpecialists ? base->specialist_total : 0);
	
	// limit talents by base size
	
	base->talent_total = clamp(base->talent_total, 0, worker_count);
	
	// limit drones/superdrones by base size and redistribute them
	
	if (base->drone_total + base->superdrone_total <= 0)
	{
		base->drone_total = 0;
		base->superdrone_total = 0;
	}
	else if (base->drone_total + base->superdrone_total >= 2 * worker_count)
	{
		base->drone_total = worker_count;
		base->superdrone_total = worker_count;
	}
	else if (base->drone_total < base->superdrone_total)
	{
		int shift = (base->superdrone_total - base->drone_total + 1) / 2;
		base->drone_total += shift;
		base->superdrone_total -= shift;
	}
	else if (base->drone_total > worker_count)
	{
		int drone_excess = std::max(0, base->drone_total - worker_count);
		base->drone_total -= drone_excess;
		base->superdrone_total += drone_excess;
	}
	
	// ensure talents and drone/superdrones do not intersect
	
	while (base->talent_total + base->drone_total > worker_count)
	{
		if (base->talent_total > 0 && base->drone_total > 0)
		{
			if (base->superdrone_total == base->drone_total)
			{
				// compensate superdrone with talent
				base->talent_total--;
				base->superdrone_total--;
			}
			else if (base->superdrone_total == base->drone_total - 1)
			{
				// compensate drone with talent
				base->talent_total--;
				base->drone_total--;
			}
			else
			{
				// compress two drones into superdrone
				base->drone_total--;
				base->superdrone_total++;
			}
			
		}
		else if (base->talent_total > 0 && base->drone_total == 0)
		{
			// reduce talents to worker count
			base->talent_total = std::min(worker_count, base->talent_total);
		}
		else if (base->talent_total == 0 && base->drone_total > 0)
		{
			// reduce drones to worker count
			base->drone_total = std::min(worker_count, base->drone_total);
			base->superdrone_total = std::min(base->drone_total, base->superdrone_total);
		}
		else
		{
			// no other options
			break;
		}
		
	}
	
}

void wtp_add_psych_row(BASE *base, int num)
{
	wtp_normalize_happiness(base);
	
    if (num >= 0 && num <= 4) {
        BasePsychTalents[num] = base->talent_total;
        BasePsychNDrones[num] = base->drone_total;
        BasePsychSDrones[num] = base->superdrone_total;
    }
    
}

void __cdecl wtp_mod_base_psych(int base_id)
{
	BASE* base = &Bases[base_id];
	Faction* f = &Factions[base->faction_id];
	MFaction* m = &MFactions[base->faction_id];
	int faction_id = static_cast<unsigned char>(base->faction_id);
	base->talent_total = 0;
	base->drone_total = 0;
	base->superdrone_total = 0;
	base->pad_7 = 0;
	
	// stapled base ignores any other psych methods
	
	if (base->nerve_staple_turns_left > 0 || has_facility(FAC_PUNISHMENT_SPHERE, base_id))
	{
		for (int i = 0; i < 5; i++) wtp_add_psych_row(base, i);
		return;
	}
	
	// -5, Two extra drones for each military unit away from territory
	// -4, Extra drone for each military unit away from territory
	// -3, Extra drone if more than one military unit away from territory
	// -2, Cannot use military units as police. No nerve stapling.
	// -1, One police unit allowed. No nerve stapling.
	//  0, Can use one military unit as police
	//  1, Can use up to 2 military units as police
	//  2, Can use up to 3 military units as police!
	//  3, 3 units as police. Police effect doubled!!
	const int SE_police = base->SE_police(SE_Pending);
	const int num_police = clamp((SE_police == -1) + SE_police + 1, 0, 3);
	const int val_police = 1 + (SE_police >= 3);

	int drone_value = 0;
	int talent_value = 0;
	int police_total = 0;
	int effic_drones = 0;
	int capture_drones = 0;
	int content_pop, base_limit;
	mod_psych_check(faction_id, &content_pop, &base_limit);
	
	if (m->rule_talent) {
		// Extra talent at base (per "param" citizens, rounded up)
		talent_value += (m->rule_talent + base->pop_size - 1) / m->rule_talent;
	}
	
	// drones are not limited by specialists
	drone_value += clamp(base->pop_size - content_pop, 0, (int)base->pop_size);
	
	if (base_limit) {
		int drone_limit = (base_id % base_limit + f->base_count - base_limit) / base_limit;
		effic_drones = max(0, min(drone_limit, (int)base->pop_size));
		drone_value += effic_drones;
	}
	if (base->assimilation_turns_left > 0) {
		// Former faction_id can be the same but this can be also used for scenarios
		int v1 = (base->pop_size + (is_human(faction_id) ? f->diff_level : 3) - 2) / 4;
		int v2 = (base->assimilation_turns_left + 9) / 10;
		capture_drones = max(0, min(v1, v2));
		drone_value += capture_drones;
	}
	if (m->rule_drone) {
		// Extra drone at base (per "param" citizens, rounded down)
		drone_value += base->pop_size / m->rule_drone;
	}
	for (int i = 0; i < m->faction_bonus_count; i++) {
		// Number of drones per base made content
		if (m->faction_bonus_id[i] == RULE_NODRONE) {
			drone_value = max(0, drone_value - m->faction_bonus_val1[i]);
			break;
		}
	}
	
	if (f->SE_talent_pending >= 0) {
		talent_value += f->SE_talent_pending;
	} else {
		drone_value -= f->SE_talent_pending;
	}
	
	base->talent_total = talent_value;
	base->drone_total = drone_value;
	
	base->drone_total = max(0, min(base->drone_total, (int)base->pop_size));
	base->superdrone_total = max(0, min(drone_value + effic_drones - (int)base->pop_size, (int)base->pop_size));
	base->drone_total = max(0, min(drone_value + effic_drones, (int)base->pop_size));
	
	wtp_add_psych_row(base, 0); // Unmodified / Captured Base
	
	// facilities
	
	if (has_facility(FAC_PARADISE_GARDEN, base_id)) {
		base->talent_total += 2 + conf.base_psych_facility_extra_power;
	}
	if (has_facility(FAC_GENEJACK_FACTORY, base_id)) {
		base->drone_total += Rules->drones_induced_genejack_factory;
	}
	if (has_facility(FAC_RECREATION_COMMONS, base_id)) {
		base->drone_total -= 2 + conf.base_psych_facility_extra_power;
	}
	if (has_facility(FAC_HOLOGRAM_THEATRE, base_id)
	|| (has_project(FAC_VIRTUAL_WORLD, faction_id)
	&& has_facility(FAC_NETWORK_NODE, base_id))) {
		base->drone_total -= 2 + conf.base_psych_facility_extra_power;
	}
	if (has_project(FAC_PLANETARY_TRANSIT_SYSTEM, faction_id) && base->pop_size <= 3) {
		base->drone_total -= 1 + conf.base_psych_facility_extra_power;
	}
	if (has_facility(FAC_RESEARCH_HOSPITAL, base_id)) {
		base->drone_total -= 1 + conf.base_psych_facility_extra_power;
	}
	if (has_facility(FAC_NANOHOSPITAL, base_id)) {
		base->drone_total -= 1 + conf.base_psych_facility_extra_power;
	}

	wtp_add_psych_row(base, 1); // Facilities
	
	// police
	
	std::priority_queue<int> units;
	if (SE_police >= -1) {
		if (has_project(FAC_SELF_AWARE_COLONY, faction_id)) {
			units.push({val_police});
		}
		for (int i = 0; i < *VehCount; i++) {
			VEH* veh = &Vehs[i];
			if (veh->x == base->x && veh->y == base->y && veh->faction_id == base->faction_id
			&& veh->triad() != TRIAD_SEA && veh->plan() <= PLAN_RECON) {
				int value = val_police + (has_abil(veh->unit_id, ABL_POLICE_2X)
					|| (veh->is_native_unit() && m->rule_flags & RFLAG_WORMPOLICE));
				units.push({value});
			}
		}
		for (int i = 0; i < num_police && units.size() > 0; i++) {
			police_total += units.top() + conf.base_psych_police_extra_power;
			units.pop();
		}
		base->drone_total -= police_total;
	}

	if (SE_police == -3 && *BaseVehPacifismCount > 1) {
		base->drone_total += *BaseVehPacifismCount - 1;
	} else if (SE_police == -4 && *BaseVehPacifismCount > 0) {
		base->drone_total += *BaseVehPacifismCount;
	} else if (SE_police <= -5 && *BaseVehPacifismCount > 0) {
		base->drone_total += *BaseVehPacifismCount * 2;
	}
	
	wtp_add_psych_row(base, 2); // Police / Pacifism
	
	// secret projects
	
	if (has_project(FAC_HUMAN_GENOME_PROJECT, faction_id)) {
		base->talent_total += 1;
	}
	if (has_project(FAC_CLINICAL_IMMORTALITY, faction_id)) {
		base->talent_total += 1;
	}
	// Planned reduces drones by two, while Simple and Green reduces by one
	if (has_project(FAC_LONGEVITY_VACCINE, faction_id)) {
		int value = (f->SE_Economics_pending == SOCIAL_M_PLANNED) + (f->SE_Economics_pending != SOCIAL_M_FREE_MARKET);
		base->drone_total -= value;
	}
	
	wtp_add_psych_row(base, 3); // Secret Projects
	
	// psych
	
	int psych_effect = max(0, base->psych_total / conf.base_psych_cost);
	
	// psych removes drones first
	if (base->drone_total + base->superdrone_total > 0)
	{
		int drone_removed = std::min(base->drone_total + base->superdrone_total, psych_effect);
		psych_effect -= drone_removed;
		base->drone_total -= drone_removed;
	}
	
	// remaining psych creates talents
	base->talent_total += psych_effect;
	
	wtp_add_psych_row(base, 4); // Psych
	
	// specialists
	
	// happiness before specialists
	
	int happiness_before_specialists = base->talent_total - (base->drone_total + base->superdrone_total);
	
	// normalize happiness with specialists added
	
	wtp_normalize_happiness(base, true);
	
	// happiness after specialists
	
	int happiness_after_specialists = base->talent_total - (base->drone_total + base->superdrone_total);
	
	// more psych required to make sure specialists do not increase happiness
	
	if (happiness_after_specialists > happiness_before_specialists)
	{
		int psych_val_increase = std::max(0, happiness_after_specialists - happiness_before_specialists);
		int psych_increase = conf.base_psych_cost * psych_val_increase;
		int economy_decrease = conf.base_psych_economy_conversion_ratio * psych_increase;
		
		base->psych_total += psych_increase;
		base->economy_total -= economy_decrease;
		base->pad_7 = static_cast<int8_t>(economy_decrease);
		base->pad_8 = psych_increase;
		
	}
	else
	{
		base->pad_7 = 0;
		base->pad_8 = 0;
	}
	
}

/*
Finds best specialist by econ/labs/psych weight.
*/
std::vector<int> getAvailableSpecialistTypes(int factionId, int basePopSize)
{
	std::vector<int> availableSpecialists;

	for (int citizenId = 0; citizenId < MaxSpecialistNum; citizenId++)
	{
		CCitizen const &citizen = Citizen[citizenId];

		// available
		
		if (!has_tech(citizen.preq_tech, factionId) || has_tech(citizen.obsol_tech, factionId))
			continue;

		// no advanced specialist in small base

		if (basePopSize < Rules->min_base_size_specialists && citizen.psych_bonus == 0)
			continue;

		// add available specialist

		availableSpecialists.push_back(citizenId);

	}
	
	return availableSpecialists;
	
}

/*
Finds best specialist by econ/labs/psych weight.
*/
int getBestSpecialistType(std::vector<int> const &availableSpecialists, double econValue, double labsValue, double psychValue)
{
	int bestCitizenId = 1; // doctor
	double bestScore = 0.0;
	for (int citizenId : availableSpecialists)
	{
		CCitizen const &citizen = Citizen[citizenId];

		double score = econValue * (double)citizen.econ_bonus + labsValue * (double)citizen.labs_bonus + psychValue * (double)citizen.psych_bonus;
		if (score > bestScore)
		{
			bestCitizenId = citizenId;
			bestScore = score;
		}

	}

	return bestCitizenId;

}

bool wtp_base_pop_boom(int base_id)
{
	BASE* b = &Bases[base_id];
	
	// required nutrient surplus
	if (b->nutrient_surplus < Rules->nutrient_intake_req_citizen)
		return false;
	
	// Cloning Vats triggers population boom if enabled
	if (conf.cloning_vats_se_growth == 0 && has_project(FAC_CLONING_VATS, b->faction_id))
		return true;
	
	// required Golden Age if configured
	if (conf.pop_boom_requires_golden_age || !b->golden_age())
		return false;
	
	// required GROWTH rating
	if (b->SE_growth(true) >= conf.pop_boom_requires_growth_rating)
		return true;
	
	// not population boom by defalut
	return false;
	
}

/*
flat hurry cost
*/
int wtp_flat_hurry_cost(int base_id, int item_id, int hurry_mins)
{
	assert(base_id >= 0 && base_id < *BaseCount);
	
	BASE* base = &Bases[base_id];
	MFaction* mfaction = &MFactions[base->faction_id];
	
	// mineral cost
	
	int remainingMineralCost = max(0, mineral_cost(base_id, item_id) - base->minerals_accumulated);
	
	// hurry cost
	
	int hurryCost;
	
	if (item_id >= 0)
	{
		// unit
		hurryCost = remainingMineralCost * conf.flat_hurry_cost_multiplier_unit;
	}
	else if (item_id > -SP_ID_First)
	{
		// facility
		hurryCost = remainingMineralCost * conf.flat_hurry_cost_multiplier_facility;
	}
	else
	{
		// project
		hurryCost = remainingMineralCost * conf.flat_hurry_cost_multiplier_project;
	}
	
	// The Voice of Planet modifier
	
	if (has_project(FAC_VOICE_OF_PLANET))
	{
		hurryCost *= 2;
	}
	
	// faction modifier
	
	hurryCost = hurryCost * mfaction->rule_hurry / 100;
	
	// fix hurry cost if fixed mineral contribution is configured
	
	if (conf.fix_mineral_contribution)
	{
		int mineralCostFactor = mod_cost_factor(base->faction_id, RSC_MINERAL, -1);
		hurryCost = (hurryCost * Rules->mineral_cost_multi + (mineralCostFactor - 1)) / mineralCostFactor;
	}
	
	// compute proportional payment
	
	if (hurry_mins > 0 && remainingMineralCost > 0)
	{
		return hurry_mins * hurryCost / remainingMineralCost + (((hurry_mins * hurryCost) % remainingMineralCost) != 0);
	}
	
	return 0;
	
}

int wtp_terraform_eco_damage(int base_id)
{
	BASE* base = &Bases[base_id];
	
	int value = 0;
	for (const auto& m : iterate_tiles(base->x, base->y, 0, 21)) {
		int num = __builtin_popcount(m.sq->items & (BIT_THERMAL_BORE|BIT_ECH_MIRROR|
			BIT_CONDENSER|BIT_SOIL_ENRICHER|BIT_FARM|BIT_SOLAR|BIT_MINE|BIT_MAGTUBE|BIT_ROAD));
		if ((1 << m.i) & base->worked_tiles) {
			num *= 2;
		}
		if (m.sq->items & BIT_THERMAL_BORE) {
			num += 8;
		}
		if (m.sq->items & BIT_ECH_MIRROR) {
			num += 6;
		}
		if (m.sq->items & BIT_CONDENSER) {
			num += 4;
		}
		if (m.sq->items & BIT_FOREST) {
			num -= 1;
		}
		value += num;
	}
	
	if (conf.facility_terraforming_ecodamage_halved)
	{
		// each facility halfes terraforming eco-damage
		
		if (has_facility(FAC_TREE_FARM, base_id) != 0)
		{
			value /= 2;
		}
		if (has_facility(FAC_HYBRID_FOREST, base_id) != 0)
		{
			value /= 2;
		}
		
	}
	else
	{
		// each facility reduces terraforming eco-damage by 50%
		
		if (has_facility(FAC_TREE_FARM, base_id) != 0 && has_facility(FAC_HYBRID_FOREST, base_id) != 0)
		{
			value = 0;
		}
		else if (has_facility(FAC_TREE_FARM, base_id) != 0 || has_facility(FAC_HYBRID_FOREST, base_id) != 0)
		{
			value /= 2;
		}
		
	}
		
	return value;
	
}

int findReplacementSpecialist(int factionId, int specialistId)
{
	CCitizen *specialist = &Citizen[specialistId];
	
	int bestSuperiorSpecialistId = -1;
	int bestSuperiorSpecialistValue = 0;
	int bestSpecialistId = -1;
	int bestSpecialistValue = 0;
	
	for (int otherSpecialistId = 0; otherSpecialistId < MaxSpecialistNum; otherSpecialistId++)
	{
		CCitizen *otherSpecialist = &Citizen[otherSpecialistId];
		
		if (!has_tech(otherSpecialist->preq_tech, factionId) || has_tech(otherSpecialist->obsol_tech, factionId))
			continue;
		
		int value =
			std::min(specialist->econ_bonus, otherSpecialist->econ_bonus) + std::min(specialist->psych_bonus, otherSpecialist->psych_bonus) + std::min(specialist->labs_bonus, otherSpecialist->labs_bonus)
			+ otherSpecialist->econ_bonus + otherSpecialist->psych_bonus + otherSpecialist->labs_bonus
		;
		
		if (otherSpecialist->econ_bonus >= specialist->econ_bonus && otherSpecialist->psych_bonus >= specialist->psych_bonus && otherSpecialist->labs_bonus >= specialist->labs_bonus)
		{
			if (value > bestSuperiorSpecialistValue)
			{
				bestSuperiorSpecialistId = otherSpecialistId;
				bestSuperiorSpecialistValue = value;
			}
		}
		else
		{
			if (value > bestSpecialistValue)
			{
				bestSpecialistId = otherSpecialistId;
				bestSpecialistValue = value;
			}
		}
		
	}
	
	return bestSuperiorSpecialistId != -1 ? bestSuperiorSpecialistId : bestSpecialistId != -1 ? bestSpecialistId : specialistId;
	
}

void clear_stack(int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int)
{
	
}

int getBasePsychCoefficient(int baseId)
{
	assert(baseId >= 0 && baseId < *BaseCount);
	
	BASE &base = Bases[baseId];
	int factionId = static_cast<unsigned char>(base.faction_id);
	
	int psychCoefficient = 4;
	
	if (has_facility(FAC_HOLOGRAM_THEATRE, baseId) || (has_project(FAC_VIRTUAL_WORLD, factionId) && has_facility(FAC_NETWORK_NODE, baseId)))
	{
		psychCoefficient += 2;
	}
	if (has_facility(FAC_RESEARCH_HOSPITAL, baseId))
	{
		psychCoefficient += 1;
	}
	if (has_facility(FAC_NANOHOSPITAL, baseId))
	{
		psychCoefficient += 1;
	}
	if (has_facility(FAC_TREE_FARM, baseId))
	{
		psychCoefficient += conf.energy_multipliers_tree_farm[1];
	}
	if (has_facility(FAC_HYBRID_FOREST, baseId))
	{
		psychCoefficient += conf.energy_multipliers_hybrid_forest[1];
	}
	if (has_facility(FAC_CENTAURI_PRESERVE, baseId))
	{
		psychCoefficient += conf.energy_multipliers_centauri_preserve[1];
	}
	if (has_facility(FAC_TEMPLE_OF_PLANET, baseId))
	{
		psychCoefficient += conf.energy_multipliers_temple_of_planet[1];
	}
	
	return psychCoefficient;
	
}

double getBaseTileScore(ResourceYield const &tileYield, BaseComputeParameterSet const &baseComputeParameterSet)
{
	return
		+ conf.worker_algorithm_nutrient_preference * static_cast<double>(tileYield.nutrient)
		+ conf.worker_algorithm_mineral_preference * static_cast<double>(tileYield.mineral) * baseComputeParameterSet.mineralValue
		+ conf.worker_algorithm_energy_preference * static_cast<double>(tileYield.energy) * baseComputeParameterSet.energyValue
	;
	
}

/*
Calculate the energy loss/inefficiency for the given energy intake in the base.
*/
int wtp_mod_energy_intake_lost(int base_id, int energy, int32_t* effic_energy_lost)
{
	// headquarter does not incure energy loss
	
	if (has_facility(FAC_HEADQUARTERS, base_id))
	{
		return 0;
	}
	
	// no energy loss if no energy
	
	if (energy <= 0)
	{
		return 0;
	}
	
	BASE &base = Bases[base_id];
	int max_dist = *MapHalfX * 3 / 2;
	int dist_hq = max_dist;
	for (int i = 0; i < *BaseCount; i++)
	{
		BASE &otherBase = Bases[i];
	
		if (otherBase.faction_id == base.faction_id && has_facility(FAC_HEADQUARTERS, i))
		{
			int dist = vector_dist(otherBase.x, otherBase.y, base.x, base.y);
			dist_hq = std::min(dist_hq, dist);
			break;
		}
		
	}
	
	bool has_creche = has_facility(FAC_CHILDREN_CRECHE, base_id);
	int crecheEfficiencyBonus = has_creche ? 2 /* +2 on efficiency scale */ : 0;
	
	if (effic_energy_lost)
	{
		for (int i = 0; i < 9; i++)
		{
			int efficiencyRating = 4 - i + crecheEfficiencyBonus;
			int energy_lost = getEnergyLost(energy, dist_hq, max_dist, efficiencyRating);
			effic_energy_lost[i] += energy_lost;
		}
		
	}
	
	int efficiencyRating = Factions[base.faction_id].SE_effic_pending + crecheEfficiencyBonus;
	int value = getEnergyLost(energy, dist_hq, max_dist, efficiencyRating);
	
	return value;
	
}

int getEnergyLost(int energy, int hqDistance, int maxDistance, int efficiencyRating)
{
	int efficiencyLevel = 4 + efficiencyRating;
	return std::max(0, std::min(energy, energy * 2 * hqDistance / maxDistance - energy * efficiencyLevel / 8));
}

void storeCitizenAllocation(CitizenAllocation &citizenAllocation)
{
	BASE &base = **CurrentBase;

	citizenAllocation.worked_tiles = base.worked_tiles;
	citizenAllocation.specialist_total = base.specialist_total;
	citizenAllocation.specialist_types[0] = base.specialist_types[0];
	citizenAllocation.specialist_types[1] = base.specialist_types[1];

}

void applyCitizenAllocation(CitizenAllocation &citizenAllocation)
{
	BASE &base = **CurrentBase;

	base.worked_tiles = citizenAllocation.worked_tiles;
	base.specialist_total = citizenAllocation.specialist_total;
	base.specialist_types[0] = citizenAllocation.specialist_types[0];
	base.specialist_types[1] = citizenAllocation.specialist_types[1];

}

/*
Returns base condition. The higher the value, the better the base is.
0 = not enough nutreints to feed the population
1 = not enough minerals to support units
2 =	rioting
3 = all good
*/
int getBaseCondition()
{
	BASE &base = **CurrentBase;

	int condition;

	if (base.nutrient_surplus < 0)
	{
		condition = 0;
	}
	else if (base.mineral_surplus < 0)
	{
		condition = 1;
	}
	else if (base.drone_riots())
	{
		condition = 2;
	}
	else
	{
		condition = 3;
	}

	return condition;

}

ResourceYield getBaseSurplus()
{
	BASE &base = **CurrentBase;
	return {base.nutrient_surplus, base.mineral_surplus, base.energy_surplus};
}

// Simplified version of getBaseGain
double getBaseSurplusGain()
{
	BASE &base = **CurrentBase;

	// compute gain

	double income = conf.worker_algorithm_mineral_preference * base.mineral_surplus + conf.worker_algorithm_energy_preference * base.energy_surplus;
	double populationGrowth = static_cast<double>(base.nutrient_surplus) / static_cast<double>((base.pop_size + 1) * Rules->nutrient_cost_multi);
	double relativePopulationGrowth = populationGrowth / static_cast<double>(base.pop_size);
	double incomeGrowth = income * relativePopulationGrowth;

	double gain =
		+ income
		+ incomeGrowth * conf.worker_algorithm_growth_multiplier
	;

	return gain;

}

