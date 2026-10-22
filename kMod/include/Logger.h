#pragma once
#include <string>

namespace Logger {
	void Init();
	void Shutdown();
	void Info(const std::string &msg);
	void Warn(const std::string &msg);
	void Error(const std::string &msg);
	// Куда реально упал лог-файл (для диагностики).
	std::string LogFilePath();
}
