#pragma once
#include <string>

struct WLConfig
{
	// [Config] section
	bool enable = true;
	bool immunity = true;
	std::string kickMessage = "You are not whitelisted on this server.";
	std::string filename = "whitelist.txt";
	int logMode = 0; // 0=off  1=always  2=once per player per map

	// [Database] section
	bool dbEnabled = false;
	std::string dbType = "sqlite";
	std::string dbHost = "localhost";
	std::string dbUser = "root";
	std::string dbPass = "";
	std::string dbName = "cs2whitelist";
	int dbPort = 3306;
	std::string dbPath = "addons/cs2whitelist/whitelist.db";
	std::string dbPrefix = "cs2wl";
};

// Parse cfg/cs2whitelist/core.cfg into out.
// Returns true if the file was successfully opened and parsed.
bool WL_LoadConfig(const char *filePath, WLConfig &out);

extern WLConfig g_WLConfig;
