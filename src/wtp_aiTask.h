#pragma once

#include "engine.h"

enum TaskType
{
	TT_NONE,					//  0
	TT_KILL,					//  1
	TT_SKIP,					//  2
	TT_BUILD,					// 	3
	TT_LOAD,					//  4
	TT_BOARD,					//  5
	TT_UNLOAD,					//  6
	TT_UNBOARD,					//  7
	TT_TERRAFORM,				//  8
	TT_ORDER,					//  9
	TT_HOLD,					// 10
	TT_ALERT,					// 11
	TT_MOVE,					// 12
	TT_ARTIFACT_CONTRIBUTE,		// 13
	TT_MELEE_ATTACK,			// 14
	TT_ARTILLERY_ATTACK,		// 15
	TT_CONVOY,					// 16
};

struct Task;

struct TaskHeap
{
	std::vector<Task> tasks {};

	void add(Task const&task);
	Task *get();

};

struct Task
{
	int vehiclePad0;
	TaskType type;
	MAP *destination;
	MAP *attackTarget;
	int order;
	int terraformingAction;
	double priority = 0.0;
	int baseId = -1;

	Task(int _vehicleId, TaskType _type, MAP *_destination, MAP *_attackTarget, int _order, int _terraformingAction);
	Task(int const _vehicleId, TaskType const _type, MAP const *_destination, MAP const *_attackTarget);
	Task(int const _vehicleId, TaskType const _type, MAP const *_destination);
	Task(int const _vehicleId, TaskType const _type);

	bool operator<(Task const &other) const;

	static char const *getTaskTypeName(TaskType taskType);
	char const *typeName() const;
	int getVehicleId() const;
	VEH *getTaskVehicle() const;
	void clearDestination();
	void setDestination(MAP const *_destination);
	MAP const *getDestination() const;
	MAP const *getAttackTarget() const;
	int getDestinationRange() const;
	char const *toString() const;

	int execute() const;
	int execute(int vehicleId) const;
	int executeAction(int vehicleId);
	int executeNone(int vehicleId);
	int executeKill(int vehicleId);
	int executeSkip(int vehicleId);
	int executeBuild(int vehicleId);
	int executeLoad(int vehicleId);
	int executeBoard(int vehicleId);
	int executeUnload(int vehicleId);
	int executeUnboard(int vehicleId);
	int executeTerraformingAction(int vehicleId);
	int executeOrder(int vehicleId);
	int executeHold(int vehicleId);
	int executeAlert(int vehicleId);
	int executeMove(int vehicleId);
	int executeArtifactContribute(int vehicleId);
	int executeAttack(int vehicleId);
	int executeLongRangeFire(int vehicleId);
	int executeConvoy(int vehicleId);

};

TaskHeap &getTaskHeap(int const vehicleId);
void setTask(Task const &task);
bool hasTask(int vehicleId);
void deleteVehicleTasks(int vehicleId);
Task *getTask(int vehicleId);

