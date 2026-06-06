
#include "wtp_aiTask.h"

#include "wtp_game.h"
#include "wtp_aiMove.h"
#include "wtp_aiMoveCombat.h"

static char const *taskTypeNames[]
{
	"NONE",				//  0
	"KILL",				//  1
	"SKIP",				//  2
	"BUIL",				// 	3
	"LOAD",				//  4
	"BOAR",				//  5
	"UNLO",				//  6
	"UNBO",				//  7
	"TERR",				//  8
	"ORDE",				//  9
	"HOLD",				// 10
	"ALER",				// 11
	"MOVE",				// 12
	"ARTC",				// 13
	"MELE",				// 14
	"ARTI",				// 15
	"CONV",				// 16
};

// TaskHeap

void TaskHeap::add(Task const &task)
{
	tasks.push_back(task);
}

Task *TaskHeap::get()
{
	if (tasks.empty())
		return nullptr;

	Task &highestPriorityTask = *std::max_element(tasks.begin(), tasks.end());
	return &highestPriorityTask;

}

// Task

Task::Task(int const _vehicleId, TaskType const _type, MAP const *_destination, MAP const *_attackTarget, int const _order, int const _terraformingAction)
: vehiclePad0(Vehs[_vehicleId].pad_0), type(_type), destination(_destination), attackTarget(_attackTarget), order(_order), terraformingAction(_terraformingAction)
{}
Task::Task(int const _vehicleId, TaskType const _type, MAP const *_destination, MAP const *_attackTarget)
: Task(_vehicleId, _type, _destination, _attackTarget, -1, -1)
{}
Task::Task(int const _vehicleId, TaskType const _type, MAP const *_destination)
: Task(_vehicleId, _type, _destination, nullptr, -1, -1)
{}
Task::Task(int const _vehicleId, TaskType const _type)
: Task(_vehicleId, _type, nullptr, nullptr, -1, -1)
{}

bool Task::operator<(Task const &other) const
{
	return priority < other.priority;
}

char const *Task::getTaskTypeName(TaskType const taskType)
{
	return taskTypeNames[taskType];
}

char const *Task::typeName() const
{
	return taskTypeNames[this->type];
}

int Task::getVehicleId() const
{
	for (int vehicleId = 0; vehicleId < *VehCount; vehicleId++)
	{
		VEH *vehicle = getVehicle(vehicleId);
		
		if (vehicle->pad_0 == vehiclePad0)
		{
			return vehicleId;
		}
		
	}
	
	return -1;
	
}

VEH *Task::getTaskVehicle() const
{
	for (int vehicleId = 0; vehicleId < *VehCount; vehicleId++)
	{
		VEH *vehicle = getVehicle(vehicleId);

		if (vehicle->pad_0 == vehiclePad0)
		{
			return vehicle;
		}

	}

	return nullptr;

}

void Task::clearDestination()
{
	destination = nullptr;
	attackTarget = nullptr;
}

void Task::setDestination(MAP const *_destination)
{
	assert(_destination >= *MapTiles && _destination < *MapTiles + *MapAreaTiles);
	this->destination = _destination;
}

/**
Returns vehicle destination if specified.
Otherwise, current vehicle location.
*/
MAP const *Task::getDestination() const
{
	int vehicleId = getVehicleId();
	
	// unknown vehicle
	
	if (vehicleId == -1)
		return nullptr;
	
	// return destination if set
	
	if (destination != nullptr)
	{
		return destination;
	}
	
	// otherwise, return vehicle location
	
	return getVehicleMapTile(vehicleId);
	
}

MAP const *Task::getAttackTarget() const
{
	int vehicleId = getVehicleId();

	if (vehicleId == -1)
		return nullptr;
	
	if (attackTarget != nullptr)
	{
		return attackTarget;
	}
	else
	{
		return nullptr;
	}
	
}

