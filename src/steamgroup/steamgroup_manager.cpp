#include "steamgroup_manager.h"

#include "common.h"
#include "whitelist/whitelist_manager.h"

#include <eiface.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

SteamGroupManager g_SteamGroupManager;

// Simple tag extraction helper
static int ExtractIntTag(const std::string &body, const char *open, const char *close)
{
	size_t pos = body.find(open);
	if (pos == std::string::npos)
		return 0;
	pos += strlen(open);
	size_t end = body.find(close, pos);
	if (end == std::string::npos)
		return 0;
	char *endptr = nullptr;
	long val = std::strtol(body.c_str() + pos, &endptr, 10);
	if (endptr == body.c_str() + pos)
		return 0;
	return static_cast<int>(val);
}

// Init / Shutdown
void SteamGroupManager::Init(const Config &cfg)
{
	m_cfg   = cfg;
	m_pHttp = SteamGameServerHTTP(); // may be null at this point; re-acquired lazily

	if (m_cfg.enabled)
	{
		if (m_pHttp)
			META_CONPRINTF("[WHITELIST] SteamGroup: initialized (method=%s).\n",
			               m_cfg.method == Method::API ? "api" : "xml");
		else
			META_CONPRINTF("[WHITELIST] SteamGroup: SteamGameServerHTTP() not ready yet, will retry.\n");
	}
}

void SteamGroupManager::Shutdown()
{
	if (m_pHttp)
	{
		for (auto &ctx : m_requests)
		{
			if (ctx.hRequest != INVALID_HTTPREQUEST_HANDLE)
			{
				ctx.callResult.Cancel();
				m_pHttp->ReleaseHTTPRequest(ctx.hRequest);
			}
		}
	}
	m_requests.clear();
	m_pendingXml.clear();
	m_pendingApi.clear();
	m_memberSets.clear();
	m_fetchedGroups.clear();
	m_expectedCounts.clear();
}

// FetchGroups - kick off XML pre-fetch for all configured groups
void SteamGroupManager::FetchGroups()
{
	if (!m_cfg.enabled)
		return;

	// Lazily acquire HTTP interface if it wasn't ready at Init() time
	if (!m_pHttp)
	{
		m_pHttp = SteamGameServerHTTP();
		if (!m_pHttp)
		{
			META_CONPRINTF("[WHITELIST] SteamGroup: FetchGroups() - SteamGameServerHTTP() still null, skipping.\n");
			return;
		}
		META_CONPRINTF("[WHITELIST] SteamGroup: acquired SteamGameServerHTTP().\n");
	}

	// Build effective group list: config IDs + IDs from whitelist.txt
	{
		std::unordered_set<uint64_t> seen;
		m_effectiveGroupIds.clear();
		for (uint64_t gid : m_cfg.groupIds)
			if (seen.insert(gid).second)
				m_effectiveGroupIds.push_back(gid);
		for (uint64_t gid : g_WLManager.GetFileGroupIds())
			if (seen.insert(gid).second)
				m_effectiveGroupIds.push_back(gid);
	}

	if (m_effectiveGroupIds.empty())
		return;

	if (m_cfg.method != Method::XML)
		return;

	// Reset XML state
	m_memberSets.clear();
	m_fetchedGroups.clear();
	m_expectedCounts.clear();

	// Cancel any running XML fetches
	for (auto it = m_requests.begin(); it != m_requests.end(); )
	{
		if (!it->isApi)
		{
			if (m_pHttp && it->hRequest != INVALID_HTTPREQUEST_HANDLE)
			{
				it->callResult.Cancel();
				m_pHttp->ReleaseHTTPRequest(it->hRequest);
			}
			it = m_requests.erase(it);
		}
		else
		{
			++it;
		}
	}

	// Kick off pending XML players from the new batch
	m_pendingXml.clear();

	for (uint64_t gid : m_effectiveGroupIds)
		StartXmlFetch(gid, 1);
}

