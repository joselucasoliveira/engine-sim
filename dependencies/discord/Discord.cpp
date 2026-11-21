#include "Discord.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <time.h>


CDiscord* CDiscord::m_pInstance = NULL;
static std::int64_t StartTime;


CDiscord::CDiscord()
{
	m_state = "Doing Things";
	m_bEnableDiscord = false;
	m_bConnected = false;
}

CDiscord::~CDiscord()
{
}

bool CDiscord::CreateInstance()
{
	if (m_pInstance)
		return false;

	m_pInstance = new CDiscord;

	return true;
}

void CDiscord::DestroyInstance()
{
	if (!m_pInstance)
		return;

	if (m_pInstance->m_bEnableDiscord)
		Discord_Shutdown();

	delete m_pInstance;
	m_pInstance = nullptr;
}

void CDiscord::handleDiscordReady(const DiscordUser * connectedUser)
{
	GetDiscordManager()->SetConnected(true);
}

void CDiscord::handleDiscordDisconnected(int errcode, const char* message)
{
	GetDiscordManager()->SetConnected(false);
}

void CDiscord::handleDiscordError(int errcode, const char* message)
{
}



void CDiscord::InitDiscord()
{
	const char* applicationId = std::getenv("ENGINE_SIM_DISCORD_APPLICATION_ID");
	if (applicationId == nullptr || *applicationId == '\0' ||
		applicationId[std::strspn(applicationId, "0123456789")] != '\0')
	{
		m_bEnableDiscord = false;
		return;
	}

	DiscordEventHandlers handlers{};

	handlers.ready = CDiscord::handleDiscordReady;
	handlers.disconnected = CDiscord::handleDiscordDisconnected;
	handlers.errored = CDiscord::handleDiscordError;
	handlers.joinGame = NULL;
	handlers.spectateGame = NULL;
	handlers.joinRequest = NULL;
	Discord_Initialize(applicationId, &handlers, 0, nullptr);
}

void CDiscord::UpdatePresence()
{
	if (m_bEnableDiscord == false)
		return;

	if (m_bConnected == false)
		return;

		DiscordRichPresence discordPresence;
		memset(&discordPresence, 0, sizeof(discordPresence));

		discordPresence.details = m_state.c_str();

		discordPresence.startTimestamp = StartTime;

		discordPresence.instance = 0;
		Discord_UpdatePresence(&discordPresence);
}


void CDiscord::ClearPresence()
{
	if (m_bEnableDiscord)
		Discord_ClearPresence();
}


void CDiscord::UpdateDiscordConnection()
{
	if (m_bEnableDiscord == false)
		return;

#ifdef DISCORD_DISABLE_IO_THREAD
	Discord_UpdateConnection();
#endif
	Discord_RunCallbacks();
}

void CDiscord::SetUseDiscord(bool bFlag)
{
	if (bFlag == m_bEnableDiscord)
		return;

	m_bEnableDiscord = bFlag;
	if (bFlag)
		InitDiscord();
	else
	{
		Discord_Shutdown();
		m_bConnected = false;
	}
}


void CDiscord::SetConnected(bool bFlag)
{
	m_bConnected = bFlag;
}

void CDiscord::SetStatus(DiscordRichPresence presence, std::string engineName = "", std::string buildVersion = "")
{
	if (!m_bEnableDiscord)
		return;

	std::string engineString = "Engine: " + engineName;
	std::string stateString = "Build: " + buildVersion;
	presence.state = stateString.c_str();
	presence.details = engineString.c_str();
	presence.largeImageKey = nullptr;
	presence.largeImageText = nullptr;
	presence.smallImageKey = nullptr;
	presence.smallImageText = nullptr;
	StartTime = time(NULL);
	presence.instance = 1;
	presence.startTimestamp = StartTime;
	Discord_UpdatePresence(&presence);
}
