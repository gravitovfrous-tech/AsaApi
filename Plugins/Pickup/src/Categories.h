#pragma once

#include <string>
#include <vector>

#include <API/ARK/Ark.h>

#include "Config.h"

namespace Pickup::Categories
{
	/**
	 * \brief Built-in category table. Used when Pickup.json is missing or does not define categories.
	 */
	std::vector<CategoryConfig> BuildDefaults();

	/**
	 * \brief Builds the text used by "patterns" matching: the structure blueprint, the blueprint of the
	 * item it gives back and the current variant tag. Cached per class.
	 */
	FString GetSearchText(APrimalStructure* structure);

	/**
	 * \brief Checks a structure against a category definition.
	 */
	bool Matches(APrimalStructure* structure, const FString& search_text, const CategoryConfig& category);

	/**
	 * \brief Cached APrimalStructure::StaticClass(), nullptr until the game has registered the class.
	 */
	UClass* StructureClass();

	/**
	 * \brief True when the structure still exists in the world and was not picked up/demolished yet.
	 */
	bool IsAlive(APrimalStructure* structure);

	/**
	 * \brief Calls the vanilla APrimalStructure::AllowPickupForItem of the most derived known class.
	 */
	bool VanillaAllowPickup(APrimalStructure* structure, AShooterPlayerController* player_controller);

	/**
	 * \brief Drops all internal caches (called on plugin unload).
	 */
	void ClearCache();
} // namespace Pickup::Categories
