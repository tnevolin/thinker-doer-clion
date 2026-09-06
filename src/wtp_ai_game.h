#pragma once

#include <vector>
#include <set>

#include "robin_hood.h"
#include "multikey_map.h"

#include "main.h"
#include "engine.h"
#include "wtp_aiTask.h"
#include "wtp_game.h"

constexpr int MAX_SAFE_LOCATION_SEARCH_RANGE = 6;
constexpr int STACK_MAX_BASE_RANGE = 20;
constexpr int STACK_MAX_COUNT = 20;

// estimated average land colony travel time
constexpr double AVERAGE_LAND_COLONY_TRAVEL_TIME = 10.0;

// project value multiplier comparing to mineral cost
constexpr double BASE_CAPTURE_GAIN_COEFFICIENT = 0.75;
constexpr double PROJECT_VALUE_MULTIPLIER = 1.0;

// base defense weight for remote vehicles
constexpr double BASE_DEFENSE_RANGE_AIR		= 10.0;
constexpr double BASE_DEFENSE_RANGE_SEA		=  5.0;
constexpr double BASE_DEFENSE_RANGE_LAND	=  3.0;

// cumulative normal distribution approximation
constexpr double CND_APPROXIMATION_SCALE = +2.30;

// minimal opponent relative health bonus to use in sufficiency computation
static constexpr double MIN_OPPONENT_RELATIVE_HEALTH_BONUS = 0.1;
	
// forward declarations

struct TileInfo;

/*
Available vehicle move action.
*/
struct MoveAction
{
	MAP *destination;
	int remainingMovementPoints;
};
/*
Available vehicle attack action.
*/
struct AttackAction
{
	MAP *destination;
	MAP *target;
	double hastyCoefficient;
};

/// land-unload transfer
struct Transfer
{
	MAP *passengerStop = nullptr;
	MAP *transportStop = nullptr;
	
	Transfer() {}
	
	Transfer(MAP *_passengerStop, MAP *_transportStop)
	: passengerStop{_passengerStop}
	, transportStop{_transportStop}
	{}
	
	bool valid() const
	{
		return passengerStop != nullptr && transportStop != nullptr;
	}
	
};

struct PotentialAttack
{
	int vehiclePad0;
	double hastyCoefficient;
	EngagementMode engagementMode;
};

/*
all unit-to-unit combat effects
*/
struct CombatEffectTable
{
private:
	
	// each to each unit combat effects not counting terrain
	// [attackerFactionId][attackerUnitId][defenderFactionId][defenderUnitId][engagementMode] = combatEffect
    MultiKeyMap<double, int, int, int, int, EngagementMode, MAP const *, MAP const *> combatEffects;

	static double computeUnitCombatEffect(int attackerFactionId, int attackerUnitId, int defenderFactionId, int defenderUnitId, EngagementMode engagementMode, MAP const *attackerTile, MAP const *defenderTile);
	
public:

	void clear();
	double getUnitCombatEffect(int attackerFactionId, int attackerUnitId, int defenderFactionId, int defenderUnitId, EngagementMode engagementMode, MAP const *attackerTile, MAP const *defenderTile);
	double getUnitCombatEffect(int attackerFactionId, int attackerUnitId, int defenderFactionId, int defenderUnitId, EngagementMode engagementMode);
	double getVehicleCombatEffect(int attackerVehicleId, int defenderVehicleId, EngagementMode engagementMode, MAP const *attackerTile, MAP const *defenderTile);
	double getVehicleCombatEffect(int attackerVehicleId, int defenderVehicleId, EngagementMode engagementMode);
	
};

struct Combattant
{
	int pad0;
	int factionId;
	int unitId;
	int key;
	bool melee;
	bool artillery;
	bool aircraftInFlight;
	bool needlejetInFlight;
	bool canAttackNeedlejetInFlight;
	bool canBombardAircraftInFlight;
	double strengthCoefficient;
	double health;
	double weight;
	double remainingHealth;
	
	Combattant(int factionId, int unitId, double weight, bool airbase);
	Combattant(int vehicleId, double weight, bool airbase);
	void initialize(robin_hood::unordered_flat_map<int, double>  &damageCoefficients);
	
};
struct CombattantEffect
{
	Combattant *attacker;
	Combattant *defender;
	double effect;
};

struct CombatData
{
private:
	
	MAP *tile = nullptr;
	bool airbase = false;
	bool playerAssaults = false;
	double targetGain = 0.0;
	
	std::vector<Combattant> assailants;
	std::vector<Combattant> protectors;
	
	robin_hood::unordered_flat_map<int, double> assailantGains;
	robin_hood::unordered_flat_map<int, double> protectorGains;
	
public:
	
	void initialize(MAP const *_tile, bool _playerAssaults, double _targetGain);
	
