// Survival Adrenaline
//
// While the player is in a dungeon or in combat, hunger and fatigue
// (exhaustion) build up more slowly. The part they didn't gain is kept as a
// debt, and when the adrenaline ends (out of combat and out of any dungeon)
// the whole debt lands at once: the crash.
//
// With Survival Mode Improved - SKSE it scales SMI's own rate multipliers
// (SMI_HungerRateMult / SMI_ExhaustionRateMult in Update.esm). SMI resets the
// base rates on every load but leaves these alone, which makes them the
// tunable knob that MCMs such as Survival Control Panel adjust. Without SMI it
// scales CC Survival Mode's base rate globals instead.
//
// Whatever value is in a multiplier when the adrenaline starts is remembered
// as the "normal" value and restored afterwards. If another mod changes it in
// the meantime, that change is taken as the new normal value, so MCM edits are
// never overwritten.
//
// The debt is measured, not predicted: each rise in the need's current value
// while scaled by m stands for a full-rate rise of delta / m, so the debt grows
// by delta * (1 - m) / m. That way race, vampirism, encumbrance and other
// factors Survival Mode applies are all accounted for.

namespace
{
	constexpr std::string_view kIniPath = "Data/SKSE/Plugins/SurvivalAdrenaline.ini";
	constexpr float            kMinMult = 0.05f;  // the debt can't be measured at 0

	constexpr std::uint32_t kCosaveID = 'SADR';
	constexpr std::uint32_t kCosaveVersion = 2;
	constexpr std::uint32_t kRecordHunger = 'HUNG';
	constexpr std::uint32_t kRecordFatigue = 'FATG';

	// Location keywords (Skyrim.esm) that mark a dungeon.
	constexpr RE::FormID kDungeonKeywords[] = {
		0x130DB,  // LocTypeDungeon
		0xF5E80,  // LocTypeClearable
		0x130E2,  // LocTypeDraugrCrypt
		0x130E3,  // LocTypeDwarvenAutomatons
		0x130E4,  // LocTypeFalmerHive
		0x130EB,  // LocTypeVampireLair
		0x130EC,  // LocTypeWarlockLair
		0x130E6,  // LocTypeHagravenNest
		0x130E1,  // LocTypeDragonPriestLair
		0x130DF,  // LocTypeBanditCamp
		0x130DE,  // LocTypeAnimalDen
		0x130EE,  // LocTypeForswornCamp
	};

	struct Settings
	{
		float hungerInDungeon = 0.15f;
		float fatigueInDungeon = 0.15f;
		float hungerInCombat = 0.15f;
		float fatigueInCombat = 0.15f;
		float checkInterval = 1.0f;
		bool  crashMessage = true;
	};
	Settings g_settings;

	// One Survival Mode need this mod defers.
	struct Need
	{
		const char*    name;
		RE::TESGlobal* rate = nullptr;   // the multiplier/rate global this mod scales
		RE::TESGlobal* value = nullptr;  // the need's current value
		RE::TESGlobal* max = nullptr;    // the need's maximum value

		// Saved in the co-save.
		bool  active = false;   // rate currently scaled by this mod
		float normal = 0.0f;    // the rate value to restore
		float written = 0.0f;   // the rate value this mod last wrote
		float mult = 1.0f;      // the scale currently applied
		float debt = 0.0f;      // need held back, paid at the crash

		// Not saved; re-read after a load.
		std::optional<float> lastValue;

		// Adds to the debt what this need didn't gain since the last check.
		void Measure()
		{
			const float current = value->value;
			if (active && lastValue && current > *lastValue) {
				debt += (current - *lastValue) * (1.0f - mult) / mult;
			}
			lastValue = current;
		}

		void Scale(std::optional<float> a_mult)
		{
			const float current = rate->value;
			const bool  changedByOthers = active && std::abs(current - written) > 1e-4f;

			if (!a_mult) {
				if (active && !changedByOthers) {
					rate->value = normal;
				}
				active = false;
				mult = 1.0f;
				return;
			}

			if (!active || changedByOthers) {
				normal = current;  // first time, or an MCM changed it while scaled
			}
			mult = *a_mult;
			written = normal * mult;
			rate->value = written;
			active = true;
		}

		// Pays the whole debt at once. Returns whether there was any.
		bool Crash()
		{
			if (debt <= 0.0f) {
				return false;
			}
			const float before = value->value;
			value->value = max ? std::min(before + debt, max->value) : before + debt;
			spdlog::info("{} crash: {} -> {} (debt {})", name, before, value->value, debt);
			debt = 0.0f;
			lastValue = value->value;
			return true;
		}

		bool Ready() const { return rate && value; }
	};
	Need g_hunger{ "Hunger" };
	Need g_fatigue{ "Fatigue" };

