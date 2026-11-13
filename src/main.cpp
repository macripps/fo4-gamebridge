// GameBridge: lets a local tool (such as the MCP server in server.py) talk to a running game.
//
// Listens on 127.0.0.1:47830 on its own thread, so "ping" still answers when Papyrus is jammed. Anything that touches
// game state goes through F4SE's task queue and runs on the main thread (BTI calls the console from a background
// thread, which isn't safe). It runs whatever console command it's sent, so use it on a test profile, not a playthrough.
//
// Protocol, one line each way:
//   request   id<TAB>op[<TAB>arg...]
//   reply     {"id":"...","ok":true,"result":"..."}
//   ops       ping | console <command> | vars <formid hex> <script> | player | ref <formid hex> | actor <formid hex>
//             | near <radius> [<base form type> [<x> <y> <z>]] | perf [<seconds>] | place <formid hex> [<distance>]
//             | screenshot | quest <formid hex or editor ID> | questlog [<count>] | global <formid hex or editor ID>
//             | camera status|free|release|stop | camera look <point> [<s>] | camera fly <point> <s> [<look point>]
//             | camera orbit <center> <radius> <height> <s> [<turns> [<aim height> [<start angle>]]]
//             | camera path <s> <look point or -> <point>...      (a point is x,y,z or a reference's FormID)