int Task::getDestinationRange() const
{
	// no range for no destination

	if (destination == nullptr)
		return 0;

	assert(isOnMap(destination));
	
	int x = getX(destination);
	int y = getY(destination);

	int const vehicleId = getVehicleId();

	if (vehicleId == -1)
	{
		debug("Task::getDestinationRange()\n");
		debug("\tERROR: cannot find vehicle id by pad_0.\n");
		return 0;
	}

	VEH const *vehicle = getVehicle(vehicleId);

	return map_range(vehicle->x, vehicle->y, x, y);

}

/*
returns reference to static buffer
not thread-safe, not reentrant
uses rotating buffers to allow up to 10 calls withing a single debug statement
*/
char const *Task::toString() const
{
	static int constexpr BUFFER_COUNT = 10;
	static int constexpr BUFFER_SIZE = 100;
	
    static char buffers[BUFFER_COUNT][BUFFER_SIZE];
    static int index = 0;
    
    int const i = index;
    index = (index + 1) % BUFFER_COUNT;
    
    int const vehicleId = getVehicleIdByPad0(vehiclePad0);
    
	if (vehicleId == -1)
	{
		snprintf(buffers[i], sizeof(buffers[i]), "ERROR: vehicleId is not found for pad0: {%4d}", vehiclePad0);
	}
	else
	{
		VEH const &vehicle = Vehs[vehicleId];
		snprintf
		(
			buffers[i],
			sizeof(buffers[i]),
			"{%4d} (%3d,%3d) %-32s %-4s -> %s/%s"
			, getInitialVehicleIdByPad0(vehiclePad0), vehicle.x, vehicle.y, vehicle.name()
			, typeName()
			, getLocationString(destination)
			, getLocationString(attackTarget)
		);
	}
	
    return buffers[i];
    
}

int Task::execute() const
{
	debug("Task::execute()\n");

	int const vehicleId = getVehicleId();

	if (vehicleId == -1)
	{
		debug("ERROR: cannot find vehicle id by pad_0.\n");
		return EM_DONE;
	}

	return execute(vehicleId);

}

int Task::execute(int vehicleId) const
{
	debug("Task::execute(%d)\n", vehicleId);

	VEH const *vehicle = getVehicle(vehicleId);
	MAP const *vehicleTile = getVehicleMapTile(vehicleId);

	// move

	if (destination != nullptr && vehicleTile != destination)
	{
		// proceed to destination

		debug("[%4d] %s -> %s\n", vehicleId, getLocationString({vehicle->x, vehicle->y}), getLocationString(destination));

		if (isCombatVehicle(vehicleId))
		{
			setMoveTo(vehicleId, destination);
		}
		else
		{
			setSafeMoveTo(vehicleId, destination);
		}

		// make sure to declare vendetta if moving into neutral base

		if (getRange(vehicleTile, destination) == 1 && getVehicleRemainingMoves(vehicleId) >= Rules->move_rate_roads && isBaseAt(destination))
		{
			int const destinationOwner = static_cast<uint8_t>(destination->owner);

			if (destinationOwner != -1 && isNeutral(vehicle->faction_id, destinationOwner))
			{
				enemies_war(vehicle->faction_id, destinationOwner);
			}

		}

		return EM_SYNC;

	}

	// execute action

	return executeAction(vehicleId);

}

