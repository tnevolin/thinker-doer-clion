#pragma once

#include <string>
#include <vector>
#include "main.h"

/*
Autoplay (programmatic) mode.

Started from the command line: terranx_mod.exe -autoplay [-turns N] [-stats file.csv] [-savegame file.sav] [-journal file.tsv] [-keepdraw] [-set name=value ...] save.sav
The save is loaded as usual, then every faction (including the one that was human in the save)
is played by the AI. No human input is requested. After the requested number of turns
the game exits.

Statistics are collected in memory (FactionStats) and converted to a single number (positionScore).
The score of every faction is appended to autoplay.log after each round.
A CSV with all collected values is written only when -stats is given.
-journal writes what the factions did, for reviewing AI decisions: one tab-separated record per faction (F: social engineering,
energy allocation, research, treaties), base (B: what it builds), vehicle (V: order and destination) after every round,
and a map of the land tiles (M) at the start, every 50 rounds and at the end.

-set overrides any thinker.ini option for this run only (e.g. -set ai_terraforming_mineralWeight=2.0).
The yield of the AI factions (nutrient surplus, mineral surplus, economy plus labs) is logged after each round,
together with a score that uses fixed reference weights, so no scanned parameter can move the yardstick.

Stop early: hold Ctrl+Pause or create an empty file named autoplay.stop in the game folder.
*/

/*
Everything that describes one faction at a moment in time.
Resource surpluses are summed over the faction bases.
*/
struct FactionStats
{
	int factionId = 0;
	bool alive = false;
	int bases = 0;
	int population = 0;
	int units = 0;
	// split of the units by purpose, every unit is counted in exactly one group
	int unitsCombat = 0;
	int unitsColony = 0;
	int unitsFormer = 0;
	int unitsSupply = 0;
	int unitsProbe = 0;
	int unitsOther = 0;
	// what the bases are building, one base counts once
	int buildColony = 0;
	int buildFormer = 0;
	int buildCombat = 0;
	int buildSupply = 0;
	int buildProbe = 0;
	int buildOtherUnit = 0;
	int buildFacility = 0;
	int buildProject = 0;
	// improvements on the tiles in the faction territory
	int tileFarm = 0;
	int tileMine = 0;
	int tileSolar = 0;
	int tileForest = 0;
	int tileRoad = 0;
	// what the formers are doing at the end of the round: terraforming, traveling to a tile, or neither
	int formerWorking = 0;
	int formerMoving = 0;
	int formerIdle = 0;
	// formers that were traveling to a tile last round, did not arrive, and are not going there now
	int formerRetargets = 0;
	// rockiness of the base tiles: 0 flat, 1 rolling, 2 rocky, summed over the bases
	int baseRockiness = 0;
	int techs = 0;
	int secretProjects = 0;
	int energyCredits = 0;
	int nutrientSurplus = 0;
	int mineralSurplus = 0;
	// mineral intake after multipliers and before unit support is subtracted
	int mineralIntake = 0;
	// energy has no consumption so surplus equals intake
	int energySurplus = 0;
	// split of the energy surplus
	int economy = 0;
	int psych = 0;
	int labs = 0;
	// values returned by the original game score formula
	int gameScore = 0;
	int gameScoreOther = 0;
};

/*
Linear position score weights. The values are placeholders until they are tuned.
*/
struct ScoreWeights
{
	double base = 20.0;
	double population = 5.0;
	double unit = 1.0;
	double tech = 10.0;
	double secretProject = 50.0;
	double energyCredit = 0.01;
	double nutrientSurplus = 1.0;
	double mineralSurplus = 2.0;
	double energySurplus = 1.0;
};

/*
Reference weights of the yield score (mean nutrient surplus, mineral surplus and economy plus labs per faction).
They are deliberately independent of thinker.ini: the AI parameters that are scanned must not change the yardstick.
The raw means are logged too, so any other weights can be applied afterwards.
*/
struct YieldWeights
{
	double nutrient = 1.0;
	double mineral = 1.5;
	double energy = 0.5;
};

struct AutoplayState
{
	bool active = false;
	// replace map redraw functions with no-ops
	bool stubDraws = true;
	// number of full turns to play after the save is loaded
	int turns = 10;
	// optional CSV with every collected value; empty = not written
	std::string statsPath;
	// optional save written when autoplay ends, with the human control and watched faction of the loaded save; empty = none
	std::string savePath;
	// optional tab-separated journal of faction, base and vehicle states after every round; empty = not written
	std::string journalPath;
	// write the defense experiment to the journal (E records)
	bool defenseExperiment = false;
	ScoreWeights weights;
	YieldWeights yieldWeights;
	// "name=value" strings given with -set, already applied on top of the ini files
	std::vector<std::string> overrides;
};

extern AutoplayState autoplay;

// collect the current state of one faction
FactionStats collectFactionStats(int factionId);
// single number to maximize
double positionScore(const FactionStats& stats, const ScoreWeights& weights);

// install quiet-mode hooks (draw stubs, popup suppression); called once at startup
void autoplay_install_hooks();
// called right after the startup save is loaded
void autoplay_after_load();
// called before every AI faction move
void autoplay_poll();
// replaces the human end-of-round phase
void autoplay_end_of_round();
// victory conditions are met
void autoplay_game_over();
// journal: records written during the faction turn; no-op without -journal
bool autoplay_journal_wanted();
// what the production strategy considered for one base (candidates sorted best first)
void autoplay_journal_production(int baseId, int currentChoice, int currentBuildTime, int developmentScale, bool override, int choice,
	const int* items, const double* priorities, int count);
// the threat evaluation of one base (defense computation check)
void autoplay_journal_defend(int baseId, int attackerKinds, double attackerPower, int defenders, bool sufficient, int probeUnitId, double probeEfficiency);
// a bunker cleared by a former because its gain is not positive
void autoplay_journal_bunker_removed(int factionId, int tileOwner, int x, int y, double gain);
// a vehicle assigned to a request (type "defend", "pod"); the vehicle is not moved yet
void autoplay_journal_assignment(char const* type, int vehicleId, int requestX, int requestY, double gain, double travelTime);
// a request left without a vehicle after the assignment
void autoplay_journal_unfulfilled(int factionId, char const* type, int requestX, int requestY, double gain);
// a transit request of a land passenger waiting for a sea transport: passenger class, pickup and destination; transportId is -1 when no transport is assigned
void autoplay_journal_transit(int vehicleId, char const* passengerClass, int originX, int originY, int destinationX, int destinationY, int transportId);
// a Thinker garrison decision for a vehicle without a WTP task: kind 0 stays at its base, kind 1 moves to another base
void autoplay_journal_garrison(int vehicleId, int kind, int destinationX, int destinationY);
// one row of the defense experiment: candidate efficiency for the given attacker artillery share, bombardment rounds and defenders
void autoplay_journal_experiment(int factionId, double rounds, double artilleryShare, int defenders, bool sufficient, char const* role, char const* unitName, double efficiency);
// append a line to autoplay.log
void autoplay_log(const char* format, ...);