#include <winsock2.h>
#include <ws2tcpip.h>
#undef ERROR  // wingdi.h's ERROR breaks REX::ERROR

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace Bridge
{
    constexpr std::uint16_t kPort = 47830;
    constexpr auto          kMainThreadTimeout = std::chrono::seconds(15);

    std::atomic_bool gameDataReady{ false };
    std::string      runtimeVersion;

    struct Reply
    {
        bool        ok{ false };
        std::string text;
    };

    std::string JsonEscape(std::string_view a_text)
    {
        std::string out;
        out.reserve(a_text.size() + 16);
        for (const unsigned char c : a_text) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x80) {
                    // Game text is cp1252: keep the byte's value rather than sending invalid UTF-8.
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out += static_cast<char>(c);
                }
            }
        }
        return out;
    }

    std::vector<std::string> Split(const std::string& a_line, char a_separator)
    {
        std::vector<std::string> parts;
        std::stringstream        stream{ a_line };
        std::string              part;
        while (std::getline(stream, part, a_separator)) {
            parts.push_back(part);
        }
        return parts;
    }

    // Runs a_work on the main thread and waits for it. On a timeout the task is still queued and may run later.
    template <class F>
    Reply OnMainThread(F a_work)
    {
        const auto tasks = F4SE::GetTaskInterface();
        if (!tasks) {
            return { false, "no F4SE task interface" };
        }

        auto promise = std::make_shared<std::promise<Reply>>();
        auto future = promise->get_future();
        tasks->AddTask([promise, a_work]() {
            promise->set_value(a_work());
        });

        if (future.wait_for(kMainThreadTimeout) != std::future_status::ready) {
            return { false, "timed out waiting for the main thread; the task is still queued and may run later" };
        }
        return future.get();
    }

    Reply RunConsole(const std::string& a_command)
    {
        const auto log = RE::ConsoleLog::GetSingleton();
        if (!log) {
            return { false, "no ConsoleLog yet" };
        }

        const auto  read = [&]() { return std::string{ log->buffer.empty() ? "" : log->buffer.c_str() }; };
        const auto  before = read();
        RE::Console::ExecuteCommand(a_command.c_str());
        const auto after = read();

        // The buffer is trimmed when it fills, so "before" isn't always a prefix of "after".
        if (after.size() >= before.size() && after.compare(0, before.size(), before) == 0) {
            return { true, after.substr(before.size()) };
        }
        const auto tail = after.size() > 4096 ? after.size() - 4096 : 0;
        return { true, after.substr(tail) };
    }

    std::string Describe(RE::BSScript::Variable& a_var, int a_depth = 0)
    {
        using RE::BSScript::Variable;

        if (a_var.is<std::nullptr_t>()) {
            return "None";
        }
        if (a_var.is<std::int32_t>()) {
            return std::to_string(RE::BSScript::get<std::int32_t>(a_var));
        }
        if (a_var.is<float>()) {
            return std::format("{}", RE::BSScript::get<float>(a_var));
        }
        if (a_var.is<bool>()) {
            return RE::BSScript::get<bool>(a_var) ? "True" : "False";
        }
        if (a_var.is<RE::BSFixedString>()) {
            return std::format("\"{}\"", RE::BSScript::get<RE::BSFixedString>(a_var).c_str());
        }
        if (a_var.is<RE::BSScript::Object>()) {
            const auto& object = RE::BSScript::get<RE::BSScript::Object>(a_var);
            if (!object) {
                return "None";
            }
            return std::format("[{} handle={:016X}]", object->GetTypeInfo()->GetName(), object->GetHandle());
        }
        if (a_var.is<Variable>()) {
            const auto inner = RE::BSScript::get<Variable>(a_var);
            return inner && a_depth < 2 ? "var:" + Describe(*inner, a_depth + 1) : "var";
        }
        if (a_var.is<RE::BSScript::Struct>()) {
            return "struct";
        }
        if (a_var.is<RE::BSScript::Array>()) {
            const auto array = RE::BSScript::get<RE::BSScript::Array>(a_var);
            return array ? std::format("array[{}]", array->size()) : "array[None]";
        }
        return "?";
    }

    // Every variable of a script on a form, including private ones, read through the VM's own locked accessor.
    Reply ReadVars(const std::string& a_formID, const std::string& a_script)
    {
        const auto gameVM = RE::GameVM::GetSingleton();
        const auto vm = gameVM ? gameVM->GetVM() : nullptr;
        if (!vm) {
            return { false, "no VM" };
        }

        const auto form = RE::TESForm::GetFormByID(static_cast<RE::TESFormID>(std::stoul(a_formID, nullptr, 16)));
        if (!form) {
            return { false, "no form " + a_formID };
        }

        auto&      policy = vm->GetObjectHandlePolicy();
        const auto handle = policy.GetHandleForObject(static_cast<std::uint32_t>(form->GetFormType()), form);

        RE::BSTSmartPointer<RE::BSScript::Object> object;
        if (!vm->FindBoundObject(handle, a_script.c_str(), true, object, false) || !object) {
            return { false, "no script " + a_script + " on " + a_formID };
        }

        std::vector<RE::BSScript::ObjectTypeInfo*> chain;
        for (auto type = object->GetTypeInfo(); type; type = type->parentTypeInfo.get()) {
            chain.insert(chain.begin(), type);
        }

        std::string   out;
        std::uint32_t index = 0;
        for (const auto type : chain) {
            const auto variables = type->GetVariableIter();
            for (std::uint32_t i = 0; i < type->GetNumVariables(); ++i, ++index) {
                RE::BSScript::Variable value;
                if (vm->GetVariableValue(object, index, value)) {
                    out += std::format("{} = {}\n", variables[i].name.c_str(), Describe(value));
                }
            }
        }
        return { true, out };
    }

    constexpr float kRadToDeg = 57.2957795f;

    // "0001A2B3 EditorID [Plugin.esm] "Display name"": the editor ID only when the game kept it (many form types drop it
    // at runtime), the plugin that defines the form (none for forms created in game, FF...), and the display name if any.
    std::string Name(const RE::TESForm* a_form)
    {
        if (!a_form) {
            return "None";
        }
        auto out = std::format("{:08X}", a_form->GetFormID());
        if (const auto edid = a_form->GetFormEditorID(); edid && *edid) {
            out += std::format(" {}", edid);
        }
        if (const auto file = a_form->GetFile(0)) {
            out += std::format(" [{}]", file->GetFilename());
        }
        if (const auto full = RE::TESFullName::GetFullName(*a_form); !full.empty()) {
            out += std::format(" \"{}\"", full);
        }
        return out;
    }

    std::string Where(const RE::TESObjectREFR* a_ref)
    {
        const auto& d = a_ref->data;
        return std::format("pos {:.2f} {:.2f} {:.2f} rot {:.1f} {:.1f} {:.1f}", d.location.x, d.location.y, d.location.z,
            d.angle.x * kRadToDeg, d.angle.y * kRadToDeg, d.angle.z * kRadToDeg);
    }

    // The player's cell, position and rotation (degrees). Replaces "save, then read the save".
    Reply Player()
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return { false, "no player" };
        }
        return { true, std::format("cell {}\n{}", Name(player->GetParentCell()), Where(player)) };
    }

    // One reference: base, place, state and every linked ref (keyword -> target), including ones added by script.
    Reply RefInfo(const std::string& a_formID)
    {
        const auto ref = RE::TESForm::GetFormByID(static_cast<RE::TESFormID>(std::stoul(a_formID, nullptr, 16)));
        const auto refr = ref ? ref->As<RE::TESObjectREFR>() : nullptr;
        if (!refr) {
            return { false, "no reference " + a_formID };
        }

        const auto base = refr->GetObjectReference();
        std::string out = std::format("ref {}\nbase {} ({})\ncell {}\n{}\ndisabled {}, deleted {}, 3D loaded {}\n",
            Name(refr), Name(base), base ? base->GetFormTypeString() : "?", Name(refr->GetParentCell()), Where(refr),
            refr->IsDisabled(), refr->GetDelete(), refr->Get3D() != nullptr);

        if (refr->extraList) {
            if (const auto links = refr->extraList->GetByType<RE::ExtraLinkedRef>()) {
                for (const auto& link : links->linkedRefs) {
                    out += std::format("link {} -> {}{}\n", Name(link.keyword), Name(link.REFR),
                        link.REFR && link.REFR->GetObjectReference() ? " (base " + Name(link.REFR->GetObjectReference()) + ")" : "");
                }
            }
        }
        return { true, out };
    }

    // One actor's AI and appearance: life state, talking to the player, the furniture it is using, the AI package it
    // is running (and that package's target), and the items it has equipped. Replaces screenshots and guessing.
    // Reads data members only. The first version also called virtual functions (IsInCombat, IsTalking,
    // DoGetSitSleepState) and AIProcess::GetOccupiedFurniture by address; the game crashed soon after (2026-11-10, no
    // crash log), so a vtable slot or address was probably wrong for 1.11.240. Don't add calls here without testing.
    Reply ActorInfo(const std::string& a_formID)
    {
        const auto form = RE::TESForm::GetFormByID(static_cast<RE::TESFormID>(std::stoul(a_formID, nullptr, 16)));
        const auto actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor) {
            return { false, "no actor reference " + a_formID };
        }

        static constexpr const char* kLife[] = { "alive", "dying", "dead", "unconscious", "reanimate", "recycle",
            "restrained", "essential down", "bleedout" };

        const bool loaded = actor->Get3D() != nullptr;
        std::string out = std::format("actor {}\nbase {}\ncell {}\n{}{}\n", Name(actor), Name(actor->GetObjectReference()),
            Name(actor->GetParentCell()), Where(actor), loaded ? "" : " (3D not loaded: the position may be stale)");

        const auto life = static_cast<std::uint32_t>(actor->lifeState);
        out += std::format("life {}, disabled {}, talking to player {}\n",
            life < std::size(kLife) ? kLife[life] : std::to_string(life), actor->IsDisabled(), actor->talkingToPlayer != 0);

        const auto refLine = [](const char* a_label, const RE::ObjectRefHandle& a_handle) {
            const auto ref = a_handle.get();
            return ref ? std::format("{} {} (base {}) {}\n", a_label, Name(ref.get()), Name(ref->GetObjectReference()), Where(ref.get())) :
                         std::string{};
        };

        if (const auto process = actor->currentProcess) {
            const auto& current = process->currentPackage;
            out += std::format("package {}\n", Name(current.package));
            out += refLine("package target", current.target);
            if (const auto high = process->middleHigh) {
                out += refLine("using furniture", high->occupiedFurniture);
                out += refLine("heading to furniture", high->currentFurniture);
            } else {
                out += "low process (far away): no furniture data\n";
            }
        } else {
            out += "no AI process (the actor isn't being processed)\n";
        }

        std::string worn;
        if (const auto inventory = actor->inventoryList) {
            for (const auto& item : inventory->data) {
                for (auto stack = item.stackData.get(); stack; stack = stack->nextStack.get()) {
                    if (stack->IsEquipped()) {
                        worn += std::format("  {} ({})\n", Name(item.object), item.object ? item.object->GetFormTypeString() : "?");
                        break;
                    }
                }
            }
        }
        out += worn.empty() ? "equipped: nothing\n" : "equipped:\n" + worn;
        return { true, out };
    }

    // References within a_radius of a point (default: the player), in the player's interior cell or the loaded exterior
    // cells, nearest first. a_type filters by
    // the base's form type ("LIGH", "STAT", "CONT", ...); empty means all.
    Reply Near(float a_radius, const std::string& a_type, std::optional<RE::NiPoint3> a_origin)
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        const auto cell = player ? player->GetParentCell() : nullptr;
        if (!cell) {
            return { false, "no player cell" };
        }
        const auto origin = a_origin.value_or(player->GetPosition());

        const auto tes = RE::TES::GetSingleton();
        if (!tes || a_radius <= 0.0f) {
            return { false, "no TES, or radius not above 0" };
        }

        // TES walks the interior cell, or every attached exterior cell in the loaded grid (plus the sky cell).
        std::vector<std::pair<float, std::string>> rows;
        tes->ForEachReferenceInRange(origin, a_radius, [&](RE::TESObjectREFR* a_ref) {
            const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
            if (base && a_ref != player && (a_type.empty() || a_type == base->GetFormTypeString())) {
                const auto distance = origin.GetDistance(a_ref->GetPosition());
                rows.emplace_back(distance, std::format("{:08X} base {} ({}) dist {:.0f} {}{}", a_ref->GetFormID(), Name(base),
                    base->GetFormTypeString(), distance, Where(a_ref), a_ref->IsDisabled() ? " DISABLED" : ""));
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });

        std::ranges::sort(rows, {}, &std::pair<float, std::string>::first);
        std::string out = std::format("{} references\n", rows.size());
        for (std::size_t i = 0; i < rows.size() && i < 300; ++i) {
            out += rows[i].second + "\n";
        }
        return { true, out };
    }

    // Places a new reference of a base object a_distance in front of the player, upright, turned the way the player
    // faces (so a static's front, its -Y side, faces the player). Replaces placeatme plus setpos/setangle by hand.
    Reply Place(const std::string& a_formID, float a_distance)
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        const auto cell = player ? player->GetParentCell() : nullptr;
        const auto handler = RE::TESDataHandler::GetSingleton();
        if (!cell || !handler) {
            return { false, "no player cell" };
        }
        const auto form = RE::TESForm::GetFormByID(static_cast<RE::TESFormID>(std::stoul(a_formID, nullptr, 16)));
        const auto base = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (!base) {
            return { false, "no placeable base object " + a_formID };
        }

        const auto heading = player->data.angle.z;
        const auto at = player->GetPosition();
        RE::NEW_REFR_DATA data;
        data.location = RE::NiPoint3{ at.x + std::sin(heading) * a_distance, at.y + std::cos(heading) * a_distance, at.z };
        data.direction = RE::NiPoint3{ 0.0f, 0.0f, heading };
        data.object = base;
        if (cell->IsInterior()) {
            data.interior = cell;
        } else {
            data.world = cell->worldSpace;
        }

        const auto ref = handler->CreateReferenceAtLocation(data).get();
        if (!ref) {
            return { false, "the game didn't create the reference" };
        }
        return { true, std::format("placed {}\n{}", Name(ref.get()), Where(ref.get())) };
    }

    // Queues a screenshot, as the screenshot key does. It's saved to the game's sScreenShotBaseName (Fallout4Prefs.ini).
    Reply Screenshot()
    {
        const auto controls = RE::MenuControls::GetSingleton();
        if (!controls) {
            return { false, "no MenuControls" };
        }
        return controls->QueueScreenshot() ? Reply{ true, "screenshot queued" } :
                                             Reply{ false, "a screenshot is already queued, or screenshots are off" };
    }

    // A form by load-order FormID (8 hex digits) or editor ID (forms that keep theirs at runtime: quests, globals...).
    RE::TESForm* FindForm(const std::string& a_name)
    {
        if (a_name.size() == 8 && std::ranges::all_of(a_name, [](unsigned char c) { return std::isxdigit(c) != 0; })) {
            return RE::TESForm::GetFormByID(static_cast<RE::TESFormID>(std::stoul(a_name, nullptr, 16)));
        }
        return RE::TESForm::GetFormByEditorID(RE::BSFixedString{ a_name.c_str() });
    }

    // Quest stage and start/stop events from the engine, newest last, whether or not any script hears them (a stage
    // with no log entry sets but sends Papyrus no OnStageSet). Kept from game start, across loads.
    constexpr std::size_t kQuestLogSize = 500;
    std::mutex            questLogLock;
    struct QuestLogEntry
    {
        RE::TESFormID quest{ 0 };
        std::string   text;
    };
    std::deque<QuestLogEntry> questLog;

    void AddQuestLog(RE::TESFormID a_quest, const std::string& a_what)
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        auto line = std::format("{:02}:{:02}:{:02}.{:03} {} {}", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
            Name(RE::TESForm::GetFormByID(a_quest)), a_what);
        const std::lock_guard lock{ questLogLock };
        questLog.push_back({ a_quest, std::move(line) });
        if (questLog.size() > kQuestLogSize) {
            questLog.pop_front();
        }
    }

    class QuestEvents final :
        public RE::BSTEventSink<RE::TESQuestStageEvent>,
        public RE::BSTEventSink<RE::TESQuestStartStopEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStageEvent& a_event, RE::BSTEventSource<RE::TESQuestStageEvent>*) override
        {
            AddQuestLog(a_event.questFormID, std::format("stage {} (log entry {})", a_event.stageID, a_event.itemID));
            return RE::BSEventNotifyControl::kContinue;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStartStopEvent& a_event, RE::BSTEventSource<RE::TESQuestStartStopEvent>*) override
        {
            AddQuestLog(a_event.questFormID, a_event.started ? "started" : (a_event.failed ? "stopped (failed)" : "stopped"));
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    QuestEvents questEvents;

    std::string QuestLogLines(RE::TESFormID a_quest, std::size_t a_count)
    {
        const std::lock_guard lock{ questLogLock };
        std::vector<const std::string*> lines;
        for (auto it = questLog.rbegin(); it != questLog.rend() && lines.size() < a_count; ++it) {
            if (!a_quest || it->quest == a_quest) {
                lines.push_back(&it->text);
            }
        }
        std::string out;
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            out += **it + "\n";
        }
        return out;
    }

    // A quest's state: run flags, current stage, objectives (live state and text), aliases and what each holds, and
    // this session's stage events for it. Which stages are done isn't readable (TESQuestStage's layout isn't known),
    // but the current stage is the last one set. Data members only, plus TESQuest::GetAliasedRef.
    Reply QuestInfo(const std::string& a_name)
    {
        const auto form = FindForm(a_name);
        const auto quest = form ? form->As<RE::TESQuest>() : nullptr;
        if (!quest) {
            return { false, "no quest " + a_name };
        }

        static constexpr std::pair<std::uint16_t, const char*> kQuestFlags[] = { { 0x1, "running" }, { 0x2, "completed" },
            { 0x40, "failed" }, { 0x80, "stage wait" }, { 0x100, "run once" }, { 0x800, "active (tracked)" } };
        const auto  flags = quest->data.flags;
        std::string flagText;
        for (const auto& [bit, label] : kQuestFlags) {
            if (flags & bit) {
                flagText += flagText.empty() ? label : std::string{ ", " } + label;
            }
        }
        std::string out = std::format("quest {}\nflags {:04X}: {}\ncurrent stage {}, already run {}\n", Name(quest), flags,
            flagText.empty() ? "not running" : flagText, quest->currentStage, quest->alreadyRun);

        static constexpr const char* kStates[] = { "dormant", "displayed", "completed", "completed (displayed)", "failed",
            "failed (displayed)" };
        std::vector<const RE::BGSQuestObjective*> objectives;
        for (const auto objective : quest->objectives) {
            if (objective) {
                objectives.push_back(objective);
            }
        }
        std::ranges::sort(objectives, {}, &RE::BGSQuestObjective::index);
        out += std::format("objectives ({}):\n", objectives.size());
        for (const auto objective : objectives) {
            const auto state = static_cast<unsigned char>(objective->state);
            out += std::format("  {} {} \"{}\"\n", objective->index, state < std::size(kStates) ? kStates[state] : std::to_string(state),
                objective->displayText.c_str() ? objective->displayText.c_str() : "");
        }

        static constexpr std::pair<std::uint32_t, const char*> kAliasFlags[] = { { 1u << 1, "optional" },
            { 1u << 2, "quest object" }, { 1u << 6, "essential" }, { 1u << 7, "allow disabled" }, { 1u << 10, "protected" } };
        out += std::format("aliases ({}):\n", quest->aliases.size());
        for (const auto alias : quest->aliases) {
            if (!alias) {
                continue;
            }
            std::string aliasFlags;
            for (const auto& [bit, label] : kAliasFlags) {
                if (alias->flags.underlying() & bit) {
                    aliasFlags += std::string{ ", " } + label;
                }
            }
            const auto ref = quest->GetAliasedRef(alias->aliasID).get();
            out += std::format("  {} {}{} -> {}\n", alias->aliasID, alias->aliasName.c_str(), aliasFlags,
                ref ? Name(ref.get()) + (ref->IsDisabled() ? " DISABLED" : "") : "(empty, or not a reference alias)");
        }

        const auto events = QuestLogLines(quest->GetFormID(), 50);
        out += events.empty() ? "stage events this session: none\n" : "stage events this session:\n" + events;
        return { true, out };
    }

    Reply GlobalValue(const std::string& a_name)
    {
        const auto form = FindForm(a_name);
        const auto global = form ? form->As<RE::TESGlobal>() : nullptr;
        if (!global) {
            return { false, "no global " + a_name };
        }
        return { true, std::format("{} = {}", Name(global), global->GetValue()) };
    }

    // ---------------------------------------------------------------- camera
    // Smooth moves of the free camera (tfc). Its pose is plain data (FreeCameraState: translation, rotation), so a move
    // is a write per frame: a pacing thread keeps one main-thread task queued at a time, and each task works out the
    // pose for the moment it runs. Nothing is called by address. The player doesn't move, so in exteriors the camera
    // only sees the cells already loaded around the player.

    struct CameraPose
    {
        RE::NiPoint3 pos;
        float        yaw{ 0.0f };    // radians, 0 = north (+y), clockwise, as a reference's angle z
        float        pitch{ 0.0f };  // radians, positive = looking down, as a reference's angle x
    };

    using CameraTrack = std::function<CameraPose(float)>;  // seconds since the move started -> pose

    std::atomic_uint64_t cameraGeneration{ 0 };
    std::mutex           cameraLock;
    std::string          cameraActivity = "idle";

    constexpr float kPi = 3.14159265358979f;
    constexpr float kDeg = kPi / 180.0f;

    void SetCameraActivity(std::string a_text)
    {
        std::lock_guard lock{ cameraLock };
        cameraActivity = std::move(a_text);
    }

    RE::FreeCameraState* FreeCamera()
    {
        const auto camera = RE::PlayerCamera::GetSingleton();
        if (!camera || !camera->currentState || camera->currentState->id.get() != RE::CameraState::kFree) {
            return nullptr;
        }
        return static_cast<RE::FreeCameraState*>(camera->currentState.get());
    }

    // FreeCameraState::rotation: x = pitch, y = yaw, in radians, the same signs as a reference's angles (checked in game
    // 2026-11-13: entering free mode, the raw pair matched the player's angle x and z).
    CameraPose ReadPose(const RE::FreeCameraState* a_state)
    {
        return { a_state->translation, a_state->rotation.y, a_state->rotation.x };
    }

    void WritePose(RE::FreeCameraState* a_state, const CameraPose& a_pose)
    {
        a_state->translation = a_pose.pos;
        a_state->rotation.x = a_pose.pitch;
        a_state->rotation.y = a_pose.yaw;
    }

    float Smooth(float a_t)
    {
        a_t = std::clamp(a_t, 0.0f, 1.0f);
        return a_t * a_t * (3.0f - 2.0f * a_t);
    }

    float AngleLerp(float a_from, float a_to, float a_t)
    {
        return a_from + std::remainder(a_to - a_from, 2.0f * kPi) * a_t;
    }

    RE::NiPoint3 Lerp(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_t)
    {
        return a_from + (a_to - a_from) * a_t;
    }

    void LookAlong(const RE::NiPoint3& a_direction, CameraPose& a_pose)
    {
        const auto flat = std::sqrt(a_direction.x * a_direction.x + a_direction.y * a_direction.y);
        if (flat < 0.001f && std::abs(a_direction.z) < 0.001f) {
            return;
        }
        a_pose.yaw = std::atan2(a_direction.x, a_direction.y);
        a_pose.pitch = -std::atan2(a_direction.z, flat);
    }

    // "x,y,z" or a reference's FormID (hex). Main thread.
    std::optional<RE::NiPoint3> ParsePoint(const std::string& a_text)
    {
        if (a_text.find(',') != std::string::npos) {
            const auto parts = Split(a_text, ',');
            if (parts.size() != 3) {
                return std::nullopt;
            }
            return RE::NiPoint3{ std::stof(parts[0]), std::stof(parts[1]), std::stof(parts[2]) };
        }
        const auto form = FindForm(a_text);
        const auto refr = form ? form->As<RE::TESObjectREFR>() : nullptr;
        if (!refr) {
            return std::nullopt;
        }
        return refr->GetPosition();
    }

    // Runs a_track for a_seconds. A newer move, "stop" or leaving the free camera ends it.
    void Animate(CameraTrack a_track, float a_seconds, std::string a_label)
    {
        const auto generation = ++cameraGeneration;
        SetCameraActivity(a_label);
        auto track = std::make_shared<CameraTrack>(std::move(a_track));
        std::thread([track, a_seconds, a_label, generation]() {
            const auto start = std::chrono::steady_clock::now();
            auto       pending = std::make_shared<std::atomic_bool>(false);
            auto       done = std::make_shared<std::atomic_bool>(false);
            const auto tasks = F4SE::GetTaskInterface();
            while (cameraGeneration == generation && !*done) {
                if (!pending->exchange(true)) {
                    tasks->AddTask([track, a_seconds, start, pending, done, generation]() {
                        if (cameraGeneration == generation) {
                            const auto elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
                            if (const auto state = FreeCamera()) {
                                WritePose(state, (*track)((std::min)(elapsed, a_seconds)));
                                if (elapsed >= a_seconds) {
                                    *done = true;
                                }
                            } else {
                                SetCameraActivity("stopped: the camera left free mode");
                                ++cameraGeneration;
                            }
                        }
                        *pending = false;
                    });
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (cameraGeneration == generation) {
                SetCameraActivity("idle (last: " + a_label + ")");
            }
        }).detach();
    }

    std::string PoseText(const CameraPose& a_pose)
    {
        return std::format("pos {:.0f} {:.0f} {:.0f}, yaw {:.1f}, pitch {:.1f}", a_pose.pos.x, a_pose.pos.y,
            a_pose.pos.z, a_pose.yaw / kDeg, a_pose.pitch / kDeg);
    }

    Reply CameraStatus()
    {
        const auto camera = RE::PlayerCamera::GetSingleton();
        if (!camera || !camera->currentState) {
            return { false, "no camera" };
        }
        std::string activity;
        {
            std::lock_guard lock{ cameraLock };
            activity = cameraActivity;
        }
        const auto id = static_cast<int>(camera->currentState->id.get());
        if (const auto state = FreeCamera()) {
            return { true, std::format("free camera: {} (raw rotation {:.3f} {:.3f})\nmove: {}", PoseText(ReadPose(state)),
                               state->rotation.x, state->rotation.y, activity) };
        }
        return { true, std::format("camera state {} (not free; \"free\" switches to it)\nmove: {}", id, activity) };
    }

    // Main thread. a_args after the "camera" op: a subcommand and its arguments.
    Reply Camera(const std::vector<std::string>& a_args)
    {
        const auto sub = a_args.empty() ? std::string{ "status" } : a_args[0];
        const auto arg = [&](std::size_t i) { return i < a_args.size() ? a_args[i] : std::string{}; };
        const auto point = [&](std::size_t i) {
            const auto p = ParsePoint(arg(i));
            if (!p) {
                throw std::runtime_error("not a point (x,y,z) or reference: \"" + arg(i) + "\"");
            }
            return *p;
        };

        if (sub == "status") {
            return CameraStatus();
        }
        if (sub == "stop") {
            ++cameraGeneration;
            SetCameraActivity("stopped");
            return CameraStatus();
        }
        if (sub == "free") {
            if (!FreeCamera()) {
                RunConsole("tfc");
            }
            return { true, "free camera on (it takes a frame; check with status)" };
        }
        if (sub == "release") {
            ++cameraGeneration;
            SetCameraActivity("idle");
            if (FreeCamera()) {
                RunConsole("tfc");
            }
            return { true, "camera back with the player" };
        }

        const auto state = FreeCamera();
        if (!state) {
            return { false, "the camera isn't free: run \"free\" first" };
        }
        const auto from = ReadPose(state);

        if (sub == "look") {  // look <point> [<seconds>]
            CameraPose to = from;
            LookAlong(point(1) - from.pos, to);
            const auto seconds = arg(2).empty() ? 0.0f : std::stof(arg(2));
            if (seconds <= 0.0f) {
                WritePose(state, to);
                return { true, PoseText(to) };
            }
            Animate([from, to, seconds](float a_time) {
                const auto t = Smooth(a_time / seconds);
                return CameraPose{ from.pos, AngleLerp(from.yaw, to.yaw, t), from.pitch + (to.pitch - from.pitch) * t };
            },
                seconds, "look");
            return { true, "turning to " + PoseText(to) };
        }

        if (sub == "fly") {  // fly <point> <seconds> [<look point>]
            const auto target = point(1);
            const auto seconds = (std::max)(0.1f, std::stof(arg(2)));
            const auto look = arg(3).empty() ? std::optional<RE::NiPoint3>{} : std::optional{ point(3) };
            CameraPose to{ target, from.yaw, from.pitch };
            if (look) {
                LookAlong(*look - target, to);
            }
            Animate([from, to, look, seconds](float a_time) {
                const auto t = Smooth(a_time / seconds);
                CameraPose pose{ Lerp(from.pos, to.pos, t), AngleLerp(from.yaw, to.yaw, t),
                    from.pitch + (to.pitch - from.pitch) * t };
                if (look && t > 0.0f) {
                    // Keep the target in view the whole way, blending in from the starting view.
                    CameraPose aimed = pose;
                    LookAlong(*look - pose.pos, aimed);
                    const auto blend = Smooth(a_time / (std::min)(seconds, 1.0f));
                    pose.yaw = AngleLerp(pose.yaw, aimed.yaw, blend);
                    pose.pitch = pose.pitch + (aimed.pitch - pose.pitch) * blend;
                }
                return pose;
            },
                seconds, "fly");
            return { true, std::format("flying to {:.0f} {:.0f} {:.0f} over {:.1f} s", target.x, target.y, target.z, seconds) };
        }

        if (sub == "orbit") {  // orbit <center> <radius> <height> <seconds> [<turns>] [<aim height>] [<start angle>]
            const auto center = point(1);
            const auto radius = std::stof(arg(2));
            const auto height = std::stof(arg(3));
            const auto seconds = (std::max)(0.5f, std::stof(arg(4)));
            const auto turns = arg(5).empty() ? 1.0f : std::stof(arg(5));
            const auto aim = arg(6).empty() ? 0.0f : std::stof(arg(6));
            // Start from the camera's bearing round the centre unless told otherwise, and ease onto the circle over
            // the first part of the move, so there's no jump.
            const auto startAngle = arg(7).empty() ? std::atan2(from.pos.x - center.x, from.pos.y - center.y) :
                                                     std::stof(arg(7)) * kDeg;
            const auto startRadius = std::sqrt((from.pos.x - center.x) * (from.pos.x - center.x) +
                                               (from.pos.y - center.y) * (from.pos.y - center.y));
            const auto startHeight = from.pos.z - center.z;
            const auto easeIn = (std::min)(2.0f, seconds * 0.2f);
            const RE::NiPoint3 focus{ center.x, center.y, center.z + aim };
            Animate([=](float a_time) {
                const auto blend = Smooth(a_time / easeIn);
                const auto angle = startAngle + turns * 2.0f * kPi * (a_time / seconds);
                const auto r = startRadius + (radius - startRadius) * blend;
                const auto h = startHeight + (height - startHeight) * blend;
                CameraPose pose{ { center.x + std::sin(angle) * r, center.y + std::cos(angle) * r, center.z + h } };
                CameraPose aimed = pose;
                LookAlong(focus - pose.pos, aimed);
                pose.yaw = AngleLerp(from.yaw, aimed.yaw, blend);
                pose.pitch = from.pitch + (aimed.pitch - from.pitch) * blend;
                return pose;
            },
                seconds, "orbit");
            return { true, std::format("orbiting {:.0f} {:.0f} {:.0f}, radius {:.0f}, {:.2f} turns over {:.1f} s",
                               center.x, center.y, center.z, radius, turns, seconds) };
        }

        if (sub == "path") {  // path <seconds> <look point or -> <point> <point>...
            const auto seconds = (std::max)(0.5f, std::stof(arg(1)));
            const auto look = arg(2) == "-" || arg(2).empty() ? std::optional<RE::NiPoint3>{} : std::optional{ point(2) };
            std::vector<RE::NiPoint3> points{ from.pos };
            for (std::size_t i = 3; i < a_args.size(); ++i) {
                points.push_back(point(i));
            }
            if (points.size() < 2) {
                return { false, "path needs at least one point" };
            }
            // Catmull-Rom through the points, timed by distance so the speed is even; eased at both ends.
            std::vector<float> lengths{ 0.0f };
            for (std::size_t i = 1; i < points.size(); ++i) {
                lengths.push_back(lengths.back() + points[i].GetDistance(points[i - 1]));
            }
            const auto total = (std::max)(lengths.back(), 1.0f);
            const auto at = [points, lengths, total](float a_t) {
                const auto distance = a_t * total;
                std::size_t i = 1;
                while (i + 1 < lengths.size() && lengths[i] < distance) {
                    ++i;
                }
                const auto span = (std::max)(lengths[i] - lengths[i - 1], 0.001f);
                const auto u = std::clamp((distance - lengths[i - 1]) / span, 0.0f, 1.0f);
                const auto& p1 = points[i - 1];
                const auto& p2 = points[i];
                const auto& p0 = i >= 2 ? points[i - 2] : p1;
                const auto& p3 = i + 1 < points.size() ? points[i + 1] : p2;
                const auto u2 = u * u;
                const auto u3 = u2 * u;
                return (p1 * 2.0f + (p2 - p0) * u + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * u2 +
                           (p1 * 3.0f - p0 - p2 * 3.0f + p3) * u3) *
                       0.5f;
            };
            Animate([=](float a_time) {
                const auto t = Smooth(a_time / seconds);
                CameraPose pose{ at(t), from.yaw, from.pitch };
                CameraPose aimed = pose;
                if (look) {
                    LookAlong(*look - pose.pos, aimed);
                } else {
                    LookAlong(at((std::min)(t + 0.02f, 1.0f)) - at((std::max)(t - 0.02f, 0.0f)), aimed);
                }
                const auto blend = Smooth(a_time / (std::min)(seconds, 1.0f));
                pose.yaw = AngleLerp(from.yaw, aimed.yaw, blend);
                pose.pitch = from.pitch + (aimed.pitch - from.pitch) * blend;
                return pose;
            },
                seconds, "path");
            return { true, std::format("path through {} points, {:.0f} units over {:.1f} s", points.size() - 1, total, seconds) };
        }

        return { false, "camera: status | free | release | stop | look | fly | orbit | path" };
    }

    // Frame times over a_seconds of wall time. Runs on the network thread: it queues one main-thread task at a time,
    // each reading the engine's own timer (BSTimer: realTimeDelta is the last frame's length, lastTime its timestamp),
    // and keeps one sample per distinct frame.
    Reply Perf(float a_seconds)
    {
        struct Sample
        {
            std::uint64_t frame{ 0 };
            float         delta{ 0.0f };
        };

        std::vector<float> deltas;
        std::uint64_t      lastFrame = 0;
        const auto         start = std::chrono::steady_clock::now();
        const auto         until = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<float>(a_seconds));

        while (std::chrono::steady_clock::now() < until) {
            auto       sample = std::make_shared<Sample>();
            const auto reply = OnMainThread([sample]() {
                const auto timer = RE::BSTimer::GetSingleton();
                if (!timer) {
                    return Reply{ false, "no BSTimer" };
                }
                sample->frame = timer->lastTime;
                sample->delta = timer->realTimeDelta;
                return Reply{ true, {} };
            });
            if (!reply.ok) {
                return reply;
            }
            if (sample->frame != lastFrame) {
                lastFrame = sample->frame;
                deltas.push_back(sample->delta * 1000.0f);
            }
        }

        const auto elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        if (deltas.size() < 2) {
            return { false, std::format("only {} distinct frames in {:.1f} s: the timer isn't advancing", deltas.size(), elapsed) };
        }
        deltas.erase(deltas.begin());  // the first sample's frame began before we started

        float total = 0.0f;
        for (const auto d : deltas) {
            total += d;
        }
        std::vector<float> sorted = deltas;
        std::ranges::sort(sorted);
        const auto at = [&](float a_fraction) {
            return sorted[(std::min)(sorted.size() - 1, static_cast<std::size_t>(a_fraction * sorted.size()))];
        };
        const auto mean = total / deltas.size();

        // A sample's round trip can span two frames, so not every frame is sampled; the timer covers each one it reads.
        return { true, std::format("fps {:.1f} over {:.2f} s (engine frame timer)\n"
                                   "frame ms: mean {:.2f}, median {:.2f}, 99th percentile {:.2f}, worst {:.2f}\n"
                                   "sampled {} of about {:.0f} frames",
                           1000.0f / mean, elapsed, mean, at(0.5f), at(0.99f), sorted.back(), deltas.size(),
                           elapsed * 1000.0f / mean) };
    }

    Reply Handle(const std::vector<std::string>& a_parts)
    {
        const auto& op = a_parts[1];

        if (op == "ping") {
            return { true, std::format("GameBridge {}, runtime {}, game data {}", PLUGIN_VERSION_MAJOR,
                               runtimeVersion, gameDataReady ? "ready" : "not ready") };
        }

        if (!gameDataReady) {
            return { false, "game data isn't loaded yet" };
        }

        if (op == "console" && a_parts.size() >= 3) {
            const auto command = a_parts[2];
            return OnMainThread([command]() { return RunConsole(command); });
        }

        if (op == "vars" && a_parts.size() >= 4) {
            const auto formID = a_parts[2];
            const auto script = a_parts[3];
            return OnMainThread([formID, script]() {
                try {
                    return ReadVars(formID, script);
                } catch (const std::exception& e) {
                    return Reply{ false, e.what() };
                }
            });
        }

        if (op == "player") {
            return OnMainThread([]() { return Player(); });
        }

        if (op == "ref" && a_parts.size() >= 3) {
            const auto formID = a_parts[2];
            return OnMainThread([formID]() {
                try {
                    return RefInfo(formID);
                } catch (const std::exception& e) {
                    return Reply{ false, e.what() };
                }
            });
        }

        if (op == "actor" && a_parts.size() >= 3) {
            const auto formID = a_parts[2];
            return OnMainThread([formID]() {
                try {
                    return ActorInfo(formID);
                } catch (const std::exception& e) {
                    return Reply{ false, e.what() };
                }
            });
        }

        if (op == "near" && a_parts.size() >= 3) {
            try {
                const auto radius = std::stof(a_parts[2]);
                const auto type = a_parts.size() >= 4 ? a_parts[3] : std::string{};
                std::optional<RE::NiPoint3> origin;
                if (a_parts.size() >= 7) {
                    origin = RE::NiPoint3{ std::stof(a_parts[4]), std::stof(a_parts[5]), std::stof(a_parts[6]) };
                }
                return OnMainThread([radius, type, origin]() { return Near(radius, type, origin); });
            } catch (const std::exception& e) {
                return Reply{ false, e.what() };
            }
        }

        if (op == "place" && a_parts.size() >= 3) {
            try {
                const auto formID = a_parts[2];
                const auto distance = a_parts.size() >= 4 ? std::stof(a_parts[3]) : 200.0f;
                return OnMainThread([formID, distance]() {
                    try {
                        return Place(formID, distance);
                    } catch (const std::exception& e) {
                        return Reply{ false, e.what() };
                    }
                });
            } catch (const std::exception& e) {
                return Reply{ false, e.what() };
            }
        }

        if (op == "screenshot") {
            return OnMainThread([]() { return Screenshot(); });
        }

        if (op == "quest" && a_parts.size() >= 3) {
            const auto name = a_parts[2];
            return OnMainThread([name]() {
                try {
                    return QuestInfo(name);
                } catch (const std::exception& e) {
                    return Reply{ false, e.what() };
                }
            });
        }

        if (op == "questlog") {
            try {
                const auto count = a_parts.size() >= 3 ? std::stoul(a_parts[2]) : 50ul;
                const auto lines = QuestLogLines(0, std::clamp<std::size_t>(count, 1, kQuestLogSize));
                return { true, lines.empty() ? "no quest events since the game started\n" : lines };
            } catch (const std::exception& e) {
                return Reply{ false, e.what() };
            }
        }

        if (op == "global" && a_parts.size() >= 3) {
            const auto name = a_parts[2];
            return OnMainThread([name]() {
                try {
                    return GlobalValue(name);
                } catch (const std::exception& e) {
                    return Reply{ false, e.what() };
                }
            });
        }

        if (op == "camera") {
            const std::vector<std::string> args(a_parts.begin() + 2, a_parts.end());
            return OnMainThread([args]() {
                try {
                    return Camera(args);
                } catch (const std::exception& e) {
                    return Reply{ false, e.what() };
                }
            });
        }

        if (op == "perf") {
            try {
                const auto seconds = a_parts.size() >= 3 ? std::stof(a_parts[2]) : 3.0f;
                return Perf(std::clamp(seconds, 0.5f, 10.0f));
            } catch (const std::exception& e) {
                return Reply{ false, e.what() };
            }
        }

        return { false, "unknown op: " + op };
    }

    void Serve(SOCKET a_client)
    {
        std::string pending;
        char        buffer[4096];

        for (;;) {
            const auto received = recv(a_client, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                return;
            }
            pending.append(buffer, static_cast<std::size_t>(received));

            for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
                auto line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                const auto parts = Split(line, '\t');
                if (parts.size() < 2) {
                    continue;
                }

                const auto reply = Handle(parts);
                const auto text = std::format("{{\"id\":\"{}\",\"ok\":{},\"result\":\"{}\"}}\n", JsonEscape(parts[0]),
                    reply.ok ? "true" : "false", JsonEscape(reply.text));
                if (send(a_client, text.data(), static_cast<int>(text.size()), 0) == SOCKET_ERROR) {
                    return;
                }
            }
        }
    }

    void Listen()
    {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            REX::ERROR("WSAStartup failed");
            return;
        }

        const auto server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server == INVALID_SOCKET) {
            REX::ERROR("socket failed");
            return;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kPort);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never reachable from another machine

        if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(server, 1) == SOCKET_ERROR) {
            REX::ERROR("can't listen on port {}", kPort);
            return;
        }
        REX::INFO("listening on 127.0.0.1:{}", kPort);

        for (;;) {
            const auto client = accept(server, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                continue;
            }
            Serve(client);
            closesocket(client);
        }
    }

    void OnMessage(F4SE::MessagingInterface::Message* a_message)
    {
        if (a_message->type == F4SE::MessagingInterface::kGameDataReady) {
            if (const auto stages = RE::TESQuestStageEvent::GetEventSource()) {
                stages->RegisterSink(&questEvents);
            }
            if (const auto startStop = RE::TESQuestStartStopEvent::GetEventSource()) {
                startStop->RegisterSink(&questEvents);
            }
            gameDataReady = true;
        }
    }
}