	double getUnitCombatEffect(int attackerFactionId, int attackerUnitId, int defenderFactionId, int defenderUnitId, EngagementMode engagementMode, bool attackerAtTile, bool defenderAtTile);
	double getCombattantCombatEffect(Combattant  &attacker, Combattant  &defender, EngagementMode engagementMode, bool attackerAtTile, bool defenderAtTile);
	
	Combattant &addAssailant(int vehicleId, double weight = 1.0);
	Combattant &addProtector(int vehicleId, double weight = 1.0);
	void removeLastAssailant();
	void removeLastProtector();
	
	double getAssailantWeightSum();
	double getProtectorWeightSum();
	double getAssailantHealthSum(bool range);
	double getProtectorHealthSum(bool range);
	double getAssailantRemainingHealthSum(bool range);
	double getProtectorRemainingHealthSum(bool range);
	
	// units and their remaining summary weights
	
	// [FactionUnitKey]
	robin_hood::unordered_flat_map<int, double> remainingAssailantUnitWeights;
	double remainingAssailantUnitWeightSum;
	// [FactionUnitKey]
	robin_hood::unordered_flat_map<int, double> remainingProtectorUnitWeights;
	double remainingProtectorUnitWeightSum;
	
	bool computed = false;
	bool sufficientAssault;
	bool sufficientProtect;
	
	double getAssaultSufficiency(bool artillery);
	double getProtectSufficiency(bool artillery);
	
	double getAssailantCombattantContribution(Combattant &combattant);
	double getProtectorCombattantContribution(Combattant &combattant);
	double getAssailantUnitContribution(int factionId, int unitId);
	double getProtectorUnitContribution(int factionId, int unitId);
	double getAssailantContribution(int vehicleId);
	double getProtectorContribution(int vehicleId);
	
	bool isSufficientAssault();
	bool isSufficientProtect();
	
	void compute();
	void compute(robin_hood::unordered_flat_map<int, double> assailantVehicleDamageCoefficients, robin_hood::unordered_flat_map<int, double> protectorVehicleDamageCoefficients);
	static void resolveMutualCombat(CombattantEffect &combattantEffect);
	CombattantEffect getBestCombattantEffect(std::list<Combattant *> &attackers, std::list<Combattant *> &defenders, EngagementMode engagementMode, bool attackerAtTile, bool defenderAtTile);

	static double getEnemyRelativeHealthBonus(std::vector<Combattant> opponentCombattants);
	
};

struct DefendDataAttacker
{
	int factionId;
	int unitId;
	Triad triad;
	double health;
	bool melee;
	bool artillery;
	bool bombardAir;
};
struct DefendDataDefender
{
	int vehicleId;
	int factionId;
	int unitId;
	Triad triad;
	double health;
	bool artillery;
	double minBombardmentHealth;
};
struct DefendData
{
	MAP const *tile = nullptr;
	double targetGain = 0.0;

	bool computed = false;
	bool sufficient = false;
	std::vector<DefendDataAttacker> attackers;
	std::vector<DefendDataDefender> defenders;
	robin_hood::unordered_flat_map<int, double> defenderContributions;

	DefendData(MAP const* _tile, double _targetGain);

	void addAttackerUnit(int _factionId, int _unitId, double _health);
	void addDefenderVehicle(int vehicleId);
	bool isSufficient();
	double getVehicleContribution(int vehicleId);

private:

	double getCombatEffect(int attackerFactionId, int attackerUnitId, int defenderVehicleId, EngagementMode engagementMode);

};

struct TileTransit
{
	int angle;
	TileInfo *tileInfo;
	std::array<int, MOVEMENT_TYPE_COUNT> hexCosts {};
	std::array<int, MOVEMENT_TYPE_COUNT> averageHexCosts {};
	std::array<bool, MaxPlayerNum> zocs {};

	TileTransit(int _angle, TileInfo *_tileInfo);

};

struct TileInfo
{
	int index;
	int x;
	int y;
	MAP *tile;
	
	// surface type (in boolean and numeric form)
	bool land;
	bool ocean;
	SurfaceType surfaceType;
	bool coast;
	bool rough;
	bool fungus;
	std::set<int> adjacentSeaRegions;
	bool adjacentLand;
	bool adjacentSea;
	double adjacentLandRatio;
	double adjacentSeaRatio;
	
	// items
	int baseId = -1;
	bool base = false;
	bool bunker = false;
	bool airbase = false;
	bool port = false;
	// sensor coverage
	std::array<bool, MaxPlayerNum> sensorCoverages {};
	// player base range
	std::array<int, TRIAD_COUNT> baseRanges;
//	std::array<double, TRIAD_COUNT> baseDistances;

