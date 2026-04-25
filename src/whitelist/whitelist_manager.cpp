#include "whitelist_manager.h"
#include "player/player_manager.h"
#include "utils/utils.h"
#include "db/wl_database.h"

#include <eiface.h>
#include <filesystem>
#include <fstream>
#include <ctime>

// ConVars
CConVar<bool> cv_enable("mm_whitelist_enable", FCVAR_RELEASE | FCVAR_GAMEDLL, "Enable the server whitelist (1) or disable it (0).", true);

CConVar<bool> cv_immunity("mm_whitelist_immunity", FCVAR_RELEASE | FCVAR_GAMEDLL,
						  "Skip the whitelist check for players with any cs2admin flag "
						  "(requires mm-cs2admin).",
						  true);

CConVar<CUtlString> cv_kickmessage("mm_whitelist_kickmessage", FCVAR_RELEASE | FCVAR_GAMEDLL,
								   "Message sent to the player's console when they are kicked by the whitelist.",
								   "You are not whitelisted on this server.");

CConVar<CUtlString> cv_filename("mm_whitelist_filename", FCVAR_RELEASE | FCVAR_GAMEDLL,
								"Whitelist file name, relative to <game>/cfg/cs2whitelist/. "
								"Path separators are stripped to prevent directory traversal.",
								"whitelist.txt");

CConVar<int> cv_log("mm_whitelist_log", FCVAR_RELEASE | FCVAR_GAMEDLL,
				   "Log failed join attempts to console and daily log file. "
				   "0=off  1=always  2=once per player per map.",
				   0, true, 0, true, 2);

// Global instance
WLManager g_WLManager;

// File path helper
std::string GetWhitelistFilePath()
{
	const char *raw = cv_filename.Get().Get();
	const char *basename = raw;
	for (const char *p = raw; *p; ++p)
	{
		if (*p == '/' || *p == '\\')
		{
			basename = p + 1;
		}
	}

	char path[512];
	snprintf(path, sizeof(path), "%s/cfg/cs2whitelist/%s", g_SMAPI->GetBaseDir(), basename);
	return path;
}

// File I/O
bool WLManager::LoadFile()
{
	m_whitelist.clear();
	m_blacklistCache.clear();
	m_whitelistCache.clear();

	std::string path = GetWhitelistFilePath();
	std::ifstream file(path);
	if (!file.is_open())
	{
		META_CONPRINTF("[WHITELIST] Could not open whitelist file: %s\n"
					   "[WHITELIST] Create the file with one SteamID or IP per line.\n",
					   path.c_str());
		return false;
	}

	int count = 0;
	std::string line;
	while (std::getline(file, line))
	{
		std::string entry = NormalizeEntry(line.c_str());
		if (!entry.empty())
		{
			m_whitelist.insert(entry);
			++count;
		}
	}

	META_CONPRINTF("[WHITELIST] Loaded %d entries from %s.\n", count, path.c_str());
	return true;
}

bool WLManager::SaveFile()
{
	std::string path = GetWhitelistFilePath();
	std::ofstream file(path);
	if (!file.is_open())
	{
		META_CONPRINTF("[WHITELIST] Could not write whitelist file: %s\n", path.c_str());
		return false;
	}

	file << "// CS2 Whitelist - managed by cs2whitelist plugin\n"
		 << "// One entry per line: STEAM_0:X:Y, SteamID64, or IPv4 address\n"
		 << "// Lines starting with // or # are comments\n\n";

	for (const auto &e : m_whitelist)
	{
		file << e << "\n";
	}

	return true;
}

// Entry management
bool WLManager::AddEntry(const char *entry)
{
	std::string normalized = NormalizeEntry(entry);
	if (normalized.empty())
	{
		return false;
	}

	bool inserted = m_whitelist.insert(normalized).second;
	if (inserted && g_WLDatabase.IsConnected())
	{
		g_WLDatabase.AddEntry(normalized);
	}
	return inserted;
}