int Task::executeAction(int const vehicleId)
{
	debug("Task::executeAction(%d)\n", vehicleId);
	
	switch (type)
	{
	case TT_NONE:
		return executeNone(vehicleId);

	case TT_KILL:
		return executeKill(vehicleId);

	case TT_SKIP:
		return executeSkip(vehicleId);

	case TT_BUILD:
		return executeBuild(vehicleId);

	case TT_LOAD:
		return executeLoad(vehicleId);

	case TT_BOARD:
		return executeBoard(vehicleId);

	case TT_UNLOAD:
		return executeUnload(vehicleId);

	case TT_UNBOARD:
		return executeUnboard(vehicleId);

	case TT_TERRAFORM:
		return executeTerraformingAction(vehicleId);

	case TT_ORDER:
		return executeOrder(vehicleId);

	case TT_HOLD:
		return executeHold(vehicleId);

	case TT_ALERT:
		return executeAlert(vehicleId);

	case TT_MOVE:
		return executeMove(vehicleId);

	case TT_ARTIFACT_CONTRIBUTE:
		return executeArtifactContribute(vehicleId);

	case TT_MELEE_ATTACK:
		return executeAttack(vehicleId);

	case TT_ARTILLERY_ATTACK:
		return executeLongRangeFire(vehicleId);

	case TT_CONVOY:
		return executeConvoy(vehicleId);

	default:
		return EM_DONE;
	
	}
	
}

int Task::executeNone(int)
{
	debug("Task::executeNone\n");

	return EM_SYNC;

}

int Task::executeKill(int vehicleId)
{
	debug("Task::executeKill\n");

	mod_veh_kill(vehicleId);
	return EM_DONE;

}

int Task::executeSkip(int vehicleId)
{
	debug("Task::executeSkip\n");

	mod_veh_skip(vehicleId);
	return EM_DONE;

}

int Task::executeBuild(int vehicleId)
{
	debug("Task::executeBuild\n");

	VEH *vehicle = getVehicle(vehicleId);
	MAP *vehicleTile = getVehicleMapTile(vehicleId);

	// check there is no adjacent bases

	for (MAP *adjacentTile : getBaseAdjacentTiles(vehicle->x, vehicle->y, true))
	{
		// base in tile

		if (map_has_item(adjacentTile, BIT_BASE_IN_TILE))
		{
			mod_veh_skip(vehicleId);
			return EM_DONE;
		}

	}

	// remove fungus and rocks if any before building

	vehicleTile->items &= ~((uint32_t)BIT_FUNGUS);
	if (map_rockiness(vehicleTile) == 2)
	{
		vehicleTile->val3 &= ~((byte)TILE_ROCKY);
		vehicleTile->val3 |= (byte)TILE_ROLLING;
	}

	// make all tiles visible around base

	for (MAP *tile : getBaseRadiusTiles(vehicle->x, vehicle->y, true))
	{
		setMapTileVisibleToFaction(tile, vehicle->faction_id);
	}

	// build base

	action_build(vehicleId, 0);

	return EM_DONE;

}

int Task::executeLoad(int seaTransportVehicleId)
{
	debug("Task::executeLoad\n");
	
	VEH *seaTransportVehicle = getVehicle(seaTransportVehicleId);
	
	// retrieve transit request
	
	std::vector<TransitRequest *> transitRequests;

	for (TransitRequest &transitRequest : aiData.transportControl.transitRequests)
	{
		if (transitRequest.getSeaTransportVehicleId() == seaTransportVehicleId)
		{
			transitRequests.push_back(&transitRequest);
		}
		
	}
	
	for (TransitRequest *transitRequest : transitRequests)
	{
		int passengerVehicleId = transitRequest->getVehicleId();
		
		if (passengerVehicleId == -1)
		{
			debug("\tpassenger is not found\n");
			return EM_DONE;
		}
		
		VEH *passengerVehicle = getVehicle(passengerVehicleId);
		
		// passenger should be adjacent to the transport
		
		int range = map_range(passengerVehicle->x, passengerVehicle->y, seaTransportVehicle->x, seaTransportVehicle->y);
		
		if (range > 1)
		{
			debug("\tpassenger range=%2d\n", range);
			return EM_DONE;
		}
		
		// board passenger
		
		debug("\tboard passenger: [%3d] %s->%s\n", passengerVehicleId, getLocationString({passengerVehicle->x, passengerVehicle->y}), getLocationString({seaTransportVehicle->x, seaTransportVehicle->y}));
		veh_put(passengerVehicleId, seaTransportVehicle->x, seaTransportVehicle->y);
		board(passengerVehicleId, seaTransportVehicleId);
		
	}
	
	mod_veh_skip(seaTransportVehicleId);
	return EM_SYNC;

}