	// land vehicle is allowed here without transport
	bool landAllowed = false;
	
	// vehicles at tile
	
	std::vector<int> vehicleIds;
	std::vector<int> playerVehicleIds;
	std::array<bool, MaxPlayerNum> factionNeedlejetInFlights;
	std::array<bool, MaxPlayerNum> unfriendlyNeedlejetInFlights;
	
	// adjacent tiles
	std::vector<TileInfo *> adjacentTileInfos;
	// range tiles
	std::vector<TileInfo *> range2CenterTileInfos;
	std::vector<TileInfo *> range2NoCenterTileInfos;

	// blocks
	std::array<bool, MaxPlayerNum> blocks;
	// tile transits
	std::vector<TileTransit> tileTransits;
	
	// danger zones
	// artifact or empty base can be captured
	bool unfriendlyDangerZone;
	// units can be attacked by hostiles
	bool hostileDangerZone;
	// units can be bombarded by hostiles
	bool artilleryDangerZone;
	// possible enemy attacks
	std::vector<PotentialAttack> potentialAttacks;
	
	// nearest bases
	std::array<IdIntValue, 3> nearestBaseRanges;
	
	// combat data
	double maxBombardmentDamage;
	std::array<double, MaxPlayerNum> sensorOffenseMultipliers;
	std::array<double, MaxPlayerNum> sensorDefenseMultipliers;
	std::array<double, ATTACK_TRIAD_COUNT> terrainDefenseMultipliers;
	double getOffenseMultiplier(int attackerFactionId, int attackerUnitId, int defenderFactionId, int defenderUnitId);
	double getDefenseMultiplier(int attackerFactionId, int attackerUnitId, int defenderFactionId, int defenderUnitId, bool atTile);
	
};

struct BasePoliceData
{
	std::array<int, 2> policeTypePowers;
	std::array<double, 2> policeTypeGains;
	int allowedPolice;
	int requiredPower;
	std::vector<int> providedPowers;

	double getUnitPoliceGain(int unitId, int factionId) const;
	double getVehiclePoliceGain(int vehicleId) const;
	void addVehicle(int vehicleId);
	bool isSufficientForType(int policeTypeIndex) const;
	bool isSufficientForVehicle(int vehicleId) const;

};

struct BaseProbeData
{
	double safeTime = 0.0;
	double requiredEffect = 0.0;
	double providedEffect = 0.0;
	double providedEffectPresent = 0.0;
	
	void clear()
	{
		requiredEffect = 0.0;
		providedEffect = 0.0;
		providedEffectPresent = 0.0;
	}
	
	void reset()
	{
		providedEffect = 0.0;
		providedEffectPresent = 0.0;
	}
	
	void addVehicle(int  vehicleId, bool  present)
	{
		double vehicleEffect = getVehicleMoraleMultiplier(vehicleId);
		
		providedEffect += vehicleEffect;
		
		if (present)
		{
			providedEffectPresent += vehicleEffect;
		}
		
	}
	
	bool isSatisfied(bool  present) const
	{
		return (present ? providedEffectPresent : providedEffect) >= requiredEffect;
	}
	
	double getDemand(bool  present) const
	{
		double required = requiredEffect;
		double provided = (present ? providedEffectPresent : providedEffect);
		return ((required > 0.0 && provided < required) ? 1.0 - provided / required : 0.0);
	}
	
};

struct BaseInfo
{
	BASE snapshot;
	int id;
	MAP *tile;
	int factionId;
	bool port;
	int enemyAirbaseDistance;
	
	// economical data
	double gain;
	
	// police data	
	BasePoliceData policeData;
	
	// combat data
	std::array<double, 4> moraleMultipliers;
	bool artillery;
	CombatData combatData;
	BaseProbeData probeData;
	
	// enemy base data
	
	// total gain received from capturing base
	// includes combat unit expenses (build, support)
	double captureGain;
	
	// protector unit weights
	// [factionId][unitId]
	robin_hood::unordered_flat_map<int, robin_hood::unordered_flat_map<int, double>> protectorUnitWeights;
	
	// combat data
	
	// range to closest player base
	int closestPlayerBaseRange;
	
	// reset base
	
	void reset()
	{
		Profiling::start("- BaseInfo::reset");
        memcpy(&Bases[id], &snapshot, sizeof(BASE));
		Profiling::stop("- BaseInfo::reset");
	}
	
	// protector operations
	
	void addProtector(int vehicleId)
	{
		policeData.addVehicle(vehicleId);
		combatData.addProtector(vehicleId);
	}
	bool isSufficient()
	{
		return policeData.isSufficientForType(0) && combatData.isSufficientProtect();
	}
	bool isSufficientForPoliceType(int policeTypeIndex)
	{
		return policeData.isSufficientForType(policeTypeIndex) && combatData.isSufficientProtect();
	}

};