void SteamGroupManager::StartXmlFetch(uint64_t groupId, int page)
{
	if (!m_pHttp)
		return;

	char url[300];
	if (page <= 1)
	{
		snprintf(url, sizeof(url),
		         "https://steamcommunity.com/gid/%llu/memberslistxml/?xml=1",
		         (unsigned long long)groupId);
	}
	else
	{
		snprintf(url, sizeof(url),
		         "https://steamcommunity.com/gid/%llu/memberslistxml/?xml=1&p=%d",
		         (unsigned long long)groupId, page);
	}

	HTTPRequestHandle hReq = m_pHttp->CreateHTTPRequest(k_EHTTPMethodGET, url);
	if (hReq == INVALID_HTTPREQUEST_HANDLE)
	{
		META_CONPRINTF("[WHITELIST] SteamGroup: failed to create request for group %llu p%d\n",
		               (unsigned long long)groupId, page);
		return;
	}

	m_requests.emplace_back();
	RequestCtx &ctx = m_requests.back();
	ctx.isApi   = false;
	ctx.groupId = groupId;
	ctx.page    = page;
	ctx.slot    = -1;
	ctx.xuid    = 0;
	ctx.hRequest = hReq;

	SteamAPICall_t hCall = k_uAPICallInvalid;
	if (!m_pHttp->SendHTTPRequest(hReq, &hCall) || hCall == k_uAPICallInvalid)
	{
		m_pHttp->ReleaseHTTPRequest(hReq);
		m_requests.pop_back();
		META_CONPRINTF("[WHITELIST] SteamGroup: failed to send request for group %llu p%d\n",
		               (unsigned long long)groupId, page);
		return;
	}

	ctx.callResult.Set(hCall, this, &SteamGroupManager::OnHTTPResponse);
	META_CONPRINTF("[WHITELIST] SteamGroup: fetching group %llu page %d...\n",
	               (unsigned long long)groupId, page);
}

// API per-player fetch; returns true if the request was successfully enqueued
bool SteamGroupManager::StartApiFetch(int slot, uint64_t xuid)
{
	if (!m_pHttp || m_cfg.apiKey.empty())
	{
		META_CONPRINTF("[WHITELIST] SteamGroup: StartApiFetch skipped (http=%s key=%s)\n",
		               m_pHttp ? "ok" : "null",
		               m_cfg.apiKey.empty() ? "empty" : "set");
		return false;
	}

	char url[600];
	snprintf(url, sizeof(url),
	         "https://api.steampowered.com/ISteamUser/GetUserGroupList/v1/?key=%s&steamid=%llu",
	         m_cfg.apiKey.c_str(), (unsigned long long)xuid);

	HTTPRequestHandle hReq = m_pHttp->CreateHTTPRequest(k_EHTTPMethodGET, url);
	if (hReq == INVALID_HTTPREQUEST_HANDLE)
	{
		META_CONPRINTF("[WHITELIST] SteamGroup: failed to create API request for slot=%d xuid=%llu\n",
		               slot, (unsigned long long)xuid);
		return false;
	}

	m_requests.emplace_back();
	RequestCtx &ctx = m_requests.back();
	ctx.isApi    = true;
	ctx.groupId  = 0;
	ctx.page     = 0;
	ctx.slot     = slot;
	ctx.xuid     = xuid;
	ctx.hRequest = hReq;

	SteamAPICall_t hCall = k_uAPICallInvalid;
	if (!m_pHttp->SendHTTPRequest(hReq, &hCall) || hCall == k_uAPICallInvalid)
	{
		m_pHttp->ReleaseHTTPRequest(hReq);
		m_requests.pop_back();
		META_CONPRINTF("[WHITELIST] SteamGroup: failed to send API request for slot=%d xuid=%llu\n",
		               slot, (unsigned long long)xuid);
		return false;
	}

	ctx.callResult.Set(hCall, this, &SteamGroupManager::OnHTTPResponse);

	PendingPlayer pp;
	pp.xuid      = xuid;
	pp.startTime = std::chrono::steady_clock::now();
	m_pendingApi[slot] = pp;

	META_CONPRINTF("[WHITELIST] SteamGroup: API check started for slot=%d xuid=%llu\n",
	               slot, (unsigned long long)xuid);
	return true;
}

