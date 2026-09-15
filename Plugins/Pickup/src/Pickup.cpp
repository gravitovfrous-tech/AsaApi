#include "Pickup.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <API/ARK/Ark.h>

#include "Categories.h"
#include "Config.h"
#include "Utils.h"

namespace Pickup
{
	namespace
	{
		constexpr int max_message_chunk = 400;

		/**
		 * State of the temporary "allow damaged structures" bypass. It is only active while the pickup
		 * command of one player is being processed, so the vanilla behaviour is kept everywhere else.
		 */
		struct BypassContext
		{
			bool active = false;
			AShooterPlayerController* controller = nullptr;
			int tribe_id = 0;
		};

		struct Candidate
		{
			double distance = 0.0;
			APrimalStructure* structure = nullptr;
			const CategoryConfig* category = nullptr;
		};

		struct Statistics
		{
			int in_range = 0;
			int picked = 0;
			int foreign = 0;
			int other_type = 0;
			int damaged = 0;
			int not_allowed = 0;
			int no_item = 0;
			int failed = 0;
			int overflow = 0;
			int out_of_height = 0;
			std::vector<std::pair<const CategoryConfig*, int>> per_category;
		};

		BypassContext bypass_context;
		bool hooks_installed = false;

		std::unordered_map<std::string, double> cooldowns;

		std::vector<FString> registered_commands;
		std::vector<FString> registered_help_commands;
		std::vector<FString> registered_reload_commands;
		std::string registered_rcon_command;
		bool chat_callback_registered = false;

		double GetWorldTime()
		{
			UWorld* world = AsaApi::GetApiUtils().GetWorld();

			return world != nullptr ? static_cast<double>(world->TimeSecondsField()) : 0.0;
		}

		/**
		 * Formats a message and never throws - a broken template in the config must not crash the server.
		 */
		template <typename... Args>
		FString SafeFormat(const FString& format, Args&&... args)
		{
			try
			{
				return FString::Format(*format, std::forward<Args>(args)...);
			}
			catch (const std::exception& error)
			{
				Log::GetLog()->warn("Pickup: broken message template '{}' ({})", Utils::ToUtf8(format), error.what());
				return format;
			}
		}

		/**
		 * Sends a long message to the player, splitting it into chat sized chunks.
		 */
		void SendText(AShooterPlayerController* player_controller, const FLinearColor& color, const FString& text)
		{
			if (player_controller == nullptr || text.Len() == 0)
				return;

			// SendServerMessage() runs the text through fmt one more time, so the braces have to be escaped.
			const FString escaped = text.Replace(L"{", L"{{", ESearchCase::CaseSensitive)
				.Replace(L"}", L"}}", ESearchCase::CaseSensitive);

			for (const FString& chunk : Utils::SplitToChunks(escaped, max_message_chunk))
				AsaApi::GetApiUtils().SendServerMessage(player_controller, color, *chunk);
		}

		void SendKey(AShooterPlayerController* player_controller, const FLinearColor& color, const std::string& key)
		{
			SendText(player_controller, color, Config::Get().GetMessage(key));
		}

		FString BuildTypesList()
		{
			std::vector<FString> entries;

			for (const CategoryConfig* category : Config::Get().GetEnabledCategories())
			{
				if (category->aliases.empty())
					continue;

				FString entry = Utils::FromUtf8(category->display_name);
				entry.Append(L" (", 2);
				entry += Utils::FromUtf8(category->aliases.front());
				entry.Append(L")", 1);

				entries.push_back(entry);
			}

			return Utils::Join(entries, Utils::FromUtf8(", "));
		}

