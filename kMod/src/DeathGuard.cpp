#include "pch.h"
#include "../include/DeathGuard.h"
#include "../include/Logger.h"
#include <core/Functions.h>
#include <mutex>
#include <map>

namespace {
	void (*g_origApplyDamage)(MedicalSystem::HealthPartStatus*, const Damages&) = nullptr;
	void (*g_origMedApplyDamage)(MedicalSystem*, MedicalSystem::HealthPartStatus*, const Damages&, bool, bool, const Ogre::Vector3&) = nullptr;
	bool (*g_origMedIsDead)(const MedicalSystem*) = nullptr;
	void (*g_origUpdateDamageState)(MedicalSystem*) = nullptr;
	bool (*g_origCharIsDead)(const Character*) = nullptr;
	bool (*g_origPartIsDead)(const MedicalSystem::HealthPartStatus*) = nullptr;
	void (*g_origDeclareDead)(Character*) = nullptr;
	void (*g_origDropGear)(Character*, bool) = nullptr;
	bool (*g_origWorldDestroy)(GameWorld*, RootObject*, bool, const char*) = nullptr;
	volatile LONG g_deathSuppressions = 0;
	LONG g_deathSuppLogged = 0;

	std::mutex g_throttleMutex;
	std::map<std::pair<Character*, int>, ULONGLONG> g_lastClampLog;

	bool ShouldLogClamp(Character* c, int kind)
	{
		const ULONGLONG now = GetTickCount64();
		std::lock_guard<std::mutex> lk(g_throttleMutex);
		auto key = std::make_pair(c, kind);
		auto it = g_lastClampLog.find(key);
		if (it != g_lastClampLog.end() && now - it->second < 10000)
			return false;
		g_lastClampLog[key] = now;
		return true;
	}