struct BunkerInfo
{
	MAP *tile;
	
	bool playerTerritory = false;
	
	// combat data
	CombatData combatData;
	
	// economical data
	double gain;
	
};

/// faction data for all factions
struct FactionInfo
{
	std::vector<int> baseIds;
	
	std::array<std::array<bool, 3>, 2> canBuildMelee;
	double offenseMultiplier;
	double defenseMultiplier;
	double fanaticBonusMultiplier;
	int maxConOffenseValue;
	int maxConDefenseValue;
	std::array<int, 3> fastestTriadChassisIds;
	double threatCoefficient;
	int enemyCount;
	int bestSeaTransportUnitId;
	int bestSeaTransportUnitSpeed;
	int bestSeaTransportUnitCapacity;
	
	// economical data
	
	double averageBaseGain;
	double stolenTechnologyGain;

	// terraforming data
	
	int airFormerCount;
	robin_hood::unordered_flat_map<int, int> seaClusterFormerCounts;
	robin_hood::unordered_flat_map<int, int> landTransportedClusterFormerCounts;
	
	// combat data
	
	int totalVendettaCount; // excluding aliens and player
	double productionPower;
	std::vector<int> availableUnitIds;
	std::vector<int> buildableUnitIds;
	std::vector<int> combatUnitIds;
	std::vector<int> combatVehicleIds;
	
	// diplomacy
	
    int32_t diplo_status[8];
	
};

struct OffenseEffect
{
	EngagementMode engagementMode;
	double effect;
};

struct EnemyStackInfo
{
private:
	
	static constexpr int BOMBARDMENT_ROUNDS = 5;
//	std::array<double, 2 * MaxProtoFactionNum * CombatMode_COUNT> offenseEffects;	// including location bonuses
//	std::array<double, 2 * MaxProtoFactionNum> defenseEffects;						// excluding location bonuses
//	std::vector<CombatEffect> unitEffects = std::vector<CombatEffect>(2 * MaxProtoFactionNum * CombatMode_COUNT);
//	std::array<double, 2 * MaxProtoFactionNum> effects;
	
public:
	
	MAP *tile = nullptr;
	
	std::vector<int> vehiclePad0s;
	std::vector<int> airVehiclePad0s;
	std::vector<int> artilleryVehiclePad0s;
	std::vector<int> nonArtilleryVehiclePad0s;
	
	int baseRange;
	bool tileBase;
	bool tileBaseOrBunker;
	bool tileAirbase;
	int tileBaseId;
	
	bool hostile = false;
	bool breakTreaty = false;
	bool alien = false;
	bool alienMelee = false;
	bool alienMindWorms = false;
	bool alienSporeLauncher = false;
	bool alienFungalTower = false;
	bool needlejetInFlight = false;
	bool artifact = false;
	int lowestSpeed = -1;
	bool artillery = false;
	bool bombardment = false;
	bool targetable = false;
	bool bombardmentDestructive = false;
	double averageUnitCost = 0.0;
	
	// combat data
	
	CombatData combatData;
	std::array<std::array<double, CombatMode_COUNT>, 2 * MaxProtoFactionNum> offenseEffects;	// including location bonuses
	double destructionGain;
	
	double requiredSuperiority;
	double desiredSuperiority;
	// sum of relative powers
	double weight;
	// max relative power that can be destroyed by bombardment
	double maxBombardmentEffect;
	
	void addVehicle(int vehicleId);
	bool isUnitCanMeleeAttackStack(int unitId, MAP *position = nullptr) const;
	bool isUnitCanArtilleryAttackStack(int unitId, MAP *position = nullptr) const;
	
	// unit effects
	
	void setUnitOffenseEffect(int unitId, CombatMode combatMode, double effect);
	double getUnitOffenseEffect(int unitId, CombatMode combatMode) const;
	double getVehicleOffenseEffect(int vehicleId, CombatMode combatMode) const;
	double getUnitBombardmentEffect(int unitId) const;
	double getVehicleBombardmentEffect(int vehicleId) const;
	double getUnitDirectEffect(int unitId) const;
	double getVehicleDirectEffect(int vehicleId) const;
	
	// attack variables
	
	std::vector<IdDoubleValue> bombardmentVechileTravelTimes;
	std::vector<IdDoubleValue> directVechileTravelTimes;
	double combinedBombardmentEffect;	// one turn relative destruction of nonArtillery part of the stack
	double combinedDirectEffect;		// one to one combat effect: either artillery duel or melee
	bool bombardmentSufficient;
	bool directSufficient;
	// time until attackers gather required superiority
	double coordinatorTravelTime;
	// extra weight that can be added by building or pulling in
	double extraWeight;
	// 
	double destructionRatio;
	bool requiredSuperioritySatisfied;
	
