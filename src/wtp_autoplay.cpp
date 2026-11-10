#include "wtp_autoplay.h"

#include <cstdarg>
#include <map>
#include <algorithm>
#include <vector>

#include "patch.h"
#include "wtp_ai_game.h"

AutoplayState autoplay;

static FILE* logFile = nullptr;
static FILE* statsFile = nullptr;
static DWORD startTick = 0;
static int startTurn = 0;
static int suppressedPopups = 0;

// animations skipped: explosions and unit slides
static int skippedBooms = 0;
static int skippedScoots = 0;

// pacing delays the game asked for through clock_wait, by the address that asked: calls and milliseconds
static std::map<uintptr_t, std::pair<int, long long>> requestedDelays;
static bool stopRequested = false;
static bool finished = false;

static void finish(const char* reason);

void autoplay_log(const char* format, ...)
{
	if (!logFile)
	{
		logFile = fopen("autoplay.log", "a");
		if (!logFile)
			return;
	}
	fprintf(logFile, "[%8lu ms] ", (GetTickCount() - startTick));
	va_list args;
	va_start(args, format);
	vfprintf(logFile, format, args);
	va_end(args);
	fputc('\n', logFile);
	fflush(logFile);
}

/*
Map redraw functions are stateless blits. Autoplay does not need the screen to follow the game.
All of them are cdecl, so a single argument-less stub is stack safe.
*/
static void __cdecl autoplayNoopDraw()
{
}

/*
Every modal dialog funnels into BasePop::exec. In autoplay nobody can answer, so each dialog
is logged with its caller address (resolve it against the IDA export) and answered with 0.
*/
static bool readableString(const char* text)
{
	if (reinterpret_cast<uintptr_t>(text) < 0x10000 || IsBadReadPtr(text, 4))
		return false;
	for (int i = 0; i < 40; i++)
	{
		if (IsBadReadPtr(text + i, 1))
			return false;
		if (text[i] == 0)
			return i > 1;
		if (static_cast<unsigned char>(text[i]) < 32 || static_cast<unsigned char>(text[i]) > 126)
			return false;
	}
	return true;
}

/*
Original code uses ebp frames. Walks them and logs each return address together with the first two
stack arguments of that frame when they look like strings (popup file and label, e.g. "SCRIPT", "WONLOST").
*/
static int __thiscall autoplayPopupExec(void* UNUSED(This), int UNUSED(a2), int UNUSED(a3))
{
	suppressedPopups++;
	char trace[512] = {};
	uintptr_t* frame = static_cast<uintptr_t*>(__builtin_frame_address(0));
	for (int level = 0; level < 4 && frame && !IsBadReadPtr(frame, 16); level++)
	{
		// this function's own frame holds the saved ebp of its caller first
		uintptr_t* callerFrame = reinterpret_cast<uintptr_t*>(frame[0]);
		if (!callerFrame || IsBadReadPtr(callerFrame, 16))
			break;
		size_t length = strlen(trace);
		snprintf(trace + length, sizeof(trace) - length, " <-%08X", callerFrame[1]);
		const char* arg0 = reinterpret_cast<const char*>(callerFrame[2]);
		const char* arg1 = reinterpret_cast<const char*>(callerFrame[3]);
		if (readableString(arg0))
		{
			length = strlen(trace);
			snprintf(trace + length, sizeof(trace) - length, "(%s", arg0);
			if (readableString(arg1))
			{
				length = strlen(trace);
				snprintf(trace + length, sizeof(trace) - length, ",%s", arg1);
			}
			length = strlen(trace);
			snprintf(trace + length, sizeof(trace) - length, ")");
		}
		frame = callerFrame;
	}
	autoplay_log("popup suppressed, turn %d, exec caller %p,%s", *CurrentTurn, __builtin_return_address(0), trace);
	return 0;
}

