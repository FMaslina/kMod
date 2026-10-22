// dllmain.cpp : точка входа DLL.
#include "pch.h"
#include <cstring>

namespace {
	void WriteLoadMarker()
	{
		// Маркер загрузки — в %TEMP%, он доступен для записи всегда
		// (в отличие от каталога игры). Доказывает, что Windows загрузила
		// DLL, даже если RE_Kenshi не вызвал startPlugin.
		wchar_t temp[MAX_PATH] = {};
		if (GetTempPathW(MAX_PATH, temp) == 0) return;
		std::wstring path(temp);
		path += L"Genius_kMod_loaded.txt";
		HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return;
		SetFilePointer(h, 0, nullptr, FILE_END);
		char stamp[128] = {};
		DWORD tick = GetTickCount();
		int n = 0;
		// Короткая ASCII-строка без CRT-зависимостей форматирования.
		const char* p1 = "kMod.dll loaded, tick=";
		DWORD w = 0;
		WriteFile(h, p1, (DWORD)strlen(p1), &w, nullptr);
		char num[16];
		int len = 0;
		DWORD v = tick;
		if (v == 0) { num[len++] = '0'; }
		else { char rev[16]; int r = 0; while (v && r < 15) { rev[r++] = (char)('0' + v % 10); v /= 10; } while (r--) num[len++] = rev[r]; }
		num[len++] = '\r'; num[len++] = '\n';
		WriteFile(h, num, len, &w, nullptr);
		(void)n; (void)stamp;
		FlushFileBuffers(h);
		CloseHandle(h);
	}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hModule);
		OutputDebugStringA("Genius kMod: DLL_PROCESS_ATTACH\n");
		WriteLoadMarker();
		break;
	case DLL_PROCESS_DETACH:
		break;
	}
	(void)lpReserved;
	return TRUE;
}
