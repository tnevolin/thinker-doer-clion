#pragma once

#include <float.h>

#include "main.h"
#include "engine.h"
#include "wtp_ai_game.h"

const int MAX_REPAIR_DISTANCE = 6;
const int MAX_PODPOP_DISTANCE = 20;

const double MIN_IMMEDIATE_ATTACK_PRIORITY = 1.0;

enum TaskPriorityRestriction
{
	TPR_NONE,
	TPR_ONE,
	TPR_BASE,
	TPR_STACK,
};
/*
Potential combat vehicle action.
*/
struct TaskPriority
{
	int vehicleId;
	double priority;
	TaskPriorityRestriction taskPriorityRestriction;
	TaskType taskType;
	MAP *destination;
	double travelTime = INF;
	// protected base
	int baseId = -1;
	// combat parameters
	CombatMode combatMode = CM_MELEE;
	MAP *attackTarget = nullptr;
	bool destructive = true;
	double effect = 0.0;
	
	TaskPriority(int  _vehicleId, double  _priority, TaskPriorityRestriction  _taskPriorityRestriction, TaskType  _taskType, MAP const *_destination, double  _travelTime, int  _baseId, CombatMode  _combatMode, MAP const *_attackTarget, bool  _destructive, double  _effect)
	: vehicleId(_vehicleId), priority(_priority), taskPriorityRestriction(_taskPriorityRestriction), taskType(_taskType), destination(const_cast<MAP *>(_destination)), travelTime(_travelTime), baseId(_baseId), combatMode(_combatMode), attackTarget(const_cast<MAP *>(_attackTarget)), destructive(_destructive), effect(_effect)
	{}
	
	TaskPriority(int  _vehicleId, double  _priority, TaskPriorityRestriction  _taskPriorityRestriction, TaskType  _taskType, MAP const *_destination)
	: TaskPriority(_vehicleId, _priority, _taskPriorityRestriction, _taskType, _destination, INF, -1, CM_MELEE, nullptr, true, 0.0)
	{}
	
	TaskPriority(int  _vehicleId, double  _priority, TaskPriorityRestriction  _taskPriorityRestriction, TaskType  _taskType, MAP const *_destination, double  _travelTime)
	: TaskPriority(_vehicleId, _priority, _taskPriorityRestriction, _taskType, _destination, _travelTime, -1, CM_MELEE, nullptr, true, 0.0)
	{}
	
	TaskPriority(int  _vehicleId, double  _priority, TaskPriorityRestriction  _taskPriorityRestriction, TaskType  _taskType, MAP const *_destination, double  _travelTime, int  _baseId)
	: TaskPriority(_vehicleId, _priority, _taskPriorityRestriction, _taskType, _destination, _travelTime, _baseId, CM_MELEE, nullptr, true, 0.0)
	{}
	
	TaskPriority(int  _vehicleId, double  _priority, TaskPriorityRestriction  _taskPriorityRestriction, TaskType  _taskType, MAP const *_destination, double  _travelTime, CombatMode  _combatMode, MAP const *_attackTarget, bool  _destructive, double  _effect)
	: TaskPriority(_vehicleId, _priority, _taskPriorityRestriction, _taskType, _destination, _travelTime, -1, _combatMode, _attackTarget, _destructive, _effect)
	{}
	
};

struct ProvidedEffect
{
	double destructive = 0.0;
	double bombardment = 0.0;
	double firstDestructiveAttackTime = DBL_MAX;
	std::vector<TaskPriority *> nonDestructiveTaskPriorities;
	
	void addDestructive(double effect);
	void addBombardment(double effect);
	double getCombined();
	
};

struct AttackActionEffect
{
	int vehicleId;
	CombatMode combatMode;
	MAP *position;
	MAP *target;
	double effect;
	double priority;
};

struct EnemyStackAttackInfo
{
	EnemyStackInfo *enemyStackInfo;
	std::vector<AttackActionEffect> attackActionEffects;
	double combinedEffect;
	double superiority;
	double averageEffect;
	double averagePriority;
	
	EnemyStackAttackInfo(EnemyStackInfo *_enemyStackInfo)
	: enemyStackInfo(_enemyStackInfo)
	{}
	
};

struct CombatAction
{
	int vehicleId = -1;
	double gain;
	MAP *destination;
	int remainingMoves;
	EngagementMode engagementMode;
	MAP *target;
	double hastyCoefficient;
	
	void setMove(int _vehicleId, double _gain, MAP *_destination, int _remainingMoves);
	void setAttack(int _vehicleId, double _gain, MAP *_destination, int _remainingMoves, EngagementMode _engagementMode, MAP *_target, double _hastyCoefficient);
	
};

struct CombatOrder
{
	int vehicleId;
	MAP *tile = nullptr;
	CombatRequest *combatRequest = nullptr;

	explicit CombatOrder(int _vehicleId);

};

void moveCombatStrategy();
void popualteOrders();
void generateRequests();
void generateRepairMonolithRequests();
void generatePodRequests();
void generateDefendBaseRequests();
void generateDefendBunkerRequests();
void generateCaptureBaseRequests();
void generateAttackStackRequests();
void assignRequests();
//
void immediateAttack();
void moveBaseProtectors();
void moveBunkerProtectors();
void moveCombat();
void populatePolice2xTasks(std::vector<TaskPriority> &taskPriorities);
void populatePoliceTasks(std::vector<TaskPriority> &taskPriorities);
void populateEmptyBaseCaptureTasks(std::vector<TaskPriority> &taskPriorities);
void populateEnemyStackAttackTasks(std::vector<TaskPriority> &taskPriorities);
void coordinateAttack();
bool isVehicleAvailable(int vehicleId, bool notAvailableInWarzone);
bool isVehicleAvailable(int vehicleId);
int getVehicleProtectionRange(int vehicleId);
bool compareTaskPriorityDescending( TaskPriority &a,  TaskPriority &b);
double getDuelCombatCostCoefficient(int vehicleId, double effect, double enemyUnitCost);
double getBombardmentCostCoefficient(int vehicleId, double effect, double enemyUnitCost);
bool isPrimaryEffect(int vehicleId, MAP const *enemyStackTile, EnemyStackInfo &enemyStackInfo, CombatMode combatMode);
double getDefendGain(int defenderVehicleId, MAP const *tile, double defenderHealth, int excludeEnemyVehiclePad0 = -1);
double getMeleeAttackGain(int vehicleId, MAP const *destination, MAP const *target, double hastyCoefficient);
double getArtilleryAttackGain(int vehicleId, MAP const *destination, MAP const *target);
CombatAction selectVehicleCombatAction(int vehicleId);
void aiEnemyMoveCombatVehicles();

