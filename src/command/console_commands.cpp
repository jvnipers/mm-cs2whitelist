#include "common.h"
#include "whitelist/whitelist_manager.h"
#include "steamgroup/steamgroup_manager.h"
#include "db/wl_database.h"
#include "utils/utils.h"
#include "ics2admin.h"

#include <tier1/convar.h>

CON_COMMAND_F(mm_whitelist_status, "Show whitelist status (enabled, entry count, file name).", FCVAR_GAMEDLL | FCVAR_RELEASE)
{
	int slot = context.GetPlayerSlot().Get();
	if (!HasAdminAccess(slot, CS2ADMIN_FLAG_GENERIC))
	{
		return;
	}

	ReplyToSlot(slot, "[WHITELIST] enabled=%s | entries=%d | wl_cache=%d | bl_cache=%d | file=%s\n",
				cv_enable.Get() ? "yes" : "no",
				g_WLManager.GetEntryCount(),
				g_WLManager.GetWhitelistCacheCount(),
				g_WLManager.GetBlacklistCacheCount(),
				cv_filename.Get().Get());
}

CON_COMMAND_F(mm_whitelist_reload, "Reload the whitelist file from disk.", FCVAR_GAMEDLL | FCVAR_RELEASE)
{
	int slot = context.GetPlayerSlot().Get();
	if (!HasAdminAccess(slot, CS2ADMIN_FLAG_CONVARS))
	{
		return;
	}

	if (g_WLManager.LoadFile())
	{
		g_SteamGroupManager.FetchGroups();
		if (g_WLDatabase.IsConnected())
		{
			g_WLDatabase.LoadEntries(g_WLManager.GetSet(),
				[slot](int count)
				{
					ReplyToSlot(slot, "[WHITELIST] Reloaded %d entries from disk + %d from database.\n",
						g_WLManager.GetEntryCount() - count, count);
				});
		}
		else
		{
			ReplyToSlot(slot, "[WHITELIST] Reloaded %d entries from disk.\n", g_WLManager.GetEntryCount());
		}
	}
	else
	{
		ReplyToSlot(slot, "[WHITELIST] Failed to reload whitelist file (check server log).\n");
	}
}

CON_COMMAND_F(mm_whitelist_list, "List all entries in the currently loaded whitelist.", FCVAR_GAMEDLL | FCVAR_RELEASE)
{
	int slot = context.GetPlayerSlot().Get();
	if (!HasAdminAccess(slot, CS2ADMIN_FLAG_GENERIC))
	{
		return;
	}

	g_WLManager.PrintList(slot);
}

CON_COMMAND_F(mm_whitelist_add,
			  "Add a SteamID (STEAM_0:X:Y or SteamID64) or IP address to the whitelist. "
			  "Usage: mm_whitelist_add <steamid|ip>",
			  FCVAR_GAMEDLL | FCVAR_RELEASE)
{
	int slot = context.GetPlayerSlot().Get();
	if (!HasAdminAccess(slot, CS2ADMIN_FLAG_UNBAN))
	{
		return;
	}

	if (args.ArgC() < 2)
	{
		ReplyToSlot(slot, "[WHITELIST] Usage: mm_whitelist_add <steamid|ip>\n");
		return;
	}

	const char *entry = args.Arg(1);
	if (g_WLManager.AddEntry(entry))
	{
		ReplyToSlot(slot, "[WHITELIST] Added '%s'. Saving file...\n", entry);
		g_WLManager.SaveFile();
	}
	else
	{
		ReplyToSlot(slot, "[WHITELIST] '%s' is already in the whitelist (or is invalid).\n", entry);
	}
}

CON_COMMAND_F(mm_whitelist_remove,
			  "Remove a SteamID or IP address from the whitelist. "
			  "Usage: mm_whitelist_remove <steamid|ip>",
			  FCVAR_GAMEDLL | FCVAR_RELEASE)
{
	int slot = context.GetPlayerSlot().Get();
	if (!HasAdminAccess(slot, CS2ADMIN_FLAG_UNBAN))
	{
		return;
	}

	if (args.ArgC() < 2)
	{
		ReplyToSlot(slot, "[WHITELIST] Usage: mm_whitelist_remove <steamid|ip>\n");
		return;
	}

	const char *entry = args.Arg(1);
	if (g_WLManager.RemoveEntry(entry))
	{
		ReplyToSlot(slot, "[WHITELIST] Removed '%s'. Saving file...\n", entry);
		g_WLManager.SaveFile();
	}
	else
	{
		ReplyToSlot(slot, "[WHITELIST] '%s' was not found in the whitelist.\n", entry);
	}
}

CON_COMMAND_F(mm_whitelist_exist,
			  "Check whether a SteamID or IP is in the currently loaded whitelist. "
			  "Usage: mm_whitelist_exist <steamid|ip>",
			  FCVAR_GAMEDLL | FCVAR_RELEASE)
{
	int slot = context.GetPlayerSlot().Get();
	if (!HasAdminAccess(slot, CS2ADMIN_FLAG_GENERIC))
	{
		return;
	}

	if (args.ArgC() < 2)
	{
		ReplyToSlot(slot, "[WHITELIST] Usage: mm_whitelist_exist <steamid|ip>\n");
		return;
	}

	const char *entry = args.Arg(1);
	if (g_WLManager.IsEntryWhitelisted(entry))
	{
		ReplyToSlot(slot, "[WHITELIST] '%s' IS in the whitelist.\n", entry);
	}
	else
	{
		ReplyToSlot(slot, "[WHITELIST] '%s' is NOT in the whitelist.\n", entry);
	}
}

CON_COMMAND_F(mm_whitelist_cache_clear,
			  "Clear both the per-map whitelist cache (confirmed-allowed) and rejection cache (confirmed-rejected). "
			  "Forces all players to re-run the full whitelist check on next connect.",
			  FCVAR_GAMEDLL | FCVAR_RELEASE)
{
	int slot = context.GetPlayerSlot().Get();
	if (!HasAdminAccess(slot, CS2ADMIN_FLAG_CONVARS))
	{
		return;
	}

	g_WLManager.ClearBlacklistCache();
	g_WLManager.ClearWhitelistCache();
	ReplyToSlot(slot, "[WHITELIST] Cache cleared.\n");
}
