#pragma once

#include <string>
#include <vector>
#include "main.h"

/*
Autoplay (programmatic) mode.

Started from the command line: terranx_mod.exe -autoplay [-turns N] [-stats file.csv] [-keepdraw] [-set name=value ...] save.sav
The save is loaded as usual, then every faction (including the one that was human in the save)
is played by the AI. No human input is requested. After the requested number of turns
the game exits.

Statistics are collected in memory (FactionStats) and converted to a single number (positionScore).
The score of every faction is appended to autoplay.log after each round.
A CSV with all collected values is written only when -stats is given.

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
	int techs = 0;
	int secretProjects = 0;
	int energyCredits = 0;
	int nutrientSurplus = 0;
	int mineralSurplus = 0;
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
// append a line to autoplay.log
void autoplay_log(const char* format, ...);

