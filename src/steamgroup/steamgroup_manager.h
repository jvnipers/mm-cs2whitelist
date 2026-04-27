#ifndef _INCLUDE_WL_STEAMGROUP_MANAGER_H_
#define _INCLUDE_WL_STEAMGROUP_MANAGER_H_

#include <cstdint>
#include <string>
#include <vector>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <chrono>

#include <steam/isteamhttp.h>

// Forward-declared here; full type pulled in via isteamhttp.h -> steam_api_common.h
// CCallResult<T,P> and HTTPRequestCompleted_t are available after that include.

class SteamGroupManager
{
public:
	enum class Method
	{
		XML, // Pre-fetch full member list via public XML endpoint (no API key)
		API, // Per-player check via Steam Web API (requires API key)
	};

	struct Config
	{
		bool        enabled  = false;
		Method      method   = Method::XML;
		std::string apiKey;                   // Required for Method::API
		float       timeout  = 10.0f;         // Seconds before a pending player is kicked
		std::vector<uint64_t> groupIds;       // 64-bit Steam group IDs
	};

	void Init(const Config &cfg);
	void Shutdown();

	// Kick off XML pre-fetches (call on plugin load and each map start).
	void FetchGroups();

	// Check whether this player is whitelisted via a Steam group.
	//   pending=true  -> async check started; caller MUST NOT kick yet.
	//   pending=false -> synchronous result; return value is the answer.
	bool CheckPlayer(int slot, uint64_t xuid, bool &pending);

	// Timeout/cleanup tick - call once per game frame.
	void OnGameFrame();

	// Cancel any in-progress check for a disconnecting player.
	void OnPlayerDisconnect(int slot);

	bool IsEnabled() const { return m_cfg.enabled; }

private:
	// One context per active HTTP request. Stored in std::list for pointer
	// stability (CCallResult cannot be moved or copied).
	struct RequestCtx
	{
		bool     isApi;        // true = API per-player check; false = XML group fetch
		uint64_t groupId;      // XML: group being fetched
		int      page;         // XML: 1-based page number
		int      slot;         // API: player slot (-1 for XML)
		uint64_t xuid;         // API: player steamid64
		HTTPRequestHandle hRequest = INVALID_HTTPREQUEST_HANDLE;
		CCallResult<SteamGroupManager, HTTPRequestCompleted_t> callResult;
	};

	struct PendingPlayer
	{
		uint64_t xuid;
		std::chrono::steady_clock::time_point startTime;
	};

	// XML member sets: groupId64 -> set of member steamid64s
	std::unordered_map<uint64_t, std::unordered_set<uint64_t>> m_memberSets;
	// Expected total member count per group (read from first XML page)
	std::unordered_map<uint64_t, int> m_expectedCounts;
	// Groups whose XML fetch is complete
	std::unordered_set<uint64_t> m_fetchedGroups;

	// Active HTTP requests (std::list provides pointer-stable storage)
	std::list<RequestCtx> m_requests;

	// Players waiting for XML data to finish loading: slot -> info
	std::unordered_map<int, PendingPlayer> m_pendingXml;
	// Players waiting for an API response: slot -> info
	std::unordered_map<int, PendingPlayer> m_pendingApi;

	Config      m_cfg;
	ISteamHTTP *m_pHttp = nullptr;
	// Merged group IDs: m_cfg.groupIds n IDs from whitelist.txt (populated in FetchGroups)
	std::vector<uint64_t> m_effectiveGroupIds;

	void StartXmlFetch(uint64_t groupId, int page);
	bool StartApiFetch(int slot, uint64_t xuid);

	void OnHTTPResponse(HTTPRequestCompleted_t *pResult, bool bIOFailure);

	void ParseXmlBody(uint64_t groupId, int page, const std::string &body);
	bool ParseApiResponse(uint64_t xuid, const std::string &body) const;

	bool IsXuidInAnyGroup(uint64_t xuid) const;
	bool AllGroupsFetched() const;

	void ProcessPendingXmlPlayers();
	void AllowPlayer(int slot, uint64_t xuid);
	void KickPlayer(int slot);
};

extern SteamGroupManager g_SteamGroupManager;

#endif // _INCLUDE_WL_STEAMGROUP_MANAGER_H_