	void resetAttackParameters()
	{
		bombardmentVechileTravelTimes.clear();
		directVechileTravelTimes.clear();
		combinedBombardmentEffect = 0.0;
		combinedDirectEffect = 0.0;
		bombardmentSufficient = false;
		directSufficient = false;
		coordinatorTravelTime = 0.0;
		extraWeight = 0.0;
		destructionRatio = 0.0;
		requiredSuperioritySatisfied = false;
	}
	
	double getTotalBombardmentEffect() const;
	double getTotalBombardmentEffectRatio() const;
	double getRequiredEffect() const;
	double getDestructionRatio() const;
	bool isSufficient(bool desired) const;
	bool addAttacker(IdDoubleValue const &vehicleTravelTime);
	void computeAttackParameters();
	
};

struct TERRAFORMING_OPTION
{
	// recognizable name
	char const *name;
	// land or ocean
	bool ocean;
	// applies to rocky tile only
	bool rocky;
	// area effect
	bool areaEffect;
	// requires base yield computation
	bool yield;
	// main action that is required to be discovered
	FormerItem requiredAction;
	// list of actions
	std::set<FormerItem> actions;
};

// terraforming request assigned to formers
struct TerraformingRequest
{
	MAP *tile;
	TERRAFORMING_OPTION const *option;
	FormerItem action;
	double incomeGain;
	int terraformingTime;
	double formerGain;
	mutable bool assigned = false;

	TerraformingRequest(MAP *_tile, TERRAFORMING_OPTION const *_option, FormerItem _action, double _incomeGain);

	// // compare by formerGain descending
	// bool operator<(TerraformingRequest const & other) const;

};

struct ConvoyRequest
{
	MAP *tile;
	double gain;
};

enum CombatRequestType
{
	CRT_REPAIR_MONOLITH,	// repair and monolith promotion can collocate
	CRT_POP_POD,
	CRT_DEFEND_BASE,		// defend base and police can collocate
	CRT_DEFEND_BUNKER,
	CRT_CAPTURE_BASE,
	CRT_ATTACK_STACK,
};
struct CombatRequest
{
	CombatRequestType type;
	MAP const *tile = nullptr;
	int vehicleId = -1;
	double gain = 0.0;

	CombatRequest(CombatRequestType _type, MAP const *_tile, int _vehicleId, double _gain);
	CombatRequest(CombatRequestType _type, MAP const *_tile, int _vehicleId);
	CombatRequest(CombatRequestType _type, MAP const* _tile);

};

struct Production
{
	robin_hood::unordered_flat_set<MAP *> unavailableBuildSites;
	
	std::vector<TerraformingRequest> terraformingRequests;
	std::vector<ConvoyRequest> convoyRequests;
	
	// combat unit demands
	
	robin_hood::unordered_flat_map<int, double> basePoliceGains;
	robin_hood::unordered_flat_map<int, double> baseProtectionGains;
	robin_hood::unordered_flat_map<int, double> enemyBaseCaptureGains;
	std::vector<EnemyStackInfo *> untargetedEnemyStackInfos;
	
	void clear()
	{
		unavailableBuildSites.clear();
		basePoliceGains.clear();
		baseProtectionGains.clear();
		enemyBaseCaptureGains.clear();
		untargetedEnemyStackInfos.clear();
	}
	
};

struct UnloadRequest
{
	int vehiclePad0;
	MAP *destination;
	MAP *unboardLocation;
	
	UnloadRequest(int _vehicleId, MAP *_destination, MAP *_unboardLocation);
	int getVehicleId();
	
};
struct TransitRequest
{
	int vehiclePad0;
	MAP *origin;
	MAP *destination;
	int seaTransportVehiclePad0 = -1;
	
	TransitRequest(int _vehicleId, MAP *_origin, MAP *_destination);
	int getVehicleId();
	void setSeaTransportVehicleId(int seaTransportVehicleId);
	int getSeaTransportVehicleId();
	bool isFulfilled();
	
};
struct TransportControl
{
	robin_hood::unordered_flat_map<int, std::vector<UnloadRequest>> unloadRequests;
	std::vector<TransitRequest> transitRequests;
	
	void clear();
	void addUnloadRequest(int seaTransportVehicleId, int vehicleId, MAP *destination, MAP *unboardLocation);
	std::vector<UnloadRequest> getSeaTransportUnloadRequests(int seaTransportVehicleId);
	
};

struct Data
{
	// global variables
	
	double podGain;
	double newBaseGain;
	double psiVehicleRatio;

	// map data
	