int Task::executeBoard(int vehicleId)
{
	debug("Task::executeBoard\n");

	mod_veh_skip(vehicleId);
	
	return EM_SYNC;

}

int Task::executeUnload(int vehicleId)
{
	debug("Task::executeUnload\n");

	MAP *vehicleTile = getVehicleMapTile(vehicleId);

	// wake up passengers and direct to unboard location

	for (UnloadRequest &unloadRequest : aiData.transportControl.getSeaTransportUnloadRequests(vehicleId))
	{
		// correct location

		if (unloadRequest.destination != vehicleTile)
			continue;

		// passenger

		int passengerVehicleId = unloadRequest.getVehicleId();

		if (passengerVehicleId == -1)
			continue;

		// verify passenger is indeed a passenger of this transport

		int vehicleTransportId = getVehicleTransportId(passengerVehicleId);

		if (vehicleTransportId != vehicleId)
			continue;

		// move to unboarding location

		executeUnboard(passengerVehicleId);

	}

	// wait

	mod_veh_skip(vehicleId);
	return EM_DONE;

}

int Task::executeUnboard(int vehicleId)
{
	setMoveTo(vehicleId, destination);
	return EM_DONE;

}

int Task::executeTerraformingAction(int vehicleId)
{
	debug("Task::executeTerraformingAction\n");

	// execute terraforming action

	setTerraformingAction(vehicleId, this->terraformingAction);
	return EM_SYNC;

}

int Task::executeOrder(int vehicleId)
{
	debug("Task::executeOrder\n");

	// set order

	setVehicleOrder(vehicleId, order);
	return EM_DONE;

}

int Task::executeHold(int vehicleId)
{
	debug("Task::executeHold\n");

	MAP *vehicleTile = getVehicleMapTile(vehicleId);

	// set order

	setVehicleOrder(vehicleId, ORDER_HOLD);

	if (isLandArtilleryVehicle(vehicleId) && map_has_item(vehicleTile, BIT_BASE_IN_TILE))
	{
		// set land artillery on alert

		setVehicleOnAlert(vehicleId);

	}

	// complete move

	return EM_DONE;

}

int Task::executeAlert(int vehicleId)
{
	debug("Task::executeAlert\n");

	// set order

	setVehicleOnAlert(vehicleId);

	// complete move

	return EM_DONE;

}

int Task::executeMove(int vehicleId)
{
	debug("Task::executeMove\n");

	// set order

	mod_veh_skip(vehicleId);
	return EM_DONE;

}

int Task::executeArtifactContribute(int vehicleId)
{
	debug("Task::executeArtifactContribute\n");

	MAP *vechileTile = getVehicleMapTile(vehicleId);

	// in own base

	int baseId = getBaseAt(vechileTile);
	if (baseId != -1)
	{
		BASE *base = getBase(baseId);

		if (base->faction_id == aiFactionId)
		{
			// base is building project

			if (isBaseBuildingProject(baseId))
			{
				// destroy vehicle and contribute to project

				killVehicle(vehicleId);
				base->minerals_accumulated += 50;

				return EM_DONE;

			}

		}

	}

	// otherwise, skip

	mod_veh_skip(vehicleId);
	return EM_DONE;

}

