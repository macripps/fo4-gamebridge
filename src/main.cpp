// GameBridge: lets a local tool (such as the MCP server in server.py) talk to a running game.
//
// Listens on 127.0.0.1:47830 on its own thread, so "ping" still answers when Papyrus is jammed. Anything that touches
// game state goes through F4SE's task queue and runs on the main thread (BTI calls the console from a background
// thread, which isn't safe). It runs whatever console command it's sent, so use it on a test profile, not a playthrough.
//
// Protocol, one line each way:
//   request   id<TAB>op[<TAB>arg...]
//   reply     {"id":"...","ok":true,"result":"..."}
//   ops       ping | console <command> | vars <formid hex> <script>

#include <winsock2.h>
#include <ws2tcpip.h>
#undef ERROR  // wingdi.h's ERROR breaks REX::ERROR

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