	std::vector<TileInfo> tileInfos;
	robin_hood::unordered_flat_map<int, int> regionAreas;
	robin_hood::unordered_flat_set<int> enemyRegions;
	std::vector<MAP *> monoliths;
	std::vector<MAP *> pods;
	
	// pad_0 to vehicleId mapping
	// [pad0] = vehicleId
	robin_hood::unordered_flat_map<int, int> pad0VehicleIds;
	// current vehicles
	// [pad0] = vehicle
	std::array<VEH, MaxVehModNum> savedVehicles;
	
	// bases, bunkers, defenseLocations
	
	std::array<BaseInfo, MaxBaseNum> baseInfos;
	void resetBase(int  baseId)
	{
		baseInfos.at(baseId).reset();
	}
	robin_hood::unordered_flat_map<MAP const *, BunkerInfo> bunkerInfos;
	robin_hood::unordered_flat_map<MAP const *, DefendData> defendLocations;

	// faction infos
	
	std::array<FactionInfo, MaxPlayerNum> factionInfos;
	double playerPsiOffenseProportion;
	double playerPsiDefenseProportion;
	double enemyPsiOffenseProportion;
	double enemyPsiDefenseProportion;
	
	// statistical data
	
	double averageCitizenMineralIntake;
	double averageCitizenEconomyIntake;
	double averageCitizenLabsIntake;
	double averageCitizenPsychIntake;
	double averageCitizenMineralIntake2;
	double averageCitizenEconomyIntake2;
	double averageCitizenLabsIntake2;
	double averageCitizenPsychIntake2;
	double averageCitizenResourceIncome;
	
	double averageWorkerNutrientIntake2;
	double averageWorkerMineralIntake2;
	double averageWorkerEnergyIntake2;
	double averageWorkerResourceIncome;
	
	// development parameters
	
	double developmentScale;
	
	// global combat values
	
	int maxConOffenseValue;
	int maxConDefenseValue;
	double avgConOffenseValue;
	double avgConDefenseValue;
	
	// transports assigned passengers and capacity
	// Engine has passenger -> transport relationship but for transport it is pretty difficult to find its passengers. Here is the cashed passengers.
	
	robin_hood::unordered_flat_map<int, std::vector<int>> allTransportPassengers;
	
	// player combat data
	
	// combat data
	
	std::array<double, MaxProtoNum> unitDestructionGains;
	std::vector<int> rivalFactionIds;
	std::vector<int> unfriendlyFactionIds;
	std::vector<int> hostileFactionIds;
	// each to each unit combat effects
	// [attackerFactionId][attackerUnitId][defenderFactionId][defenderUnitId][combatMode] = combatEffect
	CombatEffectTable combatEffectTable;
	
	// enemy stacks
	robin_hood::unordered_flat_map<MAP const*, EnemyStackInfo> enemyStacks;
	bool hasEnemyStack(MAP const* tile) const
	{
		return enemyStacks.find(tile) != enemyStacks.end();
	}
	bool isEnemyStackAt(MAP const* tile) const
	{
		return enemyStacks.find(tile) != enemyStacks.end();
	}
	EnemyStackInfo &getEnemyStackInfo(MAP const* tile)
	{
		return enemyStacks.at(tile);
	}
	
	// unprotected enemy bases
	std::vector<int> emptyEnemyBaseIds;
	robin_hood::unordered_flat_set<MAP const*> emptyEnemyBaseTiles;
	bool isEmptyEnemyBaseAt(MAP const* tile) const
	{
		return emptyEnemyBaseTiles.find(tile) != emptyEnemyBaseTiles.end();
	}
	
	// best units
	robin_hood::unordered_flat_map<int, double> unitWeightedEffects;
	std::vector<double> globalAverageUnitEffects = std::vector<double>(2 * MaxProtoFactionNum);
	
	// other global variables
	