	// POD-only кламп под SEH. Пол считается от ЦЕЛЕВОГО MaxHP (по Toughness),
	// а не от текущего _maxHealth: текущее значение может колебаться в
	// борьбе с пересчётами движка, а пол обязан быть стабильным.
	// Голова/грудь/живот: minHP = 1 - targetMaxHP (на единицу выше смерти);
	// руки/ноги — не ниже порога утраты -100: сверхсмертельный урон
	// нормализуется, но сама утрата (<= -100) и восстановление работают.
	// Возврат: 0 нет, 1 голова, 2 торс, 3 конечность.
	int TryClampPart(MedicalSystem::HealthPartStatus* part, Character** outMe, float* outFloor)
	{
		__try {
			if (!GeniusUseHpFloor) return 0; // полы отключены: минус любой
			if (!part) return 0;
			Character* me = part->me;
			if (outMe) *outMe = me;
			if (!IsGeniusCharacter(me)) return 0;
			if (!SaneBodyPart(me, part)) return 0;
			typedef MedicalSystem::HealthPartStatus HPS;
			const bool isHead = part->whatAmI == HPS::PART_HEAD;
			const bool isTorso = part->whatAmI == HPS::PART_TORSO;
			const bool isLimb = part->whatAmI == HPS::PART_ARM ||
			                    part->whatAmI == HPS::PART_LEG;
			if (!isHead && !isTorso && !isLimb) return 0;
			float floor = GeniusLimbLossHp;
			if (isHead || isTorso) {
				const int target = GeniusTargetMaxHP(GetGeniusToughness(me));
				if (target < 3) return 0;
				floor = GeniusMinHp(static_cast<float>(target));
			}
			if (outFloor) *outFloor = floor;
			if (part->flesh < floor) {
				part->flesh = floor;
				return isHead ? 1 : (isTorso ? 2 : 3);
			}
			return 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	void ClampHeadTorso(MedicalSystem::HealthPartStatus* part)
	{
		Character* me = nullptr;
		float floor = 0.0f;
		const int kind = TryClampPart(part, &me, &floor);
	}

	// POD-only страховка крови/флага смерти. Возврат битами: 1 кровь долита, 2 флаг снят.
	int TryBloodAndDead(Character* character)
	{
		__try {
			if (!character) return 0;
			MedicalSystem* med = &character->medical;
			int done = 0;
			const float maxBlood = med->getMaxBlood();
			if (maxBlood > 0.0f && med->blood < maxBlood) {
				med->blood = maxBlood;
				done |= 1;
			}
			if (med->dead) {
				med->dead = false;
				done |= 2;
			}
			return done;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	void TryRemoveFromParade(Character* character)
	{
		__try {
			if (ou && character) ou->removeFromDeathParade(character);
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	// POD-only: остановка кровотечения (чтобы реген шёл в плюс).
	void TryStopBleeding(Character* character)
	{
		__try {
			if (!character) return;
			MedicalSystem* med = &character->medical;
			med->extraBloodLossFromBodyparts = 0.0f;
			med->currentBleedRate = 0.0f;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	void TryNudgeKnockout(Character* character, float step)
	{
		__try {
			if (!character) return;
			MedicalSystem* med = &character->medical;
			if (!med->dead && med->unconcious && med->knockoutTimer > 0.0f) {
				med->knockoutTimer -= step;
				if (!(med->knockoutTimer > 0.0f))
					med->knockoutTimer = 0.0f;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	// Перехват урона: сначала штатная обработка, затем ограничение.
	// Кламп в том же вызове действует атомарно относительно игровой
	// логики смерти — движок не увидит значение ниже (1 - MaxHP).
	void ApplyDamageHook(MedicalSystem::HealthPartStatus* part, const Damages& damage)
	{
		if (!g_origApplyDamage) return;
		g_origApplyDamage(part, damage);
		ClampHeadTorso(part);
	}

	// Верхний уровень нанесения урона (путь ранений ближнего боя и т.п.):
	// часть урона может идти мимо HealthPartStatus::applyDamage, поэтому
	// клампим и здесь — перерасход урона гасится до нашего пола.
	void MedApplyDamageHook(MedicalSystem* med, MedicalSystem::HealthPartStatus* part,
		const Damages& damage, bool loadingSavestate, bool canSever, const Ogre::Vector3& force)
	{
		if (!g_origMedApplyDamage) return;
		g_origMedApplyDamage(med, part, damage, loadingSavestate, canSever, force);
		ClampHeadTorso(part);
	}

	// Полный запрет события смерти для расы (все функции POD-only, без
	// объектов C++ — можно __try; логов внутри нет, только счётчик).
	// Реализация UpdateDamageStateHook — ниже, через VetoDamageState.

	// Опросы смерти: для Genius всегда "жив".
	bool MedIsDeadHook(const MedicalSystem* med)
	{
		const bool r = g_origMedIsDead ? g_origMedIsDead(med) : false;
		if (!r) return false;
		__try {
			if (!med) return r;
			Character* c = med->me;
			if (c && IsGeniusCharacter(c)) {
				InterlockedIncrement(&g_deathSuppressions);
				return false;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		return r;
	}

	bool CharIsDeadHook(const Character* c)
	{
		const bool r = g_origCharIsDead ? g_origCharIsDead(c) : false;
		if (!r) return false;
		__try {
			if (c && IsGeniusCharacter(const_cast<Character*>(c))) {
				InterlockedIncrement(&g_deathSuppressions);
				return false;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		return r;
	}

	// Смерть ЧАСТИ: для головы/груди/живота Genius — никогда.
	// Руки/ноги не трогаем: их утрата нужна механике восстановления.
	bool PartIsDeadHook(const MedicalSystem::HealthPartStatus* part)
	{
		const bool r = g_origPartIsDead ? g_origPartIsDead(part) : false;
		if (!r) return false;
		__try {
			if (!part) return r;
			typedef MedicalSystem::HealthPartStatus HPS;
			if (part->whatAmI != HPS::PART_HEAD && part->whatAmI != HPS::PART_TORSO)
				return r;
			Character* me = part->me;
			if (me && IsGeniusCharacter(me)) {
				InterlockedIncrement(&g_deathSuppressions);
				return false;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		return r;
	}

	// Полное воскрешение перед сносом: HP/кровь в максимум, флаги сняты.
	void ResurrectGenius(Character* c)
	{
		__try {
			if (!c) return;
			MedicalSystem* med = &c->medical;
			MedicalSystem::HealthPartStatus* parts[16];
			const int n = CollectBodyParts(c, parts, 16);
			for (int i = 0; i < n; ++i) {
				MedicalSystem::HealthPartStatus* p = parts[i];
				__try {
					if (!p || p->isRobotic()) continue;
					if (p->_maxHealth > 0.0f && p->flesh < p->_maxHealth)
						p->flesh = p->_maxHealth;
				} __except (EXCEPTION_EXECUTE_HANDLER) {}
			}
			__try {
				const float mb = med->getMaxBlood();
				if (mb > 0.0f) med->blood = mb;
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
			med->dead = false;
			med->unconcious = false;
			med->knockoutTimer = 0.0f;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	// declareDead: для Genius не объявляем вовсе.
	void DeclareDeadHook(Character* c)
	{
		__try {
			if (c && IsGeniusCharacter(c)) {
				InterlockedIncrement(&g_deathSuppressions);
				return;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		if (g_origDeclareDead)
			g_origDeclareDead(c);
	}

	// Сброс снаряжения трупа: для Genius не раздеваем.
	void DropGearHook(Character* c, bool vampiricDeath)
	{
		__try {
			if (c && IsGeniusCharacter(c)) return;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		if (g_origDropGear)
			g_origDropGear(c, vampiricDeath);
	}

	// Последний рубеж: физическое уничтожение объекта. Блокируем только
	// смерть (justUnloaded==false); выгрузку зон и сейв-загрузку не трогаем.
	// Перед отказом — полное воскрешение, чтобы объект остался валиден.
	bool WorldDestroyHook(GameWorld* world, RootObject* obj, bool justUnloaded, const char* debugInfo)
	{
		__try {
			if (!justUnloaded && obj) {
				// RootObject — первый базовый класс Character (смещение 0),
				// поэтому указатель reinterpret-преобразуем и проверяем расу.
				Character* c = reinterpret_cast<Character*>(obj);
				if (IsGeniusCharacter(c)) {
					ResurrectGenius(c);
					InterlockedIncrement(&g_deathSuppressions);
					return false; // не удалён
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		if (g_origWorldDestroy)
			return g_origWorldDestroy(world, obj, justUnloaded, debugInfo);
		return false;
	}

	// Переоценка состояний движком (именно здесь выставляется dead).
	// Гасим смерть в том же вызове: ниже по кадру движок уже не должен
	// увидеть dead=true (иначе — снятие с обновлений, заморозка, отряд).
	// Биты возврата: 1 смерть заветирована, 2 кровь долита.
	int VetoDamageState(MedicalSystem* med)
	{
		__try {
			if (!med) return 0;
			Character* me = med->me;
			if (!IsGeniusCharacter(me)) return 0;
			int r = 0;
			if (med->dead) {
				med->dead = false;
				r |= 1;
			}
			__try {
				const float maxBlood = med->getMaxBlood();
				if (maxBlood > 0.0f && med->blood < maxBlood) {
					med->blood = maxBlood;
					r |= 2;
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
			return r;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	// POD-only: вето смерти + возврат персонажа (без объектов C++).
	int VetoAndGetChar(MedicalSystem* med, Character** outMe)
	{
		__try {
			if (outMe) *outMe = nullptr;
			if (!med) return 0;
			const int r = VetoDamageState(med);
			if (outMe) {
				__try { *outMe = med->me; } __except (EXCEPTION_EXECUTE_HANDLER) {}
			}
			return r;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	void UpdateDamageStateHook(MedicalSystem* med)
	{
		if (g_origUpdateDamageState)
			g_origUpdateDamageState(med);
		if (!med) return;
		Character* me = nullptr;
		const int r = VetoAndGetChar(med, &me);
		if (r && me) {
			MedicalSystem::HealthPartStatus* parts[16];
			const int n = CollectBodyParts(me, parts, 16);
			for (int i = 0; i < n; ++i)
				ClampHeadTorso(parts[i]);
		}
	}
}

bool DeathGuard_InstallHook()
{
	const intptr_t addr = KenshiLib::GetRealAddress(&MedicalSystem::HealthPartStatus::applyDamage);
	if (!addr) {
		return false;
	}
	if (KenshiLib::AddHook(addr, (void*)&ApplyDamageHook, &g_origApplyDamage) != KenshiLib::SUCCESS) {
		return false;
	}
	const intptr_t medAddr = KenshiLib::GetRealAddress(&MedicalSystem::applyDamage);
	if (!medAddr) {
		return false;
	}
	if (KenshiLib::AddHook(medAddr, (void*)&MedApplyDamageHook, &g_origMedApplyDamage) != KenshiLib::SUCCESS) {
		return false;
	}

	bool ok = true;
	// Остальные хуки ставятся ниже поименно (адреса нужны для логов).
	const intptr_t udsAddr = KenshiLib::GetRealAddress(&MedicalSystem::updateDamageState);
	if (!udsAddr) {
		ok = false;
	} else if (KenshiLib::AddHook(udsAddr, (void*)&UpdateDamageStateHook, &g_origUpdateDamageState) != KenshiLib::SUCCESS) {
		ok = false;
	} else {
		Logger::Info("DeathGuard: death-setter hook installed (dead can no longer stick on Genius)");
	}

	const intptr_t midAddr = KenshiLib::GetRealAddress(&MedicalSystem::isDead);
	if (!midAddr) {
		ok = false;
	} else if (KenshiLib::AddHook(midAddr, (void*)&MedIsDeadHook, &g_origMedIsDead) != KenshiLib::SUCCESS) {
		ok = false;
	} else {
		Logger::Info("DeathGuard: medical isDead hook installed (Genius always reports alive)");
	}

	const intptr_t cidAddr = KenshiLib::GetRealAddress(&Character::isDead);
	if (!cidAddr) {
		ok = false;
	} else if (KenshiLib::AddHook(cidAddr, (void*)&CharIsDeadHook, &g_origCharIsDead) != KenshiLib::SUCCESS) {
		ok = false;
	} else {
		Logger::Info("DeathGuard: character isDead hook installed");
	}

	const intptr_t pidAddr = KenshiLib::GetRealAddress(&MedicalSystem::HealthPartStatus::isDead);
	if (!pidAddr) {
		ok = false;
	} else if (KenshiLib::AddHook(pidAddr, (void*)&PartIsDeadHook, &g_origPartIsDead) != KenshiLib::SUCCESS) {
		ok = false;
	} else {
		Logger::Info("DeathGuard: head/torso part-isDead hook installed (limbs untouched)");
	}

	const intptr_t ddAddr = KenshiLib::GetRealAddress(&Character::declareDead);
	if (!ddAddr) {
		ok = false;
	} else if (KenshiLib::AddHook(ddAddr, (void*)&DeclareDeadHook, &g_origDeclareDead) != KenshiLib::SUCCESS) {
		ok = false;
	} else {
		Logger::Info("DeathGuard: declareDead hook installed (never declared for Genius)");
	}

	const intptr_t dgAddr = KenshiLib::GetRealAddress(&Character::dropGearOnDeath);
	if (!dgAddr) {
		ok = false;
	} else if (KenshiLib::AddHook(dgAddr, (void*)&DropGearHook, &g_origDropGear) != KenshiLib::SUCCESS) {
		ok = false;
	} else {
		Logger::Info("DeathGuard: dropGear hook installed (Genius keeps gear)");
	}

	const intptr_t wdAddr = KenshiLib::GetRealAddress(&GameWorld::destroy);
	if (!wdAddr) {
		ok = false;
	} else if (KenshiLib::AddHook(wdAddr, (void*)&WorldDestroyHook, &g_origWorldDestroy) != KenshiLib::SUCCESS) {
		ok = false;
	} else {
		Logger::Info("DeathGuard: world-destroy hook installed (last resort for Genius)");
	}
	return ok;
}

void DeathGuard_ApplyTick(Character* character, float dtSeconds)
{
	if (!IsGeniusCharacter(character)) return;

	MedicalSystem::HealthPartStatus* parts[16];
	const int n = CollectBodyParts(character, parts, 16);
	for (int i = 0; i < n; ++i)
		ClampHeadTorso(parts[i]); // страховка: обходные пути урона, загрузка

	const int done = TryBloodAndDead(character);
	if (done & 2) {
		TryRemoveFromParade(character);
	}
	TryStopBleeding(character);

	// Сводка подавлений смерти (счётчик тикает в хуках без логов).
	const LONG supp = g_deathSuppressions;
	if (supp != g_deathSuppLogged) {
		g_deathSuppLogged = supp;
	}

	// Замороженный таймер нокаута: если персонаж жив, но без сознания,
	// а движок не тикает пробуждение — двигаем таймер сами.
	if (dtSeconds > 0.0f) {
		const float step = dtSeconds > 1.0f ? 1.0f : dtSeconds;
		TryNudgeKnockout(character, step);
	}
}
