#ifndef _INCLUDE_WL_WHITELIST_MANAGER_H_
#define _INCLUDE_WL_WHITELIST_MANAGER_H_

#include "common.h"
#include <string>
#include <unordered_set>
#include <tier1/convar.h>

// ConVar declarations
extern CConVar<bool> cv_enable;
extern CConVar<bool> cv_immunity;
extern CConVar<CUtlString> cv_kickmessage;
extern CConVar<CUtlString> cv_filename;
extern CConVar<int> cv_log;

// Build the absolute path to the active whitelist file.
std::string GetWhitelistFilePath();

// Log a kick to the server console and (if cv_log > 0) to the daily log file.
// cv_log == 1: always  cv_log == 2: only first time per player per map.
// Pass alreadyCached=true when the player was already in the blacklist cache.
void WLLogKick(const char *name, uint64_t xuid, const char *ip, bool alreadyCached);

class WLManager
{
public:
	bool LoadFile();
	bool SaveFile();

	bool AddEntry(const char *entry);
	bool RemoveEntry(const char *entry);

	bool IsPlayerWhitelisted(int slot) const;
	bool IsEntryWhitelisted(const char *entry) const;

	int GetEntryCount() const
	{
		return static_cast<int>(m_whitelist.size());
	}

	void PrintList(int slot) const;

	// Blacklist cache: tracks xuids rejected this map to skip checking repeatedly on reconnects.
	bool IsBlacklisted(uint64_t xuid) const;
	void AddToBlacklistCache(uint64_t xuid);
	void ClearBlacklistCache();
	int GetBlacklistCacheCount() const { return static_cast<int>(m_blacklistCache.size()); }

	// Whitelist cache: tracks xuids confirmed whitelisted this map.
	// Allows in on reconnect (and future async checks like Steam groups).
	bool IsWhitelistCached(uint64_t xuid) const;
	void AddToWhitelistCache(uint64_t xuid);
	void ClearWhitelistCache();
	int GetWhitelistCacheCount() const { return static_cast<int>(m_whitelistCache.size()); }

	std::unordered_set<std::string> &GetSet()
	{
		return m_whitelist;
	}

private:
	std::unordered_set<std::string> m_whitelist;
	std::unordered_set<uint64_t> m_blacklistCache;
	std::unordered_set<uint64_t> m_whitelistCache;
};

extern WLManager g_WLManager;

#endif // _INCLUDE_WL_WHITELIST_MANAGER_H_