/*
The new-technology window of the faction that is shown on screen (MapWin->cOwner) is a separate modal window
that does not go through BasePop::exec. In autoplay the faction that was human in the save keeps that role on screen,
and its window waits for a click on OK, which nobody gives.
*/
static void __thiscall autoplayNewTechExec(void* UNUSED(This), int UNUSED(a2), int UNUSED(a3))
{
	suppressedPopups++;
	autoplay_log("new technology window suppressed, turn %d", *CurrentTurn);
}

/*
The faction that was human in the save keeps the player role on screen, so the engine shows it full-screen windows
that wait for a click: the Datalinks page of a secret project it completes and the story interludes.
Nobody gives that click in autoplay.
*/
static void __cdecl autoplayNoopHelpProject(int UNUSED(item_id))
{
	suppressedPopups++;
	autoplay_log("project help window suppressed, turn %d", *CurrentTurn);
}

static void __cdecl autoplayNoopInterlude(int event_id, const char* UNUSED(text), int UNUSED(a3), int UNUSED(a4))
{
	suppressedPopups++;
	autoplay_log("interlude %d suppressed, turn %d", event_id, *CurrentTurn);
}

/*
The Planetary Council session (CouncWin::init) shows the vote screen to the watched faction and waits for a click
on VOTE. Autoplay skips the session, so no resolution is passed. Only the voting itself is skipped:
the call announcement and the convened flag are set by the caller.
*/
static void __thiscall autoplayNoopCouncil(void* UNUSED(This), int UNUSED(proposal), int UNUSED(faction))
{
	suppressedPopups++;
	autoplay_log("planetary council session suppressed, turn %d", *CurrentTurn);
}

/*
clock_wait(ms) is the game's pacing delay: it redraws and pumps messages until the time has passed. Animations,
random event effects and the visible moves of other factions use it. Autoplay never waits, and the requested
time is summed per caller to report how much real time the delays would have taken.
*/
static void __cdecl autoplayClockWait(int milliseconds)
{
	std::pair<int, long long>& entry = requestedDelays[reinterpret_cast<uintptr_t>(__builtin_return_address(0))];
	entry.first++;
	entry.second += milliseconds;
}

static int __cdecl autoplayBoom(int UNUSED(x), int UNUSED(y), int UNUSED(type))
{
	skippedBooms++;
	return 0;
}

static void __cdecl autoplayVehScoot(int UNUSED(vehicleId), int UNUSED(x), int UNUSED(y), int UNUSED(offset), int UNUSED(flag))
{
	skippedScoots++;
}

void autoplay_install_hooks()
{
	if (autoplay.stubDraws)
	{
		write_jump(0x46AEF0, reinterpret_cast<int32_t>(autoplayNoopDraw)); // draw_radius
		write_jump(0x46AF40, reinterpret_cast<int32_t>(autoplayNoopDraw)); // draw_tile
		write_jump(0x46AF90, reinterpret_cast<int32_t>(autoplayNoopDraw)); // draw_tile_fixup
		write_jump(0x46B040, reinterpret_cast<int32_t>(autoplayNoopDraw)); // draw_tile_fixup_2
		write_jump(0x46B140, reinterpret_cast<int32_t>(autoplayNoopDraw)); // draw_tiles
		write_jump(0x46B190, reinterpret_cast<int32_t>(autoplayNoopDraw)); // draw_map
	}
	write_jump(0x602600, reinterpret_cast<int32_t>(autoplayPopupExec)); // BasePop::exec
	write_jump(0x483F90, reinterpret_cast<int32_t>(autoplayNewTechExec)); // NewTechWin::exec
	write_jump(0x50DD90, reinterpret_cast<int32_t>(autoplayClockWait)); // clock_wait
	write_jump(0x504AA0, reinterpret_cast<int32_t>(autoplayBoom)); // boom
	write_jump(0x55A150, reinterpret_cast<int32_t>(autoplayVehScoot)); // veh_scoot
	write_jump(0x44C9E0, reinterpret_cast<int32_t>(autoplayNoopHelpProject)); // help_project
	write_jump(0x5230E0, reinterpret_cast<int32_t>(autoplayNoopInterlude)); // interlude
	write_jump(0x427230, reinterpret_cast<int32_t>(autoplayNoopCouncil)); // CouncWin::init
}

