#include "pch.h"
#include "../include/RegenSystem.h"
#include "../include/Logger.h"
#include <cstdio>

namespace {
	// POD-only: можно __try. Возврат: -1 недоступно/сбой, 0 протез,
	// 1 лечить нечего, 2 вылечено (deltaFlesh/deltaStun и новое flesh).
	// Стан (fleshStun) снимаем с той же скоростью, что и flesh: иначе
	// «полоска» стана остаётся на движке (ванильно медленно) и персонаж
	// лежит в ноке при полном HP.
	int TryRegenPart(MedicalSystem::HealthPartStatus* part, float rate,
		float* outFleshDelta, float* outFleshNew, float* outStunDelta)
	{
		__try {
			if (!part) return -1;
			if (part->isRobotic()) return 0;
			const float maxHp = part->_maxHealth;
			if (!(maxHp > 0.0f)) return -1;
			const float amount = maxHp * rate;
			float dFlesh = 0.0f;
			float dStun = 0.0f;
			if (part->flesh < maxHp) {
				float healed = part->flesh + amount;
				if (healed > maxHp) healed = maxHp;
				dFlesh = healed - part->flesh;
				part->flesh = healed;
			}
			if (GeniusHealStun && part->fleshStun > 0.0f) {
				float stun = part->fleshStun - amount;
				if (stun < 0.0f) stun = 0.0f;
				dStun = part->fleshStun - stun;
				part->fleshStun = stun;
			}
			if (dFlesh > 0.0f || dStun > 0.0f) {
				if (outFleshDelta) *outFleshDelta = dFlesh;
				if (outFleshNew) *outFleshNew = part->flesh;
				if (outStunDelta) *outStunDelta = dStun;
				return 2;
			}
			return 1;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return -1;
		}
	}