		FString BuildSkippedText(const Statistics& statistics)
		{
			Config& config = Config::Get();
			std::vector<FString> parts;

			const auto add = [&parts, &config](const std::string& key, int value)
			{
				if (value <= 0)
					return;

				parts.push_back(SafeFormat(config.GetMessage(key), value));
			};

			add("SkippedDamaged", statistics.damaged);
			add("SkippedNotAllowed", statistics.not_allowed);
			add("SkippedNoItem", statistics.no_item);
			add("SkippedFailed", statistics.failed);
			add("SkippedOverflow", statistics.overflow);
			add("SkippedHeight", statistics.out_of_height);

			if (parts.empty())
				return config.GetMessage("SkippedNone");

			return Utils::Join(parts, Utils::FromUtf8(", "));
		}

		FString BuildPerCategoryText(const Statistics& statistics)
		{
			Config& config = Config::Get();
			std::vector<FString> parts;

			for (const auto& entry : statistics.per_category)
			{
				if (entry.second <= 0)
					continue;

				parts.push_back(SafeFormat(config.GetMessage("CategoryCount"),
					*Utils::FromUtf8(entry.first->display_name), entry.second));
			}

			if (parts.empty())
				return Utils::FromUtf8("-");

			return Utils::Join(parts, Utils::FromUtf8(", "));
		}

		bool IsHelpToken(const FString& token)
		{
			static const std::vector<const wchar_t*> help_tokens = {
				L"help", L"h", L"?", L"-h", L"--help", L"помощь", L"помошь", L"справка"
			};

			for (const wchar_t* help_token : help_tokens)
			{
				if (Utils::EqualsInvariant(token, FString(help_token)))
					return true;
			}

			return false;
		}

		bool ParseOctreeGroup(const std::string& name, EServerOctreeGroup::Type& out_group)
		{
			if (name == "STRUCTURES")
				out_group = EServerOctreeGroup::STRUCTURES;
			else if (name == "STRUCTURES_CORE")
				out_group = EServerOctreeGroup::STRUCTURES_CORE;
			else if (name == "THERMALSTRUCTURES")
				out_group = EServerOctreeGroup::THERMALSTRUCTURES;
			else if (name == "TARGETABLEACTORS")
				out_group = EServerOctreeGroup::TARGETABLEACTORS;
			else if (name == "ALL_SPATIAL")
				out_group = EServerOctreeGroup::ALL_SPATIAL;
			else
			{
				Log::GetLog()->warn("Pickup: unknown octree group '{}' in the configuration, it is ignored", name);
				return false;
			}

			return true;
		}

		/**
		 * Collects the structures of the player tribe that match the requested categories.
		 */
		std::vector<Candidate> CollectCandidates(AShooterPlayerController* player_controller, const Settings& settings,
			const std::vector<const CategoryConfig*>& categories, int tribe_id, double radius_units,
			Statistics& statistics)
		{
			std::vector<Candidate> candidates;
			std::unordered_set<const AActor*> seen;

			UClass* structure_class = Categories::StructureClass();
			if (structure_class == nullptr)
			{
				Log::GetLog()->error("Pickup: APrimalStructure::StaticClass() is not available yet, nothing to do");
				return candidates;
			}

			const FVector origin = AsaApi::IApiUtils::GetPosition(player_controller);

			for (const std::string& group_name : settings.octree_groups)
			{
				EServerOctreeGroup::Type group = EServerOctreeGroup::STRUCTURES;
				if (!ParseOctreeGroup(group_name, group))
					continue;

				TArray<AActor*> actors =
					AsaApi::GetApiUtils().GetAllActorsInRange(origin, static_cast<float>(radius_units), group);

				for (AActor* actor : actors)
				{
					if (actor == nullptr || seen.find(actor) != seen.end())
						continue;

					seen.insert(actor);

					if (!actor->IsA(structure_class))
						continue;

					auto* structure = static_cast<APrimalStructure*>(actor);
					if (!Categories::IsAlive(structure))
						continue;

					if (settings.skip_environment_structures && structure->bIsEnvironmentStructure().Get())
						continue;

					statistics.in_range++;

					// The most important check: a structure of another tribe/player is never touched.
					if (settings.only_same_tribe && structure->TargetingTeamField() != tribe_id)
					{
						statistics.foreign++;
						continue;
					}

					const FString search_text = Categories::GetSearchText(structure);

					const CategoryConfig* matched = nullptr;
					for (const CategoryConfig* category : categories)
					{
						if (Categories::Matches(structure, search_text, *category))
						{
							matched = category;
							break;
						}
					}

					if (matched == nullptr)
					{
						statistics.other_type++;
						continue;
					}

					const FVector position = structure->GetLocation();

					if (settings.max_height > 0.0)
					{
						const double height_limit = settings.max_height * settings.foundation_size;
						if (std::fabs(position.Z - origin.Z) > height_limit)
						{
							statistics.out_of_height++;
							continue;
						}
					}

					Candidate candidate;
					candidate.distance = FVector::Dist(origin, position);
					candidate.structure = structure;
					candidate.category = matched;

					candidates.push_back(candidate);
				}
			}

			return candidates;
		}

