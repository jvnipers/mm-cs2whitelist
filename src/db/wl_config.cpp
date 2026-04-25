#include "wl_config.h"

#include <fstream>
#include <string>
#include <algorithm>
#include <cstdlib>

// Reuse the headeronly KV tokeniser from mm-cs2admin
#include "vendor/mm-cs2admin/src/config/kv_parser.h"

WLConfig g_WLConfig;

static std::string ToLower(const std::string &s)
{
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return out;
}

static void OnKV(const std::string &section, const std::string &key, const std::string &value, void *userdata)
{
	WLConfig *cfg = static_cast<WLConfig *>(userdata);
	std::string sec = ToLower(section);
	std::string k = ToLower(key);

	if (sec == "config")
	{
		if (k == "enable")
		{
			cfg->enable = (value != "0");
		}
		else if (k == "immunity")
		{
			cfg->immunity = (value != "0");
		}
		else if (k == "kickmessage")
		{
			cfg->kickMessage = value;
		}
		else if (k == "filename")
		{
			cfg->filename = value;
		}		else if (k == "log")
		{
			cfg->logMode = std::atoi(value.c_str());
		}	}
	else if (sec == "database")
	{
		if (k == "enabled")
		{
			cfg->dbEnabled = (value != "0");
		}
		else if (k == "type")
		{
			cfg->dbType = ToLower(value);
		}
		else if (k == "host")
		{
			cfg->dbHost = value;
		}
		else if (k == "user" || k == "username")
		{
			cfg->dbUser = value;
		}
		else if (k == "pass" || k == "password")
		{
			cfg->dbPass = value;
		}
		else if (k == "database" || k == "name" || k == "dbname")
		{
			cfg->dbName = value;
		}
		else if (k == "port")
		{
			cfg->dbPort = std::atoi(value.c_str());
		}
		else if (k == "path" || k == "db_path")
		{
			cfg->dbPath = value;
		}
		else if (k == "prefix")
		{
			cfg->dbPrefix = value;
		}
	}
}

bool WL_LoadConfig(const char *filePath, WLConfig &out)
{
	std::ifstream file(filePath);
	if (!file.is_open())
	{
		return false;
	}

	// Expect: "cs2whitelist" { ... }
	kv::Token root = kv::NextToken(file);
	if (root.kind != kv::TokenType::String)
	{
		return false;
	}

	kv::Token brace = kv::NextToken(file);
	if (brace.kind != kv::TokenType::OpenBrace)
	{
		return false;
	}

	kv::ParseSection(file, root.value, OnKV, &out);
	return true;
}
