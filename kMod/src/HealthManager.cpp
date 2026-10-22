#include "pch.h"
#include "../include/HealthManager.h"
#include "../include/Logger.h"
#include "../include/RegenSystem.h"
#include <core/Functions.h>
#include <cstdio>

namespace {
	void (*g_origUpdateStats)(MedicalSystem*) = nullptr;
	bool (*g_origPartUpdate)(MedicalSystem::HealthPartStatus*, float, float, float, float, float, float) = nullptr;
	void (*g_origValidate)(MedicalSystem*) = nullptr;

	std::mutex g_diagMutex;

	// Диагностика на персонажа: когда движок сам перезаписал _maxHealth
	// (и мы вернули своё значение) и когда писали последний хартбит.
	// Именно по этим строкам видно «момент», когда восстановление HP ломается.
	struct Diag
	{
		ULONGLONG lastRewriteLog;  // троттлинг сообщений о перезаписи (1 c)
		ULONGLONG lastBeat;        // троттлинг хартбита (10 c)
		ULONGLONG lastDrainLog;    // троттлинг сообщений о сливе flesh (10 c)
		int rewritesSinceBeat;     // сколько перезаписей с прошлого хартбита
		int drainsSinceBeat;       // сколько раз движок сливал flesh с прошлого хартбита
		unsigned srcMask;          // какие пути вызывали ApplyTick за интервал
		float hmMin;               // мин. положительный healMultFlesh движка
		float hmMax;               // макс. положительный healMultFlesh движка
		int hmSamples;             // вызовов, где движок сам лечил flesh
	};
	std::map<Character*, Diag> g_diag;

	// Сколько движок сам лечит flesh (положительный healMultFlesh) — это и
	// есть ванильная скорость восстановления. По хартбиту видно, включается
	// ли она в «момент», когда восстановление выглядит ванильным.
	void NoteEngineHeal(Character* c, float healMultFlesh)
	{
		if (!c || !(healMultFlesh > 0.0f)) return;
		std::lock_guard<std::mutex> lk(g_diagMutex);
		Diag& d = g_diag[c];
		if (d.hmSamples == 0) {
			d.hmMin = healMultFlesh;
			d.hmMax = healMultFlesh;
		} else {
			if (healMultFlesh < d.hmMin) d.hmMin = healMultFlesh;
			if (healMultFlesh > d.hmMax) d.hmMax = healMultFlesh;
		}
		++d.hmSamples;
	}

	const char* PartName(int type)
	{
		switch (type) {
		case MedicalSystem::HealthPartStatus::PART_TORSO: return "torso";
		case MedicalSystem::HealthPartStatus::PART_LEG: return "leg";
		case MedicalSystem::HealthPartStatus::PART_ARM: return "arm";
		case MedicalSystem::HealthPartStatus::PART_HEAD: return "head";
		default: return "part";
		}
	}

