#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <SimpleIni.h>

using namespace std::literals;

// ── Config ────────────────────────────────────────────────────────────────────

struct Config {
    // Distance (in Skyrim units) from the camera at which culling starts.
    // ~128 units ≈ one "step" in the editor. Tweak to taste.
    float cullRadius    = 128.f;

    // "hard" = instant AppCulled toggle
    // "fade"  = write alpha to BSLightingShaderProperty
    // "both"  = fade inside inner radius, hard-cull inside cullRadius * hardRatio
    enum class Mode { Hard, Fade, Both } mode = Mode::Both;
    float hardRatio     = 0.5f; // fraction of cullRadius below which hard-cull fires

    // Per-type toggles
    bool cullStatics    = true;  // TESObjectSTAT, furniture, containers, etc.
    bool cullTrees      = true;  // TESObjectTREE
    bool cullFlora      = true;  // TESFlora (harvestable plants / small bushes)

    // Only cull when the player is in 3rd person
    bool only3rdPerson  = true;

    // How often (in frames) to re-scan nearby refs.
    // 1 = every frame (most responsive, slightly more CPU).
    // 3 = every 3rd frame (good default).
    int  scanInterval   = 3;
} g_cfg;

void LoadConfig()
{
    CSimpleIniA ini;
    ini.SetUnicode();
    const auto path = SKSE::GetPluginConfigPath("CameraCull"sv);
    if (ini.LoadFile(path.c_str()) < 0)
        return;

    g_cfg.cullRadius   = static_cast<float>(ini.GetDoubleValue("General", "fCullRadius",   g_cfg.cullRadius));
    g_cfg.hardRatio    = static_cast<float>(ini.GetDoubleValue("General", "fHardRatio",    g_cfg.hardRatio));
    g_cfg.scanInterval = static_cast<int>  (ini.GetLongValue  ("General", "iScanInterval", g_cfg.scanInterval));
    g_cfg.only3rdPerson= ini.GetBoolValue  ("General", "bOnly3rdPerson", g_cfg.only3rdPerson);

    const std::string modeStr = ini.GetValue("General", "sMode", "both");
    if      (modeStr == "hard") g_cfg.mode = Config::Mode::Hard;
    else if (modeStr == "fade") g_cfg.mode = Config::Mode::Fade;
    else                        g_cfg.mode = Config::Mode::Both;

    g_cfg.cullStatics  = ini.GetBoolValue("Types", "bCullStatics", g_cfg.cullStatics);
    g_cfg.cullTrees    = ini.GetBoolValue("Types", "bCullTrees",   g_cfg.cullTrees);
    g_cfg.cullFlora    = ini.GetBoolValue("Types", "bCullFlora",   g_cfg.cullFlora);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

// Returns true if this ref's base form is one of the types we care about.
static bool ShouldConsiderRef(RE::TESObjectREFR* ref)
{
    if (!ref || ref->IsDeleted() || ref->IsDisabled())
        return false;

    const auto base = ref->GetBaseObject();
    if (!base)
        return false;

    const auto type = base->GetFormType();

    if (g_cfg.cullStatics &&
        (type == RE::FormType::Static      ||
         type == RE::FormType::MovableStatic ||
         type == RE::FormType::Furniture   ||
         type == RE::FormType::Container))
        return true;

    if (g_cfg.cullTrees && type == RE::FormType::Tree)
        return true;

    if (g_cfg.cullFlora && type == RE::FormType::Flora)
        return true;

    return false;
}

// Set (or clear) alpha on every BSLightingShaderProperty in the node tree.
// alpha == 1.0 restores full visibility; alpha == 0.0 is fully invisible.
static void SetNodeAlpha(RE::NiAVObject* node, float alpha)
{
    if (!node)
        return;

    // Walk BSGeometry leaves via CullGeometry/NiNode cast
    if (auto* niNode = node->AsNode()) {
        for (auto& child : niNode->GetChildren()) {
            if (child)
                SetNodeAlpha(child.get(), alpha);
        }
    }

    // If there's a shader property on this object, adjust its alpha
    if (auto* geom = node->AsGeometry()) {
        if (auto* effect = geom->properties[RE::BSGeometry::States::kEffect]) {
            if (auto* shader = netimmerse_cast<RE::BSLightingShaderProperty*>(effect.get())) {
                shader->alpha = alpha;
            }
        }
    }
}

// ── Per-ref state tracking ────────────────────────────────────────────────────
// We track which refs we've touched so we can restore them cleanly.

struct RefState {
    bool  hardCulled = false;
    float lastAlpha  = 1.f;
};

static std::unordered_map<RE::FormID, RefState> g_touched;

static void RestoreRef(RE::TESObjectREFR* ref, RE::NiAVObject* node)
{
    auto it = g_touched.find(ref->GetFormID());
    if (it == g_touched.end())
        return;

    if (it->second.hardCulled) {
        node->SetAppCulled(false);
    } else if (it->second.lastAlpha < 1.f) {
        SetNodeAlpha(node, 1.f);
    }
    g_touched.erase(it);
}

// ── Main update ───────────────────────────────────────────────────────────────

static void UpdateCull()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player)
        return;

    // Optionally skip in first-person
    if (g_cfg.only3rdPerson) {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam || cam->IsInFirstPerson())
            return;
    }

    // Get camera world position
    RE::NiPoint3 camPos;
    {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam || !cam->cameraRoot)
            return;
        camPos = cam->cameraRoot->world.translate;
    }

    auto* cell = player->GetParentCell();
    if (!cell)
        return;

    const float outerSq = g_cfg.cullRadius * g_cfg.cullRadius;
    const float innerSq = outerSq * (g_cfg.hardRatio * g_cfg.hardRatio);

    // Track which refs are in range this frame so we can restore out-of-range ones
    std::unordered_set<RE::FormID> inRangeThisFrame;

    cell->ForEachReferenceInRange(camPos, g_cfg.cullRadius,
        [&](RE::TESObjectREFR& ref) -> RE::BSContainer::ForEachResult {
            if (!ShouldConsiderRef(&ref))
                return RE::BSContainer::ForEachResult::kContinue;

            auto* node = ref.Get3D();
            if (!node)
                return RE::BSContainer::ForEachResult::kContinue;

            const float distSq = camPos.GetSquaredDistance(ref.GetPosition());
            inRangeThisFrame.insert(ref.GetFormID());

            RefState& state = g_touched[ref.GetFormID()];

            if (g_cfg.mode == Config::Mode::Hard ||
                (g_cfg.mode == Config::Mode::Both && distSq <= innerSq))
            {
                // Hard cull
                if (!state.hardCulled) {
                    // Preserve alpha state before hard-culling
                    state.hardCulled = true;
                    node->SetAppCulled(true);
                }
            }
            else
            {
                // Fade: alpha = 0 at camera, 1 at cullRadius
                const float dist   = std::sqrt(distSq);
                const float alpha  = dist / g_cfg.cullRadius;
                const float clamped = std::clamp(alpha, 0.f, 1.f);

                if (state.hardCulled) {
                    node->SetAppCulled(false);
                    state.hardCulled = false;
                }

                if (std::abs(clamped - state.lastAlpha) > 0.01f) {
                    SetNodeAlpha(node, clamped);
                    state.lastAlpha = clamped;
                }
            }

            return RE::BSContainer::ForEachResult::kContinue;
        });

    // Restore any refs that left the radius since last frame
    for (auto it = g_touched.begin(); it != g_touched.end(); ) {
        if (inRangeThisFrame.count(it->first) == 0) {
            // Look up the ref to restore its node
            auto* form = RE::TESForm::LookupByID<RE::TESObjectREFR>(it->first);
            if (form) {
                auto* node = form->Get3D();
                if (node) {
                    if (it->second.hardCulled)
                        node->SetAppCulled(false);
                    else if (it->second.lastAlpha < 1.f)
                        SetNodeAlpha(node, 1.f);
                }
            }
            it = g_touched.erase(it);
        } else {
            ++it;
        }
    }
}

