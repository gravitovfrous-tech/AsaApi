// ============================================================================
// SpawnGuard 2.0  -  ARK: Survival Ascended server plugin (AsaApi)
//
// Blocks structure placement near:
//   * player spawn points (all spawn points of the running level),
//   * supply drops (spawn volumes + active crates + the engine's own
//     "is this point near a supply crate spawn" check),
//   * explorer notes (everything derived from ExplorerChest_Base_C: notes,
//     dossiers, Extinction glitches, Fjordur runes, HLN-A, ...),
//   * custom zones placed by admins (cheat SG.AddPoint / SG.RemovePoint).
//
// ----------------------------------------------------------------------------
// FIXES / CHANGES vs. the first version (README.md has the full list)
// ----------------------------------------------------------------------------
//  1) BLOCKING ACTUALLY WORKS NOW. The int returned by
//     APrimalStructure.IsAllowedToBuild is a *reason*: 0 = "no reason to
//     forbid the placement" (compare APrimalStructure.BPIsAllowedToBuild(
//     FPlacementData&, int CurrentAllowedReason) and
//     APrimalStructure.GetCantBuildReasonString(int ReasonVal, FString&)).
//     The old code returned 0 to block, i.e. it told the engine "everything is
//     fine". The value is now configurable: General.BlockReturnValue (default 1),
//     0 = notify only.
//  2) Command arguments: AsaApi passes the WHOLE command line to the callback
//     ("SG.AddPoint Base 15"), so the command token has to be skipped. Before
//     this fix every zone was created as "SG.AddPoint" and SG.RemovePoint could
//     never remove anything.
//  3) Every SetHook is wrapped in try/catch and its result is checked: a missing
//     offset no longer aborts the plugin load (the core throws after a 10s sleep
//     when Configs/config.json has HooksDoNotThrow:false).
//  4) Runtime texts are passed as ARGUMENTS of literal "{}" format strings, so a
//     '{' in config.json can no longer make fmt throw inside the placement hook.
//  5) Supply drop volumes use the volume bounding box (AActor.GetActorBounds)
//     plus padding instead of a radius around the volume origin.
//  6) Notifications have a per-player cooldown - the placement hook runs many
//     times per second while a structure is held.
//  7) Whitelists/blacklists match the actor name, the class chain and a
//     normalized form, so "PrimalItemStructure_SleepingBag_Hide",
//     "PrimalStructure_SleepingBag_Hide_C" and "sleepingbag" all match.
//  8) Robust start-up: hooks at plugin load, data as soon as the server is up,
//     and a watchdog timer that initializes the plugin even if the
//     AShooterGameMode.BeginPlay hook never fires (plugin hot reload).
//  9) config.json is never overwritten with an empty file when it could not be
//     parsed.
// 10) Diagnostics: the values the engine returns from IsAllowedToBuild are
//     sampled and logged once, so "0 = allowed" can be verified on the server.
// ============================================================================