/*
Same effect as the player preferences but forced for the session only.
Game*Preferences are copies of the values in Alpha Centauri.ini and are never written back.
*/
static void applyQuietPreferences()
{
	*GamePreferences &= ~(PREF_AV_SECRET_PROJECT_MOVIES | PREF_AV_MAP_ANIMATIONS | PREF_AV_SLIDING_WINDOWS
		| PREF_BSC_PAUSE_END_TURN | PREF_BSC_AUTOSAVE_EACH_TURN | PREF_BSC_TUTORIAL_MSGS
		| PREF_BSC_DONT_QUICK_MOVE_ENEMY_VEH | PREF_BSC_DONT_QUICK_MOVE_ALLY_VEH);
	*GamePreferences |= PREF_ADV_FAST_BATTLE_RESOLUTION | PREF_AV_INTERLUDES_DISABLED;
	*GameMorePreferences &= ~MPREF_ADV_PAUSE_AFTER_BATTLES;
	*GameMorePreferences |= MPREF_ADV_QUICK_MOVE_ALL_VEH | MPREF_AV_MONUMENTS_DISABLED;
	conf.autosave_interval = 0;
}

FactionStats collectFactionStats(int factionId)
{
	FactionStats stats;
	stats.factionId = factionId;
	stats.alive = is_alive(factionId);

	if (!stats.alive)
		return stats;

	for (int baseId = 0; baseId < *BaseCount; baseId++)
	{
		BASE* base = &Bases[baseId];
		if (base->faction_id != factionId)
			continue;
		stats.bases++;
		stats.population += base->pop_size;
		stats.nutrientSurplus += base->nutrient_surplus;
		stats.mineralSurplus += base->mineral_surplus;
		stats.mineralIntake += base->mineral_intake_2;
		stats.baseRockiness += map_rockiness(getBaseMapTile(baseId));

		int item = base->item();
		if (item >= 0)
		{
			if (isColonyUnit(item))
				stats.buildColony++;
			else if (isFormerUnit(item))
				stats.buildFormer++;
			else if (isSupplyUnit(item))
				stats.buildSupply++;
			else if (isProbeUnit(item))
				stats.buildProbe++;
			else if (isCombatUnit(item))
				stats.buildCombat++;
			else
				stats.buildOtherUnit++;
		}
		else if (-item >= SP_ID_First && -item <= SP_ID_Last)
			stats.buildProject++;
		else
			stats.buildFacility++;
		stats.energySurplus += base->energy_surplus;
		stats.economy += base->economy_total;
		stats.psych += base->psych_total;
		stats.labs += base->labs_total;
	}
	for (int vehicleId = 0; vehicleId < *VehCount; vehicleId++)
	{
		if (Vehs[vehicleId].faction_id != factionId)
			continue;
		stats.units++;
		if (isColonyVehicle(vehicleId))
			stats.unitsColony++;
		else if (isFormerVehicle(vehicleId))
			stats.unitsFormer++;
		else if (isSupplyVehicle(vehicleId))
			stats.unitsSupply++;
		else if (isProbeVehicle(vehicleId))
			stats.unitsProbe++;
		else if (isCombatVehicle(vehicleId))
			stats.unitsCombat++;
		else
			stats.unitsOther++;
	}
	for (int mapIndex = 0; mapIndex < *MapAreaTiles; mapIndex++)
	{
		MAP *tile = *MapTiles + mapIndex;
		if (tile->owner != factionId)
			continue;
		if (tile->items & BIT_FARM)
			stats.tileFarm++;
		if (tile->items & BIT_MINE)
			stats.tileMine++;
		if (tile->items & BIT_SOLAR)
			stats.tileSolar++;
		if (tile->items & BIT_FOREST)
			stats.tileForest++;
		if (tile->items & BIT_ROAD)
			stats.tileRoad++;
	}
	for (int techId = 0; techId < MaxTechnologyNum; techId++)
	{
		if (has_tech(techId, factionId))
			stats.techs++;
	}
	// a secret project holds the id of the base that built it, or a negative value when not built
	for (int i = 0; i <= SP_ID_Last - SP_ID_First; i++)
	{
		int baseId = SecretProjects[i];
		if (baseId >= 0 && baseId < *BaseCount && Bases[baseId].faction_id == factionId)
			stats.secretProjects++;
	}
	stats.energyCredits = Factions[factionId].energy_credits;

	// compute_score writes through both pointers beyond the first int; keep the buffer away from other locals
	static int scoreBuffer[32];
	memset(scoreBuffer, 0, sizeof(scoreBuffer));
	compute_score(factionId, &scoreBuffer[0], &scoreBuffer[16], 1);
	stats.gameScore = scoreBuffer[0];
	stats.gameScoreOther = scoreBuffer[16];

	return stats;
}