bool WLManager::RemoveEntry(const char *entry)
{
	std::string normalized = NormalizeEntry(entry);
	if (normalized.empty())
	{
		return false;
	}

	bool erased = m_whitelist.erase(normalized) > 0;
	if (erased && g_WLDatabase.IsConnected())
	{
		g_WLDatabase.RemoveEntry(normalized);
	}
	return erased;
}

// Queries
bool WLManager::IsPlayerWhitelisted(int slot) const
{
	const PlayerInfo *p = g_WLPlayerManager.GetPlayer(slot);
	if (!p)
	{
		return false;
	}

	if (!p->ip.empty() && m_whitelist.count(p->ip))
	{
		return true;
	}

	if (p->xuid != 0)
	{
		if (m_whitelist.count(SteamID64ToAuthId(p->xuid)))
		{
			return true;
		}

		char buf[32];
		snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(p->xuid));
		if (m_whitelist.count(buf))
		{
			return true;
		}
	}

	return false;
}

bool WLManager::IsEntryWhitelisted(const char *entry) const
{
	std::string normalized = NormalizeEntry(entry);
	if (normalized.empty())
	{
		return false;
	}
	return m_whitelist.count(normalized) > 0;
}

void WLManager::PrintList(int slot) const
{
	ReplyToSlot(slot, "[WHITELIST] %d entries:\n", static_cast<int>(m_whitelist.size()));
	for (const auto &e : m_whitelist)
	{
		ReplyToSlot(slot, "  %s\n", e.c_str());
	}
}

// Blacklist cache
bool WLManager::IsBlacklisted(uint64_t xuid) const
{
	return xuid != 0 && m_blacklistCache.count(xuid) > 0;
}

void WLManager::AddToBlacklistCache(uint64_t xuid)
{
	if (xuid != 0)
		m_blacklistCache.insert(xuid);
}

void WLManager::ClearBlacklistCache()
{
	m_blacklistCache.clear();
}

// Whitelist cache
bool WLManager::IsWhitelistCached(uint64_t xuid) const
{
	return xuid != 0 && m_whitelistCache.count(xuid) > 0;
}

void WLManager::AddToWhitelistCache(uint64_t xuid)
{
	if (xuid != 0)
		m_whitelistCache.insert(xuid);
}

void WLManager::ClearWhitelistCache()
{
	m_whitelistCache.clear();
}

// Kick logging
void WLLogKick(const char *name, uint64_t xuid, const char *ip, bool alreadyCached)
{
	int logMode = cv_log.Get();
	if (logMode == 0)
		return;
	// mode 2: only log first time (alreadyCached == false means first rejection)
	if (logMode == 2 && alreadyCached)
		return;

	std::string authid = xuid ? SteamID64ToAuthId(xuid) : "unknown";

	time_t now = time(nullptr);
	struct tm tm_info;
#ifdef _WIN32
	localtime_s(&tm_info, &now);
#else
	localtime_r(&now, &tm_info);
#endif
	char timebuf[32];
	strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_info);

	char datebuf[16];
	strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", &tm_info);

	const char *safeName = name ? name : "?";
	const char *safeIp = ip ? ip : "?";

	META_CONPRINTF("[WHITELIST] [%s] Kick: \"%s\" xuid=%llu authid=%s ip=%s\n",
				   timebuf, safeName,
				   static_cast<unsigned long long>(xuid),
				   authid.c_str(), safeIp);

	char logDir[512];
	snprintf(logDir, sizeof(logDir), "%s/addons/cs2whitelist/logs", g_SMAPI->GetBaseDir());

	std::error_code ec;
	std::filesystem::create_directories(logDir, ec);

	char logPath[600];
	snprintf(logPath, sizeof(logPath), "%s/%s.log", logDir, datebuf);

	std::ofstream f(logPath, std::ios::app);
	if (f.is_open())
	{
		f << "[" << timebuf << "] Kick: \""
		  << safeName << "\" xuid=" << xuid
		  << " authid=" << authid
		  << " ip=" << safeIp << "\n";
	}
}