		/**
		 * Restores the game mode flag and disables the hook bypass even when an exception escapes.
		 */
		struct PickupGuard
		{
			AShooterGameMode* game_mode = nullptr;
			bool restore_flag = false;
			bool previous_flag = false;

			PickupGuard(AShooterGameMode* mode, bool ignore_placement_time_limit)
				: game_mode(mode)
			{
				if (game_mode == nullptr || !ignore_placement_time_limit)
					return;

				previous_flag = game_mode->bAlwaysAllowStructurePickupField();
				if (!previous_flag)
				{
					game_mode->bAlwaysAllowStructurePickupField() = true;
					restore_flag = true;
				}
			}

			PickupGuard(const PickupGuard&) = delete;
			PickupGuard& operator=(const PickupGuard&) = delete;

			~PickupGuard()
			{
				if (restore_flag && game_mode != nullptr)
					game_mode->bAlwaysAllowStructurePickupField() = previous_flag;

				bypass_context = BypassContext();
			}
		};

		/**
		 * Picks up the collected structures, the nearest ones first.
		 */
		void PerformPickup(AShooterPlayerController* player_controller, const Settings& settings, int tribe_id,
			const std::vector<Candidate>& candidates, Statistics& statistics)
		{
			AShooterGameMode* game_mode = AsaApi::GetApiUtils().GetShooterGameMode();

			const PickupGuard guard(game_mode, settings.ignore_placement_time_limit);

			bypass_context.active = settings.allow_damaged_structures;
			bypass_context.controller = player_controller;
			bypass_context.tribe_id = tribe_id;

			for (const Candidate& candidate : candidates)
			{
				APrimalStructure* structure = candidate.structure;

				// Picking up one structure can destroy the ones attached to it.
				if (!Categories::IsAlive(structure))
					continue;

				if (settings.only_same_tribe && structure->TargetingTeamField() != tribe_id)
				{
					statistics.foreign++;
					continue;
				}

				if (settings.require_full_health && !settings.allow_damaged_structures)
				{
					const float max_health = structure->GetMaxHealth();
					const float health = structure->GetHealth();

					if (max_health > 0.0f && health < max_health - 0.5f)
					{
						statistics.damaged++;
						continue;
					}
				}

				if (settings.require_pickup_item && structure->PickupGivesItemField().uClass == nullptr)
				{
					statistics.no_item++;
					continue;
				}

				if (settings.use_vanilla_allow_check && !Categories::VanillaAllowPickup(structure, player_controller))
				{
					statistics.not_allowed++;

					if (statistics.not_allowed == 1)
					{
						Log::GetLog()->info("Pickup: the game rejected '{}' ({}), health {}/{}",
							Utils::ToUtf8(Categories::GetSearchText(structure)),
							Utils::ToUtf8(structure->ClassPrivateField()->NameField().ToString()),
							structure->GetHealth(), structure->GetMaxHealth());
					}

					continue;
				}

				UPrimalItem* item = structure->PickupStructure(true, player_controller);
				if (item != nullptr)
				{
					statistics.picked++;

					for (auto& entry : statistics.per_category)
					{
						if (entry.first == candidate.category)
						{
							entry.second++;
							break;
						}
					}
				}
				else
				{
					statistics.failed++;

					if (statistics.failed <= 3)
					{
						Log::GetLog()->info("Pickup: PickupStructure() returned nothing for '{}'",
							Utils::ToUtf8(Categories::GetSearchText(structure)));
					}
				}
			}
		}

