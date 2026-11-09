#include "wtp_autoplay.h"

#include <cstdarg>

#include "patch.h"
#include "wtp_ai_game.h"

AutoplayState autoplay;

static FILE* logFile = nullptr;
static FILE* statsFile = nullptr;
static DWORD startTick = 0;
static int startTurn = 0;
static int suppressedPopups = 0;
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
		stats.energySurplus += base->energy_surplus;
		stats.economy += base->economy_total;
		stats.psych += base->psych_total;
		stats.labs += base->labs_total;
	}
	for (int vehicleId = 0; vehicleId < *VehCount; vehicleId++)
	{
		if (Vehs[vehicleId].faction_id == factionId)
			stats.units++;
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

	for (int factionId = 1; factionId < MaxPlayerNum; factionId++)
	{
		FactionStats stats = collectFactionStats(factionId);
		double score = positionScore(stats, autoplay.weights);

		size_t length = strlen(scores);
		snprintf(scores + length, sizeof(scores) - length, " %d=%.1f", factionId, score);

		if (statsFile)
		{
			fprintf(statsFile, "%s,%d,%d,%lu,%d,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.2f\n",
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
				stats.techs,
				stats.secretProjects,
				stats.energyCredits,
				stats.nutrientSurplus,
				stats.mineralSurplus,
				stats.energySurplus,
				stats.economy,
				stats.psych,
				stats.labs,
				stats.gameScore,
				stats.gameScoreOther,
				score
			);
		}
	}

	if (statsFile)
	{
		fflush(statsFile);
	}
	autoplay_log("position score %s, turn %d:%s", phase, *CurrentTurn, scores);
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
			fprintf(statsFile, "phase,turn,year,elapsed_ms,faction,name,alive,wtp,bases,pop,units,techs,projects,"
				"energy_credits,nutrient_surplus,mineral_surplus,energy_surplus,economy,psych,labs,"
				"game_score,game_score_other,position_score\n");
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
