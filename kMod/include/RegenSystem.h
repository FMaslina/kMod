#pragma once
#include "CharacterUtils.h"

// RegenSystem: GeniusRegenPerSecond (0.1% MaxHP) в секунду для каждой
// части тела Genius. Требование: реген идёт ОДИНАКОВО в любом состоянии
// персонажа (нок, сознание, бой) и даже если движок перестал звать
// medicalUpdate для этого персонажа.
//
//  - RegenSystem_ApplyTick — основной путь, dt = время кадра движка
//    (вызывается из тика medicalUpdate);
//  - RegenSystem_ApplyFallback — страховочный путь для хуков без времени
//    кадра (часть тела, статы, validateHealthValues): то же начисление по
//    реальному времени с измеренным коэффициентом «движковое/реальное».
//    Молчит, пока основной тик жив, поэтому двойного начисления нет.
//
// Оба возвращают, сколько HP реально добавлено за вызов. Раз в 10 c в лог
// идёт сводка `Regen: mod=..HP eng=..HP dt=..s/..s tick=.. idle=.. ...`
// (или `Regen: NO EFFECT ...`): mod — наш прирост, eng — что с flesh
// сделал сам движок, tick/idle — каким путём начисляли.
float RegenSystem_ApplyTick(Character* character, float dtSeconds);
float RegenSystem_ApplyFallback(Character* character);