		bool CheckCooldown(AShooterPlayerController* player_controller, const Settings& settings)
		{
			if (settings.cooldown <= 0.0)
				return true;

			const std::string key = Utils::ToUtf8(AsaApi::IApiUtils::GetEOSIDFromController(player_controller));
			if (key.empty())
				return true;

			const double now = GetWorldTime();

			if (cooldowns.size() > 512)
			{
				for (auto iterator = cooldowns.begin(); iterator != cooldowns.end();)
				{
					if (now - iterator->second > settings.cooldown)
						iterator = cooldowns.erase(iterator);
					else
						++iterator;
				}
			}

			const auto entry = cooldowns.find(key);
			if (entry != cooldowns.end() && now - entry->second < settings.cooldown)
			{
				const double remaining = settings.cooldown - (now - entry->second);
				const int seconds = static_cast<int>(std::ceil(remaining));

				SendText(player_controller, settings.color_error,
					SafeFormat(Config::Get().GetMessage("Cooldown"), seconds > 0 ? seconds : 1));

				return false;
			}

			cooldowns[key] = now;
			return true;
		}

		void SendUsage(AShooterPlayerController* player_controller)
		{
			const Settings settings = Config::Get().GetSettings();
			const std::vector<FString> commands = Config::Get().GetChatCommands(settings.commands);
			const FString primary = commands.empty() ? FString(L"/pickup") : commands.front();
			const FString types = BuildTypesList();

			SendText(player_controller, settings.color_info,
				SafeFormat(Config::Get().GetMessage("Usage"), *primary, *types));
		}
	} // namespace

	/**
	 * Health restriction bypass hooks. They are installed only when "AllowDamagedStructures" is enabled and
	 * they are active only for the player controller that is running the pickup command right now.
	 */
	DECLARE_HOOK(APrimalStructure_AllowPickupForItem, bool, APrimalStructure*, AShooterPlayerController*);
	DECLARE_HOOK(APrimalStructureDoor_AllowPickupForItem, bool, APrimalStructureDoor*, AShooterPlayerController*);
	DECLARE_HOOK(APrimalStructureBed_AllowPickupForItem, bool, APrimalStructureBed*, AShooterPlayerController*);
	DECLARE_HOOK(APrimalStructureSeating_AllowPickupForItem, bool, APrimalStructureSeating*, AShooterPlayerController*);
	DECLARE_HOOK(APrimalStructureBearTrap_AllowPickupForItem, bool, APrimalStructureBearTrap*, AShooterPlayerController*);

	namespace
	{
		bool ApplyBypass(APrimalStructure* structure, AShooterPlayerController* player_controller, bool result)
		{
			if (result || !bypass_context.active || structure == nullptr || bypass_context.controller != player_controller)
				return result;

			const int tribe_id = structure->TargetingTeamField();
			if (Config::Get().GetSettings().only_same_tribe && tribe_id != bypass_context.tribe_id)
				return result;

			return true;
		}
	} // namespace

	bool Hook_APrimalStructure_AllowPickupForItem(APrimalStructure* structure, AShooterPlayerController* player_controller)
	{
		const bool result = APrimalStructure_AllowPickupForItem_original(structure, player_controller);

		return ApplyBypass(structure, player_controller, result);
	}

	bool Hook_APrimalStructureDoor_AllowPickupForItem(APrimalStructureDoor* structure,
		AShooterPlayerController* player_controller)
	{
		const bool result = APrimalStructureDoor_AllowPickupForItem_original(structure, player_controller);

		return ApplyBypass(structure, player_controller, result);
	}

