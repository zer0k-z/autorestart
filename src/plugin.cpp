#include <stdio.h>
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

SH_DECL_HOOK1_void(IGameSystem, ServerGamePostSimulate, SH_NOATTRIB, false, const EventServerGamePostSimulate_t *);
SH_DECL_HOOK0_void(IServerGameDLL, GameServerSteamAPIActivated, SH_NOATTRIB, 0);

MMSPlugin g_ThisPlugin;
PLUGIN_EXPOSE(MMSPlugin, g_ThisPlugin);

IGameEventSystem *g_pGameEventSystem = nullptr;
ISource2Server *g_pServer = nullptr;
static void *g_pGameResourceService = nullptr;
static CModule *g_serverModule = nullptr;
static int g_serverGamePostSimulateHook = 0;

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
		if (SteamGameServer()->WasRestartRequested() && GetPlayerCount() == 0)
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
		ServerGamePostSimulate,
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

	return true;
}

void MMSPlugin::AllPluginsLoaded()
{
	/* This is where we'd do stuff that relies on the mod or other plugins 
	 * being initialized (for example, cvars added and events registered).
	 */
}