// CheckPlayer - called from Hook_ClientPutInServer after the file-whitelist miss
bool SteamGroupManager::CheckPlayer(int slot, uint64_t xuid, bool &pending)
{
	pending = false;

	// Lazily acquire HTTP interface
	if (!m_pHttp)
		m_pHttp = SteamGameServerHTTP();

	if (!m_cfg.enabled)
	{
		META_CONPRINTF("[WHITELIST] SteamGroup: CheckPlayer slot=%d - disabled.\n", slot);
		return false;
	}
	if (!m_pHttp)
	{
		META_CONPRINTF("[WHITELIST] SteamGroup: CheckPlayer slot=%d - no HTTP interface.\n", slot);
		return false;
	}
	if (m_effectiveGroupIds.empty())
	{
		META_CONPRINTF("[WHITELIST] SteamGroup: CheckPlayer slot=%d - no group IDs configured.\n", slot);
		return false;
	}

	if (m_cfg.method == Method::XML)
	{
		if (!AllGroupsFetched())
		{
			// XML data not ready yet, queue player
			PendingPlayer pp;
			pp.xuid      = xuid;
			pp.startTime = std::chrono::steady_clock::now();
			m_pendingXml[slot] = pp;
			pending = true;
			return false;
		}
		return IsXuidInAnyGroup(xuid);
	}
	else // API
	{
		if (m_pendingApi.count(slot))
		{
			// Already checking, caller should keep waiting
			pending = true;
			return false;
		}
		if (!StartApiFetch(slot, xuid))
		{
			META_CONPRINTF("[WHITELIST] SteamGroup: StartApiFetch failed for slot=%d xuid=%llu\n",
			               slot, (unsigned long long)xuid);
			return false; // fail open to kick rather than hang
		}
		pending = true;
		return false;
	}
}

// OnHTTPResponse - unified callback for all requests

void SteamGroupManager::OnHTTPResponse(HTTPRequestCompleted_t *pResult, bool bIOFailure)
{
	// Find the context for this request
	auto it = m_requests.begin();
	for (; it != m_requests.end(); ++it)
	{
		if (it->hRequest == pResult->m_hRequest)
			break;
	}
	if (it == m_requests.end())
		return;

	// Copy identifying data before erasing
	const bool     isApi   = it->isApi;
	const uint64_t groupId = it->groupId;
	const int      page    = it->page;
	const int      slot    = it->slot;
	const uint64_t xuid    = it->xuid;

	// Read the response body
	std::string body;
	const bool ok = !bIOFailure
	             && pResult->m_bRequestSuccessful
	             && pResult->m_eStatusCode == k_EHTTPStatusCode200OK
	             && pResult->m_unBodySize > 0
	             && pResult->m_unBodySize < (4u * 1024u * 1024u); // sanity limit: 4 MB

	if (ok)
	{
		body.resize(pResult->m_unBodySize);
		m_pHttp->GetHTTPResponseBodyData(pResult->m_hRequest,
		                                 reinterpret_cast<uint8 *>(body.data()),
		                                 pResult->m_unBodySize);
	}

	// Release the HTTP handle and erase the context.
	// Safe: CCallResult::Run() sets m_hAPICall = k_uAPICallInvalid before calling us,
	// so the destructor's Cancel() is a no-op and the Steam framework holds no further
	// reference to this CCallResult.
	m_pHttp->ReleaseHTTPRequest(pResult->m_hRequest);
	m_requests.erase(it);

	// Process
	if (!isApi)
	{
		if (!body.empty())
		{
			ParseXmlBody(groupId, page, body);
		}
		else
		{
			META_CONPRINTF("[WHITELIST] SteamGroup: XML fetch failed for group %llu p%d (HTTP %d)\n",
			               (unsigned long long)groupId, page,
			               static_cast<int>(pResult->m_eStatusCode));
			// Mark as done so pending players aren't stuck forever
			m_fetchedGroups.insert(groupId);
			if (AllGroupsFetched())
				ProcessPendingXmlPlayers();
		}
	}
	else
	{
		m_pendingApi.erase(slot);

		const bool inGroup = !body.empty() && ParseApiResponse(xuid, body);
		META_CONPRINTF("[WHITELIST] SteamGroup: API response for slot=%d xuid=%llu: %s (body_len=%u)\n",
		               slot, (unsigned long long)xuid,
		               inGroup ? "IN GROUP" : "NOT IN GROUP",
		               (unsigned)body.size());
		if (inGroup)
			AllowPlayer(slot, xuid);
		else
			KickPlayer(slot);
	}
}