	bool Hook_APrimalStructureBed_AllowPickupForItem(APrimalStructureBed* structure,
		AShooterPlayerController* player_controller)
	{
		const bool result = APrimalStructureBed_AllowPickupForItem_original(structure, player_controller);

		return ApplyBypass(structure, player_controller, result);
	}

	bool Hook_APrimalStructureSeating_AllowPickupForItem(APrimalStructureSeating* structure,
		AShooterPlayerController* player_controller)
	{
		const bool result = APrimalStructureSeating_AllowPickupForItem_original(structure, player_controller);

		return ApplyBypass(structure, player_controller, result);
	}

	bool Hook_APrimalStructureBearTrap_AllowPickupForItem(APrimalStructureBearTrap* structure,
		AShooterPlayerController* player_controller)
	{
		const bool result = APrimalStructureBearTrap_AllowPickupForItem_original(structure, player_controller);

		return ApplyBypass(structure, player_controller, result);
	}

	namespace
	{
		void InstallHooks()
		{
			if (hooks_installed)
				return;

			auto& hooks = AsaApi::GetHooks();

			if (!hooks.SetHook("APrimalStructure.AllowPickupForItem(AShooterPlayerController*)",
				&Hook_APrimalStructure_AllowPickupForItem, &APrimalStructure_AllowPickupForItem_original))
				Log::GetLog()->warn("Pickup: failed to hook APrimalStructure.AllowPickupForItem");

			if (!hooks.SetHook("APrimalStructureDoor.AllowPickupForItem(AShooterPlayerController*)",
				&Hook_APrimalStructureDoor_AllowPickupForItem, &APrimalStructureDoor_AllowPickupForItem_original))
				Log::GetLog()->warn("Pickup: failed to hook APrimalStructureDoor.AllowPickupForItem");

			if (!hooks.SetHook("APrimalStructureBed.AllowPickupForItem(AShooterPlayerController*)",
				&Hook_APrimalStructureBed_AllowPickupForItem, &APrimalStructureBed_AllowPickupForItem_original))
				Log::GetLog()->warn("Pickup: failed to hook APrimalStructureBed.AllowPickupForItem");

			if (!hooks.SetHook("APrimalStructureSeating.AllowPickupForItem(AShooterPlayerController*)",
				&Hook_APrimalStructureSeating_AllowPickupForItem, &APrimalStructureSeating_AllowPickupForItem_original))
				Log::GetLog()->warn("Pickup: failed to hook APrimalStructureSeating.AllowPickupForItem");

			if (!hooks.SetHook("APrimalStructureBearTrap.AllowPickupForItem(AShooterPlayerController*)",
				&Hook_APrimalStructureBearTrap_AllowPickupForItem,
				&APrimalStructureBearTrap_AllowPickupForItem_original))
				Log::GetLog()->warn("Pickup: failed to hook APrimalStructureBearTrap.AllowPickupForItem");

			hooks_installed = true;
		}

		void DisableHooks()
		{
			if (!hooks_installed)
				return;

			auto& hooks = AsaApi::GetHooks();

			hooks.DisableHook("APrimalStructure.AllowPickupForItem(AShooterPlayerController*)",
				&Hook_APrimalStructure_AllowPickupForItem);
			hooks.DisableHook("APrimalStructureDoor.AllowPickupForItem(AShooterPlayerController*)",
				&Hook_APrimalStructureDoor_AllowPickupForItem);
			hooks.DisableHook("APrimalStructureBed.AllowPickupForItem(AShooterPlayerController*)",
				&Hook_APrimalStructureBed_AllowPickupForItem);
			hooks.DisableHook("APrimalStructureSeating.AllowPickupForItem(AShooterPlayerController*)",
				&Hook_APrimalStructureSeating_AllowPickupForItem);
			hooks.DisableHook("APrimalStructureBearTrap.AllowPickupForItem(AShooterPlayerController*)",
				&Hook_APrimalStructureBearTrap_AllowPickupForItem);

			hooks_installed = false;
			bypass_context = BypassContext();
		}

