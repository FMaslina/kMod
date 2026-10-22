#pragma once
#include "CharacterUtils.h"

// LimbRecovery: отслеживает потерю рук/ног (культя: state == LIMB_STUMP)
// и восстанавливает их при достижении показанного HP (flesh - fleshStun)
// >= 1. Протезированные конечности (LIMB_REPLACED) не трогает.
void LimbRecovery_ApplyTick(Character* character);