	bool TryReadFlesh(MedicalSystem::HealthPartStatus* part, float* outFlesh)
	{
		__try {
			if (!part || !outFlesh) return false;
			*outFlesh = part->flesh;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool TryReadStun(MedicalSystem::HealthPartStatus* part, float* outStun)
	{
		__try {
			if (!part || !outStun) return false;
			*outStun = part->fleshStun;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Часть ниже максимума прямо сейчас (для строки лога).
	bool TryPartBelowMax(MedicalSystem::HealthPartStatus* part, float* outFlesh, float* outMaxHp)
	{
		__try {
			if (!part || !outFlesh || !outMaxHp) return false;
			if (part->isRobotic()) return false;
			const float maxHp = part->_maxHealth;
			if (!(maxHp > 0.0f)) return false;
			*outFlesh = part->flesh;
			*outMaxHp = maxHp;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	std::mutex g_stateMutex;

	// ТРЕБОВАНИЕ: реген идёт одинаково в любом состоянии персонажа —
	// в ноке, в сознании, в бою, и даже если движок перестал звать
	// medicalUpdate для этого персонажа. Отсюда два пути начисления:
	//
	//  - основной: тик medicalUpdate, dt = время кадра движка (та же шкала
	//    времени, что у остальных механик игры);
	//  - страховочный: хуки, где времени кадра нет (часть тела, статы,
	//    validateHealthValues) — двигаем реген по реальному времени,
	//    умноженному на измеренный коэффициент «секунд движка на реальную
	//    секунду» (EMA по основному пути).
	//
	// Двойного начисления нет: страховка молчит, пока основной тик жив
	// (менее 250 мс назад он что-то начислил).
	struct State
	{
		ULONGLONG lastLog;           // троттлинг сводки (10 c)
		ULONGLONG lastApply;         // последнее начисление (любым путём)
		ULONGLONG lastEngine;        // последнее начисление по времени движка
		ULONGLONG lastEngineSample;  // предыдущий замер для коэффициента
		bool hasApply;
		bool hasEngine;
		bool hasEngineSample;
		bool hasRatio;
		float ratio;                 // секунд движка на реальную секунду (EMA)

		// Интервальная статистика: `mod` — наш прирост, `eng` — чистое
		// изменение flesh движком за тот же интервал (по снимку flesh),
		// `lost` — сколько из записанного нами движок стёр к следующему
		// кадру (прямое доказательство, что flesh перезаписывается движком).
		float healedSinceLog;
		float lostSinceLog;
		float stunSinceLog;
		float lostStunSinceLog;
		float dtSinceLog;
		int healEvents;
		int roboticEvents;
		int invalidEvents;
		int frames;
		int engineApplies;           // начислений по тику
		int idleApplies;             // начислений страховочным путём
		bool sawBelowMax;

		// Снимок flesh/fleshStun сразу после нашей записи в прошлом кадре —
		// с ним сравниваем этот кадр, чтобы увидеть потери.
		MedicalSystem::HealthPartStatus* lastWrittenPart[16];
		float lastWrittenFlesh[16];
		float lastWrittenStun[16];
		int lastWrittenCount;

		MedicalSystem::HealthPartStatus* prevPart[16];
		float prevFlesh[16];
		int prevCount;
	};
	std::map<Character*, State> g_state;

	// Ядро: начислить реген за dt, посчитать статистику, раз в 10 c вывести
	// строку. engineDt < 0 — времени кадра нет (страховочный путь).
	float ApplyRegen(Character* character, float engineDt, bool haveEngineDt)
	{
		if (!IsGeniusCharacter(character)) return 0.0f;
		const ULONGLONG now = GetTickCount64();

		MedicalSystem::HealthPartStatus* parts[16];
		const int n = CollectBodyParts(character, parts, 16);

		// Части, которые мы вылечили в этом кадре (снимок для замера потерь).
		MedicalSystem::HealthPartStatus* writtenPart[16];
		float writtenFlesh[16];
		float writtenStun[16];
		int writtenCount = 0;

		// Потери: сколько из записанного нами в прошлом кадре движок стёр к
		// этому кадру (отдельно flesh и стан). Прямое доказательство, что
		// движок перезаписывает наши значения (например, выводя их из
		// «бинта»/раны): тогда наш реген уходит в никуда.
		float lost = 0.0f;
		float lostStun = 0.0f;

		float dt = 0.0f;
		{
			std::lock_guard<std::mutex> lk(g_stateMutex);
			State& s = g_state[character];
			if (haveEngineDt) {
				if (!(engineDt > 0.0f)) engineDt = 0.0f;
				if (engineDt > 1.0f) engineDt = 1.0f;
				// Коэффициент «движковое время / реальное время»: у
				// Kenshi игровые часы идут быстрее реальных, поэтому
				// 0.1%/с по времени кадра — это быстрее, чем 0.1% за
				// реальную секунду. Страховочный путь должен давать
				// ту же скорость, что и основной, — отсюда замер.
				if (s.hasEngineSample) {
					const float realDt = (now - s.lastEngineSample) / 1000.0f;
					if (realDt > 0.0005f) {
						const float inst = engineDt / realDt;
						if (inst > 0.05f && inst < 64.0f) {
							s.ratio = s.hasRatio ? (s.ratio * 0.9f + inst * 0.1f) : inst;
							s.hasRatio = true;
						}
					}
				}
				s.lastEngineSample = now;
				s.hasEngineSample = true;
				dt = engineDt;
				s.lastEngine = now;
				s.hasEngine = true;
				++s.engineApplies;
			} else {
				// Страховка: если основной тик жив, он начислит сам.
				if (s.hasEngine && now - s.lastEngine <= 250) return 0.0f;
				float since = s.hasApply ? (now - s.lastApply) / 1000.0f : 0.0f;
				if (since < 0.0f) since = 0.0f;
				if (since > 1.0f) since = 1.0f;
				dt = since * (s.hasRatio ? s.ratio : 1.0f);
				if (!(dt > 0.0f)) return 0.0f;
				// Предохранитель: страховка не может «выстрелить» больше,
				// чем ~0.4% MaxHP за одно начисление, даже если коэффициент
				// успел измериться неверно или был длинный столл кадров.
				if (dt > 4.0f) dt = 4.0f;
				++s.idleApplies;
			}
			s.lastApply = now;
			s.hasApply = true;

			// Потери считаем до начисления этого кадра (flesh/стан ещё «как
			// оставил движок») и только по частям, которые мы лечили в
			// прошлом кадре. Совпадение указателя гарантирует, что часть
			// жива. Растущее значение потерь = движок стирает нашу запись.
			for (int i = 0; i < n; ++i) {
				if (!parts[i]) continue;
				for (int j = 0; j < s.lastWrittenCount; ++j) {
					if (s.lastWrittenPart[j] != parts[i]) continue;
					float curF = 0.0f, curS = 0.0f;
					if (TryReadFlesh(parts[i], &curF)) {
						const float drop = s.lastWrittenFlesh[j] - curF;
						if (drop > 0.0f) lost += drop;
					}
					if (TryReadStun(parts[i], &curS)) {
						const float dropS = s.lastWrittenStun[j] - curS;
						if (dropS > 0.0f) lostStun += dropS;
					}
					break;
				}
			}
		}

		const float rate = GeniusRegenPerSecond * dt; // доля MaxHP за тик
		float healedSum = 0.0f;
		float stunSum = 0.0f;
		int healEvents = 0, roboticEvents = 0, invalidEvents = 0;
		for (int i = 0; i < n; ++i) {
			float delta = 0.0f, newFlesh = 0.0f, deltaStun = 0.0f;
			switch (TryRegenPart(parts[i], rate, &delta, &newFlesh, &deltaStun)) {
			case 2:
				healedSum += delta;
				stunSum += deltaStun;
				++healEvents;
				if (writtenCount < 16) {
					writtenPart[writtenCount] = parts[i];
					writtenFlesh[writtenCount] = newFlesh;
					writtenStun[writtenCount] = 0.0f;
					TryReadStun(parts[i], &writtenStun[writtenCount]);
					++writtenCount;
				}
				break;
			case 1: break; // лечить нечего
			case 0: ++roboticEvents; break;
			default: ++invalidEvents; break;
			}
		}
		// Потери засчитываем, только если мы в этом кадре что-то записали
		// (иначе снимок не обновится и то же самое посчитается дважды).
		if (writtenCount == 0) {
			lost = 0.0f;
			lostStun = 0.0f;
		}
		const bool belowThisFrame = (healEvents + roboticEvents + invalidEvents) > 0;

		// Сводка раз в 10 c: пара mod/eng показывает момент, когда наш реген
		// перестаёт быть тем, что двигает HP.
		bool show = false;
		bool noEffect = false;
		bool engineCancels = false;
		bool engineRewrites = false;
		bool stunStuck = false;
		float healedForLog = 0.0f;
		float engForLog = 0.0f;
		float lostForLog = 0.0f;
		float stunForLog = 0.0f;
		float lostStunForLog = 0.0f;
		float dtForLog = 0.0f;
		int healEventsForLog = 0, roboticForLog = 0, invalidForLog = 0, framesForLog = 0;
		int engineAppliesForLog = 0, idleAppliesForLog = 0, realMs = 0;
		{
			std::lock_guard<std::mutex> lk(g_stateMutex);
			State& s = g_state[character];
			s.healedSinceLog += healedSum;
			s.lostSinceLog += lost;
			s.stunSinceLog += stunSum;
			s.lostStunSinceLog += lostStun;
			s.dtSinceLog += dt;
			s.healEvents += healEvents;
			s.roboticEvents += roboticEvents;
			s.invalidEvents += invalidEvents;
			++s.frames;
			if (belowThisFrame) s.sawBelowMax = true;

			// Снимок того, что мы записали в этом кадре: с ним сравним
			// flesh/стан в следующем кадре, чтобы увидеть потери.
			s.lastWrittenCount = 0;
			for (int i = 0; i < writtenCount && s.lastWrittenCount < 16; ++i) {
				s.lastWrittenPart[s.lastWrittenCount] = writtenPart[i];
				s.lastWrittenFlesh[s.lastWrittenCount] = writtenFlesh[i];
				s.lastWrittenStun[s.lastWrittenCount] = writtenStun[i];
				++s.lastWrittenCount;
			}

			if (now - s.lastLog >= 10000) {
				realMs = static_cast<int>(now - s.lastLog);
				s.lastLog = now;

				// Чистое изменение flesh движком: сопоставляем части по
				// указателю (старые указатели не разыменовываем).
				float eng = 0.0f;
				for (int i = 0; i < n; ++i) {
					if (!parts[i]) continue;
					for (int j = 0; j < s.prevCount; ++j) {
						if (s.prevPart[j] != parts[i]) continue;
						float cur = 0.0f;
						if (TryReadFlesh(parts[i], &cur)) eng += cur - s.prevFlesh[j];
						break;
					}
				}

				healedForLog = s.healedSinceLog;
				engForLog = eng;
				lostForLog = s.lostSinceLog;
				stunForLog = s.stunSinceLog;
				lostStunForLog = s.lostStunSinceLog;
				dtForLog = s.dtSinceLog;
				healEventsForLog = s.healEvents;
				roboticForLog = s.roboticEvents;
				invalidForLog = s.invalidEvents;
				framesForLog = s.frames;
				engineAppliesForLog = s.engineApplies;
				idleAppliesForLog = s.idleApplies;
				noEffect = (s.healEvents == 0 && s.sawBelowMax);
				engineCancels = (s.healedSinceLog > 0.0f && eng < -0.25f * s.healedSinceLog);
				// Движок стирает нашу запись: то, что мы вылечили, к
				// следующему кадру исчезает. Тогда видно только ванильное
				// восстановление (бинт/рана), а наш реген уходит в никуда.
				engineRewrites = (s.healedSinceLog > 0.3f && s.lostSinceLog > 0.3f * s.healedSinceLog);
				// Стан снимаем мы, но движок возвращает его обратно.
				stunStuck = (s.stunSinceLog > 0.3f && s.lostStunSinceLog > 0.5f * s.stunSinceLog);
				show = (s.healedSinceLog > 0.0f) || (s.stunSinceLog > 0.0f) ||
					(s.invalidEvents > 0) || noEffect;

				s.healedSinceLog = 0.0f;
				s.lostSinceLog = 0.0f;
				s.stunSinceLog = 0.0f;
				s.lostStunSinceLog = 0.0f;
				s.dtSinceLog = 0.0f;
				s.healEvents = 0;
				s.roboticEvents = 0;
				s.invalidEvents = 0;
				s.frames = 0;
				s.engineApplies = 0;
				s.idleApplies = 0;
				s.sawBelowMax = false;

				// Новый снимок flesh (уже с нашим приростом этого кадра).
				s.prevCount = 0;
				for (int i = 0; i < n && s.prevCount < 16; ++i) {
					float f = 0.0f;
					if (!TryReadFlesh(parts[i], &f)) continue;
					s.prevPart[s.prevCount] = parts[i];
					s.prevFlesh[s.prevCount] = f;
					++s.prevCount;
				}
			}
		}
		if (show) {
			int below = 0;
			for (int i = 0; i < n; ++i) {
				float f = 0.0f, mx = 0.0f;
				if (TryPartBelowMax(parts[i], &f, &mx) && f < mx) ++below;
			}
			char bufMod[32] = {};
			char bufEng[32] = {};
			char bufLost[32] = {};
			char bufStun[32] = {};
			char bufLostStun[32] = {};
			char bufDt[48] = {};
			sprintf_s(bufMod, "%+.2f", healedForLog);
			sprintf_s(bufEng, "%+.2f", engForLog);
			sprintf_s(bufLost, "%.2f", lostForLog);
			sprintf_s(bufStun, "%+.2f", stunForLog);
			sprintf_s(bufLostStun, "%.2f", lostStunForLog);
			sprintf_s(bufDt, "%.1fs/%.1fs", dtForLog, realMs / 1000.0f);
			const std::string head = std::string("Regen: mod=") + bufMod + "HP eng=" + bufEng +
				"HP lost=" + bufLost +
				"HP stun=" + bufStun + "HP lostStun=" + bufLostStun +
				"HP dt=" + bufDt + " frames=" + std::to_string(framesForLog) +
				" tick=" + std::to_string(engineAppliesForLog) +
				" idle=" + std::to_string(idleAppliesForLog) +
				" below=" + std::to_string(below) +
				" robotic=" + std::to_string(roboticForLog) +
				" invalid=" + std::to_string(invalidForLog);
			if (noEffect) {
				Logger::Warn(head + " - part is below max but mod healed 0 HP (regen not applied)");
			} else if (engineRewrites) {
				Logger::Warn(head + " - engine erases what mod wrote (flesh rewritten; heal goes through the engine wound/bandage, not flesh)");
			} else if (engineCancels) {
				Logger::Warn(head + " - engine removed what mod healed (regen lost to engine drain)");
			} else if (stunStuck) {
				Logger::Warn(head + " - engine puts stun back faster than mod removes it");
			} else {
				Logger::Info(head);
			}
		}
		return healedSum;
	}
}

// Основной путь: тик medicalUpdate, dt — время кадра движка. dt <= 0 тоже
// принимаем: это значит «тик жив, но время не идёт» (например, кадр с
// нулевым frameTime) — тогда страховочный путь молчит, а не лечит по
// реальному времени.
float RegenSystem_ApplyTick(Character* character, float dtSeconds)
{
	return ApplyRegen(character, dtSeconds, true);
}

// Страховочный путь: движок не дал времени кадра (хуки части тела, статов,
// validateHealthValues). Работает по реальному времени с измеренным
// коэффициентом, поэтому темп регена тот же, что и в тике. Нужен, чтобы
// реген шёл в любом состоянии персонажа, даже если движок перестал звать
// medicalUpdate для него.
float RegenSystem_ApplyFallback(Character* character)
{
	return ApplyRegen(character, 0.0f, false);
}