		void UpdateHooks()
		{
			if (Config::Get().GetSettings().allow_damaged_structures)
				InstallHooks();
			else
				DisableHooks();
		}

		void ExecutePickup(AShooterPlayerController* player_controller, FString* message, int /*mode*/, int /*platform*/)
		{
			try
			{
				Config& config = Config::Get();
				const Settings settings = config.GetSettings();

				if (player_controller == nullptr || message == nullptr)
					return;

				TArray<FString> args;
				message->ParseIntoArray(args, L" ", true);

				bool help_requested = false;
				bool radius_explicit = false;
				double radius_foundations = settings.default_radius;
				FString unknown_type;
				std::vector<const CategoryConfig*> categories;

				for (int index = 1; index < args.Num(); ++index)
				{
					const FString token = args[index];
					if (token.Len() == 0)
						continue;

					if (IsHelpToken(token))
					{
						help_requested = true;
						break;
					}

					// "<number>" is the radius in foundations, "<number>u" is the radius in game units.
					FString number_part = token;
					bool raw_units = false;

					const wchar_t last_character = token[token.Len() - 1];
					if (token.Len() > 1 && (last_character == L'u' || last_character == L'U'))
					{
						number_part = token.Left(token.Len() - 1);
						raw_units = true;
					}

					double value = 0.0;
					if (Utils::TryParseDouble(number_part, value))
					{
						if (!radius_explicit)
						{
							radius_explicit = true;
							radius_foundations = raw_units ? value / settings.foundation_size : value;
						}

						continue;
					}

					const CategoryConfig* category = config.ResolveAlias(token);
					if (category == nullptr)
					{
						unknown_type = token;
						break;
					}

					if (std::find(categories.begin(), categories.end(), category) == categories.end())
						categories.push_back(category);
				}

				if (help_requested)
				{
					SendUsage(player_controller);
					return;
				}

				if (unknown_type.Len() > 0)
				{
					const FString types = BuildTypesList();
					SendText(player_controller, settings.color_error,
						SafeFormat(config.GetMessage("UnknownType"), *unknown_type, *types));

					return;
				}

				if (radius_explicit && (radius_foundations <= 0.0 || radius_foundations > settings.max_radius))
				{
					SendText(player_controller, settings.color_error,
						SafeFormat(config.GetMessage("InvalidRadius"), radius_foundations, settings.max_radius));

					return;
				}

				if (!radius_explicit)
					radius_foundations = settings.default_radius;

				if (categories.empty())
					categories = config.GetDefaultCategories();

				if (categories.empty())
				{
					SendUsage(player_controller);
					return;
				}

				if (AsaApi::IApiUtils::IsPlayerDead(player_controller))
				{
					SendKey(player_controller, settings.color_error, "Dead");
					return;
				}

				const int tribe_id = AsaApi::IApiUtils::GetTribeID(player_controller);
				if (settings.only_same_tribe && tribe_id == 0)
				{
					SendKey(player_controller, settings.color_error, "NoTribe");
					return;
				}

				if (!CheckCooldown(player_controller, settings))
					return;

				const double radius_units = radius_foundations * settings.foundation_size;

				Statistics statistics;
				for (const CategoryConfig* category : categories)
					statistics.per_category.emplace_back(category, 0);

				std::vector<Candidate> candidates =
					CollectCandidates(player_controller, settings, categories, tribe_id, radius_units, statistics);

				std::sort(candidates.begin(), candidates.end(),
					[](const Candidate& left, const Candidate& right) { return left.distance < right.distance; });

				if (settings.max_structures > 0 && static_cast<int>(candidates.size()) > settings.max_structures)
				{
					statistics.overflow = static_cast<int>(candidates.size()) - settings.max_structures;
					candidates.resize(static_cast<size_t>(settings.max_structures));
				}

				PerformPickup(player_controller, settings, tribe_id, candidates, statistics);

				const std::string player_name =
					Utils::ToUtf8(AsaApi::IApiUtils::GetSteamName(player_controller));
				const std::string player_id = Utils::ToUtf8(AsaApi::IApiUtils::GetEOSIDFromController(player_controller));

				Log::GetLog()->info(
					"Pickup: '{}' ({}) tribe {} picked {} structures in {} foundations (in range {}, foreign {}, "
					"other type {}, damaged {}, rejected {}, failed {})",
					player_name, player_id, tribe_id, statistics.picked, radius_foundations, statistics.in_range,
					statistics.foreign, statistics.other_type, statistics.damaged, statistics.not_allowed,
					statistics.failed);

				if (statistics.picked == 0 && statistics.damaged == 0 && statistics.not_allowed == 0
					&& statistics.no_item == 0 && statistics.failed == 0)
				{
					SendText(player_controller, settings.color_error,
						SafeFormat(config.GetMessage("NothingFound"), radius_foundations));

					if (settings.report_foreign_structures && statistics.foreign > 0)
					{
						SendText(player_controller, settings.color_info,
							SafeFormat(config.GetMessage("ForeignSkipped"), statistics.foreign));
					}

					return;
				}

				const FString details = BuildPerCategoryText(statistics);
				const FString skipped = BuildSkippedText(statistics);

				SendText(player_controller, settings.color_success,
					SafeFormat(config.GetMessage("Picked"), statistics.picked, *details, *skipped, radius_foundations));

				if (statistics.overflow > 0)
				{
					SendText(player_controller, settings.color_info,
						SafeFormat(config.GetMessage("LimitReached"), settings.max_structures));
				}

				if (settings.report_foreign_structures && statistics.foreign > 0)
				{
					SendText(player_controller, settings.color_info,
						SafeFormat(config.GetMessage("ForeignSkipped"), statistics.foreign));
				}
			}
			catch (const std::exception& error)
			{
				Log::GetLog()->error("Pickup: command failed ({})", error.what());

				bypass_context = BypassContext();
				SendKey(player_controller, Config::Get().GetSettings().color_error, "Internal");
			}
		}