namespace Main
{
    static bool isInit = false;

    bool InitPlugin(const F4SE::LoadInterface* a_f4se)
    {
        if (isInit)
            return true;

        static std::once_flag once;
        std::call_once(once, [&]() {
            F4SE::Init(a_f4se);
            Bridge::runtimeVersion = a_f4se->RuntimeVersion().string();
            isInit = true;
        });

        return isInit;
    }

    F4SE_PLUGIN_QUERY(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
    {
        if (const auto data = F4SE::PluginVersionData::GetSingleton()) {
            a_info->infoVersion = F4SE::PluginInfo::kVersion;
            a_info->name = data->GetPluginName().data();
            a_info->version = data->GetPluginVersion().pack();
        }

        return a_f4se->RuntimeVersion() >= REL::Version(F4SE::RUNTIME_1_10_163);
    }

    F4SE_PLUGIN_PRELOAD(const F4SE::LoadInterface* a_f4se)
    {
        return InitPlugin(a_f4se);
    }

    F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
    {
        if (!InitPlugin(a_f4se)) {
            return false;
        }

        if (const auto messaging = F4SE::GetMessagingInterface()) {
            messaging->RegisterListener(Bridge::OnMessage);
        }
        std::thread(Bridge::Listen).detach();
        return true;
    }
}
