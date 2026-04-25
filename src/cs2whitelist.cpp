#include <cstring>
#include <algorithm>

#include "cs2whitelist.h"
#include "db/wl_config.h"
#include "db/wl_database.h"
#include "ics2admin.h"
#include "player/player_manager.h"
#include "utils/utils.h"
#include "whitelist/whitelist_manager.h"

#include <eiface.h>
#include <iserver.h>
#include <tier1/convar.h>

// SourceHook hook declarations
SH_DECL_HOOK6_void(IServerGameClients, OnClientConnected, SH_NOATTRIB, 0, CPlayerSlot, const char *, uint64, const char *, const char *, bool);
SH_DECL_HOOK4_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0, CPlayerSlot, char const *, int, uint64);
SH_DECL_HOOK5_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, CPlayerSlot, ENetworkDisconnectionReason, const char *, uint64,
				   const char *);

// Plugin instance
CS2WhitelistPlugin g_ThisPlugin;
PLUGIN_EXPOSE(CS2WhitelistPlugin, g_ThisPlugin);

// Engine + Metamod globals
PLUGIN_GLOBALVARS();
IVEngineServer *g_pEngine = nullptr;
IServerGameClients *g_pGameClients = nullptr;
IServerGameDLL *g_pServerGameDLL = nullptr;
ICvar *g_pICvar = nullptr;
ICS2Admin *g_pCS2Admin = nullptr;

// Load / Unload
bool CS2WhitelistPlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pICvar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pServerGameDLL, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_ANY(GetServerFactory, g_pGameClients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS);

	m_bLateLoaded = late;
	g_SMAPI->AddListener(this, this);

	SH_ADD_HOOK(IServerGameClients, OnClientConnected, g_pGameClients, SH_MEMBER(this, &CS2WhitelistPlugin::Hook_OnClientConnected), false);
	SH_ADD_HOOK(IServerGameClients, ClientPutInServer, g_pGameClients, SH_MEMBER(this, &CS2WhitelistPlugin::Hook_ClientPutInServer), true);
	SH_ADD_HOOK(IServerGameClients, ClientDisconnect, g_pGameClients, SH_MEMBER(this, &CS2WhitelistPlugin::Hook_ClientDisconnect), true);

	g_pCVar = g_pICvar;
	META_CONVAR_REGISTER(FCVAR_RELEASE | FCVAR_GAMEDLL);

	g_WLManager.LoadFile();

	META_CONPRINTF("[WHITELIST] Plugin loaded (v%s)%s.\n", PLUGIN_FULL_VERSION, late ? " [late]" : "");
	return true;
}

bool CS2WhitelistPlugin::Unload(char *error, size_t maxlen)
{
	SH_REMOVE_HOOK(IServerGameClients, OnClientConnected, g_pGameClients, SH_MEMBER(this, &CS2WhitelistPlugin::Hook_OnClientConnected), false);
	SH_REMOVE_HOOK(IServerGameClients, ClientPutInServer, g_pGameClients, SH_MEMBER(this, &CS2WhitelistPlugin::Hook_ClientPutInServer), true);
	SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, g_pGameClients, SH_MEMBER(this, &CS2WhitelistPlugin::Hook_ClientDisconnect), true);

	g_WLDatabase.Shutdown();
	META_CONPRINTF("[WHITELIST] Plugin unloaded.\n");
	return true;
}

// AllPluginsLoaded / OnLevelInit
void CS2WhitelistPlugin::AllPluginsLoaded()
{
	g_pCS2Admin = static_cast<ICS2Admin *>(g_SMAPI->MetaFactory(CS2ADMIN_INTERFACE, nullptr, nullptr));

	if (g_pCS2Admin)
	{
		META_CONPRINTF("[WHITELIST] mm-cs2admin interface acquired! "
					   "Admin immunity and in-game commands enabled.\n");
	}
	else
	{
		META_CONPRINTF("[WHITELIST] mm-cs2admin not loaded. "
					   "Admin commands restricted to server console.\n");
	}

	char cfgPath[512];
	snprintf(cfgPath, sizeof(cfgPath), "%s/cfg/cs2whitelist/core.cfg", g_SMAPI->GetBaseDir());

	if (WL_LoadConfig(cfgPath, g_WLConfig))
	{
		cv_enable.Set(g_WLConfig.enable);
		cv_immunity.Set(g_WLConfig.immunity);
		cv_kickmessage.Set(CUtlString(g_WLConfig.kickMessage.c_str()));
		cv_filename.Set(CUtlString(g_WLConfig.filename.c_str()));
		cv_log.Set(g_WLConfig.logMode);
		META_CONPRINTF("[WHITELIST] Loaded core.cfg.\n");
	}
	else
	{
		META_CONPRINTF("[WHITELIST] core.cfg not found, using ConVar defaults.\n");
	}

	if (g_WLDatabase.Init(g_WLConfig))
	{
		g_WLDatabase.Connect(
			[this](bool success)
			{
				if (success)
				{
					g_WLDatabase.LoadEntries(g_WLManager.GetSet(),
											 [](int count) { META_CONPRINTF("[WHITELIST] Loaded %d entries from database.\n", count); });
				}
			});
	}
}

void CS2WhitelistPlugin::OnLevelInit(char const *pMapName, char const *pMapEntities, char const *pOldLevel, char const *pLandmarkName, bool loadGame,
									 bool background)
{
	g_WLManager.ClearBlacklistCache();
	g_WLManager.ClearWhitelistCache();
	g_WLManager.LoadFile();

	if (g_WLDatabase.IsConnected())
	{
		g_WLDatabase.LoadEntries(g_WLManager.GetSet(), [](int count) { META_CONPRINTF("[WHITELIST] Merged %d DB entries on map load.\n", count); });
	}
}