	std::vector<RE::BGSKeyword*> g_dungeonKeywords;

	void SetupLog()
	{
		auto dir = SKSE::log::log_directory();
		if (!dir) {
			return;
		}
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "SurvivalAdrenaline.log").string(), true);
		auto logger = std::make_shared<spdlog::logger>("log", std::move(sink));
		logger->set_level(spdlog::level::info);
		logger->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(logger));
		spdlog::set_pattern("[%H:%M:%S] %v");
	}

	std::string Trim(std::string_view a_str)
	{
		const auto begin = a_str.find_first_not_of(" \t\r\n");
		if (begin == std::string_view::npos) {
			return {};
		}
		const auto end = a_str.find_last_not_of(" \t\r\n");
		return std::string(a_str.substr(begin, end - begin + 1));
	}

	void LoadSettings()
	{
		std::ifstream file{ std::string(kIniPath) };
		if (!file) {
			spdlog::warn("{} not found; using defaults", kIniPath);
			return;
		}
		float crashMessage = 1.0f;
		const std::map<std::string, float*> keys{
			{ "fHungerMultInDungeon", &g_settings.hungerInDungeon },
			{ "fFatigueMultInDungeon", &g_settings.fatigueInDungeon },
			{ "fHungerMultInCombat", &g_settings.hungerInCombat },
			{ "fFatigueMultInCombat", &g_settings.fatigueInCombat },
			{ "fCheckIntervalSeconds", &g_settings.checkInterval },
			{ "bCrashMessage", &crashMessage },
		};
		std::string line;
		while (std::getline(file, line)) {
			if (const auto comment = line.find_first_of(";#"); comment != std::string::npos) {
				line.erase(comment);
			}
			const auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			const auto it = keys.find(Trim(std::string_view(line).substr(0, eq)));
			if (it == keys.end()) {
				continue;
			}
			try {
				*it->second = std::stof(Trim(std::string_view(line).substr(eq + 1)));
			} catch (...) {
				spdlog::warn("Ignoring unreadable line '{}'", line);
			}
		}
		for (float* mult : { &g_settings.hungerInDungeon, &g_settings.fatigueInDungeon, &g_settings.hungerInCombat, &g_settings.fatigueInCombat }) {
			*mult = std::clamp(*mult, kMinMult, 1.0f);
		}
		g_settings.checkInterval = std::clamp(g_settings.checkInterval, 0.25f, 10.0f);
		g_settings.crashMessage = crashMessage != 0.0f;
		spdlog::info("Dungeon: hunger x{}, fatigue x{}. Combat: hunger x{}, fatigue x{}.",
			g_settings.hungerInDungeon, g_settings.fatigueInDungeon, g_settings.hungerInCombat, g_settings.fatigueInCombat);
	}

	bool IsInDungeon(RE::PlayerCharacter* a_player)
	{
		auto cell = a_player->GetParentCell();
		if (!cell || !cell->IsInteriorCell()) {
			return false;
		}
		auto location = a_player->GetCurrentLocation();
		for (int depth = 0; location && depth < 16; ++depth, location = location->parentLoc) {
			for (auto* keyword : g_dungeonKeywords) {
				if (location->HasKeyword(keyword)) {
					return true;
				}
			}
		}
		return false;
	}

	// The multiplier for a need right now, or none when there's no adrenaline.
	// In combat inside a dungeon, the stronger slowdown wins.
	std::optional<float> CurrentMult(bool a_dungeon, bool a_combat, float a_dungeonMult, float a_combatMult)
	{
		if (a_dungeon && a_combat) {
			return std::min(a_dungeonMult, a_combatMult);
		}
		if (a_dungeon) {
			return a_dungeonMult;
		}
		if (a_combat) {
			return a_combatMult;
		}
		return std::nullopt;
	}

	void Tick()
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell() || !player->Is3DLoaded()) {
			return;
		}
		const bool dungeon = IsInDungeon(player);
		const bool combat = player->IsInCombat();

		bool crashed = false;
		for (auto [need, dungeonMult, combatMult] : {
				 std::tuple{ &g_hunger, g_settings.hungerInDungeon, g_settings.hungerInCombat },
				 std::tuple{ &g_fatigue, g_settings.fatigueInDungeon, g_settings.fatigueInCombat } }) {
			need->Measure();
			const auto mult = CurrentMult(dungeon, combat, dungeonMult, combatMult);
			need->Scale(mult);
			if (!mult) {
				crashed |= need->Crash();
			}
		}
		if (crashed && g_settings.crashMessage) {
			RE::SendHUDMessage::ShowHUDMessage("The adrenaline fades. Your hunger and weariness catch up with you.");
		}
	}

	void StartTicking()
	{
		static bool started = false;
		if (started) {
			return;
		}
		started = true;
		std::thread([]() {
			for (;;) {
				std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(g_settings.checkInterval * 1000.0f)));
				SKSE::GetTaskInterface()->AddTask(Tick);
			}
		}).detach();
	}

	void OnDataLoaded()
	{
		LoadSettings();
		auto dataHandler = RE::TESDataHandler::GetSingleton();

		for (const auto id : kDungeonKeywords) {
			if (auto keyword = dataHandler->LookupForm<RE::BGSKeyword>(id, "Skyrim.esm")) {
				g_dungeonKeywords.push_back(keyword);
			}
		}

		constexpr auto ccPlugin = "ccqdrsse001-survivalmode.esl";
		g_hunger.value = dataHandler->LookupForm<RE::TESGlobal>(0x81A, ccPlugin);   // Survival_HungerNeedValue
		g_hunger.max = dataHandler->LookupForm<RE::TESGlobal>(0x80C, ccPlugin);     // Survival_HungerNeedMaxValue
		g_fatigue.value = dataHandler->LookupForm<RE::TESGlobal>(0x816, ccPlugin);  // Survival_ExhaustionNeedValue
		g_fatigue.max = dataHandler->LookupForm<RE::TESGlobal>(0x84A, ccPlugin);    // Survival_ExhaustionNeedMaxValue

		if (REX::W32::GetModuleHandleW(L"SurvivalModeImproved.dll")) {
			g_hunger.rate = dataHandler->LookupForm<RE::TESGlobal>(0xB6F30, "Update.esm");   // SMI_HungerRateMult
			g_fatigue.rate = dataHandler->LookupForm<RE::TESGlobal>(0xB6F31, "Update.esm");  // SMI_ExhaustionRateMult
			spdlog::info("Survival Mode Improved - SKSE found; scaling its rate multipliers");
		} else {
			g_hunger.rate = dataHandler->LookupForm<RE::TESGlobal>(0x829, ccPlugin);   // Survival_HungerNeedRate
			g_fatigue.rate = dataHandler->LookupForm<RE::TESGlobal>(0x824, ccPlugin);  // Survival_ExhaustionNeedRate
			spdlog::info("Survival Mode Improved - SKSE not found; scaling CC Survival Mode's base rates");
		}
		if (!g_hunger.Ready() || !g_fatigue.Ready()) {
			spdlog::error("Survival Mode's globals weren't found; nothing will change. Is Survival Mode installed?");
			return;
		}
		StartTicking();
	}

	// --- Co-save: a save made mid-adrenaline keeps its debt and normal rates -------

	void SaveNeed(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type, const Need& a_need)
	{
		if (a_intfc->OpenRecord(a_type, kCosaveVersion)) {
			a_intfc->WriteRecordData(a_need.active);
			a_intfc->WriteRecordData(a_need.normal);
			a_intfc->WriteRecordData(a_need.written);
			a_intfc->WriteRecordData(a_need.mult);
			a_intfc->WriteRecordData(a_need.debt);
		}
	}

	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		SaveNeed(a_intfc, kRecordHunger, g_hunger);
		SaveNeed(a_intfc, kRecordFatigue, g_fatigue);
	}

	void OnLoad(SKSE::SerializationInterface* a_intfc)
	{
		std::uint32_t type, version, length;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			Need* need = type == kRecordHunger ? &g_hunger : type == kRecordFatigue ? &g_fatigue : nullptr;
			if (!need || version != kCosaveVersion) {
				continue;
			}
			a_intfc->ReadRecordData(need->active);
			a_intfc->ReadRecordData(need->normal);
			a_intfc->ReadRecordData(need->written);
			a_intfc->ReadRecordData(need->mult);
			a_intfc->ReadRecordData(need->debt);
			need->mult = std::clamp(need->mult, kMinMult, 1.0f);
		}
	}

	void OnRevert(SKSE::SerializationInterface*)
	{
		for (Need* need : { &g_hunger, &g_fatigue }) {
			need->active = false;
			need->mult = 1.0f;
			need->debt = 0.0f;
			need->lastValue.reset();
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			OnDataLoaded();
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SetupLog();
	SKSE::Init(a_skse);
	spdlog::info("Survival Adrenaline loaded");

	auto serialization = SKSE::GetSerializationInterface();
	serialization->SetUniqueID(kCosaveID);
	serialization->SetSaveCallback(OnSave);
	serialization->SetLoadCallback(OnLoad);
	serialization->SetRevertCallback(OnRevert);

	if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
		spdlog::error("Failed to register message listener");
		return false;
	}
	return true;
}
