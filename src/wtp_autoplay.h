#pragma once

#include <string>
#include "main.h"

/*
Autoplay (programmatic) mode.

Started from the command line: terranx_mod.exe -autoplay [-turns N] [-stats file.csv] [-keepdraw] save.sav
The save is loaded as usual, then every faction (including the one that was human in the save)
is played by the AI. No human input is requested. After the requested number of turns
the game exits.

Statistics are collected in memory (FactionStats) and converted to a single number (positionScore).
The score of every faction is appended to autoplay.log after each round.
A CSV with all collected values is written only when -stats is given.

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