	std::vector<int> baseIds;
	std::vector<int> vehicleIds;
	std::vector<int> combatVehicleIds;
	std::vector<int> scoutVehicleIds;
	std::vector<int> outsideCombatVehicleIds;
	std::vector<int> combatUnitIds;
	std::vector<int> colonyUnitIds;
	std::vector<int> formerUnitIds;
	std::vector<int> landColonyUnitIds;
	std::vector<int> seaColonyUnitIds;
	std::vector<int> airColonyUnitIds;
	std::vector<int> colonyVehicleIds;
	std::vector<int> formerVehicleIds;
	std::vector<int> airFormerVehicleIds;
	robin_hood::unordered_flat_set<int> unfriendlyEndangeredVehicleIds;
	robin_hood::unordered_flat_set<int> hostileEndangeredVehicleIds;
	robin_hood::unordered_flat_set<int> artilleryEndangeredVehicleIds;
	robin_hood::unordered_flat_map<int, std::vector<int>> landFormerVehicleIds;
	robin_hood::unordered_flat_map<int, std::vector<int>> seaFormerVehicleIds;
	// sea transports by clusters
	robin_hood::unordered_flat_map<int, std::vector<int>> seaTransportVehicleIds;
	robin_hood::unordered_flat_map<int, std::vector<int>> regionSurfaceCombatVehicleIds;
	robin_hood::unordered_flat_map<int, std::vector<int>> regionSurfaceScoutVehicleIds;
	int totalMineralIntake2;
	int maxMineralSurplus;
	int maxMineralIntake2;
	int bestLandUnitId;
	int bestSeaUnitId;
	int bestAirUnitId;
	std::vector<int> landCombatUnitIds;
	std::vector<int> seaCombatUnitIds;
	std::vector<int> airCombatUnitIds;
	std::vector<int> landAndAirCombatUnitIds;
	std::vector<int> seaAndAirCombatUnitIds;
	robin_hood::unordered_flat_map<int, int> seaTransportRequestCounts;
	TransportControl transportControl;
	robin_hood::unordered_flat_map<int, TaskHeap> tasks;
	Production production;
	int netIncome;
	int grossIncome;
	
	// police 2x unit available
	
	bool police2xUnitAvailable;
	
	// reset all variables
	
	void clear();
	
	// access global data arrays

	TileInfo &getTileInfo(int tileIndex);
	TileInfo &getTileInfo(MAP const* tile);
	TileInfo &getBaseTileInfo(int baseId);
	TileInfo &getVehicleTileInfo(int vehicleId);
	bool isSea(MAP const* tile);
	bool isLand(MAP const* tile);
	bool isSeaUnitAllowed(MAP const* tile, int factionId);
	bool isLandUnitAllowed(MAP const* tile);

	BaseInfo &getBaseInfo(int baseId);
	BunkerInfo &getBunkerInfo(MAP const* tile);
	
	// utility methods
	
	void addSeaTransportRequest(int seaCluster);
	
};

extern Data aiData;
extern int aiFactionId;
extern MFaction *aiMFaction;
extern Faction *aiFaction;
extern FactionInfo *aiFactionInfo;

bool isWarzone(MAP const *tile);

struct MutualLoss
{
	double own = 0.0;
	double foe = 0.0;
	
	void accumulate(MutualLoss &mutualLoss);
	
};

bool isVendettaStoppedWith(int enemyFactionId);
double getVehicleDestructionGain(int vehicleId);



void clearPlayerFactionReferences();
void setPlayerFactionReferences(int factionId);

void computeUnitDestructionGains();

