#ifndef _INCLUDE_CS2WHITELIST_H_
#define _INCLUDE_CS2WHITELIST_H_

#include "common.h"
#include "version_gen.h"
#include "public/ics2whitelist.h"

#include <vector>

class CS2WhitelistPlugin : public ISmmPlugin, public IMetamodListener, public ICS2Whitelist
{
public:
	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late);
	bool Unload(char *error, size_t maxlen);
	void AllPluginsLoaded();
	void *OnMetamodQuery(const char *iface, int *ret);

public: // IMetamodListener
	void OnLevelInit(char const *pMapName, char const *pMapEntities, char const *pOldLevel, char const *pLandmarkName, bool loadGame,
					 bool background);

public: // ISmmPlugin info
	const char *GetAuthor()
	{
		return PLUGIN_AUTHOR;
	}

	const char *GetName()
	{
		return PLUGIN_DISPLAY_NAME;
	}

	const char *GetDescription()
	{
		return PLUGIN_DESCRIPTION;
	}

	const char *GetURL()
	{
		return PLUGIN_URL;
	}

	const char *GetLicense()
	{
		return PLUGIN_LICENSE;
	}

	const char *GetVersion()
	{
		return PLUGIN_FULL_VERSION;
	}

	const char *GetDate()
	{
		return __DATE__;
	}

	const char *GetLogTag()
	{
		return PLUGIN_LOGTAG;
	}

public: // SH hook handlers (thin wrappers — logic lives in the managers)
	void Hook_OnClientConnected(CPlayerSlot slot, const char *pszName, uint64 xuid, const char *pszNetworkID, const char *pszAddress,
								bool bFakePlayer);
	void Hook_ClientPutInServer(CPlayerSlot slot, char const *pszName, int type, uint64 xuid);
	void Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason, const char *pszName, uint64 xuid, const char *pszNetworkID);

public: // ICS2Whitelist (delegates to g_WLManager)
	bool IsPlayerWhitelisted(int slot) const override;
	bool IsEntryWhitelisted(const char *entry) const override;
	int GetEntryCount() const override;

	bool IsPlayerWhitelistCached(int slot) const override;
	int GetWhitelistCacheCount() const override;

	bool IsPlayerBlacklisted(int slot) const override;
	int GetBlacklistCacheCount() const override;

	bool ReloadFile() override;
	bool AddEntry(const char *entry) override;
	bool RemoveEntry(const char *entry) override;

	void AddListener(ICS2WhitelistListener *listener) override;
	void RemoveListener(ICS2WhitelistListener *listener) override;

private:
	bool m_bLateLoaded = false;
	std::vector<ICS2WhitelistListener *> m_listeners;
};

extern CS2WhitelistPlugin g_ThisPlugin;

#endif // _INCLUDE_CS2WHITELIST_H_
