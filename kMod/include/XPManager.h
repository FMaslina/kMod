#pragma once
#include "CharacterUtils.h"

// XPManager: множитель x3 для всех характеристик Genius.
// Перехватываются обе воронки выдачи опыта CharStats:
// xpStat_eventBased (событийный) и xpStat_timeBased (временной).
// Все остальные xp* движка (xpRunning, xpMelee, ...) стекаются в них.
bool XPManager_InstallHooks();