double positionScore(const FactionStats& stats, const ScoreWeights& weights)
{
	if (!stats.alive)
		return 0.0;

	return
		weights.base * stats.bases
		+ weights.population * stats.population
		+ weights.unit * stats.units
		+ weights.tech * stats.techs
		+ weights.secretProject * stats.secretProjects
		+ weights.energyCredit * stats.energyCredits
		+ weights.nutrientSurplus * stats.nutrientSurplus
		+ weights.mineralSurplus * stats.mineralSurplus
		+ weights.energySurplus * stats.energySurplus
	;
}

/*
Collects all factions, logs the position scores and writes the optional CSV.
*/
static void recordStats(const char* phase)
{
	char scores[256] = {};
	double nutrientSum = 0.0;
	double mineralSum = 0.0;
	double energySum = 0.0;
	int combatSum = 0, colonySum = 0, formerSum = 0, supplySum = 0, probeSum = 0, otherSum = 0;
	int buildSum[8] = {};
	int tileSum[5] = {};
	int rockinessSum = 0;
	int baseSum = 0;
	int yieldCount = 0;

	for (int factionId = 1; factionId < MaxPlayerNum; factionId++)
	{
		FactionStats stats = collectFactionStats(factionId);
		double score = positionScore(stats, autoplay.weights);

		if (stats.alive)
		{
			// Yield of the bases before spending: gross mineral intake (unit support is a decision of its own),
			// economy and labs (facility maintenance is the same), and the nutrient surplus (feeding citizens is
			// not a choice, and every citizen brings a tile).
			nutrientSum += stats.nutrientSurplus;
			mineralSum += stats.mineralIntake;
			energySum += stats.economy + stats.labs;
			combatSum += stats.unitsCombat;
			colonySum += stats.unitsColony;
			formerSum += stats.unitsFormer;
			supplySum += stats.unitsSupply;
			probeSum += stats.unitsProbe;
			otherSum += stats.unitsOther;
			buildSum[0] += stats.buildColony;
			buildSum[1] += stats.buildFormer;
			buildSum[2] += stats.buildCombat;
			buildSum[3] += stats.buildSupply;
			buildSum[4] += stats.buildProbe;
			buildSum[5] += stats.buildOtherUnit;
			buildSum[6] += stats.buildFacility;
			buildSum[7] += stats.buildProject;
			tileSum[0] += stats.tileFarm;
			tileSum[1] += stats.tileMine;
			tileSum[2] += stats.tileSolar;
			tileSum[3] += stats.tileForest;
			tileSum[4] += stats.tileRoad;
			rockinessSum += stats.baseRockiness;
			baseSum += stats.bases;
			yieldCount++;
		}

		size_t length = strlen(scores);
		snprintf(scores + length, sizeof(scores) - length, " %d=%.1f", factionId, score);

		if (statsFile)
		{
			fprintf(statsFile, "%s,%d,%d,%lu,%d,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.2f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
				phase,
				*CurrentTurn,
				*CurrentMissionYear,
				(GetTickCount() - startTick),
				factionId,
				MFactions[factionId].noun_faction,
				stats.alive ? 1 : 0,
				isWtpEnabledFaction(factionId) ? 1 : 0,
				stats.bases,
				stats.population,
				stats.units,
				stats.unitsCombat,
				stats.unitsColony,
				stats.unitsFormer,
				stats.unitsSupply,
				stats.unitsProbe,
				stats.unitsOther,
				stats.techs,
				stats.secretProjects,
				stats.energyCredits,
				stats.nutrientSurplus,
				stats.mineralSurplus,
				stats.mineralIntake,
				stats.energySurplus,
				stats.economy,
				stats.psych,
				stats.labs,
				stats.gameScore,
				stats.gameScoreOther,
				score,
				stats.buildColony,
				stats.buildFormer,
				stats.buildCombat,
				stats.buildSupply,
				stats.buildProbe,
				stats.buildOtherUnit,
				stats.buildFacility,
				stats.buildProject,
				stats.tileFarm,
				stats.tileMine,
				stats.tileSolar,
				stats.tileForest,
				stats.tileRoad,
				stats.baseRockiness
			);
		}
	}

	if (statsFile)
	{
		fflush(statsFile);
	}
	autoplay_log("position score %s, turn %d:%s", phase, *CurrentTurn, scores);
	if (yieldCount > 0)
	{
		double nutrient = nutrientSum / yieldCount;
		double mineral = mineralSum / yieldCount;
		double energy = energySum / yieldCount;
		const YieldWeights& yieldWeights = autoplay.yieldWeights;
		double yieldScore = yieldWeights.nutrient * nutrient + yieldWeights.mineral * mineral + yieldWeights.energy * energy;
		autoplay_log("yield %s, turn %d: nutrient %.3f mineral %.3f energy %.3f score %.3f (means per faction over %d factions, weights %.2f/%.2f/%.2f)",
			phase, *CurrentTurn, nutrient, mineral, energy, yieldScore, yieldCount, yieldWeights.nutrient, yieldWeights.mineral, yieldWeights.energy);
		autoplay_log("units %s, turn %d: combat %.3f colony %.3f former %.3f supply %.3f probe %.3f other %.3f (means per faction over %d factions)",
			phase, *CurrentTurn, (double)combatSum / yieldCount, (double)colonySum / yieldCount, (double)formerSum / yieldCount,
			(double)supplySum / yieldCount, (double)probeSum / yieldCount, (double)otherSum / yieldCount, yieldCount);
		autoplay_log("builds %s, turn %d: colony %.3f former %.3f combat %.3f supply %.3f probe %.3f other %.3f facility %.3f project %.3f (bases building, means per faction)",
			phase, *CurrentTurn, (double)buildSum[0] / yieldCount, (double)buildSum[1] / yieldCount, (double)buildSum[2] / yieldCount,
			(double)buildSum[3] / yieldCount, (double)buildSum[4] / yieldCount, (double)buildSum[5] / yieldCount,
			(double)buildSum[6] / yieldCount, (double)buildSum[7] / yieldCount);
		autoplay_log("tiles %s, turn %d: farm %.3f mine %.3f solar %.3f forest %.3f road %.3f, base rockiness %.3f (means per faction, rockiness per base)",
			phase, *CurrentTurn, (double)tileSum[0] / yieldCount, (double)tileSum[1] / yieldCount, (double)tileSum[2] / yieldCount,
			(double)tileSum[3] / yieldCount, (double)tileSum[4] / yieldCount, baseSum > 0 ? (double)rockinessSum / baseSum : 0.0);
	}
}

