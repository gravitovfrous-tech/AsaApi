#include <API/ARK/Ark.h>

#include "Pickup.h"

extern "C"
{
	/**
	 * Called by AsaApi when the plugin is loaded.
	 */
	__declspec(dllexport) void Plugin_Init()
	{
		Log::Get().Init(PROJECT_NAME);

		try
		{
			Pickup::Install();
		}
		catch (const std::exception& error)
		{
			Log::GetLog()->error("Pickup: initialization failed ({})", error.what());
		}
		catch (...)
		{
			Log::GetLog()->error("Pickup: initialization failed with an unknown error");
		}
	}

	/**
	 * Called by AsaApi just before the plugin is unloaded.
	 */
	__declspec(dllexport) void Plugin_Unload()
	{
		try
		{
			Pickup::Uninstall();
		}
		catch (const std::exception& error)
		{
			Log::GetLog()->error("Pickup: unloading failed ({})", error.what());
		}
		catch (...)
		{
			Log::GetLog()->error("Pickup: unloading failed with an unknown error");
		}
	}
}