// ParseXmlBody
void SteamGroupManager::ParseXmlBody(uint64_t groupId, int page, const std::string &body)
{
	auto &members = m_memberSets[groupId];

	// First page: read expected member count
	if (page == 1)
	{
		int total = ExtractIntTag(body, "<memberCount>", "</memberCount>");
		m_expectedCounts[groupId] = total;
		META_CONPRINTF("[WHITELIST] SteamGroup: group %llu has %d members.\n",
		               (unsigned long long)groupId, total);
	}

	// Parse <members> block
	const size_t membersStart = body.find("<members>");
	const size_t membersEnd   = body.find("</members>", membersStart);
	if (membersStart == std::string::npos || membersEnd == std::string::npos)
	{
		m_fetchedGroups.insert(groupId);
		if (AllGroupsFetched())
			ProcessPendingXmlPlayers();
		return;
	}

	static const char *kOpen  = "<steamID64>";
	static const char *kClose = "</steamID64>";
	static const size_t kOpenLen  = 11; // strlen("<steamID64>")
	static const size_t kCloseLen = 12; // strlen("</steamID64>")

	int count = 0;
	size_t pos = membersStart;
	while (true)
	{
		size_t tagStart = body.find(kOpen, pos);
		if (tagStart == std::string::npos || tagStart >= membersEnd)
			break;
		tagStart += kOpenLen;
		size_t tagEnd = body.find(kClose, tagStart);
		if (tagEnd == std::string::npos || tagEnd >= membersEnd)
			break;

		const std::string idStr = body.substr(tagStart, tagEnd - tagStart);
		uint64_t id = std::strtoull(idStr.c_str(), nullptr, 10);
		if (id != 0)
		{
			members.insert(id);
			++count;
		}

		pos = tagEnd + kCloseLen;
	}

	META_CONPRINTF("[WHITELIST] SteamGroup: group %llu p%d: +%d members (%d total in set)\n",
	               (unsigned long long)groupId, page, count, (int)members.size());

	// Check if more pages are needed
	const int expected = m_expectedCounts.count(groupId) ? m_expectedCounts.at(groupId) : 0;
	if (expected > 0 && static_cast<int>(members.size()) < expected)
	{
		StartXmlFetch(groupId, page + 1);
	}
	else
	{
		m_fetchedGroups.insert(groupId);
		META_CONPRINTF("[WHITELIST] SteamGroup: group %llu complete (%d members).\n",
		               (unsigned long long)groupId, static_cast<int>(members.size()));
		if (AllGroupsFetched())
		{
			META_CONPRINTF("[WHITELIST] SteamGroup: all groups fetched.\n");
			ProcessPendingXmlPlayers();
		}
	}
}

// ParseApiResponse, minimal JSON parser for GetUserGroupList response
bool SteamGroupManager::ParseApiResponse(uint64_t xuid, const std::string &body) const
{
	// Response: {"response":{"success":true,"groups":[{"gid":"103582791..."},...]}}
	// We just scan for "gid":"VALUE" patterns.
	(void)xuid; // xuid not needed to identify the player here; groupIds are what matter

	size_t pos = 0;
	while (true)
	{
		pos = body.find("\"gid\"", pos);
		if (pos == std::string::npos)
			break;
		pos += 5; // skip past "gid"

		// skip : " (possibly with whitespace)
		while (pos < body.size() && (body[pos] == ':' || body[pos] == '"' || body[pos] == ' '))
			++pos;

		// read digits
		size_t end = pos;
		while (end < body.size() && body[end] >= '0' && body[end] <= '9')
			++end;

		if (end > pos)
		{
			uint64_t gid = std::strtoull(body.c_str() + pos, nullptr, 10);

			for (uint64_t wanted : m_effectiveGroupIds)
			{
				if (gid == wanted)
					return true;
			}
		}
		pos = end;
	}
	return false;
}

