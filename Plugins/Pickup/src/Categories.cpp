#include "Categories.h"

#include <unordered_map>
#include <utility>

#include "Utils.h"

namespace Pickup::Categories
{
	namespace
	{
		// Cache of the class part of the search text (blueprint paths do not change during the server life time).
		std::unordered_map<const UClass*, FString> class_text_cache;

		// Cache of UTF-8 -> FString conversions used by pattern matching.
		std::unordered_map<std::string, FString> pattern_cache;

		// Cache of resolved native classes.
		std::unordered_map<std::string, UClass*> native_class_cache;

		CategoryConfig Make(std::string id, std::string display_name, std::vector<std::string> aliases)
		{
			CategoryConfig category;
			category.id = std::move(id);
			category.display_name = std::move(display_name);
			category.aliases = std::move(aliases);
			return category;
		}

		bool GetStructureFlag(APrimalStructure* structure, const std::string& name, bool& out_value)
		{
			if (name == "bIsWall")
			{
				out_value = structure->bIsWall().Get();
				return true;
			}
			if (name == "bIsFloor")
			{
				out_value = structure->bIsFloor().Get();
				return true;
			}
			if (name == "bIsFoundation")
			{
				out_value = structure->bIsFoundation().Get();
				return true;
			}
			if (name == "bIsFenceFoundation")
			{
				out_value = structure->bIsFenceFoundation().Get();
				return true;
			}
			if (name == "bIsDoorframe")
			{
				out_value = structure->bIsDoorframe().Get();
				return true;
			}
			if (name == "bIsBed")
			{
				out_value = structure->bIsBed().Get();
				return true;
			}
			if (name == "bIsCoreStructure")
			{
				out_value = structure->bIsCoreStructure().Get();
				return true;
			}
			if (name == "bIsEnvironmentStructure")
			{
				out_value = structure->bIsEnvironmentStructure().Get();
				return true;
			}
			if (name == "bIsTeleporter")
			{
				out_value = structure->bIsTeleporter().Get();
				return true;
			}

			return false;
		}

		UClass* ResolveNativeClass(const std::string& name)
		{
			const auto cached = native_class_cache.find(name);
			if (cached != native_class_cache.end())
				return cached->second;

			UClass* result = nullptr;

			if (name == "APrimalStructure")
				result = APrimalStructure::GetPrivateStaticClass();
			else if (name == "APrimalStructureBearTrap")
				result = APrimalStructureBearTrap::GetPrivateStaticClass();
			else if (name == "APrimalStructureBed")
				result = APrimalStructureBed::GetPrivateStaticClass();
			else if (name == "APrimalStructureDoor")
				result = APrimalStructureDoor::GetPrivateStaticClass();
			else if (name == "APrimalStructureSkeletalDoor")
				result = APrimalStructureSkeletalDoor::GetPrivateStaticClass();
			else if (name == "APrimalStructureItemContainer")
				result = APrimalStructureItemContainer::GetPrivateStaticClass();
			else if (name == "APrimalStructureItemContainer_SupplyCrate")
				result = APrimalStructureItemContainer_SupplyCrate::GetPrivateStaticClass();
			else if (name == "APrimalStructureItemContainer_HordeCrate")
				result = APrimalStructureItemContainer_HordeCrate::GetPrivateStaticClass();
			else if (name == "APrimalStructureSeating")
				result = APrimalStructureSeating::GetPrivateStaticClass();
			else if (name == "APrimalStructureKeypad")
				result = APrimalStructureKeypad::GetPrivateStaticClass();
			else if (name == "APrimalStructureLadder")
				result = APrimalStructureLadder::GetPrivateStaticClass();
			else if (name == "APrimalStructurePortableLadder")
				result = APrimalStructurePortableLadder::GetPrivateStaticClass();
			else if (name == "APrimalStructureElevatorTrack")
				result = APrimalStructureElevatorTrack::GetPrivateStaticClass();
			else if (name == "APrimalStructureSeaMine")
				result = APrimalStructureSeaMine::GetPrivateStaticClass();
			else if (name == "APrimalStructureTribeFlag")
				result = APrimalStructureTribeFlag::GetPrivateStaticClass();
			else if (name == "APrimalStructureTurret")
				result = APrimalStructureTurret::GetPrivateStaticClass();
			else if (name == "APrimalStructureTurretBallista")
				result = APrimalStructureTurretBallista::GetPrivateStaticClass();
			else if (name == "APrimalStructureTurretPlant")
				result = APrimalStructureTurretPlant::GetPrivateStaticClass();

			// A nullptr is not cached: the class may become available later (server still loading).
			if (result != nullptr)
				native_class_cache.emplace(name, result);

			return result;
		}

