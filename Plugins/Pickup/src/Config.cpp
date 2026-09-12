#include "Config.h"

#include <algorithm>
#include <fstream>
#include <iomanip>

#include "Categories.h"
#include "Utils.h"

namespace Pickup
{
	namespace
	{
		constexpr const char* config_file_name = "Pickup.json";

		std::string NormalizeKey(const FString& value)
		{
			return Utils::ToUtf8(Utils::ToLowerInvariant(value));
		}

		std::string NormalizeKey(const std::string& value)
		{
			return Utils::ToUtf8(Utils::ToLowerInvariant(Utils::FromUtf8(value)));
		}

		std::string GetPluginDirectory()
		{
			HMODULE module = nullptr;

			GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&GetPluginDirectory),
				&module);

			char path[MAX_PATH] = {};
			if (module == nullptr || GetModuleFileNameA(module, path, MAX_PATH) == 0)
			{
				// Fallback: the conventional plugin location next to the server executable.
				return AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/Pickup";
			}

			const std::string full_path(path);
			const auto position = full_path.find_last_of("\\/");

			return position == std::string::npos ? std::string() : full_path.substr(0, position);
		}

		template <typename T>
		T ReadValue(const nlohmann::json& node, const char* key, const T& default_value)
		{
			if (!node.is_object() || !node.contains(key))
				return default_value;

			try
			{
				return node.at(key).get<T>();
			}
			catch (const std::exception& error)
			{
				Log::GetLog()->warn("Pickup: invalid value of '{}' ({}), the default is used", key, error.what());
				return default_value;
			}
		}

		std::vector<std::string> ReadStrings(const nlohmann::json& node, const char* key,
			const std::vector<std::string>& default_value)
		{
			if (!node.is_object() || !node.contains(key) || !node.at(key).is_array())
				return default_value;

			std::vector<std::string> result;

			for (const auto& entry : node.at(key))
			{
				if (entry.is_string())
					result.push_back(entry.get<std::string>());
				else if (entry.is_number())
					result.push_back(entry.dump());
			}

			return result;
		}

		FLinearColor ReadColor(const nlohmann::json& node, const char* key, const FLinearColor& default_value)
		{
			if (!node.is_object() || !node.contains(key) || !node.at(key).is_array() || node.at(key).size() < 3)
				return default_value;

			const auto& array = node.at(key);
			if (!array[0].is_number() || !array[1].is_number() || !array[2].is_number())
				return default_value;

			const float alpha = array.size() > 3 && array[3].is_number()
				? static_cast<float>(array[3].get<double>())
				: default_value.A;

			return FLinearColor(static_cast<float>(array[0].get<double>()),
				static_cast<float>(array[1].get<double>()),
				static_cast<float>(array[2].get<double>()),
				alpha);
		}

		nlohmann::json WriteStrings(const std::vector<std::string>& values)
		{
			nlohmann::json array = nlohmann::json::array();
			for (const std::string& value : values)
				array.push_back(value);

			return array;
		}

		nlohmann::json WriteColor(const FLinearColor& color)
		{
			return nlohmann::json::array({color.R, color.G, color.B, color.A});
		}

		void AddUniqueCommand(std::vector<FString>& commands, const FString& command)
		{
			if (command.Len() == 0)
				return;

			for (const FString& existing : commands)
			{
				if (Utils::EqualsInvariant(existing, command))
					return;
			}

			commands.push_back(command);
		}
	} // namespace

	Config& Config::Get()
	{
		static Config instance;
		return instance;
	}

	Config::Config()
	{
		const std::string directory = GetPluginDirectory();
		config_path_ = directory.empty() ? std::string(config_file_name) : directory + "/" + config_file_name;

		BuildDefaults();
	}

	void Config::BuildDefaults()
	{
		settings_ = Settings();
		settings_.commands = {"/pickup", "/подбор", "/подобрать", "/пикап"};
		settings_.help_commands = {"/pickuphelp", "/подборпомощь"};
		settings_.reload_commands = {"/pickupreload", "/подборперезагрузка"};
		settings_.default_categories = {"all"};
		settings_.octree_groups = {"STRUCTURES"};

		categories_ = Categories::BuildDefaults();

		messages_.clear();

		SetMessage("ru", "Usage",
			"Использование: {0} [тип] [радиус]. Примеры: {0} w 5 - стены в радиусе 5 фундаментов; {0} f - только "
			"фундаменты; {0} - всё рядом. Типы: {1}");
		SetMessage("ru", "UnknownType", "Неизвестный тип структуры: {0}. Доступные типы: {1}");
		SetMessage("ru", "InvalidRadius", "Некорректный радиус: {0}. Допустимо от 0.5 до {1} фундаментов.");
		SetMessage("ru", "Cooldown", "Слишком часто. Подождите ещё {0} сек.");
		SetMessage("ru", "Dead", "Подбор структур недоступен, пока вы мертвы.");
		SetMessage("ru", "NoTribe", "Не удалось определить вашу принадлежность к племени - подбор отменён.");
		SetMessage("ru", "NothingFound", "В радиусе {0} фунд. нет структур вашего племени выбранного типа.");
		SetMessage("ru", "Picked", "Подобрано структур: {0} ({1}). Пропущено: {2}. Радиус: {3} фунд.");
		SetMessage("ru", "LimitReached", "Достигнут лимит {0} структур за одну команду - уменьшите радиус.");
		SetMessage("ru", "ForeignSkipped", "Чужих структур в радиусе не тронуто: {0}.");
		SetMessage("ru", "Reloaded", "Конфигурация Pickup перезагружена: категорий {0}, команд {1}.");
		SetMessage("ru", "ReloadDenied", "Перезагрузка конфигурации доступна только администраторам.");
		SetMessage("ru", "Internal", "Ошибка при выполнении команды, подробности в логе сервера.");
		SetMessage("ru", "CategoryCount", "{0}: {1}");
		SetMessage("ru", "SkippedNone", "нет");
		SetMessage("ru", "SkippedDamaged", "повреждено: {0}");
		SetMessage("ru", "SkippedNotAllowed", "недоступно для подбора: {0}");
		SetMessage("ru", "SkippedNoItem", "не возвращается предмет: {0}");
		SetMessage("ru", "SkippedFailed", "сбой подбора: {0}");
		SetMessage("ru", "SkippedOverflow", "сверх лимита за команду: {0}");
		SetMessage("ru", "SkippedHeight", "по высоте: {0}");

		SetMessage("en", "Usage",
			"Usage: {0} [type] [radius]. Examples: {0} w 5 - walls within 5 foundations; {0} f - foundations only; "
			"{0} - everything nearby. Types: {1}");
		SetMessage("en", "UnknownType", "Unknown structure type: {0}. Available types: {1}");
		SetMessage("en", "InvalidRadius", "Invalid radius: {0}. Allowed range is 0.5 to {1} foundations.");
		SetMessage("en", "Cooldown", "Too fast. Wait {0} more seconds.");
		SetMessage("en", "Dead", "Structure pickup is not available while you are dead.");
		SetMessage("en", "NoTribe", "Your tribe ownership could not be detected - pickup cancelled.");
		SetMessage("en", "NothingFound", "There are no structures of your tribe of this type within {0} foundations.");
		SetMessage("en", "Picked", "Picked up: {0} structures ({1}). Skipped: {2}. Radius: {3} foundations.");
		SetMessage("en", "LimitReached", "The limit of {0} structures per command was reached - use a smaller radius.");
		SetMessage("en", "ForeignSkipped", "Foreign structures left untouched: {0}.");
		SetMessage("en", "Reloaded", "Pickup configuration reloaded: {0} categories, {1} commands.");
		SetMessage("en", "ReloadDenied", "Only administrators can reload the configuration.");
		SetMessage("en", "Internal", "Command failed, see the server log for details.");
		SetMessage("en", "CategoryCount", "{0}: {1}");
		SetMessage("en", "SkippedNone", "none");
		SetMessage("en", "SkippedDamaged", "damaged: {0}");
		SetMessage("en", "SkippedNotAllowed", "not allowed: {0}");
		SetMessage("en", "SkippedNoItem", "gives no item: {0}");
		SetMessage("en", "SkippedFailed", "pickup failed: {0}");
		SetMessage("en", "SkippedOverflow", "over the per command limit: {0}");
		SetMessage("en", "SkippedHeight", "height: {0}");
	}

	void Config::SetMessage(const std::string& language, const std::string& key, const std::string& text)
	{
		messages_[language][key] = text;
	}

	bool Config::Load()
	{
		BuildDefaults();

		std::ifstream file(config_path_);
		if (!file.is_open())
		{
			Log::GetLog()->warn("Pickup: {} was not found, the built-in defaults are used", config_path_);

			if (Save())
				Log::GetLog()->info("Pickup: default configuration was written to {}", config_path_);
			else
				Log::GetLog()->error("Pickup: failed to write the default configuration to {}", config_path_);

			BuildAliasLookup();
			return true;
		}

		try
		{
			nlohmann::json config;
			file >> config;
			file.close();

			FromJson(config);

			Log::GetLog()->info("Pickup: configuration loaded from {}", config_path_);
		}
		catch (const std::exception& error)
		{
			file.close();
			Log::GetLog()->error("Pickup: failed to parse {} ({}), the built-in defaults are used", config_path_,
				error.what());

			BuildDefaults();
		}

		BuildAliasLookup();
		return true;
	}

	bool Config::Save() const
	{
		std::ofstream file(config_path_);
		if (!file.is_open())
			return false;

		file << std::setw(4) << ToJson() << std::endl;
		file.close();

		return !file.fail();
	}

	nlohmann::json Config::ToJson() const
	{
		nlohmann::json settings;

		settings["Language"] = settings_.language;
		settings["Commands"] = WriteStrings(settings_.commands);
		settings["HelpCommands"] = WriteStrings(settings_.help_commands);
		settings["ReloadCommands"] = WriteStrings(settings_.reload_commands);
		settings["RconReloadCommand"] = settings_.rcon_reload_command;
		settings["RegisterLayoutAliases"] = settings_.register_layout_aliases;
		settings["ReloadRequiresAdmin"] = settings_.reload_requires_admin;
		settings["DefaultCategories"] = WriteStrings(settings_.default_categories);
		settings["DefaultRadius"] = settings_.default_radius;
		settings["MaxRadius"] = settings_.max_radius;
		settings["FoundationSize"] = settings_.foundation_size;
		settings["MaxHeight"] = settings_.max_height;
		settings["CooldownSeconds"] = settings_.cooldown;
		settings["MaxStructuresPerCommand"] = settings_.max_structures;
		settings["OnlySameTribe"] = settings_.only_same_tribe;
		settings["RequireFullHealth"] = settings_.require_full_health;
		settings["AllowDamagedStructures"] = settings_.allow_damaged_structures;
		settings["IgnorePlacementTimeLimit"] = settings_.ignore_placement_time_limit;
		settings["UseVanillaAllowCheck"] = settings_.use_vanilla_allow_check;
		settings["SkipEnvironmentStructures"] = settings_.skip_environment_structures;
		settings["RequirePickupItem"] = settings_.require_pickup_item;
		settings["ReportForeignStructures"] = settings_.report_foreign_structures;
		settings["OctreeGroups"] = WriteStrings(settings_.octree_groups);

		nlohmann::json colors;
		colors["Success"] = WriteColor(settings_.color_success);
		colors["Error"] = WriteColor(settings_.color_error);
		colors["Info"] = WriteColor(settings_.color_info);
		settings["Colors"] = colors;

		nlohmann::json messages;
		for (const auto& language : messages_)
		{
			nlohmann::json language_node;
			for (const auto& entry : language.second)
				language_node[entry.first] = entry.second;

			messages[language.first] = language_node;
		}

		nlohmann::json categories = nlohmann::json::array();
		for (const CategoryConfig& category : categories_)
		{
			nlohmann::json node;
			node["Id"] = category.id;
			node["DisplayName"] = category.display_name;
			node["Enabled"] = category.enabled;
			node["Aliases"] = WriteStrings(category.aliases);
			node["Flags"] = WriteStrings(category.flags);
			node["ExcludeFlags"] = WriteStrings(category.exclude_flags);
			node["Classes"] = WriteStrings(category.classes);
			node["ExcludeClasses"] = WriteStrings(category.exclude_classes);
			node["Patterns"] = WriteStrings(category.patterns);
			node["ExcludePatterns"] = WriteStrings(category.exclude_patterns);

			categories.push_back(node);
		}

		nlohmann::json config;
		config["Settings"] = settings;
		config["Messages"] = messages;
		config["Categories"] = categories;

		return config;
	}

	void Config::FromJson(const nlohmann::json& config)
	{
		if (config.is_object() && config.contains("Settings") && config.at("Settings").is_object())
		{
			const nlohmann::json& node = config.at("Settings");

			settings_.language = ReadValue<std::string>(node, "Language", settings_.language);
			settings_.commands = ReadStrings(node, "Commands", settings_.commands);
			settings_.help_commands = ReadStrings(node, "HelpCommands", settings_.help_commands);
			settings_.reload_commands = ReadStrings(node, "ReloadCommands", settings_.reload_commands);
			settings_.rcon_reload_command = ReadValue<std::string>(node, "RconReloadCommand", settings_.rcon_reload_command);
			settings_.register_layout_aliases = ReadValue<bool>(node, "RegisterLayoutAliases", settings_.register_layout_aliases);
			settings_.reload_requires_admin = ReadValue<bool>(node, "ReloadRequiresAdmin", settings_.reload_requires_admin);
			settings_.default_categories = ReadStrings(node, "DefaultCategories", settings_.default_categories);
			settings_.default_radius = ReadValue<double>(node, "DefaultRadius", settings_.default_radius);
			settings_.max_radius = ReadValue<double>(node, "MaxRadius", settings_.max_radius);
			settings_.foundation_size = ReadValue<double>(node, "FoundationSize", settings_.foundation_size);
			settings_.max_height = ReadValue<double>(node, "MaxHeight", settings_.max_height);
			settings_.cooldown = ReadValue<double>(node, "CooldownSeconds", settings_.cooldown);
			settings_.max_structures = ReadValue<int>(node, "MaxStructuresPerCommand", settings_.max_structures);
			settings_.only_same_tribe = ReadValue<bool>(node, "OnlySameTribe", settings_.only_same_tribe);
			settings_.require_full_health = ReadValue<bool>(node, "RequireFullHealth", settings_.require_full_health);
			settings_.allow_damaged_structures = ReadValue<bool>(node, "AllowDamagedStructures", settings_.allow_damaged_structures);
			settings_.ignore_placement_time_limit =
				ReadValue<bool>(node, "IgnorePlacementTimeLimit", settings_.ignore_placement_time_limit);
			settings_.use_vanilla_allow_check = ReadValue<bool>(node, "UseVanillaAllowCheck", settings_.use_vanilla_allow_check);
			settings_.skip_environment_structures =
				ReadValue<bool>(node, "SkipEnvironmentStructures", settings_.skip_environment_structures);
			settings_.require_pickup_item = ReadValue<bool>(node, "RequirePickupItem", settings_.require_pickup_item);
			settings_.report_foreign_structures =
				ReadValue<bool>(node, "ReportForeignStructures", settings_.report_foreign_structures);
			settings_.octree_groups = ReadStrings(node, "OctreeGroups", settings_.octree_groups);

			if (node.contains("Colors") && node.at("Colors").is_object())
			{
				const nlohmann::json& colors = node.at("Colors");
				settings_.color_success = ReadColor(colors, "Success", settings_.color_success);
				settings_.color_error = ReadColor(colors, "Error", settings_.color_error);
				settings_.color_info = ReadColor(colors, "Info", settings_.color_info);
			}
		}

		if (settings_.foundation_size <= 0.0)
		{
			Log::GetLog()->warn("Pickup: FoundationSize must be positive, 300 is used");
			settings_.foundation_size = 300.0;
		}

		if (settings_.max_radius <= 0.0)
			settings_.max_radius = 30.0;

		if (settings_.default_radius <= 0.0)
			settings_.default_radius = std::min(3.0, settings_.max_radius);

		if (settings_.max_structures <= 0)
			settings_.max_structures = 300;

		if (settings_.octree_groups.empty())
			settings_.octree_groups = {"STRUCTURES"};

		if (settings_.commands.empty())
			settings_.commands = {"/pickup"};

		if (config.is_object() && config.contains("Messages") && config.at("Messages").is_object())
		{
			for (const auto& language : config.at("Messages").items())
			{
				if (!language.value().is_object())
					continue;

				for (const auto& entry : language.value().items())
				{
					if (entry.value().is_string())
						SetMessage(language.key(), entry.key(), entry.value().get<std::string>());
				}
			}
		}

		if (config.is_object() && config.contains("Categories") && config.at("Categories").is_array()
			&& !config.at("Categories").empty())
		{
			std::vector<CategoryConfig> categories;

			for (const auto& node : config.at("Categories"))
			{
				if (!node.is_object())
					continue;

				CategoryConfig category;
				category.id = ReadValue<std::string>(node, "Id", "");
				category.display_name = ReadValue<std::string>(node, "DisplayName", category.id);
				category.enabled = ReadValue<bool>(node, "Enabled", true);
				category.aliases = ReadStrings(node, "Aliases", {});
				category.flags = ReadStrings(node, "Flags", {});
				category.exclude_flags = ReadStrings(node, "ExcludeFlags", {});
				category.classes = ReadStrings(node, "Classes", {});
				category.exclude_classes = ReadStrings(node, "ExcludeClasses", {});
				category.patterns = ReadStrings(node, "Patterns", {});
				category.exclude_patterns = ReadStrings(node, "ExcludePatterns", {});

				if (category.id.empty())
				{
					Log::GetLog()->warn("Pickup: a category without an Id was skipped");
					continue;
				}

				categories.push_back(std::move(category));
			}

			if (!categories.empty())
				categories_ = std::move(categories);
		}
	}

	void Config::BuildAliasLookup()
	{
		alias_lookup_.clear();

		for (const CategoryConfig& category : categories_)
		{
			if (!category.enabled)
				continue;

			for (const std::string& alias : category.aliases)
			{
				const std::string key = NormalizeKey(alias);
				if (key.empty())
					continue;

				bool exists = false;
				for (const auto& entry : alias_lookup_)
				{
					if (entry.first == key)
					{
						exists = true;
						break;
					}
				}

				if (exists)
				{
					Log::GetLog()->warn("Pickup: duplicated category alias '{}' is ignored", alias);
					continue;
				}

				alias_lookup_.emplace_back(key, &category);
			}

			// The category id itself always works as an alias.
			const std::string id_key = NormalizeKey(category.id);
			bool id_exists = false;
			for (const auto& entry : alias_lookup_)
			{
				if (entry.first == id_key)
				{
					id_exists = true;
					break;
				}
			}

			if (!id_exists)
				alias_lookup_.emplace_back(id_key, &category);
		}
	}

	const CategoryConfig* Config::FindCategoryById(const std::string& id) const
	{
		for (const CategoryConfig& category : categories_)
		{
			if (category.id == id)
				return &category;
		}

		return nullptr;
	}

	const CategoryConfig* Config::ResolveAlias(const FString& argument) const
	{
		const std::string key = NormalizeKey(argument);
		if (key.empty())
			return nullptr;

		for (const auto& entry : alias_lookup_)
		{
			if (entry.first == key)
				return entry.second;
		}

		// Support arguments typed with the wrong keyboard layout: "ц" -> "w", "а" -> "f".
		const FString alternative = Utils::ConvertRuToEn(argument);
		if (alternative.Len() > 0)
		{
			const std::string alternative_key = NormalizeKey(alternative);
			for (const auto& entry : alias_lookup_)
			{
				if (entry.first == alternative_key)
					return entry.second;
			}
		}

		return nullptr;
	}

	std::vector<const CategoryConfig*> Config::GetEnabledCategories() const
	{
		std::vector<const CategoryConfig*> result;

		for (const CategoryConfig& category : categories_)
		{
			if (category.enabled)
				result.push_back(&category);
		}

		return result;
	}

	std::vector<const CategoryConfig*> Config::GetDefaultCategories() const
	{
		std::vector<const CategoryConfig*> result;

		for (const std::string& id : settings_.default_categories)
		{
			const CategoryConfig* category = FindCategoryById(id);
			if (category != nullptr && category->enabled)
				result.push_back(category);
		}

		if (result.empty())
		{
			const CategoryConfig* fallback = FindCategoryById("all");
			if (fallback != nullptr && fallback->enabled)
				result.push_back(fallback);
			else
				result = GetEnabledCategories();
		}

		return result;
	}

	FString Config::GetMessage(const std::string& key) const
	{
		const auto language = messages_.find(settings_.language);
		if (language != messages_.end())
		{
			const auto entry = language->second.find(key);
			if (entry != language->second.end())
				return Utils::FromUtf8(entry->second);
		}

		const auto english = messages_.find("en");
		if (english != messages_.end())
		{
			const auto entry = english->second.find(key);
			if (entry != english->second.end())
				return Utils::FromUtf8(entry->second);
		}

		return Utils::FromUtf8(key);
	}

	std::vector<FString> Config::GetChatCommands(const std::vector<std::string>& configured) const
	{
		std::vector<FString> result;

		for (const std::string& command : configured)
		{
			const FString text = Utils::FromUtf8(command);
			AddUniqueCommand(result, text);

			if (settings_.register_layout_aliases)
				AddUniqueCommand(result, Utils::AlternativeLayout(text));
		}

		return result;
	}
} // namespace Pickup