int getVehicleIdByPad0(int pad0);
VEH *getVehicleByPad0(int pad0);
MAP *getClosestPod(int vehicleId);
int getFactionMaxConOffenseValue(int factionId);
int getFactionMaxConDefenseValue(int factionId);
std::array<int, 3> getFactionFastestTriadChassisIds(int factionId);
bool isWithinAlienArtilleryRange(int vehicleId);
bool isWtpEnabledFaction(int factionId);
bool compareIdIntValueAscending( IdIntValue &a,  IdIntValue &b);
bool compareMapIntValueAscending( MapIntValue &a,  MapIntValue &b);
bool compareIdIntValueDescending( IdIntValue &a,  IdIntValue &b);
bool compareIdDoubleValueAscending( IdDoubleValue &a,  IdDoubleValue &b);
bool compareIdDoubleValueDescending( IdDoubleValue &a,  IdDoubleValue &b);
bool isOffensiveUnit(int unitId, int factionId);
bool isOffensiveVehicle(int vehicleId);
double getExponentialCoefficient(double scale, double value);
double getStatisticalBaseSize(double age);
double getBaseEstimatedSize(double currentSize, double currentAge, double futureAge);
double getBaseStatisticalWorkerCount(double age);
double getBaseStatisticalProportionalWorkerCountIncrease(double currentAge, double futureAge);
int getBaseProjectedSize(int baseId, int turns);
int getBaseGrowthTime(int baseId, int targetSize);
double getResourceScore(double nutrient, double mineral, double energy);
double getResourceScore(double mineral, double energy);
int getBaseFoundingTurn(int baseId);
int getBaseAge(int baseId);
double getBaseSizeGrowth(int baseId);
double getBonusDelayEffectCoefficient(double delay);
double getBuildTimeEffectCoefficient(double delay);
int getUnitGroup(int unitId, bool airbase);
double getHarmonicMean(std::vector<std::vector<double>> parameters);
std::vector<MoveAction> getMoveActions(MovementType movementType, MAP *origin, int initialMovementAllowance, bool regardObstacle, bool regardZoc);
std::vector<MoveAction> getVehicleMoveActions(int vehicleId, bool regardObstacle);
robin_hood::unordered_flat_map<MAP *, double> getPotentialMeleeAttackTargets(int vehicleId);
robin_hood::unordered_flat_map<MAP *, double> getPotentialArtilleryAttackTargets(int vehicleId);
std::vector<AttackAction> getMeleeAttackActions(int vehicleId, bool regardObstacle, bool requireEnemy);
std::vector<AttackAction> getArtilleryAttackActions(int vehicleId, bool regardObstacle, bool requireEnemy);
robin_hood::unordered_flat_map<int, double> getMeleeAttackLocations(int vehicleId);
robin_hood::unordered_flat_set<int> getArtilleryAttackLocations(int vehicleId);
void disbandOrversupportedVehicles(int factionId);
void disbandUnneededVehicles();
bool isUnitCanCaptureBase(int unitId, MAP *baseTile);
int getCombatUnitTrueCost(int unitId);
int getCombatVehicleTrueCost(int vehicleId);
double getGain(double bonus, double income, double incomeGrowth, double incomeGrowth2);
double getGainDelay(double gain, double delay);
double getGainTimeInterval(double gain, double beginTime, double endTime);
double getGainRepetion(double gain, double probability, double period);
double getGainBonus(double bonus);
double getGainIncome(double income);
double getGainIncomeGrowth(double incomeGrowth);
double getGainIncomeGrowth2(double incomeGrowth2);
double getBaseIncome(int baseId, bool withNutrients = false);
double getAverageBaseIncome();
double getBaseCitizenIncome(int baseId);
double getMeanBaseIncome(double age);
double getMeanBaseGain(double age);
double getNewBaseGain();
double getLandColonyGain();
double getBaseGain(int popSize, int nutrientCostFactor, Resource & baseIntake2);
double getBaseGain(int baseId, Resource baseIntake2);
double getBaseGain(int baseId);
double getBaseValue(int baseId);
double getBaseImprovementGain(int baseId, Resource const &oldBaseIntake2, Resource const &newBaseIntake2);
COMBAT_TYPE getWeaponType(int vehicleId);
COMBAT_TYPE getArmorType(int vehicleId);
CombatStrength getMeleeAttackCombatStrength(int vehicleId);
void assignVehiclesToTransports();
bool isUnitCanMeleeAttack(int factionId, int unitId, MAP const *position, MAP const *target, bool checkNeedlejetInFlight);
bool isVehicleCanMeleeAttack(int vehicleId, MAP const *position, MAP const *target, bool checkNeedlejetInFlight);
bool isUnitCanArtilleryAttack(int unitId, MAP const *position);
bool isVehicleCanArtilleryAttack(int vehicleId, MAP const *position);
double getBaseExtraWorkerGain(int baseId);
MapDoubleValue getMeleeAttackPosition(int unitId, MAP const *origin, MAP const *target);
MapDoubleValue getMeleeAttackPosition(int vehicleId, MAP const *target);
MapDoubleValue getArtilleryAttackPosition(int unitId, MAP const *origin, MAP const *target);
MapDoubleValue getArtilleryAttackPosition(int vehicleId, MAP const *target);
double getWinningProbability(double combatEffect);
double getWinningHealthRatio(double combatEffect);
double getSensorOffenseMultiplier(int factionId, MAP const *tile);
double getSensorDefenseMultiplier(int factionId, MAP const *tile);
double getUnitMeleeOffenseStrengthMultipler(int attackerFactionId, int attackerUnitId, int defenderFactionId, int defenderUnitId, MAP const *tile, bool exactLocation);
double getUnitArtilleryOffenseStrengthMultipler(int attackerFactionId, int attackerUnitId, int defenderFactionId, int defenderUnitId, MAP const *tile, bool exactLocation);
int getBaseRequiredPolicePower(int baseId);
double getUnitDestructionGain(int unitId);
double getProportionalCoefficient(double minValue, double maxValue, double value);
int generatePad0FromVehicleId(int vehicleId);
int getInitialVehicleIdByPad0(int pad0);
void populateVehiclePad0Map(bool initialize = false);
double getCombatGain(int attackerVehicleId, int defenderVehicleId, EngagementMode engagementMode, MAP const *attackerTile, MAP const *defenderTile, double attackerHealth, double defenderHealth);
char const *getVehiclePad0LocationNameString(int vehicleId);
double getCNDProbability(double value);
double getWinProbability(double destroyedWeight1, double destroyedWeight2, double remainingWeight1);
double getMutualCombatGain(double attackerDestructionGain, double defenderDestructionGain, double combatEffect);
double getBombardmentGain(double defenderDestructionGain, double relativeBombardmentDamage);
double getAssignedTaskProtectDefendGain(int vehicleId);
void createFakeVehicle(int vehicleId, int factionId, int unitId);