	// POD-only сэмплирование части под SEH (только для хартбита).
	bool TrySamplePart(MedicalSystem::HealthPartStatus* part, int* type, int* side,
		float* flesh, float* maxHp, float* band, float* stun, float* hpPercent,
		float* hpMult)
	{
		__try {
			if (!part || !type || !side || !flesh || !maxHp) return false;
			*type = static_cast<int>(part->whatAmI);
			*side = static_cast<int>(part->side);
			*flesh = part->flesh;
			*maxHp = part->_maxHealth;
			if (band) *band = part->bandaging;
			if (stun) *stun = part->fleshStun;
			if (hpPercent) *hpPercent = part->derivedFleshHealthPercent;
			if (hpMult) *hpMult = part->HPMult;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// POD-only сэмплирование состояния конечности в robotLimbs (для
	// хартбита): 0 original, 1 stump, 2 replaced, 3 crushed; -1 — не
	// конечность/недоступно. Нужно, чтобы отличить живую конечность от
	// культи, когда в HP части видно «запредельный» минус.
	bool TrySampleLimbState(MedicalSystem::HealthPartStatus* part, int* outState)
	{
		__try {
			if (!part || !outState) return false;
			const int type = static_cast<int>(part->whatAmI);
			if (type != MedicalSystem::HealthPartStatus::PART_ARM &&
				type != MedicalSystem::HealthPartStatus::PART_LEG) {
				*outState = -1;
				return true;
			}
			MedicalSystem* med = part->medical;
			if (!med || !med->robotLimbs) { *outState = -1; return true; }
			const int limb = static_cast<int>(part->getRobotLimbEnum());
			if (limb < RobotLimbs::LEFT_ARM || limb > RobotLimbs::RIGHT_LEG) {
				*outState = -1;
				return true;
			}
			*outState = static_cast<int>(
				med->robotLimbs->getState(static_cast<RobotLimbs::Limb>(limb)));
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// POD-only снимок флагов медицинской системы (для хартбита): по ko=
	// видно, когда движок сам лечит тело (нокаут/отдых), а когда нет.
	// restedState / dazedOrAlert — состояние, от которого движок включает
	// своё (ванильное) лечение; worstDamage — худший урон по телу.
	bool TrySampleMed(Character* c, int* ko, int* dead, int* crippled,
		float* blood, float* maxBlood, float* rested, float* alert, float* worst)
	{
		__try {
			if (!c || !ko || !dead || !crippled || !blood || !maxBlood) return false;
			MedicalSystem* med = &c->medical;
			*ko = med->unconcious ? 1 : 0;
			*dead = med->dead ? 1 : 0;
			*crippled = med->crippled ? 1 : 0;
			*blood = med->blood;
			*maxBlood = med->getMaxBlood();
			if (rested) *rested = med->restedState;
			if (alert) *alert = med->dazedOrAlert;
			if (worst) *worst = med->worstDamage;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Бит источника вызова: 1 тик medicalUpdate, 2 updateStats,
	// 4 validateHealthValues. По хартбиту видно, если тик перестал
	// вызываться (тогда и реген, живущий в тике, перестал работать).
	unsigned SourceBit(const char* source)
	{
		if (!source) return 0;
		if (source[0] == 't' && source[1] == 'i') return 1; // "tick"
		if (source[0] == 's') return 2;                     // "stats"
		if (source[0] == 'v') return 4;                     // "validate"
		return 0;
	}

	std::string SourceList(unsigned mask)
	{
		std::string s;
		if (mask & 1) s += "tick";
		if (mask & 2) { if (!s.empty()) s += "+"; s += "stats"; }
		if (mask & 4) { if (!s.empty()) s += "+"; s += "validate"; }
		return s.empty() ? "none" : s;
	}

	// Движок сливает flesh части (отрицательный healMultFlesh и/или
	// degenerationRate). Пишем не чаще раза в 10 c на персонажа — по этой
	// строке видно, что наш реген гасится сливом движка.
	void LogEngineDrain(Character* c, float healMultFlesh, float degen)
	{
		if (!c) return;
		const ULONGLONG now = GetTickCount64();
		bool show = false;
		{
			std::lock_guard<std::mutex> lk(g_diagMutex);
			Diag& d = g_diag[c];
			++d.drainsSinceBeat;
			if (now - d.lastDrainLog >= 10000) {
				d.lastDrainLog = now;
				show = true;
			}
		}
		if (!show) return;
		char hm[32] = {};
		char dg[32] = {};
		sprintf_s(hm, "%.3f", healMultFlesh);
		sprintf_s(dg, "%.3f", degen);
		Logger::Info(std::string("HealthManager: engine drains Genius flesh (healMultFlesh=") +
			hm + ", degenerationRate=" + dg + ") - draining channels clamped to 0");
	}

	// Вся touching-память — здесь: только POD, можно __try.
	// Возвращает true, если значение реально изменилось.
	bool TryApplyMaxHP(MedicalSystem::HealthPartStatus* part, float target, int* prevMax)
	{
		__try {
			if (!part) return false;
			if (part->isRobotic()) return false;
			const int cur = static_cast<int>(part->_maxHealth);
			if (prevMax) *prevMax = cur;
			if (cur != static_cast<int>(target))
				part->_maxHealth = target;
			else
				return false;
			if (part->flesh > part->_maxHealth)
				part->flesh = part->_maxHealth;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}
}

void HealthManager_ApplyTick(Character* character, const char* source)
{
	// ВАЖНО: без фигурных скобок return ниже срабатывал ВСЕГДА, и функция
	// ничего не делала (ни HP, ни лог). Фигурные скобки обязательны.
	if (!IsGeniusCharacter(character))
		return;

	const float toughness = GetGeniusToughness(character);
	const int target = GeniusTargetMaxHP(toughness);
	MedicalSystem::HealthPartStatus* parts[16];
	const int n = CollectBodyParts(character, parts, 16);

	int rewrites = 0;
	int prevFirst = 0;
	bool havePrev = false;
	for (int i = 0; i < n; ++i) {
		int prev = 0;
		// true = движок держал ДРУГОЕ значение, пришлось вернуть наше.
		if (!TryApplyMaxHP(parts[i], static_cast<float>(target), &prev)) continue;
		if (++rewrites == 1) {
			prevFirst = prev;
			havePrev = true;
		}
	}

	const unsigned src = SourceBit(source);
	const ULONGLONG now = GetTickCount64();
	bool logRewrite = false;
	bool logBeat = false;
	std::string beat;
	{
		std::lock_guard<std::mutex> lk(g_diagMutex);
		Diag& d = g_diag[character];
		d.rewritesSinceBeat += rewrites;
		d.srcMask |= src;

		// Перезапись _maxHealth движком — не чаще раза в секунду.
		if (havePrev && now - d.lastRewriteLog >= 1000) {
			d.lastRewriteLog = now;
			logRewrite = true;
		}
		// Хартбит раз в 10 c: видно, растёт ли flesh, не упёрся ли он в cap,
		// каким путём вызван тик и в каком состоянии персонаж (ko=/rested=).
		if (now - d.lastBeat >= 10000) {
			const bool first = (d.lastBeat == 0);
			d.lastBeat = now;
			logBeat = true;
			int ko = -1, dead = -1, crippled = -1;
			float blood = -1.0f, maxBlood = -1.0f;
			float rested = -1.0f, alert = -1.0f, worst = -1.0f;
			const bool medOk = TrySampleMed(character, &ko, &dead, &crippled,
				&blood, &maxBlood, &rested, &alert, &worst);
			beat = "HealthManager: heartbeat src=" + SourceList(d.srcMask) +
				" toughness=" + std::to_string(static_cast<int>(toughness + 0.5f)) +
				" target=" + std::to_string(target) +
				" parts=" + std::to_string(n) +
				" maxHpRewrites=" + std::to_string(d.rewritesSinceBeat) +
				" engineDrains=" + std::to_string(d.drainsSinceBeat);
			if (medOk) {
				char rest[32] = {};
				char alrt[32] = {};
				char wrst[32] = {};
				sprintf_s(rest, "%.2f", rested);
				sprintf_s(alrt, "%.2f", alert);
				sprintf_s(wrst, "%.1f", worst);
				beat += " ko=" + std::to_string(ko) +
					" dead=" + std::to_string(dead) +
					" crippled=" + std::to_string(crippled) +
					" rested=" + rest +
					" alert=" + alrt +
					" worst=" + wrst +
					" blood=" + std::to_string(static_cast<int>(blood)) +
					"/" + std::to_string(static_cast<int>(maxBlood));
			}
			d.rewritesSinceBeat = 0;
			d.drainsSinceBeat = 0;
			d.srcMask = 0;
			if (d.hmSamples > 0) {
				char h1[32] = {};
				char h2[32] = {};
				sprintf_s(h1, "%.4f", d.hmMin);
				sprintf_s(h2, "%.4f", d.hmMax);
				beat += " engHeal=" + std::string(h1) + ".." + h2 +
					"/" + std::to_string(d.hmSamples);
			}
			d.hmSamples = 0;
			d.hmMin = 0.0f;
			d.hmMax = 0.0f;
			for (int i = 0; i < n; ++i) {
				int type = -1, side = -1;
				float flesh = 0.0f, maxHp = 0.0f, band = 0.0f, stun = 0.0f;
				float hpPct = -1.0f, hpMult = -1.0f;
				if (!TrySamplePart(parts[i], &type, &side, &flesh, &maxHp, &band, &stun,
					&hpPct, &hpMult)) continue;
				char bf[32] = {};
				char sf[32] = {};
				char pf[32] = {};
				char xf[32] = {};
				char ff[32] = {};
				char ef[32] = {};
				sprintf_s(bf, "%.2f", band);
				sprintf_s(sf, "%.2f", stun);
				sprintf_s(pf, "%.0f", hpPct);
				sprintf_s(xf, "%.2f", hpMult);
				sprintf_s(ff, "%.1f", flesh);
				sprintf_s(ef, "%.1f", flesh - stun);
				int st = -1;
				TrySampleLimbState(parts[i], &st);
				beat += " [" + std::string(PartName(type)) + "/" + std::to_string(side) +
					"=" + ff +
					"/" + std::to_string(static_cast<int>(maxHp)) +
					" p=" + pf + " x=" + xf +
					" b=" + bf + " s=" + sf +
					" eff=" + ef +
					" st=" + std::to_string(st) + "]";
			}
			if (first) beat += " (first)";
		}
	}
	if (logRewrite)
		Logger::Info("HealthManager: engine reset Genius maxHP (" +
			std::to_string(prevFirst) + " -> " + std::to_string(target) +
			") on " + std::to_string(rewrites) + " part(s)");
	if (logBeat)
		Logger::Info(beat);
}

void UpdateStatsHook(MedicalSystem* med)
{
	if (g_origUpdateStats)
		g_origUpdateStats(med);	// Движок только что пересчитал статы (и мог перезаписать _maxHealth
	// из расовых данных) — применяем наши значения поверх.
	if (!med) return;
	Character* c = nullptr;
	__try { c = med->me; } __except (EXCEPTION_EXECUTE_HANDLER) { c = nullptr; }
	if (c && IsGeniusCharacter(c)) {
		HealthManager_ApplyTick(c, "stats");
		// Страховка регена: работает, только если тик медсистемы для этого
		// персонажа перестал вызываться (иначе молчит).
		RegenSystem_ApplyFallback(c);
	}
}

// Хук кадрового пересчёта части тела: движок пересчитывает _maxHealth
// из расовых данных ВНУТРИ medicalUpdate, поэтому применяем наши значения
// сразу после штатного пересчёта — без борьбы каждый кадр и без спама.
bool PartUpdateHook(MedicalSystem::HealthPartStatus* part, float frameTIME,
	float healMultFlesh, float healMultRobot, float degenerationRate,
	float _age, float robotWear)
{
	// Для Genius гниение ран выключаем на входе: движок может сливать flesh
	// и через degenerationRate, и отрицательным healMultFlesh (раны
	// бодрствующего персонажа). Гасим ОБА канала, иначе наш реген (0.1%/с)
	// тонет в сливе и HP «стоит», как в ваниле. Протезы (wear) не трогаем.
	bool geniusPart = false;
	Character* owner = nullptr;
	float drainHeal = 0.0f;
	float drainDegen = 0.0f;
	__try {
		if (part && !part->isRobotic()) {
			Character* me = part->me;
			if (me && IsGeniusCharacter(me)) {
				geniusPart = true;
				owner = me;
				drainHeal = healMultFlesh;
				drainDegen = degenerationRate;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) { geniusPart = false; }
	if (geniusPart) {
		if (drainHeal > 0.0f) NoteEngineHeal(owner, drainHeal);
		if (drainHeal < 0.0f) {
			LogEngineDrain(owner, drainHeal, drainDegen);
			healMultFlesh = 0.0f;
		}
		degenerationRate = 0.0f;
	}
	const bool r = g_origPartUpdate ?
		g_origPartUpdate(part, frameTIME, healMultFlesh, healMultRobot,
			degenerationRate, _age, robotWear) : false;
	__try {
		if (part && !part->isRobotic()) {
			Character* me = part->me;
			if (me && IsGeniusCharacter(me)) {
				const int target = GeniusTargetMaxHP(GetGeniusToughness(me));
				if (static_cast<int>(part->_maxHealth) != target)
					part->_maxHealth = static_cast<float>(target);
				if (part->flesh > part->_maxHealth)
					part->flesh = part->_maxHealth;
				// Страховка регена: движок только что пересчитал эту часть
				// (здесь он и меняет flesh). Если тик медсистемы почему-то
				// не зовётся для персонажа, реген всё равно идёт отсюда —
				// в любом состоянии, а не только в ноке. Пока тик жив,
				// страховка молчит и не двоит начисление.
				RegenSystem_ApplyFallback(me);
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	return r;
}

// Хук валидации HP движком: ещё одна точка, где _maxHealth может
// перезаписываться из расовых данных. Применяем наши значения поверх.
void ValidateHook(MedicalSystem* med)
{
	if (g_origValidate)
		g_origValidate(med);
	if (!med) return;
	Character* c = nullptr;
	__try { c = med->me; } __except (EXCEPTION_EXECUTE_HANDLER) { c = nullptr; }
	if (c && IsGeniusCharacter(c)) {
		HealthManager_ApplyTick(c, "validate");
		// Страховка регена: работает, только если тик медсистемы для этого
		// персонажа перестал вызываться (иначе молчит).
		RegenSystem_ApplyFallback(c);
	}
}

bool HealthManager_InstallHook()
{
	// Все три хука ставим независимо: провал одного адреса НЕ должен
	// отключать остальные. Раньше первый же провал делал return false,
	// и пересчёт _maxHealth не ставился вообще, а в лог всё равно шло
	// «all systems online» — из-за этого поломку нельзя было увидеть.
	bool ok = true;

	const intptr_t addr = KenshiLib::GetRealAddress(&MedicalSystem::_NV_updateStats);
	if (!addr) {
		ok = false;
		Logger::Warn("HealthManager: MedicalSystem::_NV_updateStats RVA not found");
	} else if (KenshiLib::AddHook(addr, (void*)&UpdateStatsHook, &g_origUpdateStats) != KenshiLib::SUCCESS) {
		ok = false;
		Logger::Warn("HealthManager: hook _NV_updateStats failed");
	} else {
		Logger::Info("HealthManager: _NV_updateStats hook installed");
	}

	const intptr_t partAddr = KenshiLib::GetRealAddress(&MedicalSystem::HealthPartStatus::update);
	if (!partAddr) {
		ok = false;
		Logger::Warn("HealthManager: HealthPartStatus::update RVA not found (engine can keep resetting _maxHealth)");
	} else if (KenshiLib::AddHook(partAddr, (void*)&PartUpdateHook, &g_origPartUpdate) != KenshiLib::SUCCESS) {
		ok = false;
		Logger::Warn("HealthManager: hook HealthPartStatus::update failed");
	} else {
		Logger::Info("HealthManager: HealthPartStatus::update hook installed");
	}

	const intptr_t valAddr = KenshiLib::GetRealAddress(&MedicalSystem::validateHealthValues);
	if (!valAddr) {
		ok = false;
		Logger::Warn("HealthManager: validateHealthValues RVA not found");
	} else if (KenshiLib::AddHook(valAddr, (void*)&ValidateHook, &g_origValidate) != KenshiLib::SUCCESS) {
		ok = false;
		Logger::Warn("HealthManager: hook validateHealthValues failed");
	} else {
		Logger::Info("HealthManager: validateHealthValues hook installed");
	}
	return ok;
}
