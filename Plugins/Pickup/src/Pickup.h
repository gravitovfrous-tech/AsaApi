#pragma once

namespace Pickup
{
	/**
	 * \brief Loads the configuration, registers chat/RCON commands and installs the optional hooks.
	 * Called from Plugin_Init.
	 */
	void Install();

	/**
	 * \brief Removes everything the plugin has registered. Called from Plugin_Unload.
	 */
	void Uninstall();

	/**
	 * \brief Re-reads Pickup.json and re-registers the commands.
	 */
	void ReloadConfig();
} // namespace Pickup