		const FString& PatternText(const std::string& pattern)
		{
			const auto cached = pattern_cache.find(pattern);
			if (cached != pattern_cache.end())
				return cached->second;

			return pattern_cache.emplace(pattern, Utils::FromUtf8(pattern)).first->second;
		}
	} // namespace

	std::vector<CategoryConfig> BuildDefaults()
	{
		std::vector<CategoryConfig> categories;

		{
			auto category = Make("all", "все структуры", {"a", "all", "everything", "всё", "все"});
			category.classes = {"APrimalStructure"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("foundation", "фундаменты",
				{"f", "foundation", "foundations", "fundament", "фундамент", "фундаменты", "фунд"});
			category.flags = {"bIsFoundation"};
			category.exclude_flags = {"bIsFenceFoundation"};
			category.patterns = {"Foundation"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("fence", "фундаменты забора", {"fe", "fence", "fencefoundation", "забор", "изгородь"});
			category.flags = {"bIsFenceFoundation"};
			category.patterns = {"Fence"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("ceiling", "потолки",
				{"c", "ceiling", "ceilings", "потолок", "потолки", "перекрытие", "перекрытия"});
			category.flags = {"bIsFloor"};
			category.exclude_flags = {"bIsFoundation", "bIsFenceFoundation"};
			category.patterns = {"Ceiling"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("wall", "стены", {"w", "wall", "walls", "стена", "стены"});
			category.flags = {"bIsWall"};
			category.exclude_flags = {"bIsDoorframe"};
			category.patterns = {"Wall"};
			category.exclude_patterns = {"Doorframe", "Doorway", "Window"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("doorframe", "дверные проёмы",
				{"df", "doorframe", "doorframes", "doorway", "hatchframe", "проем", "проём", "дверной проем"});
			category.flags = {"bIsDoorframe"};
			category.patterns = {"Doorframe", "Doorway", "Hatchframe"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("window", "окна", {"win", "window", "windows", "windowframe", "окно", "окна"});
			category.patterns = {"Window"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("door", "двери и ворота", {"d", "door", "doors", "gate", "gates", "дверь", "двери", "ворота"});
			category.classes = {"APrimalStructureDoor", "APrimalStructureSkeletalDoor"};
			category.patterns = {"Door", "Gate"};
			category.exclude_patterns = {"Doorframe", "Doorway"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("pillar", "колонны", {"p", "pillar", "pillars", "column", "колонна", "колонны", "столб"});
			category.patterns = {"Pillar", "Column"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("ramp", "пандусы", {"r", "ramp", "ramps", "рампа", "пандус"});
			category.patterns = {"Ramp"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("stairs", "лестницы", {"st", "stairs", "staircase", "лестница", "ступени"});
			category.patterns = {"Stair"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("railing", "перила", {"rail", "railing", "ramp railing", "перила"});
			category.patterns = {"Railing"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("roof", "крыши", {"roof", "roofs", "крыша", "крыши"});
			category.patterns = {"Roof"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("bed", "кровати", {"b", "bed", "beds", "кровать", "кровати"});
			category.flags = {"bIsBed"};
			category.classes = {"APrimalStructureBed"};
			category.patterns = {"Bed"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("storage", "хранилища",
				{"s", "storage", "box", "container", "chest", "vault", "ящик", "ящики", "хранилище", "сундук"});
			category.classes = {"APrimalStructureItemContainer"};
			category.exclude_classes = {
				"APrimalStructureTurret", "APrimalStructureTurretBallista", "APrimalStructureTurretPlant",
				"APrimalStructureSeating", "APrimalStructureItemContainer_SupplyCrate",
				"APrimalStructureItemContainer_HordeCrate", "APrimalStructureKeypad", "APrimalStructureElevatorTrack"
			};
			category.patterns = {"Storage", "Chest", "Vault", "Box", "Trough", "Cabinet", "Fridge", "Preserving"};
			category.exclude_patterns = {"SupplyCrate", "HordeCrate", "Underwater", "Elevator"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("turret", "турели", {"t", "turret", "turrets", "турель", "турели"});
			category.classes = {"APrimalStructureTurret", "APrimalStructureTurretBallista", "APrimalStructureTurretPlant"};
			category.patterns = {"Turret", "Ballista", "Cannon", "Minigun"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("sign", "таблички", {"sign", "signs", "табличка", "таблички", "вывеска"});
			category.patterns = {"Sign"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("ladder", "лестницы-стремянки", {"l", "ladder", "ladders", "стремянка"});
			category.classes = {"APrimalStructureLadder", "APrimalStructurePortableLadder"};
			category.patterns = {"Ladder"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("pipe", "трубы и вода", {"pipe", "pipes", "water", "труба", "трубы", "вода", "ирригация"});
			category.patterns = {"Pipe", "Tap", "Intake", "Irrigation", "Reservoir"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("electric", "электрика",
				{"e", "electric", "wire", "cable", "электрика", "провод", "кабель", "генератор"});
			category.patterns = {"Cable", "Wire", "Generator", "Outlet", "Switch", "Lamp", "Light", "Battery", "Solar",
				"WindTurbine", "Transformer", "Electric"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("crafting", "рабочие станции",
				{"craft", "crafting", "station", "stations", "станок", "верстак", "кузница", "ремесло"});
			category.patterns = {"Forge", "Anvil", "Smithy", "Workbench", "Fabricator", "Chemistry", "Mortar",
				"Campfire", "Grill", "CookingPot", "IndustrialCooker", "Replicator", "Spinning", "Tanning", "Brewing"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("greenhouse", "теплицы", {"gh", "greenhouse", "теплица", "теплицы"});
			category.patterns = {"Greenhouse"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("tek", "тек структуры", {"tek", "тек"});
			category.patterns = {"Tek"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("trap", "ловушки", {"trap", "traps", "ловушка", "ловушки", "мина"});
			category.classes = {"APrimalStructureBearTrap", "APrimalStructureSeaMine"};
			category.patterns = {"Trap", "Tripwire", "Mine"};
			categories.push_back(std::move(category));
		}
		{
			auto category = Make("flag", "флаги племени", {"flag", "flags", "флаг", "флаги"});
			category.classes = {"APrimalStructureTribeFlag"};
			category.patterns = {"TribeFlag"};
			categories.push_back(std::move(category));
		}

		return categories;
	}

	UClass* StructureClass()
	{
		static UClass* structure_class = nullptr;

		if (structure_class == nullptr)
			structure_class = APrimalStructure::GetPrivateStaticClass();

		return structure_class;
	}

	FString GetSearchText(APrimalStructure* structure)
	{
		if (structure == nullptr)
			return FString(L"");

		FString text;

		UClass* actor_class = structure->ClassPrivateField();

		const auto cached = class_text_cache.find(actor_class);
		if (cached != class_text_cache.end())
		{
			text = cached->second;
		}
		else
		{
			text = AsaApi::IApiUtils::GetBlueprint(structure);

			TSubclassOf<UPrimalItem>& pickup_item = structure->PickupGivesItemField();
			if (pickup_item.uClass != nullptr)
			{
				text.AppendChar(L' ');
				text += AsaApi::IApiUtils::GetClassBlueprint(pickup_item.uClass);
			}

			class_text_cache.emplace(actor_class, text);
		}

		const FString variant = structure->CurrentVariantTagField().ToString();
		if (variant.Len() > 0 && !variant.Equals(L"None", ESearchCase::IgnoreCase))
		{
			text.AppendChar(L' ');
			text += variant;
		}

		return text;
	}

	bool Matches(APrimalStructure* structure, const FString& search_text, const CategoryConfig& category)
	{
		if (structure == nullptr)
			return false;

		for (const std::string& name : category.exclude_flags)
		{
			bool value = false;
			if (GetStructureFlag(structure, name, value) && value)
				return false;
		}

		for (const std::string& name : category.exclude_classes)
		{
			UClass* native_class = ResolveNativeClass(name);
			if (native_class != nullptr && structure->IsA(native_class))
				return false;
		}

		for (const std::string& pattern : category.exclude_patterns)
		{
			if (search_text.Contains(PatternText(pattern), ESearchCase::IgnoreCase))
				return false;
		}

		for (const std::string& name : category.flags)
		{
			bool value = false;
			if (GetStructureFlag(structure, name, value) && value)
				return true;
		}

		for (const std::string& name : category.classes)
		{
			UClass* native_class = ResolveNativeClass(name);
			if (native_class != nullptr && structure->IsA(native_class))
				return true;
		}

		for (const std::string& pattern : category.patterns)
		{
			if (search_text.Contains(PatternText(pattern), ESearchCase::IgnoreCase))
				return true;
		}

		return false;
	}

	bool IsAlive(APrimalStructure* structure)
	{
		if (structure == nullptr)
			return false;

		if (structure->bActorIsBeingDestroyed().Get())
			return false;

		if (structure->bDemolished().Get())
			return false;

		if (structure->bPendingRemoval().Get())
			return false;

		if (structure->bWasPickedUp().Get())
			return false;

		if (structure->bIsPreviewStructure().Get())
			return false;

		return true;
	}

	bool VanillaAllowPickup(APrimalStructure* structure, AShooterPlayerController* player_controller)
	{
		if (structure == nullptr || player_controller == nullptr)
			return false;

		// AllowPickupForItem is overridden by several classes and NativeCall does not use the virtual table,
		// so the most derived known implementation is selected manually (Bed is a Seating, Seating is checked
		// after Bed on purpose).
		const UClass* bear_trap = ResolveNativeClass("APrimalStructureBearTrap");
		if (bear_trap != nullptr && structure->IsA(const_cast<UClass*>(bear_trap)))
			return static_cast<APrimalStructureBearTrap*>(structure)->AllowPickupForItem(player_controller);

		const UClass* bed = ResolveNativeClass("APrimalStructureBed");
		if (bed != nullptr && structure->IsA(const_cast<UClass*>(bed)))
			return static_cast<APrimalStructureBed*>(structure)->AllowPickupForItem(player_controller);

		const UClass* seating = ResolveNativeClass("APrimalStructureSeating");
		if (seating != nullptr && structure->IsA(const_cast<UClass*>(seating)))
			return static_cast<APrimalStructureSeating*>(structure)->AllowPickupForItem(player_controller);

		const UClass* door = ResolveNativeClass("APrimalStructureDoor");
		if (door != nullptr && structure->IsA(const_cast<UClass*>(door)))
			return static_cast<APrimalStructureDoor*>(structure)->AllowPickupForItem(player_controller);

		return structure->AllowPickupForItem(player_controller);
	}

	void ClearCache()
	{
		class_text_cache.clear();
		pattern_cache.clear();
		native_class_cache.clear();
	}
} // namespace Pickup::Categories