		void ExecuteHelp(AShooterPlayerController* player_controller, FString* /*message*/, int /*mode*/, int /*platform*/)
		{
			SendUsage(player_controller);
		}

		void ExecuteReload(AShooterPlayerController* player_controller, FString* /*message*/, int /*mode*/, int /*platform*/)
		{
			if (Config::Get().GetSettings().reload_requires_admin
				&& (player_controller == nullptr || !player_controller->bIsAdmin().Get()))
			{
				SendKey(player_controller, Config::Get().GetSettings().color_error, "ReloadDenied");
				return;
			}

			ReloadConfig();

			SendText(player_controller, Config::Get().GetSettings().color_success,
				SafeFormat(Config::Get().GetMessage("Reloaded"),
					static_cast<int>(Config::Get().GetCategories().size()),
					static_cast<int>(Config::Get().GetSettings().commands.size())));
		}

		void ExecuteReloadRcon(RCONClientConnection* connection, RCONPacket* packet, UWorld* /*world*/)
		{
			ReloadConfig();

			FString reply = SafeFormat(Config::Get().GetMessage("Reloaded"),
				static_cast<int>(Config::Get().GetCategories().size()),
				static_cast<int>(Config::Get().GetSettings().commands.size()));

			Log::GetLog()->info("Pickup: configuration reloaded over RCON");

			if (connection != nullptr && packet != nullptr)
				connection->SendMessageW(packet->Id, 0, &reply);
		}

		/**
		 * The API matches chat commands with the game case folding, which is not reliable for Cyrillic.
		 * This callback catches the commands that were not matched directly: another case or another
		 * keyboard layout. It runs only when no other command has been executed.
		 */
		bool MatchesCommand(const FString& token, const std::vector<FString>& commands)
		{
			const FString lowered = Utils::ToLowerInvariant(token);
			const FString alternative = Utils::ToLowerInvariant(Utils::AlternativeLayout(token));

			for (const FString& command : commands)
			{
				const FString command_lowered = Utils::ToLowerInvariant(command);

				if (command_lowered == lowered)
					return true;

				if (alternative.Len() > 0 && command_lowered == alternative)
					return true;
			}

			return false;
		}

