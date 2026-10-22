#pragma once
#include "CharacterUtils.h"

// RaceManager: привязка механик к игровой расе из gojo.mod
// (RACE-запись GameData ID 5026007). Создать расу средствами RE_Kenshi
// невозможно — в KenshiLib нет API создания GameData-записей, расы
// определяются .mod-файлами игры. Идентификация — по числовому
// GameData ID и типу записи, не по имени/внешности.
void InitializeGeniusRaceBinding();