void autoplay_after_load()
{
	startTick = GetTickCount();
	startTurn = *CurrentTurn;
	stopRequested = false;
	finished = false;

	if (!autoplay.statsPath.empty())
	{
		statsFile = fopen(autoplay.statsPath.c_str(), "w");
		if (statsFile)
		{
			fprintf(statsFile, "phase,turn,year,elapsed_ms,faction,name,alive,wtp,bases,pop,units,combat,colony,former,supply,probe,other,techs,projects,"
				"energy_credits,nutrient_surplus,mineral_surplus,mineral_intake,energy_surplus,economy,psych,labs,"
				"game_score,game_score_other,position_score,"
				"build_colony,build_former,build_combat,build_supply,build_probe,build_other_unit,build_facility,build_project,"
				"tile_farm,tile_mine,tile_solar,tile_forest,tile_road,base_rockiness\n");
		}
		else
		{
			autoplay_log("cannot open stats file %s", autoplay.statsPath.c_str());
		}
	}

	// every faction becomes AI, including the one that was human in the save
	autoplay_log("autoplay start: turn %d, human mask was 0x%02X, playing %d turns, stats csv: %s",
		*CurrentTurn, FactionStatus[0], autoplay.turns, statsFile ? autoplay.statsPath.c_str() : "none");
	FactionStatus[0] = 0;
	// The watched faction (MapWin->cOwner) is the one the engine shows windows to, whatever its control.
	// Faction 0 is the natives, which no window is meant for. -1 is not an option: range checks assert on it.
	// The animations of native combat and moves are skipped by their own stubs.
	*CurrentPlayerFaction = 0;
	for (const std::string& override : autoplay.overrides)
	{
		autoplay_log("option override: %s", override.c_str());
	}

	applyQuietPreferences();
	recordStats("load");
}