		bool ExecuteChatFallback(AShooterPlayerController* player_controller, FString* message, int mode, int platform,
			bool /*spam_check*/, bool command_executed)
		{
			if (command_executed || player_controller == nullptr || message == nullptr)
				return false;

			TArray<FString> parsed;
			message->ParseIntoArray(parsed, L" ", true);

			if (!parsed.IsValidIndex(0))
				return false;

			const FString token = parsed[0];
			if (token.Len() < 2 || token[0] != L'/')
				return false;

			try
			{
				if (MatchesCommand(token, registered_commands))
				{
					ExecutePickup(player_controller, message, mode, platform);
					return true;
				}

				if (MatchesCommand(token, registered_help_commands))
				{
					ExecuteHelp(player_controller, message, mode, platform);
					return true;
				}

				if (MatchesCommand(token, registered_reload_commands))
				{
					ExecuteReload(player_controller, message, mode, platform);
					return true;
				}
			}
			catch (const std::exception& error)
			{
				Log::GetLog()->error("Pickup: chat callback failed ({})", error.what());
				bypass_context = BypassContext();
			}

			return false;
		}

		void UnregisterCommands()
		{
			auto& commands = AsaApi::GetCommands();

			for (const FString& command : registered_commands)
				commands.RemoveChatCommand(command);

			for (const FString& command : registered_help_commands)
				commands.RemoveChatCommand(command);

			for (const FString& command : registered_reload_commands)
				commands.RemoveChatCommand(command);

			if (!registered_rcon_command.empty())
				commands.RemoveRconCommand(Utils::FromUtf8(registered_rcon_command));

			if (chat_callback_registered)
				commands.RemoveOnChatMessageCallback(FString(L"Pickup"));

			chat_callback_registered = false;

			registered_commands.clear();
			registered_help_commands.clear();
			registered_reload_commands.clear();
			registered_rcon_command.clear();
		}

		void RegisterCommands()
		{
			UnregisterCommands();

			const Config& config = Config::Get();
			const Settings settings = config.GetSettings();
			auto& commands = AsaApi::GetCommands();

			registered_commands = config.GetChatCommands(settings.commands);
			for (const FString& command : registered_commands)
				commands.AddChatCommand(command, &ExecutePickup);

			registered_help_commands = config.GetChatCommands(settings.help_commands);
			for (const FString& command : registered_help_commands)
				commands.AddChatCommand(command, &ExecuteHelp);

			registered_reload_commands = config.GetChatCommands(settings.reload_commands);
			for (const FString& command : registered_reload_commands)
				commands.AddChatCommand(command, &ExecuteReload);

			if (!settings.rcon_reload_command.empty())
			{
				registered_rcon_command = settings.rcon_reload_command;
				commands.AddRconCommand(Utils::FromUtf8(registered_rcon_command), &ExecuteReloadRcon);
			}

			commands.AddOnChatMessageCallback(FString(L"Pickup"), &ExecuteChatFallback);
			chat_callback_registered = true;
		}
	} // namespace

	void ReloadConfig()
	{
		Config::Get().Load();

		Categories::ClearCache();
		RegisterCommands();
		UpdateHooks();
	}

	void Install()
	{
		Config::Get().Load();

		Categories::ClearCache();
		RegisterCommands();
		UpdateHooks();

		Log::GetLog()->info("Pickup: loaded, {} categories, {} chat commands, cooldown {} s",
			Config::Get().GetCategories().size(), registered_commands.size(), Config::Get().GetSettings().cooldown);
	}

	void Uninstall()
	{
		UnregisterCommands();
		DisableHooks();
		Categories::ClearCache();

		cooldowns.clear();
		bypass_context = BypassContext();
	}
} // namespace Pickup