// OnMetamodQuery
void *CS2WhitelistPlugin::OnMetamodQuery(const char *iface, int *ret)
{
	if (!strcmp(iface, CS2WHITELIST_INTERFACE))
	{
		if (ret)
		{
			*ret = META_IFACE_OK;
		}
		return static_cast<ICS2Whitelist *>(this);
	}
	if (ret)
	{
		*ret = META_IFACE_FAILED;
	}
	return nullptr;
}

// SH hooks
void CS2WhitelistPlugin::Hook_OnClientConnected(CPlayerSlot slot, const char *pszName, uint64 xuid, const char *pszNetworkID, const char *pszAddress,
												bool bFakePlayer)
{
	g_WLPlayerManager.OnClientConnected(slot.Get(), xuid, pszAddress, bFakePlayer);
}

void CS2WhitelistPlugin::Hook_ClientPutInServer(CPlayerSlot slot, char const *pszName, int type, uint64 xuid)
{
	int idx = slot.Get();
	const PlayerInfo *p = g_WLPlayerManager.GetPlayer(idx);
	if (!p || p->fakePlayer)
	{
		return;
	}

	if (!cv_enable.Get())
	{
		return;
	}
	if (g_WLManager.IsWhitelistCached(p->xuid))
	{
		return;
	}

	if (cv_immunity.Get() && g_pCS2Admin && g_pCS2Admin->IsAdmin(idx))
	{
		META_CONPRINTF("[WHITELIST] Slot %d (%s) has admin immunity.\n", idx, pszName ? pszName : "?");
		g_WLManager.AddToWhitelistCache(p->xuid);
		return;
	}

	if (g_WLManager.IsBlacklisted(p->xuid))
	{
		const char *msg = cv_kickmessage.Get().Get();
		char kickmsg[512];
		snprintf(kickmsg, sizeof(kickmsg), "[WHITELIST] %s\n", msg);
		if (g_pEngine)
		{
			g_pEngine->ClientPrintf(slot, kickmsg);
			g_pEngine->DisconnectClient(slot, NETWORK_DISCONNECT_KICKED, msg);
		}
		return;
	}

	if (g_WLManager.IsPlayerWhitelisted(idx))
	{
		g_WLManager.AddToWhitelistCache(p->xuid);
		return;
	}

	for (ICS2WhitelistListener *l : m_listeners)
	{
		if (l->OnWhitelistKickPre(idx) == WLKickResult::Block)
		{
			g_WLManager.AddToWhitelistCache(p->xuid);
			return;
		}
	}

	const char *msg = cv_kickmessage.Get().Get();

	if (g_pEngine)
	{
		WLLogKick(pszName, p->xuid, p->ip.c_str(), false);
		g_WLManager.AddToBlacklistCache(p->xuid);

		char kickmsg[512];
		snprintf(kickmsg, sizeof(kickmsg), "[WHITELIST] %s\n", msg);
		g_pEngine->ClientPrintf(slot, kickmsg);

		g_pEngine->DisconnectClient(slot, NETWORK_DISCONNECT_KICKED, msg);
	}
}

void CS2WhitelistPlugin::Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason, const char *pszName, uint64 xuid,
											   const char *pszNetworkID)
{
	g_WLPlayerManager.OnClientDisconnect(slot.Get());
}

// ICS2Whitelist delegation
bool CS2WhitelistPlugin::IsPlayerWhitelisted(int slot) const
{
	return g_WLManager.IsPlayerWhitelisted(slot);
}

bool CS2WhitelistPlugin::IsEntryWhitelisted(const char *entry) const
{
	return g_WLManager.IsEntryWhitelisted(entry);
}

int CS2WhitelistPlugin::GetEntryCount() const
{
	return g_WLManager.GetEntryCount();
}

bool CS2WhitelistPlugin::IsPlayerWhitelistCached(int slot) const
{
	const PlayerInfo *p = g_WLPlayerManager.GetPlayer(slot);
	return p && g_WLManager.IsWhitelistCached(p->xuid);
}

int CS2WhitelistPlugin::GetWhitelistCacheCount() const
{
	return g_WLManager.GetWhitelistCacheCount();
}

bool CS2WhitelistPlugin::IsPlayerBlacklisted(int slot) const
{
	const PlayerInfo *p = g_WLPlayerManager.GetPlayer(slot);
	return p && g_WLManager.IsBlacklisted(p->xuid);
}

int CS2WhitelistPlugin::GetBlacklistCacheCount() const
{
	return g_WLManager.GetBlacklistCacheCount();
}

bool CS2WhitelistPlugin::ReloadFile()
{
	return g_WLManager.LoadFile();
}

bool CS2WhitelistPlugin::AddEntry(const char *entry)
{
	bool ok = g_WLManager.AddEntry(entry);
	if (ok)
		g_WLManager.SaveFile();
	return ok;
}

bool CS2WhitelistPlugin::RemoveEntry(const char *entry)
{
	bool ok = g_WLManager.RemoveEntry(entry);
	if (ok)
		g_WLManager.SaveFile();
	return ok;
}

void CS2WhitelistPlugin::AddListener(ICS2WhitelistListener *listener)
{
	if (listener)
		m_listeners.push_back(listener);
}

void CS2WhitelistPlugin::RemoveListener(ICS2WhitelistListener *listener)
{
	m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), listener), m_listeners.end());
}
