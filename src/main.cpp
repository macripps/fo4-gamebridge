// GameBridge: lets a local tool (such as the MCP server in server.py) talk to a running game.
//
// Listens on 127.0.0.1:47830 on its own thread, so "ping" still answers when Papyrus is jammed. Anything that touches
// game state goes through F4SE's task queue and runs on the main thread (BTI calls the console from a background
// thread, which isn't safe). It runs whatever console command it's sent, so use it on a test profile, not a playthrough.
//
// Protocol, one line each way:
//   request   id<TAB>op[<TAB>arg...]
//   reply     {"id":"...","ok":true,"result":"..."}
//   ops       ping | console <command> | vars <formid hex> <script> | player | ref <formid hex>
//             | near <radius> [<base form type> [<x> <y> <z>]] | perf [<seconds>] | place <formid hex> [<distance>]
//             | screenshot

#include <winsock2.h>
#include <ws2tcpip.h>
#undef ERROR  // wingdi.h's ERROR breaks REX::ERROR

#include <algorithm>
#include <atomic>
#include <chrono>
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