static void pumpMessages()
{
	MSG msg;
	while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
	{
		// player input must not reach the game while AI is moving
		if ((msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST)
			|| (msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST))
			continue;
		if (msg.message == WM_QUIT)
		{
			stopRequested = true;
			continue;
		}
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}
}

void autoplay_poll()
{
	// the game resets preferences from the ini on some UI paths
	applyQuietPreferences();

	pumpMessages();

	if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_CANCEL) & 0x8000))
		stopRequested = true;
	if (GetFileAttributesA("autoplay.stop") != INVALID_FILE_ATTRIBUTES)
	{
		stopRequested = true;
		DeleteFileA("autoplay.stop");
	}
	if (stopRequested)
	{
		finish("stop requested");
	}
}

static void finish(const char* reason)
{
	if (finished)
		return;
	finished = true;
	autoplay_log("autoplay finished (%s): turn %d, %d popups suppressed", reason, *CurrentTurn, suppressedPopups);
	{
		long long totalMilliseconds = 0;
		std::vector<std::pair<long long, uintptr_t>> byTime;
		for (const auto& item : requestedDelays)
		{
			totalMilliseconds += item.second.second;
			byTime.push_back(std::make_pair(item.second.second, item.first));
		}
		std::sort(byTime.rbegin(), byTime.rend());
		autoplay_log("pacing delays skipped: %lld ms over %d callers; %d explosions and %d unit slides skipped",
			totalMilliseconds, static_cast<int>(byTime.size()), skippedBooms, skippedScoots);
		for (size_t i = 0; i < byTime.size() && i < 12; i++)
		{
			const std::pair<int, long long>& entry = requestedDelays[byTime[i].second];
			autoplay_log("	caller %p: %d calls, %lld ms", reinterpret_cast<void*>(byTime[i].second), entry.first, entry.second);
		}
	}
	if (statsFile)
	{
		fclose(statsFile);
		statsFile = nullptr;
	}
	*ControlTurnA = 1;
}

/*
Replaces Console::human_turn at the end of every round. Repeats only the state resets that function does
and has nothing to wait for.
*/
void autoplay_end_of_round()
{
	*ControlTurnMove = 0;
	for (int vehicleId = 0; vehicleId < *VehCount; vehicleId++)
	{
		Vehs[vehicleId].state &= ~VSTATE_UNK_8000000;
	}

	recordStats("round");
	autoplay_log("round done: turn %d", *CurrentTurn);

	autoplay_poll();

	if (*CurrentTurn - startTurn + 1 >= autoplay.turns)
	{
		finish("turn limit");
	}
}

void autoplay_game_over()
{
	recordStats("gameover");
	autoplay_log("game over: victory type %d", *GameVictoryType);
	finish("game over");
}