// Helpers
bool SteamGroupManager::IsXuidInAnyGroup(uint64_t xuid) const
{
	for (const auto &kv : m_memberSets)
	{
		if (kv.second.count(xuid))
			return true;
	}
	return false;
}

bool SteamGroupManager::AllGroupsFetched() const
{
	if (m_effectiveGroupIds.empty())
		return false;
	for (uint64_t gid : m_effectiveGroupIds)
	{
		if (!m_fetchedGroups.count(gid))
			return false;
	}
	return true;
}

void SteamGroupManager::ProcessPendingXmlPlayers()
{
	for (auto &[slot, pp] : m_pendingXml)
	{
		if (IsXuidInAnyGroup(pp.xuid))
			AllowPlayer(slot, pp.xuid);
		else
			KickPlayer(slot);
	}
	m_pendingXml.clear();
}

void SteamGroupManager::AllowPlayer(int slot, uint64_t xuid)
{
	g_WLManager.AddToWhitelistCache(xuid);
	META_CONPRINTF("[WHITELIST] SteamGroup: slot %d allowed via group membership.\n", slot);
}

void SteamGroupManager::KickPlayer(int slot)
{
	if (!g_pEngine)
		return;
	const char *msg = cv_kickmessage.Get().Get();
	CPlayerSlot playerSlot(slot);
	char kickmsg[512];
	snprintf(kickmsg, sizeof(kickmsg), "[WHITELIST] %s\n", msg);
	g_pEngine->ClientPrintf(playerSlot, kickmsg);
	g_pEngine->DisconnectClient(playerSlot, NETWORK_DISCONNECT_KICKED, msg);
}

// OnGameFrame - timeout enforcement
void SteamGroupManager::OnGameFrame()
{
	const auto now     = std::chrono::steady_clock::now();
	const auto timeout = std::chrono::duration<float>(m_cfg.timeout);

	// XML-pending timeout
	for (auto it = m_pendingXml.begin(); it != m_pendingXml.end(); )
	{
		if (std::chrono::duration<float>(now - it->second.startTime) >= timeout)
		{
			const int slot = it->first;
			it = m_pendingXml.erase(it);
			META_CONPRINTF("[WHITELIST] SteamGroup: slot %d timed out waiting for XML data.\n", slot);
			KickPlayer(slot);
		}
		else
		{
			++it;
		}
	}

	// API-pending timeout
	for (auto it = m_pendingApi.begin(); it != m_pendingApi.end(); )
	{
		if (std::chrono::duration<float>(now - it->second.startTime) >= timeout)
		{
			const int slot = it->first;
			it = m_pendingApi.erase(it);

			// Cancel the associated HTTP request
			for (auto rit = m_requests.begin(); rit != m_requests.end(); ++rit)
			{
				if (rit->isApi && rit->slot == slot)
				{
					rit->callResult.Cancel();
					if (m_pHttp && rit->hRequest != INVALID_HTTPREQUEST_HANDLE)
						m_pHttp->ReleaseHTTPRequest(rit->hRequest);
					m_requests.erase(rit);
					break;
				}
			}

			META_CONPRINTF("[WHITELIST] SteamGroup: slot %d API check timed out.\n", slot);
			KickPlayer(slot);
		}
		else
		{
			++it;
		}
	}
}

// OnPlayerDisconnect
void SteamGroupManager::OnPlayerDisconnect(int slot)
{
	m_pendingXml.erase(slot);

	if (m_pendingApi.erase(slot))
	{
		for (auto it = m_requests.begin(); it != m_requests.end(); ++it)
		{
			if (it->isApi && it->slot == slot)
			{
				it->callResult.Cancel();
				if (m_pHttp && it->hRequest != INVALID_HTTPREQUEST_HANDLE)
					m_pHttp->ReleaseHTTPRequest(it->hRequest);
				m_requests.erase(it);
				break;
			}
		}
	}
}