int Task::executeAttack(int vehicleId)
{
	debug("Task::executeAttack\n");
	
	// check attackLocation
	
	if (attackTarget == nullptr)
	{
		mod_veh_skip(vehicleId);
		return EM_DONE;
	}
	
	assert(isOnMap(attackTarget));
	
	// get base and vehicle at attackLocation
	
	bool baseAtAttackLocation = map_has_item(attackTarget, BIT_BASE_IN_TILE);
	bool vehicleAtAttackLocation = map_has_item(attackTarget, BIT_VEH_IN_TILE);
	
	// empty base - move in
	
	if (baseAtAttackLocation && !vehicleAtAttackLocation)
	{
		setMoveTo(vehicleId, attackTarget);
		return EM_SYNC;
	}
	
	// nobody is there
	
	if (!vehicleAtAttackLocation)
	{
		mod_veh_skip(vehicleId);
		return EM_DONE;
	}
	
	// get defender
	
	int defenderVehicleId = veh_at(getX(attackTarget), getY(attackTarget));
	
	if (defenderVehicleId == -1)
		return EM_DONE;
	
	// proceed with attack
	
	setMoveTo(vehicleId, attackTarget);
	return EM_SYNC;
	
}

int Task::executeLongRangeFire(int vehicleId)
{
	debug("Task::executeLongRangeFire\n");

	// check attackLocation

	if (attackTarget == nullptr)
	{
		mod_veh_skip(vehicleId);
		return EM_DONE;
	}

	debug("\t^ %s\n", getLocationString(attackTarget));

	// get base and vehicle at attackLocation

	bool baseAtAttackLocation = map_has_item(attackTarget, BIT_BASE_IN_TILE);
	bool vehicleAtAttackLocation = map_has_item(attackTarget, BIT_VEH_IN_TILE);

	// empty base - move in

	if (baseAtAttackLocation && !vehicleAtAttackLocation)
	{
		setMoveTo(vehicleId, attackTarget);
		return EM_SYNC;
	}

	// nobody is there

	if (!vehicleAtAttackLocation)
	{
		mod_veh_skip(vehicleId);
		return EM_DONE;
	}

	// long range fire

	longRangeFire(vehicleId, attackTarget);
	return EM_SYNC;

}

int Task::executeConvoy(int vehicleId)
{
	debug("Task::executeConvoy\n");
	
	// set order
	
	setVehicleOrder(vehicleId, ORDER_CONVOY);
	return EM_DONE;
	
}

// static functions

bool compareTaskPriorityDescending(Task const &a, Task const &b)
{
	return a.priority > b.priority;
}

void setTask(Task const &task)
{
	int const vehicleId = task.getVehicleId();
	VEH const &vehicle = Vehs[vehicleId];
	debug("setTask( vehicleId=%4d type=%2d )\n", vehicleId, task.type);

	if (aiData.tasks.find(vehicle.pad_0) == aiData.tasks.end())
	{
		aiData.tasks.emplace(vehicle.pad_0, TaskHeap());
	}

	TaskHeap &vehicleTaskHeap = aiData.tasks.at(vehicle.pad_0);
	vehicleTaskHeap.add(task);

}

bool hasTask(int const vehicleId)
{
	return (aiData.tasks.find(Vehs[vehicleId].pad_0) != aiData.tasks.end());
}

void deleteVehicleTasks(int const vehicleId)
{
	VEH const &vehicle = Vehs[vehicleId];
	aiData.tasks.erase(vehicle.pad_0);
}

// returns vehicle task heap
// automatically constructs TaskHeap if was not exist
TaskHeap &getTaskHeap(int const vehicleId)
{
	int const vehiclePad0 = Vehs[vehicleId].pad_0;

	if (aiData.tasks.find(vehiclePad0) == aiData.tasks.end())
	{
		aiData.tasks.emplace(vehiclePad0, TaskHeap());
	}

	return aiData.tasks.at(vehiclePad0);

}

// returns highest priority task
Task *getTask(int const vehicleId)
{
	int const vehiclePad0 = Vehs[vehicleId].pad_0;

	if (aiData.tasks.find(vehiclePad0) == aiData.tasks.end())
		return nullptr;

	return aiData.tasks.at(vehiclePad0).get();

}