// ── Frame hook ────────────────────────────────────────────────────────────────
// Hook Main::Update (runs once per rendered frame on the game thread).

struct MainUpdateHook {
    static void thunk(RE::Main* a_this, float a_delta)
    {
        func(a_this, a_delta);

        static int frameCount = 0;
        if (++frameCount >= g_cfg.scanInterval) {
            frameCount = 0;
            UpdateCull();
        }
    }

    static inline REL::Relocation<decltype(thunk)> func;
};

// ── SKSE plumbing ─────────────────────────────────────────────────────────────

void OnSKSEMessage(SKSE::MessagingInterface::Message* msg)
{
    if (msg->type == SKSE::MessagingInterface::kDataLoaded) {
        LoadConfig();
        logger::info("CameraCull: config loaded. radius={:.0f} mode={} interval={}",
            g_cfg.cullRadius,
            g_cfg.mode == Config::Mode::Hard ? "hard" :
            g_cfg.mode == Config::Mode::Fade ? "fade" : "both",
            g_cfg.scanInterval);
    }

    // On any cell change or load, restore all touched refs to prevent
    // permanent culling across loads.
    if (msg->type == SKSE::MessagingInterface::kPreLoadGame ||
        msg->type == SKSE::MessagingInterface::kNewGame)
    {
        for (auto& [id, state] : g_touched) {
            auto* form = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
            if (form) {
                auto* node = form->Get3D();
                if (node) {
                    if (state.hardCulled)
                        node->SetAppCulled(false);
                    else if (state.lastAlpha < 1.f)
                        SetNodeAlpha(node, 1.f);
                }
            }
        }
        g_touched.clear();
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);

    // Set up logging to My Games/Skyrim Special Edition/SKSE/CameraCull.log
    auto path = logger::log_directory();
    if (!path) {
        SKSE::stl::report_and_fail("Failed to find SKSE log directory.");
    }
    *path /= "CameraCull.log"sv;
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto log  = std::make_shared<spdlog::logger>("CameraCull", std::move(sink));
    log->set_level(spdlog::level::debug);
    spdlog::set_default_logger(std::move(log));
    spdlog::flush_on(spdlog::level::debug);

    logger::info("CameraCull v{} loading.", Plugin::Version.string());

    // Hook Main::Update — Address ID 35551 in AE 1.6.1170
    // (RE::Main::Update, offset into the function that runs per-frame)
    REL::Relocation<std::uintptr_t> mainUpdateTarget{ REL::RelocationID(35551, 36544) };
    SKSE::AllocTrampoline(14);
    MainUpdateHook::func = mainUpdateTarget.address();
    REL::safe_write(
        mainUpdateTarget.address(),
        SKSE::Assembly::MakePatch<5>(
            SKSE::Assembly::Op::Call,
            stl::unrestricted_cast<std::uintptr_t>(MainUpdateHook::thunk)));

    // Register for SKSE messages
    const auto msgInterface = SKSE::GetMessagingInterface();
    msgInterface->RegisterListener(OnSKSEMessage);

    return true;
}