#include "API/ARK/Ark.h"
#include "Logger/Logger.h"
#include "Tools.h"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "AsaApi.lib")

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace
{
	// ------------------------------------------------------------------------
	// constants
	// ------------------------------------------------------------------------

	// ARK: one foundation is 300 unreal units; the config uses foundations.
	constexpr double kFoundationUU = 300.0;

	// Hook names. They must be byte-identical to the names in the AsaApi offset
	// cache; both strings are copied verbatim from the generated headers
	// (AsaApi/Core/Public/API/ARK/PrimalStructure.h, lines 813/821). If a game
	// update changes a signature, the hook fails, the plugin logs an error and
	// keeps running with the remaining checks.
	constexpr const char* kHookIsAllowedToBuild =
		"APrimalStructure.IsAllowedToBuild(APlayerController*,UE::Math::TVector<double>,UE::Math::TRotator<double>,FPlacementData&,bool,UE::Math::TVector<double>,bool,bool,APawn*)";
	constexpr const char* kHookFinalStructurePlacement =
		"APrimalStructure.FinalStructurePlacement(APlayerController*,UE::Math::TVector<double>,UE::Math::TRotator<double>,UE::Math::TRotator<double>,APawn*,FName,bool,FPlacementData&)";
	constexpr const char* kHookGameModeBeginPlay = "AShooterGameMode.BeginPlay()";

	// Explorer notes are pure Blueprint actors (no native class in the SDK).
	// Notes, dossiers, glitches, runes and HLN-A all derive from
	// ExplorerChest_Base_C, so a single class lookup covers all of them.
	constexpr const char* kDefaultNoteClassNames[] = {
		"/game/primalearth/coreblueprints/explorerchest/explorerchest_base.explorerchest_base_c",
		"blueprint'/game/primalearth/coreblueprints/explorerchest/explorerchest_base.explorerchest_base_c'",
		"explorerchest_base_c"
	};

	constexpr const char* kDefaultNoteFilters[] = {
		"explorernote", "explorerchest", "explorer_note", "explorer_chest",
		"note_hlna", "fjordurrune"
	};

	// Explorer note scan limits.
	constexpr int kMaxNoteClassAttempts = 40;
	constexpr int kMaxNoteScans = 40;
	constexpr int kMaxVolumeScans = 8;

	// ------------------------------------------------------------------------
	// string helpers
	// ------------------------------------------------------------------------

	std::string ToLower(std::string s)
	{
		for (char& c : s)
		{
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
		}
		return s;
	}

	bool EqualsIgnoreCase(const std::string& a, const std::string& b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i)
		{
			char ca = a[i];
			char cb = b[i];
			if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
			if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
			if (ca != cb)
				return false;
		}
		return true;
	}

	bool NameMatches(const std::string& haystack_lower, const std::vector<std::string>& needles)
	{
		for (const auto& n : needles)
		{
			if (!n.empty() && haystack_lower.find(n) != std::string::npos)
				return true;
		}
		return false;
	}

	// Removes the noise words that differ between items and blueprints:
	//   "PrimalItemStructure_SleepingBag_Hide"  -> "sleepingbag_hide"
	//   "PrimalStructure_SleepingBag_Hide_C_14" -> "sleepingbag_hide_14"
	// One whitelist entry therefore works for both the item and the structure.
	std::string NormalizeName(const std::string& s)
	{
		static const std::unordered_set<std::string> kNoise = {
			"primalitemstructure", "primalstructure", "primalitem", "primal",
			"blueprint", "bp", "item", "structure", "c", "u", "a"
		};

		std::string out;
		std::string token;
		auto flush = [&]()
		{
			if (token.empty())
				return;
			if (kNoise.find(token) == kNoise.end())
			{
				if (!out.empty())
					out += '_';
				out += token;
			}
			token.clear();
		};

		for (char c : s)
		{
			if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
				token.push_back(c);
			else if (c >= 'A' && c <= 'Z')
				token.push_back(static_cast<char>(c - 'A' + 'a'));
			else
				flush();
		}
		flush();
		return out;
	}

	// ------------------------------------------------------------------------
	// geometry helpers
	// ------------------------------------------------------------------------

	FVector MakeVector(double x, double y, double z)
	{
		return FVector(x, y, z);
	}

	bool IsZeroVector(const FVector& v)
	{
		return v.X == 0.0 && v.Y == 0.0 && v.Z == 0.0;
	}

	double DistanceSq2D(const FVector& a, const FVector& b)
	{
		const double dx = a.X - b.X;
		const double dy = a.Y - b.Y;
		return dx * dx + dy * dy;
	}

	FVector ActorLocation(AActor* actor)
	{
		if (!actor)
			return MakeVector(0.0, 0.0, 0.0);

		const FVector loc = actor->GetLocation();
		if (!IsZeroVector(loc))
			return loc;

		// Actors without a root component report zero; fall back to the relative
		// location of the root as a last resort.
		if (USceneComponent* root = actor->RootComponentField().Get())
		{
			const FVector& relative = root->RelativeLocationField();
			if (!IsZeroVector(relative))
				return relative;
		}
		return loc;
	}

	struct CachedBox
	{
		FVector center = MakeVector(0.0, 0.0, 0.0);
		FVector extent = MakeVector(0.0, 0.0, 0.0);
	};

	bool PointInsideBox(const CachedBox& box, const FVector& p, double padding)
	{
		return std::fabs(p.X - box.center.X) <= box.extent.X + padding
			&& std::fabs(p.Y - box.center.Y) <= box.extent.Y + padding
			&& std::fabs(p.Z - box.center.Z) <= box.extent.Z + padding;
	}

	// ------------------------------------------------------------------------
	// configuration model
	// ------------------------------------------------------------------------

	struct Messages
	{
		bool send_notification = true;
		bool send_chat = true;
		std::string notification;
		std::string chat;
		FLinearColor color{ 1.f, 0.2f, 0.1f, 1.f };
	};

	struct ZoneRules
	{
		bool enabled = true;
		bool ignore_admins = false;
		bool block_all = true;                    // false = only the blacklist blocks
		double range_uu = 8.0 * kFoundationUU;
		std::vector<std::string> whitelist;       // lowered substrings
		std::vector<std::string> blacklist;
		std::vector<std::string> whitelist_norm;  // lowered + normalized substrings
		std::vector<std::string> blacklist_norm;
		Messages messages;
	};

	struct CustomZone
	{
		std::string name;
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
		double range_uu = 10.0 * kFoundationUU;
	};

	struct Config
	{
		bool enabled = true;
		std::string chat_sender = "SpawnGuard";
		bool log_points = true;
		bool debug_log_placements = false;
		int debug_log_lines = 8;
		int refresh_seconds = 30;
		double message_cooldown_seconds = 2.0;
		int block_return_value = 1;               // 0 = "no reason to block"
		bool enforce_on_final_placement = false;  // optional second gate

		// Overrides for the hook names (see General.HookNameOverrides). Empty =
		// take the name from the offset cache / the built-in default.
		std::string hook_name_is_allowed;
		std::string hook_name_final_placement;
		std::string hook_name_begin_play;

		ZoneRules spawn;                          // [SpawnPoints]
		ZoneRules drops;                          // [SupplyDrops]
		ZoneRules notes;                          // [ExplorerNotes]
		ZoneRules custom;                         // [CustomZones]

		bool protect_active_crates = true;
		bool protect_drop_volumes = true;
		bool use_engine_drop_check = true;
		bool use_engine_drop_check_on_preview = false;
		double drop_volume_padding_uu = 0.0;

		std::vector<std::string> note_class_names;
		std::vector<std::string> note_name_filters;
		std::vector<CustomZone> zones;
	};

	// ------------------------------------------------------------------------
	// runtime snapshot (immutable, read by the placement hooks)
	// ------------------------------------------------------------------------

	struct CachedPoint
	{
		FVector loc = MakeVector(0.0, 0.0, 0.0);
		double range_sq = 0.0;
	};

	struct RuntimeGroup
	{
		std::string id;
		ZoneRules rules;
		std::vector<CachedPoint> points;
	};

	struct RuntimeVolume
	{
		std::string id;
		ZoneRules rules;
		std::vector<CachedBox> boxes;
		double padding_uu = 0.0;
	};

	struct Runtime
	{
		bool enabled = true;
		int block_return_value = 1;
		double cooldown_seconds = 2.0;
		bool use_engine_drop_check = true;
		bool use_engine_drop_check_on_preview = false;
		std::string chat_sender = "SpawnGuard";
		ZoneRules drops_rules;                    // used by the engine drop check
		std::vector<RuntimeGroup> groups;
		std::vector<RuntimeVolume> volumes;
	};

	// ------------------------------------------------------------------------
	// globals
	// ------------------------------------------------------------------------

	Config g_config;
	nlohmann::json g_raw_config;
	std::string g_plugin_dir;
	bool g_config_loaded = false;

	// Snapshot shared with the hooks: replaced, never modified in place.
	std::mutex g_runtime_mutex;
	std::shared_ptr<const Runtime> g_runtime = std::make_shared<Runtime>();

	// Discovered data (game thread only; published through the snapshot).
	std::vector<FVector> g_spawns;
	std::vector<CachedBox> g_drop_boxes;
	std::vector<FVector> g_drop_crates;
	std::vector<FVector> g_notes;
	UClass* g_note_base_class = nullptr;
	int g_note_class_attempts = 0;
	int g_note_attempts = 0;
	bool g_notes_frozen = false;
	int g_volume_attempts = 0;
	bool g_volumes_frozen = false;

	// RCON packets are processed on the RCON thread, so a reload requested over
	// RCON must not touch config/hooks directly - it only sets this flag and the
	// game thread (OnTimer) performs the reload.
	std::atomic<bool> g_pending_reload{ false };

	// Life cycle / diagnostics (game thread only).
	bool g_initialized = false;
	bool g_timer_registered = false;
	int g_refresh_tick = 0;
	int g_debug_lines_left = 0;
	int g_sample_total = 0;
	int g_sample_counts[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	bool g_samples_logged = false;
	bool g_allow_hook_installed = false;
	bool g_final_hook_installed = false;
	bool g_begin_play_hook_installed = false;

	// Names actually handed to SetHook/DisableHook (may differ from the built-in
	// constants when the offset cache of the running game build uses another
	// parameter list).
	std::string g_hook_name_is_allowed = kHookIsAllowedToBuild;
	std::string g_hook_name_final = kHookFinalStructurePlacement;
	std::string g_hook_name_begin_play = kHookGameModeBeginPlay;
	UWorld* g_last_world = nullptr;
	std::unordered_map<const void*, std::chrono::steady_clock::time_point> g_last_notify;

	// ------------------------------------------------------------------------
	// forward declarations
	// ------------------------------------------------------------------------

	bool LoadConfigFile();
	void SaveCustomZonesLocked();
	void PublishRuntime();
	void RefreshPoints(bool force_heavy);
	void EnsureInit(const char* reason);

	// ------------------------------------------------------------------------
	// paths
	// ------------------------------------------------------------------------

	std::string PluginDir()
	{
		if (g_plugin_dir.empty())
			g_plugin_dir = AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/SpawnGuard";
		return g_plugin_dir;
	}

	// Safe to call from any thread (commands may also arrive over RCON).
	std::string SenderName()
	{
		std::lock_guard<std::mutex> lock(g_runtime_mutex);
		return g_config.chat_sender.empty() ? std::string("SpawnGuard") : g_config.chat_sender;
	}

	// ------------------------------------------------------------------------
	// config reading
	// ------------------------------------------------------------------------

	FLinearColor ReadColor(const nlohmann::json& j)
	{
		FLinearColor c{ 1.f, 0.2f, 0.1f, 1.f };
		if (!j.is_object())
			return c;
		c.R = j.value("R", c.R);
		c.G = j.value("G", c.G);
		c.B = j.value("B", c.B);
		c.A = j.value("A", c.A);
		return c;
	}

	void ReadMessages(const nlohmann::json& j, Messages& m, const std::string& fallback)
	{
		if (!j.is_object())
		{
			m.notification = fallback;
			m.chat = fallback;
			return;
		}
		m.send_notification = j.value("SendNotification", true);
		m.send_chat = j.value("SendChatMessage", true);
		m.notification = j.value("NotificationText", fallback);
		m.chat = j.value("ChatMessageText", fallback);
		if (j.contains("NotificationColor"))
			m.color = ReadColor(j["NotificationColor"]);
	}

	std::vector<std::string> ReadStringArray(const nlohmann::json& j)
	{
		std::vector<std::string> out;
		if (!j.is_array())
			return out;
		for (const auto& e : j)
		{
			if (e.is_string())
				out.push_back(ToLower(e.get<std::string>()));
		}
		return out;
	}

	void ReadZoneRules(const nlohmann::json& j, ZoneRules& z, double default_foundations,
		const char* range_key, const std::string& fallback_msg)
	{
		z.range_uu = default_foundations * kFoundationUU;
		z.messages.notification = fallback_msg;
		z.messages.chat = fallback_msg;

		if (!j.is_object())
			return;

		z.enabled = j.value("Enabled", true);
		z.ignore_admins = j.value("IgnoreAdmins", false);

		const std::string mode = j.value("BlockMode", std::string("BlockAll"));
		z.block_all = ToLower(mode) != "blockonlyblacklisted";

		z.range_uu = j.value(range_key, j.value("RangeInFoundations", default_foundations)) * kFoundationUU;
		if (z.range_uu < 0.0)
			z.range_uu = 0.0;

		z.whitelist = ReadStringArray(j.value("StructuresWhiteList", nlohmann::json::array()));
		z.blacklist = ReadStringArray(j.value("StructuresBlackList", nlohmann::json::array()));
		for (const auto& n : z.whitelist)
			z.whitelist_norm.push_back(NormalizeName(n));
		for (const auto& n : z.blacklist)
			z.blacklist_norm.push_back(NormalizeName(n));

		ReadMessages(j.value("Messages", nlohmann::json::object()), z.messages, fallback_msg);
	}

	void InstallHooks();   // defined after the hook bodies

	bool LoadConfigFile()
	{
		const std::string path = PluginDir() + "/config.json";
		std::ifstream file(path);
		if (!file.is_open())
		{
			Log::GetLog()->error("SpawnGuard: cannot open {}", path);
			return false;
		}

		nlohmann::json j;
		try
		{
			file >> j;
		}
		catch (const std::exception& e)
		{
			Log::GetLog()->error("SpawnGuard: invalid config.json: {}", e.what());
			return false;
		}

		Config cfg;
		const auto general = j.value("General", nlohmann::json::object());
		cfg.enabled = general.value("Enabled", true);
		cfg.chat_sender = general.value("ChatSenderName", std::string("SpawnGuard"));
		cfg.log_points = general.value("LogDiscoveredPoints", true);
		cfg.debug_log_placements = general.value("DebugLogPlacements", false);
		cfg.debug_log_lines = general.value("DebugLogLines", 8);
		cfg.refresh_seconds = general.value("RefreshSpawnPointsSeconds", 30);
		if (cfg.refresh_seconds < 5)
			cfg.refresh_seconds = 5;
		cfg.message_cooldown_seconds = general.value("MessageCooldownSeconds", 2.0);
		if (cfg.message_cooldown_seconds < 0.0)
			cfg.message_cooldown_seconds = 0.0;
		cfg.block_return_value = general.value("BlockReturnValue", 1);
		cfg.enforce_on_final_placement = general.value("EnforceOnFinalPlacement", false);

		const auto hook_names = general.value("HookNameOverrides", nlohmann::json::object());
		if (hook_names.is_object())
		{
			cfg.hook_name_is_allowed = hook_names.value("IsAllowedToBuild", std::string());
			cfg.hook_name_final_placement = hook_names.value("FinalStructurePlacement", std::string());
			cfg.hook_name_begin_play = hook_names.value("GameModeBeginPlay", std::string());
		}

		ReadZoneRules(j.value("SpawnPoints", nlohmann::json::object()), cfg.spawn, 8.0,
			"RangeInFoundations", "You cannot build near a player spawn point!");
		ReadZoneRules(j.value("SupplyDrops", nlohmann::json::object()), cfg.drops, 10.0,
			"RangeInFoundations", "You cannot build near a supply drop!");
		ReadZoneRules(j.value("ExplorerNotes", nlohmann::json::object()), cfg.notes, 2.0,
			"RangeInFoundations", "You cannot build near an explorer note!");
		ReadZoneRules(j.value("CustomZones", nlohmann::json::object()), cfg.custom, 10.0,
			"DefaultRangeInFoundations", "You cannot build in a restricted zone!");

		const auto drops_j = j.value("SupplyDrops", nlohmann::json::object());
		cfg.protect_active_crates = drops_j.value("ProtectActiveCrates", true);
		cfg.protect_drop_volumes = drops_j.value("ProtectDropVolumes", true);
		cfg.use_engine_drop_check = drops_j.value("UseEngineDropSpawnCheck", true);
		cfg.use_engine_drop_check_on_preview = drops_j.value("UseEngineDropSpawnCheckOnPreview", false);
		cfg.drop_volume_padding_uu = drops_j.value("VolumePaddingUU", 0.0);

		const auto notes_j = j.value("ExplorerNotes", nlohmann::json::object());
		cfg.note_name_filters = ReadStringArray(notes_j.value("NameContains", nlohmann::json::array()));
		if (cfg.note_name_filters.empty())
		{
			for (const char* f : kDefaultNoteFilters)
				cfg.note_name_filters.push_back(f);
		}
		cfg.note_class_names = ReadStringArray(notes_j.value("BaseClassNames", nlohmann::json::array()));
		if (cfg.note_class_names.empty())
		{
			for (const char* n : kDefaultNoteClassNames)
				cfg.note_class_names.push_back(n);
		}

		const auto custom = j.value("CustomZones", nlohmann::json::object());
		for (const auto& z : custom.value("Zones", nlohmann::json::array()))
		{
			if (!z.is_object())
				continue;
			CustomZone cz;
			cz.name = z.value("Name", std::string("Unnamed"));
			if (cz.name.empty() || cz.name.size() > 48)
				continue;
			cz.x = z.value("X", 0.0);
			cz.y = z.value("Y", 0.0);
			cz.z = z.value("Z", 0.0);
			const double foundations = z.value("RangeInFoundations", cfg.custom.range_uu / kFoundationUU);
			cz.range_uu = foundations > 0.0 ? foundations * kFoundationUU : cfg.custom.range_uu;
			cfg.zones.push_back(std::move(cz));
		}

		size_t note_classes = cfg.note_class_names.size();
		size_t zones = cfg.zones.size();
		{
			std::lock_guard<std::mutex> lock(g_runtime_mutex);
			g_config = std::move(cfg);
			g_raw_config = std::move(j);
			g_config_loaded = true;

			// Force a full rescan with the new settings.
			g_notes_frozen = false;
			g_volumes_frozen = false;
			g_note_attempts = 0;
			g_volume_attempts = 0;
			g_note_class_attempts = 0;
			g_note_base_class = nullptr;

			PublishRuntime();
		}

		g_debug_lines_left = g_config.debug_log_placements ? g_config.debug_log_lines : 0;

		// (Re)install the hooks - also handles enabling/disabling the optional gate.
		InstallHooks();

		Log::GetLog()->info("SpawnGuard: config loaded ({} custom zones, {} note class names)",
			zones, note_classes);
		return true;
	}

	void SaveCustomZonesLocked()
	{
		if (!g_config_loaded)
		{
			// Never overwrite a config file we could not parse.
			Log::GetLog()->error("SpawnGuard: refusing to save zones - config.json was not loaded");
			return;
		}

		if (!g_raw_config.contains("CustomZones") || !g_raw_config["CustomZones"].is_object())
			g_raw_config["CustomZones"] = nlohmann::json::object();

		nlohmann::json arr = nlohmann::json::array();
		for (const auto& z : g_config.zones)
		{
			arr.push_back({
				{ "Name", z.name },
				{ "X", z.x },
				{ "Y", z.y },
				{ "Z", z.z },
				{ "RangeInFoundations", z.range_uu / kFoundationUU }
			});
		}
		g_raw_config["CustomZones"]["Zones"] = std::move(arr);

		const std::string path = PluginDir() + "/config.json";
		std::ofstream out(path);
		if (!out.is_open())
		{
			Log::GetLog()->error("SpawnGuard: cannot write {}", path);
			return;
		}
		out << g_raw_config.dump(4);
	}

	// ------------------------------------------------------------------------
	// runtime snapshot
	// ------------------------------------------------------------------------

	std::shared_ptr<const Runtime> GetRuntime()
	{
		std::lock_guard<std::mutex> lock(g_runtime_mutex);
		return g_runtime;
	}

	void InsertPoint(std::vector<CachedPoint>& dst, const FVector& loc, double range_uu)
	{
		CachedPoint p;
		p.loc = loc;
		p.range_sq = range_uu * range_uu;
		dst.push_back(p);
	}

	// NOTE: the caller must hold g_runtime_mutex.
	void PublishRuntime()
	{
		auto rt = std::make_shared<Runtime>();
		rt->enabled = g_config.enabled;
		rt->block_return_value = g_config.block_return_value;
		rt->cooldown_seconds = g_config.message_cooldown_seconds;
		rt->use_engine_drop_check = g_config.use_engine_drop_check && g_config.drops.enabled;
		rt->use_engine_drop_check_on_preview = g_config.use_engine_drop_check_on_preview;
		// NOTE: do not call SenderName() here - this function already holds the
		// mutex, and std::mutex is not recursive.
		rt->chat_sender = g_config.chat_sender.empty() ? std::string("SpawnGuard") : g_config.chat_sender;
		rt->drops_rules = g_config.drops;

		if (g_config.spawn.enabled && !g_spawns.empty())
		{
			RuntimeGroup g;
			g.id = "a player spawn point";
			g.rules = g_config.spawn;
			for (const auto& s : g_spawns)
				InsertPoint(g.points, s, g_config.spawn.range_uu);
			rt->groups.push_back(std::move(g));
		}

		if (g_config.drops.enabled && g_config.protect_active_crates && !g_drop_crates.empty())
		{
			RuntimeGroup g;
			g.id = "a supply crate";
			g.rules = g_config.drops;
			for (const auto& s : g_drop_crates)
				InsertPoint(g.points, s, g_config.drops.range_uu);
			rt->groups.push_back(std::move(g));
		}

		if (g_config.notes.enabled && !g_notes.empty())
		{
			RuntimeGroup g;
			g.id = "an explorer note";
			g.rules = g_config.notes;
			for (const auto& s : g_notes)
				InsertPoint(g.points, s, g_config.notes.range_uu);
			rt->groups.push_back(std::move(g));
		}

		if (g_config.custom.enabled && !g_config.zones.empty())
		{
			RuntimeGroup g;
			g.id = "a custom zone";
			g.rules = g_config.custom;
			for (const auto& z : g_config.zones)
				InsertPoint(g.points, MakeVector(z.x, z.y, z.z), z.range_uu);
			rt->groups.push_back(std::move(g));
		}

		if (g_config.drops.enabled && g_config.protect_drop_volumes && !g_drop_boxes.empty())
		{
			RuntimeVolume v;
			v.id = "a supply drop landing zone";
			v.rules = g_config.drops;
			v.boxes = g_drop_boxes;
			v.padding_uu = g_config.drop_volume_padding_uu;
			rt->volumes.push_back(std::move(v));
		}

		g_runtime = std::move(rt);
	}

	// ------------------------------------------------------------------------
	// SEH guarded engine calls
	//
	// __try must live in a leaf function (MSVC C2712) and output arrays are heap
	// allocated by the caller: if the engine call faults we deliberately leak the
	// array instead of running its destructor on corrupted data.
	// ------------------------------------------------------------------------

	bool GetAllActorsSafe(UWorld* world, UClass* cls, TArray<AActor*, TSizedDefaultAllocator<32>>* out)
	{
		__try
		{
			UGameplayStatics::GetAllActorsOfClass(world, TSubclassOf<AActor>(cls), out);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool GetActorBoundsSafe(AActor* actor, FVector* origin, FVector* extent)
	{
		__try
		{
			actor->GetActorBounds(false, origin, extent, true);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool GetAllPlayerStartsSafe(UWorld* world, TArray<AActor*, TSizedDefaultAllocator<32>>* out)
	{
		__try
		{
			UVictoryCore::GetAllPlayerStarts(out, world);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool IsPointNearSupplyCrateSpawnSafe(UWorld* world, UE::Math::TVector<double>* point)
	{
		__try
		{
			return APrimalStructure::IsPointNearSupplyCrateSpawn(world, point) != 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	UClass* SafeBPLoadClassRaw(const FString* path)
	{
		__try
		{
			return UVictoryCore::BPLoadClass(*path);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	// ------------------------------------------------------------------------
	// class helpers
	// ------------------------------------------------------------------------

	// "aactor > primalstructure > explorerchest_base_c > bp_note_helena_c"
	std::string ClassChainKey(UStruct* cls)
	{
		std::string key;
		int depth = 0;
		for (UStruct* s = cls; s && depth < 16; s = s->SuperStructField(), ++depth)
		{
			if (!key.empty())
				key += " > ";
			key += ToLower(s->NamePrivateField().ToString().ToString());
		}
		return key;
	}

	// "Blueprint'/Game/.../ExplorerChest_Base.ExplorerChest_Base_C'" -> "explorerchest_base_c"
	std::string ClassShortName(const std::string& path)
	{
		std::string s = ToLower(path);
		const auto q = s.rfind('\'');
		if (q != std::string::npos)
			s = s.substr(0, q);
		const auto dot = s.rfind('.');
		if (dot != std::string::npos)
			s = s.substr(dot + 1);
		return s;
	}

	UClass* FindClassInChain(UClass* cls, const std::vector<std::string>& short_names)
	{
		if (short_names.empty())
			return nullptr;
		int depth = 0;
		for (UStruct* s = cls; s && depth < 16; s = s->SuperStructField(), ++depth)
		{
			const std::string name = ToLower(s->NamePrivateField().ToString().ToString());
			for (const auto& n : short_names)
			{
				if (n == name)
					return static_cast<UClass*>(s);
			}
		}
		return nullptr;
	}

	UClass* ResolveClassByNames(const std::vector<std::string>& names)
	{
		for (const auto& name : names)
		{
			if (name.empty())
				continue;
			FString path(name);
			if (UClass* cls = SafeBPLoadClassRaw(&path))
				return cls;
		}
		return nullptr;
	}

	std::vector<std::string> NoteBaseShortNames(const std::vector<std::string>& class_names)
	{
		std::vector<std::string> out;
		for (const auto& n : class_names)
		{
			std::string sn = ClassShortName(n);
			if (!sn.empty())
				out.push_back(std::move(sn));
		}
		if (out.empty())
		{
			for (const char* n : kDefaultNoteClassNames)
				out.push_back(ClassShortName(n));
		}
		return out;
	}

	// ------------------------------------------------------------------------
	// scanning
	// ------------------------------------------------------------------------

	std::vector<FVector> CollectSpawnPoints(UWorld* world)
	{
		std::vector<FVector> out;

		auto* starts = new TArray<AActor*, TSizedDefaultAllocator<32>>();
		if (GetAllPlayerStartsSafe(world, starts))
		{
			out.reserve(starts->Num());
			for (int i = 0; i < starts->Num(); ++i)
			{
				if (AActor* actor = (*starts)[i])
					out.push_back(ActorLocation(actor));
			}
			delete starts;
		}
		// else: the engine call faulted - the (possibly corrupted) array is leaked.

		if (out.empty())
		{
			// Fallback: the game mode keeps its own list. APlayerStart is only
			// forward declared in the SDK, hence the reinterpret_cast.
			auto* gm = static_cast<APrimalGameMode*>(AsaApi::GetApiUtils().GetShooterGameMode());
			if (gm)
			{
				auto& arr = gm->PlayerStartsField();
				for (int i = 0; i < arr.Num(); ++i)
				{
					if (AActor* actor = reinterpret_cast<AActor*>(arr[i]))
						out.push_back(ActorLocation(actor));
				}
			}
		}

		return out;
	}

	std::vector<FVector> CollectActorLocations(UWorld* world, UClass* cls)
	{
		std::vector<FVector> out;
		if (!world || !cls)
			return out;

		auto* arr = new TArray<AActor*, TSizedDefaultAllocator<32>>();
		if (GetAllActorsSafe(world, cls, arr))
		{
			out.reserve(arr->Num());
			for (int i = 0; i < arr->Num(); ++i)
			{
				if (AActor* actor = (*arr)[i])
					out.push_back(ActorLocation(actor));
			}
			delete arr;
		}
		return out;
	}

	std::vector<CachedBox> CollectDropVolumeBoxes(UWorld* world)
	{
		std::vector<CachedBox> out;
		if (!world)
			return out;

		auto* arr = new TArray<AActor*, TSizedDefaultAllocator<32>>();
		if (!GetAllActorsSafe(world, ASupplyCrateSpawningVolume::StaticClass(), arr))
			return out;

		out.reserve(arr->Num());
		for (int i = 0; i < arr->Num(); ++i)
		{
			AActor* actor = (*arr)[i];
			if (!actor)
				continue;

			FVector origin, extent;
			if (!GetActorBoundsSafe(actor, &origin, &extent))
			{
				origin = ActorLocation(actor);
				extent = MakeVector(0.0, 0.0, 0.0);
			}
			if (IsZeroVector(extent) && IsZeroVector(origin))
				continue;

			CachedBox box;
			box.center = origin;
			box.extent = extent;
			out.push_back(box);
		}
		delete arr;
		return out;
	}

	// Explorer note discovery:
	//   1) exact scan by the resolved ExplorerChest_Base_C class (engine IsA -
	//      catches notes, dossiers, glitches, runes and HLN-A),
	//   2) fallback broad scan over all world actors matching the full class
	//      chain against ExplorerNotes.NameContains (custom maps).
	// While scanning, the ExplorerChest base UClass is captured from a matched
	// actor if the direct class lookup failed.
	void CollectNoteActors(UWorld* world, UClass* base_class, bool broad_scan,
		const std::vector<std::string>& filters, const std::vector<std::string>& base_short_names,
		bool log_points, std::vector<FVector>& out_notes, UClass** out_base_class)
	{
		std::unordered_set<AActor*> seen;
		std::vector<AActor*> found;

		if (base_class)
		{
			auto* arr = new TArray<AActor*, TSizedDefaultAllocator<32>>();
			if (GetAllActorsSafe(world, base_class, arr))
			{
				for (int i = 0; i < arr->Num(); ++i)
				{
					if (AActor* a = (*arr)[i])
					{
						if (seen.insert(a).second)
							found.push_back(a);
					}
				}
				delete arr;
			}
			if (log_points)
				Log::GetLog()->info("SpawnGuard: note scan by ExplorerChest class: {} actors", found.size());
		}

		if (broad_scan)
		{
			static const char* kCandidateWords[] = {
				"note", "dossier", "explorer", "hlna", "rune", "glitch", "chest"
			};
			const std::vector<std::string> candidate_words(
				kCandidateWords, kCandidateWords + sizeof(kCandidateWords) / sizeof(kCandidateWords[0]));

			auto* arr = new TArray<AActor*, TSizedDefaultAllocator<32>>();
			if (GetAllActorsSafe(world, AActor::GetPrivateStaticClass(), arr))
			{
				std::unordered_map<UClass*, bool> verdict;      // class -> is a note?
				std::unordered_map<UClass*, std::string> chain; // class -> chain key
				std::unordered_set<std::string> logged_candidates;
				int total = 0;

				for (int i = 0; i < arr->Num(); ++i)
				{
					AActor* a = (*arr)[i];
					if (!a)
						continue;
					++total;

					UClass* cls = a->ClassPrivateField();
					if (!cls)
						continue;

					std::string chain_key;
					if (auto it = chain.find(cls); it != chain.end())
						chain_key = it->second;
					else
					{
						chain_key = ClassChainKey(cls);
						chain.emplace(cls, chain_key);
					}

					bool is_note;
					if (auto it = verdict.find(cls); it != verdict.end())
						is_note = it->second;
					else
					{
						is_note = NameMatches(chain_key, filters);
						verdict.emplace(cls, is_note);
					}

					if (is_note)
					{
						if (seen.insert(a).second)
							found.push_back(a);
						if (*out_base_class == nullptr)
						{
							if (UClass* base = FindClassInChain(cls, base_short_names))
							{
								*out_base_class = base;
								Log::GetLog()->info("SpawnGuard: note base class captured from a world actor");
							}
						}
					}
					else if (log_points && logged_candidates.size() < 20
						&& NameMatches(chain_key, candidate_words))
					{
						logged_candidates.insert(chain_key);
					}
				}

				if (log_points)
				{
					Log::GetLog()->info("SpawnGuard: broad note scan: {} actors, {} matched filters",
						total, found.size());
					for (const auto& c : logged_candidates)
						Log::GetLog()->info("SpawnGuard: note-candidate class: {}", c);
				}
				delete arr;
			}
		}

		out_notes.reserve(out_notes.size() + found.size());
		for (AActor* a : found)
			out_notes.push_back(ActorLocation(a));
	}

	void RefreshPoints(bool force_heavy)
	{
		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (!world)
			return;

		bool want_drops = false;
		bool protect_crates = true;
		bool protect_volumes = true;
		bool scan_volumes = false;
		bool scan_notes = false;
		bool log_points = false;
		std::vector<std::string> note_filters;
		std::vector<std::string> note_class_names;
		{
			std::lock_guard<std::mutex> lock(g_runtime_mutex);
			want_drops = g_config.drops.enabled;
			protect_crates = g_config.protect_active_crates;
			protect_volumes = g_config.protect_drop_volumes;
			log_points = g_config.log_points;
			scan_volumes = want_drops && protect_volumes && (force_heavy || !g_volumes_frozen);
			scan_notes = g_config.notes.enabled && (force_heavy || !g_notes_frozen);
			note_filters = g_config.note_name_filters;
			note_class_names = g_config.note_class_names;
		}

		std::vector<FVector> spawns = CollectSpawnPoints(world);

		std::vector<CachedBox> boxes;
		if (scan_volumes)
			boxes = CollectDropVolumeBoxes(world);

		std::vector<FVector> crates;
		if (want_drops && protect_crates)
			crates = CollectActorLocations(world, APrimalStructureItemContainer_SupplyCrate::GetPrivateStaticClass());

		std::vector<FVector> notes;
		if (scan_notes)
		{
			if (!g_note_base_class && g_note_class_attempts < kMaxNoteClassAttempts)
			{
				++g_note_class_attempts;
				g_note_base_class = ResolveClassByNames(note_class_names);
				Log::GetLog()->info("SpawnGuard: explorer note base class {}",
					g_note_base_class ? "resolved" : "not found (will retry)");
			}

			CollectNoteActors(world, g_note_base_class, true, note_filters,
				NoteBaseShortNames(note_class_names), log_points, notes, &g_note_base_class);
		}

		size_t n_spawns = 0, n_boxes = 0, n_crates = 0, n_notes = 0;
		bool notes_frozen = false;
		{
			std::lock_guard<std::mutex> lock(g_runtime_mutex);
			g_spawns = std::move(spawns);

			if (scan_volumes)
			{
				g_drop_boxes = std::move(boxes);
				++g_volume_attempts;
				if (!g_drop_boxes.empty() || g_volume_attempts >= kMaxVolumeScans)
					g_volumes_frozen = true;
			}
			if (scan_notes)
			{
				g_notes = std::move(notes);
				++g_note_attempts;
				// Notes are static: freeze as soon as they were found, or after a
				// few empty scans once the class is known (or after the budget).
				if (!g_notes.empty()
					|| (g_note_base_class && g_note_attempts >= 8)
					|| g_note_attempts >= kMaxNoteScans)
				{
					g_notes_frozen = true;
				}
			}

			if (want_drops && protect_crates)
				g_drop_crates = std::move(crates);
			else
				g_drop_crates.clear();

			PublishRuntime();

			n_spawns = g_spawns.size();
			n_boxes = g_drop_boxes.size();
			n_crates = g_drop_crates.size();
			n_notes = g_notes.size();
			notes_frozen = g_notes_frozen;
		}

		if (log_points && force_heavy)
		{
			Log::GetLog()->info("SpawnGuard: scan - {} spawn points, {} drop volume boxes, {} crates, {} notes (frozen={})",
				n_spawns, n_boxes, n_crates, n_notes, notes_frozen ? 1 : 0);

			std::vector<FVector> sample;
			{
				std::lock_guard<std::mutex> lock(g_runtime_mutex);
				for (size_t i = 0; i < g_spawns.size() && i < 3; ++i)
					sample.push_back(g_spawns[i]);
				for (size_t i = 0; i < g_notes.size() && i < 3; ++i)
					sample.push_back(g_notes[i]);
			}
			for (const auto& p : sample)
				Log::GetLog()->info("SpawnGuard: point {:.0f} {:.0f} {:.0f}", p.X, p.Y, p.Z);
		}
	}

	// ------------------------------------------------------------------------
	// rules
	// ------------------------------------------------------------------------

	// Actor name + whole class chain (lowered).
	std::string StructureName(APrimalStructure* structure)
	{
		if (!structure)
			return {};

		std::string name = ToLower(structure->NamePrivateField().ToString().ToString());
		if (UClass* cls = structure->ClassPrivateField())
		{
			name += " ";
			name += ClassChainKey(cls);
		}
		return name;
	}

	bool IsAdmin(APlayerController* pc)
	{
		auto* spc = static_cast<AShooterPlayerController*>(pc);
		return spc && spc->bIsAdmin()();
	}

	bool RulesNeedStructureName(const ZoneRules& r)
	{
		if (!r.whitelist.empty() || !r.whitelist_norm.empty())
			return true;
		if (!r.block_all && (!r.blacklist.empty() || !r.blacklist_norm.empty()))
			return true;
		return false;
	}

	bool StructureBlockedByRules(const ZoneRules& r, const std::string& name, const std::string& normalized)
	{
		if (NameMatches(name, r.whitelist) || NameMatches(normalized, r.whitelist_norm))
			return false;               // whitelisted
		if (r.block_all)
			return true;                // everything else is forbidden
		return NameMatches(name, r.blacklist) || NameMatches(normalized, r.blacklist_norm);
	}

	struct Verdict
	{
		bool blocked = false;
		std::string zone;
		double distance_foundations = 0.0;
		Messages messages;
	};

	// ------------------------------------------------------------------------
	// messaging
	// ------------------------------------------------------------------------

	void PruneNotifications()
	{
		if (g_last_notify.size() < 128)
			return;
		const auto now = std::chrono::steady_clock::now();
		for (auto it = g_last_notify.begin(); it != g_last_notify.end();)
		{
			if (now - it->second > std::chrono::seconds(60))
				it = g_last_notify.erase(it);
			else
				++it;
		}
	}

	void NotifyBlocked(APlayerController* pc, const Runtime& rt, const Verdict& verdict)
	{
		auto* spc = static_cast<AShooterPlayerController*>(pc);
		if (!spc)
			return;

		const auto now = std::chrono::steady_clock::now();
		auto it = g_last_notify.find(pc);
		if (it != g_last_notify.end())
		{
			const double elapsed = std::chrono::duration<double>(now - it->second).count();
			if (elapsed < rt.cooldown_seconds)
				return;
		}
		g_last_notify[pc] = now;
		PruneNotifications();

		// Runtime texts are always passed as ARGUMENTS of a literal format string:
		// no brace escaping needed, no fmt-version pitfalls.
		if (verdict.messages.send_notification)
		{
			AsaApi::GetApiUtils().SendNotification(spc, verdict.messages.color, 1.2f, 6.f, nullptr,
				"{}", verdict.messages.notification);
		}
		if (verdict.messages.send_chat)
		{
			AsaApi::GetApiUtils().SendChatMessage(spc, FString(rt.chat_sender), "{}", verdict.messages.chat);
		}
	}

	// ------------------------------------------------------------------------
	// the placement check itself
	// ------------------------------------------------------------------------

	FVector PlacementLocation(const UE::Math::TVector<double>& requested, FPlacementData* placement_data)
	{
		if (placement_data)
		{
			const auto& adjusted = placement_data->AdjustedLocationField();
			if (!IsZeroVector(adjusted))
				return MakeVector(adjusted.X, adjusted.Y, adjusted.Z);
		}
		return MakeVector(requested.X, requested.Y, requested.Z);
	}

	Verdict EvaluatePlacement(const FVector& loc, APlayerController* pc, APrimalStructure* structure,
		bool final_placement)
	{
		Verdict verdict;

		const auto rt = GetRuntime();
		if (!rt || !rt->enabled)
			return verdict;

		// bIsAdmin() is a native bit field read - only touch it if a rule needs it.
		bool admin = false;
		bool admin_ready = false;
		auto is_admin = [&]()
		{
			if (!admin_ready)
			{
				admin = IsAdmin(pc);
				admin_ready = true;
			}
			return admin;
		};

		std::string name;
		std::string normalized;
		bool name_ready = false;
		auto ensure_name = [&]()
		{
			if (!name_ready)
			{
				name = StructureName(structure);
				normalized = NormalizeName(name);
				name_ready = true;
			}
		};

		// 1) cached points: spawn points, crates, notes, custom zones
		for (const auto& group : rt->groups)
		{
			const ZoneRules& rules = group.rules;
			if (!rules.enabled)
				continue;
			if (rules.ignore_admins && is_admin())
				continue;

			const bool need_name = RulesNeedStructureName(rules);
			if (need_name)
				ensure_name();

			for (const auto& p : group.points)
			{
				const double dist_sq = DistanceSq2D(loc, p.loc);
				if (dist_sq > p.range_sq)
					continue;
				if (need_name && !StructureBlockedByRules(rules, name, normalized))
					continue;   // this structure is whitelisted here

				verdict.blocked = true;
				verdict.zone = group.id;
				verdict.messages = rules.messages;
				verdict.distance_foundations = std::sqrt(dist_sq) / kFoundationUU;
				return verdict;
			}
		}

		// 2) supply drop landing volumes (bounds of ASupplyCrateSpawningVolume)
		for (const auto& volume : rt->volumes)
		{
			const ZoneRules& rules = volume.rules;
			if (!rules.enabled)
				continue;
			if (rules.ignore_admins && is_admin())
				continue;

			const bool need_name = RulesNeedStructureName(rules);
			if (need_name)
				ensure_name();

			for (const auto& box : volume.boxes)
			{
				if (!PointInsideBox(box, loc, volume.padding_uu))
					continue;
				if (need_name && !StructureBlockedByRules(rules, name, normalized))
					continue;

				verdict.blocked = true;
				verdict.zone = volume.id;
				verdict.messages = rules.messages;
				return verdict;
			}
		}

		// 3) the engine's own check ("is this point near a supply crate spawn?")
		if (rt->use_engine_drop_check && (final_placement || rt->use_engine_drop_check_on_preview))
		{
			UWorld* world = AsaApi::GetApiUtils().GetWorld();
			if (world)
			{
				UE::Math::TVector<double> probe(loc.X, loc.Y, loc.Z);
				if (IsPointNearSupplyCrateSpawnSafe(world, &probe))
				{
					const ZoneRules& rules = rt->drops_rules;
					if (!(rules.ignore_admins && is_admin()))
					{
						const bool need_name = RulesNeedStructureName(rules);
						if (need_name)
							ensure_name();
						if (!need_name || StructureBlockedByRules(rules, name, normalized))
						{
							verdict.blocked = true;
							verdict.zone = "a supply crate spawn point";
							verdict.messages = rules.messages;
							return verdict;
						}
					}
				}
			}
		}

		return verdict;
	}

	// ------------------------------------------------------------------------
	// offset cache access
	//
	// AsaApi keeps the authoritative list of hookable names for the exact game
	// build in "ArkApi/Cache/.../cached_offsets.cache". The file is a binary
	// serialized unordered_map<string, intptr_t>:
	//
	//     repeat: [uint64 key_size][key bytes][int64 value]
	//
	// (see Cache::validateSerializedMap in AsaApi/Core/Private/Cache.h)
	//
	// Why the plugin reads it: the parameter list of a hook name depends on the
	// game build ("FPlacementData&" vs "FPlacementData*", an extra APawn*, ...).
	// Handing a name that is not in this file to SetHook/GetAddress leads to
	// "Failed to get the offset of ...", and with Configs/config.json
	// HooksDoNotThrow:false the core then does Sleep(10000) and a throwing path.
	// So the name is verified against the cache first and, if it does not match,
	// the hook is simply not installed - the server stays alive and the log says
	// exactly which names the cache does contain.
	// ------------------------------------------------------------------------

	using fsys = std::filesystem;

	bool ScanOffsetCacheForKeys(const std::string& path, const std::string& prefix,
		std::vector<std::string>& matches)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in.is_open())
			return false;

		while (in.good())
		{
			std::uint64_t key_size = 0;
			in.read(reinterpret_cast<char*>(&key_size), sizeof(key_size));
			if (!in || key_size == 0 || key_size > (1ULL << 20))
				break;

			std::string key(static_cast<size_t>(key_size), '\0');
			in.read(key.data(), static_cast<std::streamsize>(key_size));
			if (!in)
				break;

			std::int64_t value = 0;
			in.read(reinterpret_cast<char*>(&value), sizeof(value));
			if (!in)
				break;

			if (key.size() >= prefix.size() && key.compare(0, prefix.size(), prefix) == 0)
				matches.push_back(std::move(key));
		}
		return !matches.empty();
	}

	int CountOffsetCacheKeysWithPrefix(const std::string& path, const std::string& prefix)
	{
		std::vector<std::string> matches;
		ScanOffsetCacheForKeys(path, prefix, matches);
		return static_cast<int>(matches.size());
	}

	std::string FindOffsetCacheFile()
	{
		std::error_code ec;
		const fsys::path cwd(AsaApi::Tools::GetCurrentDir());
		const fsys::path cache_root = cwd / "ArkApi" / "Cache";

		std::string best;
		fsys::file_time_type best_time{};
		bool have = false;

		auto consider = [&](const fsys::path& p)
		{
			std::error_code e;
			if (!fsys::is_regular_file(p, e) || e)
				return;
			const auto t = fsys::last_write_time(p, e);
			if (!have || e || t > best_time)
			{
				have = true;
				best = p.string();
				best_time = e ? fsys::file_time_type{} : t;
			}
		};

		consider(cache_root / "cached_offsets.cache");
		consider(cwd / "ArkApi" / "cached_offsets.cache");
		consider(cwd / "cached_offsets.cache");

		if (fsys::is_directory(cache_root, ec))
		{
			fsys::recursive_directory_iterator it(
				cache_root, fsys::directory_options::skip_permission_denied, ec);
			const fsys::recursive_directory_iterator end;
			for (; !ec && it != end; it.increment(ec))
			{
				std::error_code e;
				if (it.depth() > 4)
				{
					it.disable_recursion_pending();
					continue;
				}
				const auto& p = it->path();
				if (p.filename() == "cached_offsets.cache" && it->is_regular_file(e))
					consider(p);
			}
		}

		return have ? best : std::string();
	}

	std::string OffsetCachePath()
	{
		// Not cached on purpose: ArkApi may download a fresh cache after the
		// plugin was loaded, and the path contains a generation folder.
		return FindOffsetCacheFile();
	}

	struct HookName
	{
		std::string name;                     // empty = do not install the hook
		bool in_cache = false;                // the name is present in the offset cache
		std::string cache_path;
		std::vector<std::string> variants;    // what the cache has for this method
	};

	// Picks the hook name: config override > the exact expected name from the
	// offset cache > the built-in default (only when there is no cache at all).
	HookName ResolveHookName(const char* method_prefix, const std::string& expected,
		const std::string& override_name)
	{
		HookName result;
		result.cache_path = OffsetCachePath();

		if (!result.cache_path.empty())
			ScanOffsetCacheForKeys(result.cache_path, method_prefix, result.variants);

		if (!override_name.empty())
		{
			result.name = override_name;
			result.in_cache = std::find(result.variants.begin(), result.variants.end(), override_name)
				!= result.variants.end();
			if (!result.in_cache)
			{
				Log::GetLog()->warn("SpawnGuard: hook name override for '{}' is not present in the offset "
					"cache - using it as forced. If the parameter list does not match the compiled hook, "
					"the server will crash.", method_prefix);
			}
			return result;
		}

		for (const auto& v : result.variants)
		{
			if (v == expected)
			{
				result.name = expected;
				result.in_cache = true;
				return result;
			}
		}

		if (!result.cache_path.empty())
			result.name.clear();          // cache exists: never guess, see the section comment
		else
			result.name = expected;       // no cache to check against - try as before

		return result;
	}

	void LogHookNameProblem(const char* what, const std::string& expected, const HookName& info)
	{
		if (!info.variants.empty())
		{
			Log::GetLog()->error("SpawnGuard: '{}' has a different signature in the offset cache.", what);
			for (const auto& v : info.variants)
				Log::GetLog()->error("SpawnGuard:   cache:    {}", v);
			Log::GetLog()->error("SpawnGuard:   expected: {}", expected);
			Log::GetLog()->error("SpawnGuard: the hook was NOT installed (avoids Sleep(10000) + a throwing "
				"path in the core). Paste the correct name into General.HookNameOverrides in config.json: "
				"the parameter list must match the DECLARE_HOOK in Main.cpp, otherwise the server will crash.");
		}
		else if (!info.cache_path.empty())
		{
			const int class_keys = CountOffsetCacheKeysWithPrefix(info.cache_path, "APrimalStructure.");
			Log::GetLog()->error("SpawnGuard: '{}' is not in the offset cache ({}); the cache holds {} "
				"'APrimalStructure.*' names. The function was probably renamed or removed by a game update, "
				"or ArkApi could not download the cache for this game build - delete ArkApi\\Cache and "
				"restart the server to force a fresh download.", what, info.cache_path, class_keys);
		}
		else
		{
			Log::GetLog()->warn("SpawnGuard: offset cache not found, '{}' was tried as-is.", what);
		}
	}

	// ------------------------------------------------------------------------
	// commands
	// ------------------------------------------------------------------------

	std::string SanitizeZoneName(std::string s)
	{
		for (char& c : s)
		{
			if (c == '"' || c == '\'' || c == '{' || c == '}' || c == '\\')
				c = '_';
		}
		if (s.size() > 48)
			s.resize(48);
		return s;
	}

	// AsaApi hands the WHOLE command line to the callback ("SG.AddPoint Base 15"),
	// so the command token has to be dropped. Quoted arguments are supported.
	std::vector<std::string> ParseArgs(const FString& params, const char* command_name)
	{
		std::vector<std::string> tokens;
		const std::string s = params.ToString();
		std::string cur;
		bool in_quotes = false;

		for (char c : s)
		{
			if (c == '"')
			{
				in_quotes = !in_quotes;
				continue;
			}
			if (!in_quotes && (c == ' ' || c == '\t'))
			{
				if (!cur.empty())
				{
					tokens.push_back(cur);
					cur.clear();
				}
				continue;
			}
			cur.push_back(c);
		}
		if (!cur.empty())
			tokens.push_back(cur);

		if (!tokens.empty() && command_name && *command_name
			&& EqualsIgnoreCase(tokens[0], command_name))
		{
			tokens.erase(tokens.begin());
		}
		return tokens;
	}

	std::string IntToStr(double value)
	{
		return std::to_string(static_cast<long long>(value));
	}

	std::string FoundationsToStr(double range_uu)
	{
		return IntToStr(range_uu / kFoundationUU);
	}

	void Reply(APlayerController* pc, const std::string& text)
	{
		auto* spc = static_cast<AShooterPlayerController*>(pc);
		if (spc)
			AsaApi::GetApiUtils().SendChatMessage(spc, FString(SenderName()), "{}", text);
		Log::GetLog()->info("SpawnGuard: {}", text);
	}

	void CmdReload(APlayerController* pc, FString*, bool)
	{
		if (LoadConfigFile())
		{
			RefreshPoints(true);
			Reply(pc, "Config reloaded, points rescanned.");
		}
		else
		{
			Reply(pc, "Config reload FAILED - see ArkApi.log.");
		}
	}

	void RconReload(RCONClientConnection* conn, RCONPacket* packet, UWorld*)
	{
		// This callback runs on the RCON thread: never touch the config, the
		// hooks or the world from here. The game thread picks the request up in
		// OnTimer() within a second.
		if (!conn || !packet)
			return;

		g_pending_reload = true;

		FString reply = FString("SpawnGuard: reload queued (applied within a second)");
		conn->SendMessageW(packet->Id, 0, &reply);
	}

	void CmdRescan(APlayerController* pc, FString*, bool)
	{
		{
			std::lock_guard<std::mutex> lock(g_runtime_mutex);
			g_volumes_frozen = false;
			g_notes_frozen = false;
			g_volume_attempts = 0;
			g_note_attempts = 0;
			g_note_class_attempts = 0;
		}
		RefreshPoints(true);

		size_t spawns = 0, boxes = 0, crates = 0, notes = 0;
		bool note_class = false;
		{
			std::lock_guard<std::mutex> lock(g_runtime_mutex);
			spawns = g_spawns.size();
			boxes = g_drop_boxes.size();
			crates = g_drop_crates.size();
			notes = g_notes.size();
			note_class = g_note_base_class != nullptr;
		}

		Reply(pc, "Rescan: spawns=" + std::to_string(spawns)
			+ " volumes=" + std::to_string(boxes)
			+ " crates=" + std::to_string(crates)
			+ " notes=" + std::to_string(notes)
			+ (note_class ? "" : " (note class NOT resolved)"));
	}

	void CmdList(APlayerController* pc, FString*, bool)
	{
		size_t spawns = 0, boxes = 0, crates = 0, notes = 0;
		bool note_class = false;
		int block_value = 1;
		std::vector<CustomZone> copy;
		{
			std::lock_guard<std::mutex> lock(g_runtime_mutex);
			spawns = g_spawns.size();
			boxes = g_drop_boxes.size();
			crates = g_drop_crates.size();
			notes = g_notes.size();
			note_class = g_note_base_class != nullptr;
			block_value = g_config.block_return_value;
			copy = g_config.zones;
		}

		Reply(pc, "SpawnGuard - spawns=" + std::to_string(spawns)
			+ " dropVolumes=" + std::to_string(boxes)
			+ " crates=" + std::to_string(crates)
			+ " notes=" + std::to_string(notes)
			+ " (class " + (note_class ? std::string("ok") : std::string("not found")) + ")"
			+ " zones=" + std::to_string(copy.size()));

		Reply(pc, std::string("hooks: IsAllowedToBuild=")
			+ (g_allow_hook_installed ? "OK" : "FAILED")
			+ ", FinalStructurePlacement=" + (g_final_hook_installed ? "OK" : "off")
			+ ", block value=" + std::to_string(block_value)
			+ (block_value == 0 ? " (notify only!)" : ""));

		for (const auto& z : copy)
		{
			Reply(pc, " - " + z.name + " (" + IntToStr(z.x) + ", " + IntToStr(z.y) + ", "
				+ IntToStr(z.z) + ") range=" + FoundationsToStr(z.range_uu) + " foundations");
		}
	}

	void CmdNear(APlayerController* pc, FString*, bool)
	{
		auto* spc = static_cast<AShooterPlayerController*>(pc);
		if (!spc)
			return;

		const FVector loc = AsaApi::GetApiUtils().GetPosition(spc);
		const auto rt = GetRuntime();

		double best = -1.0;
		std::string best_zone = "none";

		// NOTE: never name a variable or a lambda "check", "verify" or "ensure":
		// those are function-like macros in the UE headers that AsaApi includes
		// (API/UE/Misc/AssertionMacros.h), and the preprocessor rewrites any
		// "check(...)" into an assertion, which produces a cascade of parse errors.
		auto consider = [&](const std::string& id, const FVector& p)
		{
			const double d = std::sqrt(DistanceSq2D(loc, p));
			if (best < 0.0 || d < best)
			{
				best = d;
				best_zone = id;
			}
		};

		if (rt)
		{
			for (const auto& g : rt->groups)
			{
				for (const auto& p : g.points)
					consider(g.id, p.loc);
			}
			for (const auto& v : rt->volumes)
			{
				for (const auto& b : v.boxes)
				{
					consider(v.id, b.center);
					if (PointInsideBox(b, loc, v.padding_uu))
					{
						best = 0.0;
						best_zone = v.id + " (you are inside)";
					}
				}
			}
		}

		Reply(pc, "You: " + IntToStr(loc.X) + " " + IntToStr(loc.Y) + " " + IntToStr(loc.Z)
			+ " | nearest protected point: " + best_zone
			+ (best > 0.0 ? " at " + FoundationsToStr(best) + " foundations" : ""));
	}

	void CmdHooks(APlayerController* pc, FString*, bool)
	{
		Reply(pc, std::string("IsAllowedToBuild hook: ")
			+ (g_allow_hook_installed ? g_hook_name_is_allowed : std::string("NOT INSTALLED")));
		Reply(pc, std::string("FinalStructurePlacement hook: ")
			+ (g_final_hook_installed ? g_hook_name_final : std::string("off")));
		Reply(pc, std::string("GameMode.BeginPlay hook: ")
			+ (g_begin_play_hook_installed ? g_hook_name_begin_play : std::string("NOT INSTALLED")));

		const std::string cache = OffsetCachePath();
		if (cache.empty())
		{
			Reply(pc, "offset cache: NOT FOUND (ArkApi/Cache/.../cached_offsets.cache)");
			return;
		}

		Reply(pc, "offset cache: " + cache);
		Reply(pc, "APrimalStructure.* names in the cache: "
			+ std::to_string(CountOffsetCacheKeysWithPrefix(cache, "APrimalStructure.")));

		std::vector<std::string> variants;
		if (ScanOffsetCacheForKeys(cache, "APrimalStructure.IsAllowedToBuild(", variants))
		{
			Reply(pc, "cache keys for IsAllowedToBuild (" + std::to_string(variants.size()) + "):");
			for (const auto& v : variants)
				Reply(pc, "  " + v);
		}
		else
		{
			Reply(pc, "cache has NO key starting with APrimalStructure.IsAllowedToBuild(");
		}
	}

	void CmdAddPoint(APlayerController* pc, FString* params, bool)
	{
		auto* spc = static_cast<AShooterPlayerController*>(pc);
		if (!spc)
			return;

		const FString empty;
		const std::vector<std::string> args = ParseArgs(params ? *params : empty, "SG.AddPoint");
		if (args.empty())
		{
			Reply(pc, "Usage: cheat SG.AddPoint <Name> [RangeInFoundations]");
			return;
		}

		const FVector loc = AsaApi::GetApiUtils().GetPosition(spc);

		CustomZone zone;
		zone.name = SanitizeZoneName(args[0]);
		zone.x = loc.X;
		zone.y = loc.Y;
		zone.z = loc.Z;

		{
			std::lock_guard<std::mutex> lock(g_runtime_mutex);
			zone.range_uu = g_config.custom.range_uu;
			if (args.size() >= 2)
			{
				try
				{
					const double foundations = std::stod(args[1]);
					if (foundations > 0.0 && foundations <= 10000.0)
						zone.range_uu = foundations * kFoundationUU;
				}
				catch (...)
				{
					// keep the default range
				}
			}

			const std::string wanted = ToLower(zone.name);
			g_config.zones.erase(
				std::remove_if(g_config.zones.begin(), g_config.zones.end(),
					[&](const CustomZone& c) { return ToLower(c.name) == wanted; }),
				g_config.zones.end());
			g_config.zones.push_back(zone);

			SaveCustomZonesLocked();
			PublishRuntime();
		}

		Reply(pc, "Zone '" + zone.name + "' added at " + IntToStr(zone.x) + " " + IntToStr(zone.y)
			+ " " + IntToStr(zone.z) + " (range " + FoundationsToStr(zone.range_uu) + " foundations)");
	}

	void CmdRemovePoint(APlayerController* pc, FString* params, bool)
	{
		const FString empty;
		const std::vector<std::string> args = ParseArgs(params ? *params : empty, "SG.RemovePoint");
		if (args.empty())
		{
			Reply(pc, "Usage: cheat SG.RemovePoint <Name>");
			return;
		}

		const std::string wanted = ToLower(args[0]);
		bool removed = false;
		{
			std::lock_guard<std::mutex> lock(g_runtime_mutex);
			const size_t before = g_config.zones.size();
			g_config.zones.erase(
				std::remove_if(g_config.zones.begin(), g_config.zones.end(),
					[&](const CustomZone& c) { return ToLower(c.name) == wanted; }),
				g_config.zones.end());
			removed = g_config.zones.size() != before;
			if (removed)
			{
				SaveCustomZonesLocked();
				PublishRuntime();
			}
		}

		Reply(pc, removed ? "Zone removed." : "Zone not found.");
	}

	// ------------------------------------------------------------------------
	// hooks
	// ------------------------------------------------------------------------

	DECLARE_HOOK(AShooterGameMode_BeginPlay, void, AShooterGameMode*);
	void Hook_AShooterGameMode_BeginPlay(AShooterGameMode* _this)
	{
		AShooterGameMode_BeginPlay_original(_this);

		// This hook can be missed (plugin hot reload); the watchdog timer calls
		// EnsureInit() as well.
		try
		{
			EnsureInit("BeginPlay");
		}
		catch (const std::exception& e)
		{
			Log::GetLog()->error("SpawnGuard: init failed: {}", e.what());
		}
		catch (...)
		{
			Log::GetLog()->error("SpawnGuard: init failed");
		}
	}

	DECLARE_HOOK(APrimalStructure_IsAllowedToBuild, int, APrimalStructure*, APlayerController*,
		UE::Math::TVector<double>, UE::Math::TRotator<double>, FPlacementData*, bool,
		UE::Math::TRotator<double>, bool, bool, APawn*);

	int Hook_APrimalStructure_IsAllowedToBuild(APrimalStructure* _this, APlayerController* pc,
		UE::Math::TVector<double> at_location, UE::Math::TRotator<double> at_rotation,
		FPlacementData* out_placement_data, bool dont_adjust_for_max_range,
		UE::Math::TRotator<double> player_view_rotation, bool final_placement,
		bool use_original_out_placement_data, APawn* building_pawn_override)
	{
		const int original = APrimalStructure_IsAllowedToBuild_original(_this, pc, at_location, at_rotation,
			out_placement_data, dont_adjust_for_max_range, player_view_rotation, final_placement,
			use_original_out_placement_data, building_pawn_override);

		// Sample what the engine returns: 0 is expected to mean "no reason to
		// block". Logged once so the meaning can be verified on the live server.
		if (!g_samples_logged)
		{
			const int idx = original < 0 ? 0 : (original > 7 ? 7 : original);
			++g_sample_counts[idx];
			if (++g_sample_total >= 60)
			{
				g_samples_logged = true;
				std::string line;
				for (int i = 0; i < 8; ++i)
				{
					if (g_sample_counts[i] == 0)
						continue;
					line += " [" + std::to_string(i) + (i == 7 ? "+" : "") + "]="
						+ std::to_string(g_sample_counts[i]);
				}
				Log::GetLog()->info("SpawnGuard diagnostics: IsAllowedToBuild returned:{}"
					" (0 = 'no reason to block' = allowed, non-zero = 'reason to block').", line);
			}
		}

		const auto rt = GetRuntime();
		if (!rt || !rt->enabled)
			return original;

		const FVector loc = PlacementLocation(at_location, out_placement_data);
		const Verdict verdict = EvaluatePlacement(loc, pc, _this, final_placement);
		if (!verdict.blocked)
			return original;

		NotifyBlocked(pc, *rt, verdict);

		if (g_debug_lines_left > 0)
		{
			--g_debug_lines_left;
			Log::GetLog()->info("SpawnGuard: blocked '{}' near {} ({:.1f} foundations) at {:.0f} {:.0f} {:.0f} "
				"(final={}, engine returned {}, we return {})",
				_this ? _this->NamePrivateField().ToString().ToString() : "?", verdict.zone,
				verdict.distance_foundations, loc.X, loc.Y, loc.Z, final_placement ? 1 : 0, original,
				rt->block_return_value);
		}

		if (rt->block_return_value == 0)
			return original;   // "notify only" mode

		return rt->block_return_value;
	}

	// Optional second gate (General.EnforceOnFinalPlacement). Disabled by default:
	// the argument list of FinalStructurePlacement is ambiguous in the generated
	// headers (by value vs. pointer), and a wrong hook signature would crash the
	// server. Enable it only if the first gate does not stop the placement - and
	// check the name in cached_offsets.cache first.
	DECLARE_HOOK(APrimalStructure_FinalStructurePlacement, char, APrimalStructure*, APlayerController*,
		UE::Math::TVector<double>, UE::Math::TRotator<double>, UE::Math::TRotator<double>, APawn*,
		FName, bool, FPlacementData*);

	char Hook_APrimalStructure_FinalStructurePlacement(APrimalStructure* _this, APlayerController* pc,
		UE::Math::TVector<double> at_location, UE::Math::TRotator<double> at_rotation,
		UE::Math::TRotator<double> player_view_rotation, APawn* attach_to_pawn, FName bone_name,
		bool flipped, FPlacementData* placement_data)
	{
		const auto rt = GetRuntime();
		if (rt && rt->enabled)
		{
			const FVector loc = MakeVector(at_location.X, at_location.Y, at_location.Z);
			const Verdict verdict = EvaluatePlacement(loc, pc, _this, true);
			if (verdict.blocked)
			{
				NotifyBlocked(pc, *rt, verdict);
				Log::GetLog()->info("SpawnGuard: final placement refused near {}", verdict.zone);
				return 0;
			}
		}

		return APrimalStructure_FinalStructurePlacement_original(_this, pc, at_location, at_rotation,
			player_view_rotation, attach_to_pawn, bone_name, flipped, placement_data);
	}

	// ------------------------------------------------------------------------
	// life cycle
	// ------------------------------------------------------------------------

	void RegisterCommands();
	void OnTimer();

	// Installs the placement hooks. Idempotent, safe to call after every config
	// reload (e.g. after a game update the hook name may work again).
	void InstallHooks()
	{
		auto& hooks = AsaApi::GetHooks();

		if (!g_allow_hook_installed)
		{
			const HookName info = ResolveHookName("APrimalStructure.IsAllowedToBuild(",
				kHookIsAllowedToBuild, g_config.hook_name_is_allowed);

			if (!info.name.empty())
			{
				bool ok = false;
				try
				{
					ok = hooks.SetHook(info.name, &Hook_APrimalStructure_IsAllowedToBuild,
						&APrimalStructure_IsAllowedToBuild_original);
				}
				catch (...)
				{
					ok = false;
				}

				g_allow_hook_installed = ok;
				if (ok)
				{
					g_hook_name_is_allowed = info.name;
					Log::GetLog()->info("SpawnGuard: hook installed: {}", info.name);
				}
				else
				{
					Log::GetLog()->error("SpawnGuard: SetHook('{}') returned false.", info.name);
				}
			}
			else
			{
				LogHookNameProblem("APrimalStructure.IsAllowedToBuild", kHookIsAllowedToBuild, info);
			}
		}

		if (g_config.enforce_on_final_placement && !g_final_hook_installed)
		{
			const HookName info = ResolveHookName("APrimalStructure.FinalStructurePlacement(",
				kHookFinalStructurePlacement, g_config.hook_name_final_placement);

			if (!info.name.empty())
			{
				bool ok = false;
				try
				{
					ok = hooks.SetHook(info.name, &Hook_APrimalStructure_FinalStructurePlacement,
						&APrimalStructure_FinalStructurePlacement_original);
				}
				catch (...)
				{
					ok = false;
				}

				g_final_hook_installed = ok;
				if (ok)
				{
					g_hook_name_final = info.name;
					Log::GetLog()->info("SpawnGuard: hook installed: {}", info.name);
				}
				else
				{
					Log::GetLog()->error("SpawnGuard: SetHook('{}') returned false.", info.name);
				}
			}
			else
			{
				LogHookNameProblem("APrimalStructure.FinalStructurePlacement",
					kHookFinalStructurePlacement, info);
			}
		}
		else if (!g_config.enforce_on_final_placement && g_final_hook_installed)
		{
			try
			{
				hooks.DisableHook(g_hook_name_final, &Hook_APrimalStructure_FinalStructurePlacement);
			}
			catch (...) {}
			g_final_hook_installed = false;
			Log::GetLog()->info("SpawnGuard: hook removed (EnforceOnFinalPlacement=false)");
		}
	}

	void RemoveHooks()
	{
		auto& hooks = AsaApi::GetHooks();

		try
		{
			if (g_allow_hook_installed)
				hooks.DisableHook(g_hook_name_is_allowed, &Hook_APrimalStructure_IsAllowedToBuild);
		}
		catch (...) {}

		try
		{
			if (g_final_hook_installed)
				hooks.DisableHook(g_hook_name_final, &Hook_APrimalStructure_FinalStructurePlacement);
		}
		catch (...) {}

		g_allow_hook_installed = false;
		g_final_hook_installed = false;
	}

	void RegisterCommands()
	{
		if (g_timer_registered)
			return;

		AsaApi::GetCommands().AddConsoleCommand(FString("SG.Reload"), &CmdReload);
		AsaApi::GetCommands().AddConsoleCommand(FString("SGReloadConfig"), &CmdReload);
		AsaApi::GetCommands().AddConsoleCommand(FString("SG.AddPoint"), &CmdAddPoint);
		AsaApi::GetCommands().AddConsoleCommand(FString("SG.RemovePoint"), &CmdRemovePoint);
		AsaApi::GetCommands().AddConsoleCommand(FString("SG.Rescan"), &CmdRescan);
		AsaApi::GetCommands().AddConsoleCommand(FString("SG.List"), &CmdList);
		AsaApi::GetCommands().AddConsoleCommand(FString("SG.Near"), &CmdNear);
		AsaApi::GetCommands().AddConsoleCommand(FString("SG.Hooks"), &CmdHooks);
		AsaApi::GetCommands().AddRconCommand(FString("SG.Reload"), &RconReload);

		AsaApi::GetCommands().AddOnTimerCallback(FString("SpawnGuard.Refresh"), &OnTimer);

		g_timer_registered = true;
	}

	// Loads config + data as soon as the server is up. Safe to call repeatedly.
	void EnsureInit(const char* reason)
	{
		UWorld* world = AsaApi::GetApiUtils().GetWorld();

		if (g_initialized)
		{
			// A new level (map change / world reload) invalidates all cached data.
			if (world && world != g_last_world)
			{
				g_last_world = world;
				g_notes_frozen = false;
				g_volumes_frozen = false;
				g_note_attempts = 0;
				g_volume_attempts = 0;
				g_note_class_attempts = 0;
				g_note_base_class = nullptr;
				RefreshPoints(true);
				Log::GetLog()->info("SpawnGuard: new world detected, points rescanned");
			}
			return;
		}

		if (!world && AsaApi::GetApiUtils().GetStatus() != AsaApi::ServerStatus::Ready)
			return;   // no world yet - the watchdog timer retries every second

		g_initialized = true;
		g_last_world = world;

		if (!g_config_loaded)
			LoadConfigFile();

		// If the hook could not be installed earlier (e.g. the offset cache had
		// not been downloaded yet), try again now - InstallHooks() is idempotent.
		if (!g_allow_hook_installed)
			InstallHooks();

		RefreshPoints(true);
		Log::GetLog()->info("SpawnGuard: initialized ({})", reason);
	}

	void OnTimer()
	{
		// Deferred reload (requested over RCON from another thread).
		if (g_pending_reload.exchange(false))
		{
			if (LoadConfigFile())
			{
				RefreshPoints(true);
				Log::GetLog()->info("SpawnGuard: deferred reload done");
			}
			else
			{
				Log::GetLog()->error("SpawnGuard: deferred reload FAILED - see the log above");
			}
		}

		if (!g_initialized)
		{
			EnsureInit("timer watchdog");
			return;
		}

		int refresh = 30;
		{
			std::lock_guard<std::mutex> lock(g_runtime_mutex);
			refresh = g_config.refresh_seconds;
		}
		if (refresh < 5)
			refresh = 5;

		if (++g_refresh_tick < refresh)
			return;
		g_refresh_tick = 0;
		RefreshPoints(false);
	}
}

// ----------------------------------------------------------------------------
// plugin entry points
// ----------------------------------------------------------------------------

extern "C" __declspec(dllexport) void Plugin_Init()
{
	try
	{
		Log::Get().Init("SpawnGuard");
		Log::GetLog()->info("SpawnGuard: loading (folder: {})", PluginDir());

		LoadConfigFile();      // also installs the hooks
		InstallHooks();        // ... even when config.json is missing or broken
		RegisterCommands();

		const HookName begin_play = ResolveHookName("AShooterGameMode.BeginPlay(",
			kHookGameModeBeginPlay, g_config.hook_name_begin_play);
		if (!begin_play.name.empty())
		{
			try
			{
				g_begin_play_hook_installed = AsaApi::GetHooks().SetHook(begin_play.name,
					&Hook_AShooterGameMode_BeginPlay, &AShooterGameMode_BeginPlay_original);
			}
			catch (...)
			{
				g_begin_play_hook_installed = false;
			}

			if (g_begin_play_hook_installed)
			{
				g_hook_name_begin_play = begin_play.name;
			}
			else
			{
				Log::GetLog()->error("SpawnGuard: SetHook('{}') returned false", begin_play.name);
			}
		}
		else
		{
			LogHookNameProblem("AShooterGameMode.BeginPlay", kHookGameModeBeginPlay, begin_play);
		}

		if (!g_begin_play_hook_installed)
			Log::GetLog()->warn("SpawnGuard: no BeginPlay hook - the watchdog timer initializes the plugin");

		if (AsaApi::GetApiUtils().GetStatus() == AsaApi::ServerStatus::Ready)
			EnsureInit("plugin load (server already running)");

		Log::GetLog()->info("SpawnGuard: loaded");
	}
	catch (const std::exception& e)
	{
		Log::GetLog()->error("SpawnGuard: plugin init failed: {}", e.what());
	}
	catch (...)
	{
		Log::GetLog()->error("SpawnGuard: plugin init failed with an unknown error");
	}
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
	try
	{
		RemoveHooks();

		if (g_timer_registered)
		{
			AsaApi::GetCommands().RemoveConsoleCommand(FString("SG.Reload"));
			AsaApi::GetCommands().RemoveConsoleCommand(FString("SGReloadConfig"));
			AsaApi::GetCommands().RemoveConsoleCommand(FString("SG.AddPoint"));
			AsaApi::GetCommands().RemoveConsoleCommand(FString("SG.RemovePoint"));
			AsaApi::GetCommands().RemoveConsoleCommand(FString("SG.Rescan"));
			AsaApi::GetCommands().RemoveConsoleCommand(FString("SG.List"));
			AsaApi::GetCommands().RemoveConsoleCommand(FString("SG.Near"));
			AsaApi::GetCommands().RemoveConsoleCommand(FString("SG.Hooks"));
			AsaApi::GetCommands().RemoveRconCommand(FString("SG.Reload"));
			AsaApi::GetCommands().RemoveOnTimerCallback(FString("SpawnGuard.Refresh"));
			g_timer_registered = false;
		}

		try
		{
			if (g_begin_play_hook_installed)
				AsaApi::GetHooks().DisableHook(g_hook_name_begin_play, &Hook_AShooterGameMode_BeginPlay);
		}
		catch (...) {}
		g_begin_play_hook_installed = false;

		g_initialized = false;
		Log::GetLog()->info("SpawnGuard: unloaded");
	}
	catch (...)
	{
		// nothing we can do while unloading
	}
}
