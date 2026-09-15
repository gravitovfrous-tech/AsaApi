#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <API/ARK/Ark.h>
#include <json.hpp>

namespace Pickup
{
	/**
	 * \brief Description of a structure category (walls, foundations, ceilings, ...).
	 *
	 * A structure belongs to the category when at least one of the "flags", "classes" or "patterns"
	 * matches and none of the "exclude_*" entries matches.
	 */
	struct CategoryConfig
	{
		std::string id;
		std::string display_name; // UTF-8, shown to players
		bool enabled = true;

		// Command arguments that select this category, UTF-8, matched case-insensitively.
		std::vector<std::string> aliases;

		// APrimalStructure bit fields: bIsWall, bIsFloor, bIsFoundation, bIsFenceFoundation, bIsDoorframe, bIsBed ...
		std::vector<std::string> flags;
		std::vector<std::string> exclude_flags;

		// Native classes checked with IsA(): APrimalStructure, APrimalStructureDoor, APrimalStructureItemContainer ...
		std::vector<std::string> classes;
		std::vector<std::string> exclude_classes;

		// Substrings of the structure blueprint path / pickup item blueprint / variant tag.
		std::vector<std::string> patterns;
		std::vector<std::string> exclude_patterns;
	};

	struct Settings
	{
		std::string language = "ru";

		std::vector<std::string> commands;        // chat commands that perform the pickup
		std::vector<std::string> help_commands;   // chat commands that print the usage
		std::vector<std::string> reload_commands; // chat commands that reload this configuration
		std::string rcon_reload_command = "pickup.reload";
		bool register_layout_aliases = true; // also register commands typed with the wrong keyboard layout
		bool reload_requires_admin = true;

		std::vector<std::string> default_categories; // used when the player did not specify a type

		double default_radius = 3.0;    // in foundations
		double max_radius = 30.0;       // in foundations
		double foundation_size = 300.0; // game units in one foundation
		double max_height = 0.0;        // in foundations, 0 = unlimited (search in a sphere)
		double cooldown = 2.0;          // seconds between two commands of the same player
		int max_structures = 300;       // hard limit of structures picked up by a single command

		bool only_same_tribe = true;              // never touch structures of other tribes/players
		bool require_full_health = true;          // skip damaged structures (vanilla rule)
		bool allow_damaged_structures = false;    // force pickup of damaged structures (hook based, use with care)
		bool ignore_placement_time_limit = true;  // temporarily enable bAlwaysAllowStructurePickup
		bool use_vanilla_allow_check = true;      // call APrimalStructure::AllowPickupForItem before the pickup
		bool skip_environment_structures = true;  // skip cave/environment structures
		bool require_pickup_item = true;          // skip structures that do not give an item back
		bool report_foreign_structures = true;    // tell the player how many foreign structures were skipped

		std::vector<std::string> octree_groups; // server octree groups used for the area search

		FLinearColor color_success{0.2f, 1.0f, 0.35f};
		FLinearColor color_error{1.0f, 0.35f, 0.3f};
		FLinearColor color_info{1.0f, 0.85f, 0.35f};
	};

	/**
	 * \brief Plugin configuration. Everything (commands, categories, texts) is driven by Pickup.json.
	 */
	class Config
	{
	public:
		static Config& Get();

		Config(const Config&) = delete;
		Config(Config&&) = delete;
		Config& operator=(const Config&) = delete;
		Config& operator=(Config&&) = delete;

		/**
		 * \brief Loads Pickup.json. Creates the file from the built-in defaults when it is missing.
		 */
		bool Load();

		const Settings& GetSettings() const { return settings_; }
		const std::vector<CategoryConfig>& GetCategories() const { return categories_; }

		const CategoryConfig* FindCategoryById(const std::string& id) const;

		/**
		 * \brief Resolves a command argument into a category. Also understands arguments typed with
		 * the wrong keyboard layout ("/подбор ц 10" -> walls).
		 */
		const CategoryConfig* ResolveAlias(const FString& argument) const;

		std::vector<const CategoryConfig*> GetEnabledCategories() const;
		std::vector<const CategoryConfig*> GetDefaultCategories() const;

		/**
		 * \brief Returns a localized message, falls back to English and then to the key itself.
		 */
		FString GetMessage(const std::string& key) const;

		/**
		 * \brief All chat commands that have to be registered, including the wrong-layout variants.
		 */
		std::vector<FString> GetChatCommands(const std::vector<std::string>& configured) const;

		std::string GetConfigPath() const { return config_path_; }

	private:
		Config();

		void BuildDefaults();
		void BuildAliasLookup();
		void SetMessage(const std::string& language, const std::string& key, const std::string& text);

		nlohmann::json ToJson() const;
		void FromJson(const nlohmann::json& config);
		bool Save() const;

		std::string config_path_;
		Settings settings_;
		std::vector<CategoryConfig> categories_;
		std::unordered_map<std::string, std::unordered_map<std::string, std::string>> messages_;
		std::vector<std::pair<std::string, const CategoryConfig*>> alias_lookup_;
	};
} // namespace Pickup
