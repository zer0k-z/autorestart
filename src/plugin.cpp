#include <stdio.h>
#include <filesystem>
#include <map>
#include <string>
#include "plugin.h"
#include "module.h"
#include <eiface.h>
#include <igamesystem.h>
#include <schemasystem/schemasystem.h>
#include <interfaces/interfaces.h>
#include <entity2/entitysystem.h>
#include <engine/igameeventsystem.h>
#include <networksystem/inetworkmessages.h>
#include <steam/isteamgameserver.h>
#include <iserver.h>
#include "serversideclient.h"

SH_DECL_HOOK1_void(IGameSystem, OnServerGamePostSimulate, SH_NOATTRIB, false, const EventServerGamePostSimulate_t *);
SH_DECL_HOOK0_void(IServerGameDLL, GameServerSteamAPIActivated, SH_NOATTRIB, 0);

MMSPlugin g_ThisPlugin;
PLUGIN_EXPOSE(MMSPlugin, g_ThisPlugin);

IGameEventSystem *g_pGameEventSystem = nullptr;
ISource2Server *g_pServer = nullptr;
static void *g_pGameResourceService = nullptr;
static CModule *g_serverModule = nullptr;
static int g_serverGamePostSimulateHook = 0;

// Watchdog file-change state
static bool g_bWatchdogShutdownPending = false;
static std::string g_watchdogLastPath;
static std::map<std::string, std::filesystem::file_time_type> g_watchdogFiles;

static CConVar<CUtlString> cs2_watchdog_path("cs2_watchdog_path", FCVAR_NONE,
	"Absolute path to the directory whose .txt files are watched for changes (non-recursive). "
	"A change triggers a shutdown once the server is empty.", "");

static void CheckWatchdogFiles()
{
	const char *pathStr = cs2_watchdog_path.Get();
	if (!pathStr || pathStr[0] == '\0')
		return;

	std::error_code ec;
	std::filesystem::path watchDir(pathStr);

	if (!std::filesystem::is_directory(watchDir, ec))
		return;

	// Path changed — reseed baseline without triggering
	if (g_watchdogLastPath != pathStr)
	{
		g_watchdogLastPath = pathStr;
		g_watchdogFiles.clear();
		for (auto &entry : std::filesystem::directory_iterator(watchDir, ec))
		{
			if (!ec && entry.is_regular_file(ec) && !ec
				&& entry.path().extension() == ".txt")
			{
				auto mtime = entry.last_write_time(ec);
				if (!ec)
					g_watchdogFiles[entry.path().string()] = mtime;
			}
		}
		return;
	}

	// Scan current directory contents
	std::map<std::string, std::filesystem::file_time_type> current;
	for (auto &entry : std::filesystem::directory_iterator(watchDir, ec))
	{
		if (ec) break;
		if (!entry.is_regular_file(ec) || ec) continue;
		if (entry.path().extension() != ".txt") continue;

		auto mtime = entry.last_write_time(ec);
		if (ec) continue;

		const std::string path = entry.path().string();
		current[path] = mtime;

		auto it = g_watchdogFiles.find(path);
		if (it == g_watchdogFiles.end())
		{
			if (!g_bWatchdogShutdownPending)
			{
				g_bWatchdogShutdownPending = true;
				ConMsg("[AutoRestart] Watchdog: new file '%s' detected — will shut down when server is empty.\n",
					path.c_str());
			}
		}
		else if (it->second != mtime)
		{
			if (!g_bWatchdogShutdownPending)
			{
				g_bWatchdogShutdownPending = true;
				ConMsg("[AutoRestart] Watchdog: file '%s' changed — will shut down when server is empty.\n",
					path.c_str());
			}
		}
	}
	g_watchdogFiles = std::move(current);
}

// GameEntitySystem() accessor required by entity2 SDK source files.
// CGameResourceService stores the CGameEntitySystem* at a known byte offset.
#ifdef _WIN32
static constexpr int GAME_ENTITY_SYSTEM_OFFSET = 88;
static constexpr int CLIENT_LIST_OFFSET = 592;
#else
static constexpr int GAME_ENTITY_SYSTEM_OFFSET = 80;
static constexpr int CLIENT_LIST_OFFSET = 592;
#endif

CGameEntitySystem *GameEntitySystem()
{
	if (!g_pGameResourceService)
		return nullptr;
	return *reinterpret_cast<CGameEntitySystem **>((uintptr_t)g_pGameResourceService + GAME_ENTITY_SYSTEM_OFFSET);
}

CUtlVector<CServerSideClient *> *GetClientList()
{
	if (!g_pNetworkServerService)
	{
		return nullptr;
	}
	return (CUtlVector<CServerSideClient *> *)((char *)g_pNetworkServerService->GetIGameServer() + CLIENT_LIST_OFFSET);
}

int GetPlayerCount()
{
	int count = 0;
	auto clients = GetClientList();
	if (clients)
	{
		FOR_EACH_VEC(*clients, i)
		{
			CServerSideClient *client = clients->Element(i);

			if (client && client->IsConnected() && !client->IsFakeClient() && !client->IsHLTV())
			{
				count++;
			}
		}
	}
	return count;
}

static void Hook_ServerGamePostSimulate(const EventServerGamePostSimulate_t *)
{
	static double lastCheckTime = 0.0;
	if (SteamGameServer() && Plat_FloatTime() - lastCheckTime > 5.0f)
	{
		lastCheckTime = Plat_FloatTime();

		CheckWatchdogFiles();

		const bool shouldShutdown =
			SteamGameServer()->WasRestartRequested() || g_bWatchdogShutdownPending;

		if (shouldShutdown && GetPlayerCount() == 0)
		{
			g_pEngineServer->ServerCommand("quit");
		}
	}
	RETURN_META(MRES_IGNORED);
}


bool MMSPlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetServerFactory, g_pSource2GameClients, ISource2GameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
	GET_V_IFACE_CURRENT(GetServerFactory, g_pServer, ISource2Server, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngineServer, IVEngineServer2, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pGameResourceService, void, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetworkMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pGameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	g_serverModule = new CModule(GAMEBIN, "server");

	g_serverGamePostSimulateHook = SH_ADD_DVPHOOK(
		IGameSystem,
		OnServerGamePostSimulate,
		(IGameSystem *)g_serverModule->FindVirtualTable("CEntityDebugGameSystem"),
		SH_STATIC(Hook_ServerGamePostSimulate),
		false
	);
	META_CONVAR_REGISTER(FCVAR_RELEASE | FCVAR_GAMEDLL);

	return true;
}

bool MMSPlugin::Unload(char *error, size_t maxlen)
{
	SH_REMOVE_HOOK_ID(g_serverGamePostSimulateHook);

	delete g_serverModule;
	g_serverModule = nullptr;

	g_watchdogFiles.clear();
	g_watchdogLastPath.clear();
	g_bWatchdogShutdownPending = false;

	return true;
}

void MMSPlugin::AllPluginsLoaded()
{
	/* This is where we'd do stuff that relies on the mod or other plugins 
	 * being initialized (for example, cvars added and events registered).
	 */
}