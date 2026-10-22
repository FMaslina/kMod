#include "pch.h"
#include "../include/XPManager.h"
#include "../include/Logger.h"
#include <core/Functions.h>

namespace {
	// Оригиналы для вызова из хуков (заполняет MinHook).
	void (*g_origEventXp)(CharStats*, StatsEnumerated, float) = nullptr;
	void (*g_origTimeXp)(CharStats*, StatsEnumerated) = nullptr;

	// Событийная выдача опыта: утрояем порцию только для Genius.
	// Один вызов — одно умножение, повторного умножения нет.
	void XpEventHook(CharStats* stats, StatsEnumerated stat, float amount)
	{
		if (!g_origEventXp) return;
		__try {
			if (stats && stats->me && IsGeniusCharacter(stats->me))
				amount *= GeniusXpMultiplier;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		g_origEventXp(stats, stat, amount);
	}

	// Временная выдача идёт фиксированными тиками без порции,
	// поэтому для x3 повторяем тик трижды (только для Genius).
	void XpTimeHook(CharStats* stats, StatsEnumerated stat)
	{
		if (!g_origTimeXp) return;
		g_origTimeXp(stats, stat);
		bool genius = false;
		__try {
			genius = stats && stats->me && IsGeniusCharacter(stats->me);
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		if (genius) {
			g_origTimeXp(stats, stat);
			g_origTimeXp(stats, stat);
		}
	}
}

bool XPManager_InstallHooks()
{
	bool ok = true;

	const intptr_t eventAddr = KenshiLib::GetRealAddress(&CharStats::xpStat_eventBased);
	if (!eventAddr) {
		ok = false;
	} else if (KenshiLib::AddHook(eventAddr, (void*)&XpEventHook, &g_origEventXp) != KenshiLib::SUCCESS) {
		ok = false;
	}

	const intptr_t timeAddr = KenshiLib::GetRealAddress(&CharStats::xpStat_timeBased);
	if (!timeAddr) {
		ok = false;
	} else if (KenshiLib::AddHook(timeAddr, (void*)&XpTimeHook, &g_origTimeXp) != KenshiLib::SUCCESS) {
		ok = false;
	}

	return ok;
}
