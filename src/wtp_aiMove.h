#pragma once

#include "wtp_ai_game.h"

enum ENEMY_MOVE_RETURN_VALUE
{
	EM_SYNC = 0,
	EM_DONE = 1,
};

struct SeaTransit
{
	bool valid = false;
	Transfer *boardTransfer = nullptr;
	Transfer *unboardTransfer = nullptr;
	double travelTime = INF;
	
	void set(Transfer *_boardTransfer, Transfer *_unboardTransfer, double _travelTime);
	void copy(SeaTransit &seaTransit);
	
};

struct VehicleDestination
{
	int vehicleId;
	MAP const *destination;
};

void moveStrategy();
void fixUndesiredTransportDropoff();
void fixUndesiredTransportPickup();
void moveAllStrategy();
void healStrategy();
int enemyMoveVehicle(int vehicleId);
bool transitVehicle(Task const &task);
bool transitLandVehicle(Task const &task);
void balanceVehicleSupport();
int aiEnemyMove(int vehicleId);
MAP *getNearestFriendlyBase(int vehicleId);
MAP *getNearestMonolith(int x, int y, int triad);
Transfer getOptimalPickupTransfer(MAP const *org, MAP const *dst);
Transfer getOptimalDropoffTransfer(MAP const *org, MAP const *dst, int passengerVehicleId, int transportVehicleId);
void setSafeMoveTo(int vehicleId, MAP const *destination);
MapDoubleValue findClosestMonolith(int vehicleId, int maxSearchRange, bool avoidWarzone);
MAP *getSafeLocation(int vehicleId, bool hostile);
int setMoveTo(int vehicleId, MAP const *destination);
int setMoveTo(int vehicleId, const std::vector<MAP const *> &waypoints);
void aiEnemyMoveVehicles();

