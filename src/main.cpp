// GameBridge: lets a local tool (such as the MCP server in server.py) talk to a running game.
//
// Listens on 127.0.0.1:47830 on its own thread, so "ping" still answers when Papyrus is jammed. Anything that touches
// game state goes through F4SE's task queue and runs on the main thread (BTI calls the console from a background
// thread, which isn't safe). It runs whatever console command it's sent, so use it on a test profile, not a playthrough.
//
// Protocol, one line each way:
//   request   id<TAB>op[<TAB>arg...]
//   reply     {"id":"...","ok":true,"result":"..."}
//   ops       ping | console <command> | vars <formid hex> <script> [<variable>] | player | ref <formid hex> | actor <formid hex>
//             | near <radius> [<base form type> [<x> <y> <z>]] | perf [<seconds>] | place <formid hex> [<distance>]
//             | screenshot | quest <formid hex or editor ID> | questlog [<count>] | global <formid hex or editor ID>
//             | av <reference or "workshops"> <actor value>...
//             | camera status|free|release|stop | camera look <point> [<s>] | camera fly <point> <s> [<look point>]
//             | camera orbit <center> <radius> <height> <s> [<turns> [<aim height> [<start angle>]]]
//             | camera path <s> <look point or -> <point>...      (a point is x,y,z or a reference's FormID)
//             | holds <formid hex> | holdsnear <radius> [<deleted only 0|1> [<base form type>]]
//             | demote <formid hex> [<owner formid hex>] | demotenear <radius> | demoteall  (dev-only fix prototype)
//             | promotehook on [all] | off | clear | read [<count>]                 (dev-only detour)

#include "extratypes.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#undef ERROR  // wingdi.h's ERROR breaks REX::ERROR

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
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

    // Save loads, from F4SE's kPreLoadGame (the save's name) and kPostLoadGame/kNewGame (done). The "loads" op reports
    // them so a client can wait for the next load instead of being told.
    std::atomic_uint32_t loadCount{ 0 };
    std::mutex           loadLock;
    std::string          loadingSave;
    std::string          lastLoad;
    bool                 lastLoadOk{ false };
    std::string          lastLoadTime;

    std::string Clock()
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        return std::format("{:02}:{:02}:{:02}", now.wHour, now.wMinute, now.wSecond);
    }

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
            // A form's handle is 0000FFFF in the high half and its FormID in the low half (aliases differ).
            const auto handle = object->GetHandle();
            if ((handle >> 32) == 0xFFFF) {
                return std::format("[{} form={:08X}]", object->GetTypeInfo()->GetName(), static_cast<std::uint32_t>(handle));
            }
            return std::format("[{} handle={:016X}]", object->GetTypeInfo()->GetName(), handle);
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
    // With a_only, just that variable (its name, or "X" for a property's "::X_var"), with an array's elements.
    Reply ReadVars(const std::string& a_formID, const std::string& a_script, const std::string& a_only = {})
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
                const std::string name = variables[i].name.c_str();
                if (!a_only.empty() && _stricmp(name.c_str(), a_only.c_str()) != 0 &&
                    _stricmp(name.c_str(), ("::" + a_only + "_var").c_str()) != 0) {
                    continue;
                }
                RE::BSScript::Variable value;
                if (!vm->GetVariableValue(object, index, value)) {
                    continue;
                }
                out += std::format("{} = {}\n", name, Describe(value));
                if (!a_only.empty() && value.is<RE::BSScript::Array>()) {
                    if (const auto array = RE::BSScript::get<RE::BSScript::Array>(value)) {
                        for (std::uint32_t e = 0; e < array->size(); ++e) {
                            out += std::format("{}[{}] = {}\n", name, e, Describe((*array)[e]));
                        }
                    }
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

    // ---------------------------------------------------------------- holds
    // What keeps a reference alive. A deleted reference is only freed once nothing persists it, and the save's
    // change forms don't say what does (2026-11-26: 90k deleted, persistent plot accessories in one save, most with
    // no visible holder). Read here, data members only, under the engine's own locks:
    //   - form flags (0x20 deleted, 0x400 persistent, 0x800 disabled) and the engine refcount (BSHandleRefObject)
    //   - the Papyrus handle policy's persist count for the reference's handle, and a queued promote
    //   - the scripts bound to it, each script object's raw refCountAndHandleLock (the VM's attached-script table
    //     holds one; more means a variable, array or stack elsewhere holds the object)
    //   - the quest aliases holding it (ExtraAliasInstanceArray), the references linking to it (ExtraLinkedRefChildren)
    //   - the type of every extra data on it
    // Copies a_size bytes, or returns false if the memory can't be read. For layouts CommonLibF4 doesn't define.
    bool SafeRead(const void* a_src, void* a_dst, std::size_t a_size)
    {
        __try {
            std::memcpy(a_dst, a_src, a_size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // ExtraPromotedRef: who promoted the reference to persistent. Not in CommonLibF4; as in Skyrim, a
    // BSTArray<TESForm*> right after the extra header (here 0x18: data pointer, capacity, size). Read guarded, and an
    // element counts only if GetFormByID of the FormID it holds gives back the same pointer.
    std::vector<std::string> ReadPromoters(const RE::BSExtraData* a_extra)
    {
        struct
        {
            std::uintptr_t data;
            std::uint32_t  capacity;
            std::uint32_t  pad;
            std::uint32_t  size;
        } array{};
        std::vector<std::string> out;
        if (!SafeRead(reinterpret_cast<const std::byte*>(a_extra) + 0x18, &array, sizeof(array))) {
            return { "(unreadable)" };
        }
        if (array.size > 4096 || array.size > array.capacity || (array.size && !array.data)) {
            return { std::format("(unexpected layout: data {:X} capacity {} size {})", array.data, array.capacity, array.size) };
        }
        for (std::uint32_t i = 0; i < array.size; ++i) {
            std::uintptr_t element = 0;
            std::uint32_t  formID = 0;
            if (!SafeRead(reinterpret_cast<const void*>(array.data + i * 8), &element, 8) || !element ||
                !SafeRead(reinterpret_cast<const void*>(element + 0x14), &formID, 4)) {
                out.emplace_back(std::format("(unreadable element {:X})", element));
                continue;
            }
            const auto form = RE::TESForm::GetFormByID(formID);
            out.emplace_back(reinterpret_cast<std::uintptr_t>(form) == element ?
                                 Name(form) + std::format(" ({})", form->GetFormTypeString()) :
                                 std::format("(not a form: {:X}, FormID field {:08X})", element, formID));
        }
        return out;
    }

    struct Holds
    {
        std::vector<std::string>                          promoters;
        std::uint32_t                                     formFlags{ 0 };
        std::uint32_t                                     engineRefs{ 0 };
        std::uint64_t                                     handle{ 0 };
        std::optional<std::uint32_t>                      persistCount;
        bool                                              queuedPromote{ false };
        std::vector<std::pair<std::string, std::uint32_t>> scripts;
        std::vector<std::string>                          aliases;
        std::vector<std::string>                          children;
        std::vector<std::string>                          extras;
    };

    Holds ReadHolds(RE::TESObjectREFR* a_ref, bool a_childNames)
    {
        Holds h;
        h.formFlags = a_ref->formFlags;
        h.engineRefs = static_cast<const RE::BSHandleRefObject*>(a_ref)->QRefCount();

        const auto gameVM = RE::GameVM::GetSingleton();
        const auto vm = gameVM ? gameVM->GetVM() : nullptr;
        if (vm) {
            auto& policy = static_cast<RE::GameScript::HandlePolicy&>(vm->GetObjectHandlePolicy());
            h.handle = policy.GetHandleForObject(static_cast<std::uint32_t>(a_ref->GetFormType()), a_ref);
            {
                RE::BSAutoLock lock{ policy.persistLock };
                if (const auto it = policy.persistRefCount.find(h.handle); it != policy.persistRefCount.end()) {
                    h.persistCount = it->second;
                }
                h.queuedPromote = policy.queuedPromotes.contains(h.handle);
            }
            const auto impl = static_cast<RE::BSScript::Internal::VirtualMachine*>(vm.get());
            RE::BSAutoLock lock{ impl->attachedScriptsLock };
            if (const auto it = impl->attachedScripts.find(h.handle); it != impl->attachedScripts.end()) {
                for (const auto& attached : it->second) {
                    if (const auto object = attached.get()) {
                        const auto type = object->GetTypeInfo();
                        h.scripts.emplace_back(type ? type->GetName() : "?", object->refCountAndHandleLock);
                    }
                }
            }
        }

        if (const auto& list = a_ref->extraList) {
            for (std::uint32_t t = 1; t < std::size(kExtraTypeNames); ++t) {
                if (list->GetByType(static_cast<RE::EXTRA_DATA_TYPE>(t))) {
                    h.extras.emplace_back(kExtraTypeNames[t]);
                }
            }
            if (const auto promoted = list->GetByType(RE::EXTRA_DATA_TYPE::kPromotedRef)) {
                h.promoters = ReadPromoters(promoted);
            }
            if (const auto aliases = list->GetByType<RE::ExtraAliasInstanceArray>()) {
                RE::BSAutoLock<RE::BSReadWriteLock, RE::BSAutoLockReadLockPolicy> lock{ aliases->aliasArrayLock };
                for (const auto& entry : aliases->aliasArray) {
                    h.aliases.emplace_back(std::format("{} / {}", Name(entry.quest),
                        entry.alias ? entry.alias->aliasName.c_str() : "?"));
                }
            }
            if (const auto children = list->GetByType<RE::ExtraLinkedRefChildren>()) {
                for (const auto& child : children->linkedChildren) {
                    const auto ref = child.REFR.get();
                    if (a_childNames) {
                        h.children.emplace_back(std::format("{} <- {}{}", Name(child.keyword), Name(ref.get()),
                            ref ? std::format(" (base {}){}{}", Name(ref->GetObjectReference()),
                                      ref->IsDisabled() ? " DISABLED" : "", ref->GetDelete() ? " DELETED" : "") :
                                  std::string{}));
                    } else {
                        h.children.emplace_back(ref ? std::format("{:08X}", ref->GetFormID()) : "None");
                    }
                }
            }
        }
        return h;
    }

    Reply HoldsInfo(const std::string& a_formID)
    {
        const auto form = RE::TESForm::GetFormByID(static_cast<RE::TESFormID>(std::stoul(a_formID, nullptr, 16)));
        const auto refr = form ? form->As<RE::TESObjectREFR>() : nullptr;
        if (!refr) {
            return { false, "no reference " + a_formID };
        }
        const auto h = ReadHolds(refr, true);
        std::string out = std::format("ref {}\nbase {}\ncell {}\n", Name(refr), Name(refr->GetObjectReference()),
            Name(refr->GetParentCell()));
        out += std::format("form flags {:08X}: deleted {}, persistent {}, disabled {}; engine refcount {}\n", h.formFlags,
            (h.formFlags & 0x20) != 0, (h.formFlags & 0x400) != 0, (h.formFlags & 0x800) != 0, h.engineRefs);
        out += std::format("papyrus handle {:016X}: persist count {}, promote queued {}\n", h.handle,
            h.persistCount ? std::to_string(*h.persistCount) : "none", h.queuedPromote);
        out += h.scripts.empty() ? "scripts: none bound\n" : "scripts (raw refCountAndHandleLock):\n";
        for (const auto& [name, refs] : h.scripts) {
            out += std::format("  {} {:08X}\n", name, refs);
        }
        out += h.promoters.empty() ? "promoted by: no ExtraPromotedRef\n" : std::format("promoted by ({}):\n", h.promoters.size());
        for (const auto& p : h.promoters) {
            out += "  " + p + "\n";
        }
        out += h.aliases.empty() ? "aliases: none\n" : "aliases:\n";
        for (const auto& a : h.aliases) {
            out += "  " + a + "\n";
        }
        out += std::format("linked from ({}):{}", h.children.size(), h.children.empty() ? " none\n" : "\n");
        for (const auto& c : h.children) {
            out += "  " + c + "\n";
        }
        out += "extra data:";
        for (const auto& e : h.extras) {
            out += " " + e;
        }
        return { true, out + "\n" };
    }

    // The same for every reference within a_radius of the player, one line each: for sweeping a settlement's deleted
    // leftovers. Children print as FormIDs only (a link holder can have hundreds).
    Reply HoldsNear(float a_radius, bool a_deletedOnly, const std::string& a_type)
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        const auto tes = RE::TES::GetSingleton();
        if (!player || !tes || a_radius <= 0.0f) {
            return { false, "no player or TES, or radius not above 0" };
        }
        const auto origin = player->GetPosition();
        std::vector<std::pair<float, std::string>> rows;
        tes->ForEachReferenceInRange(origin, a_radius, [&](RE::TESObjectREFR* a_ref) {
            const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
            if (base && a_ref != player && (!a_deletedOnly || a_ref->GetDelete()) &&
                (a_type.empty() || a_type == base->GetFormTypeString())) {
                const auto h = ReadHolds(a_ref, false);
                std::string scripts;
                for (const auto& [name, refs] : h.scripts) {
                    scripts += std::format("{}{}:{:X}", scripts.empty() ? "" : ",", name, refs);
                }
                std::string children;
                for (const auto& c : h.children) {
                    children += (children.empty() ? "" : ",") + c;
                }
                std::string aliases;
                for (const auto& a : h.aliases) {
                    aliases += (aliases.empty() ? "" : "; ") + a;
                }
                std::string extras;
                for (const auto& e : h.extras) {
                    extras += (extras.empty() ? "" : ",") + e;
                }
                std::string promoters;
                for (const auto& p : h.promoters) {
                    promoters += (promoters.empty() ? "" : "; ") + p;
                }
                const auto distance = origin.GetDistance(a_ref->GetPosition());
                rows.emplace_back(distance, std::format("{:08X}\t{}\tflags {:08X}\trefs {}\tpersist {}{}\tscripts {}\taliases {}\tlinkedfrom {}\tpromotedby {}\textra {}",
                    a_ref->GetFormID(), Name(base), h.formFlags, h.engineRefs,
                    h.persistCount ? std::to_string(*h.persistCount) : "-", h.queuedPromote ? " (promote queued)" : "",
                    scripts.empty() ? "-" : scripts, aliases.empty() ? "-" : aliases,
                    children.empty() ? "-" : std::format("{}:{}", h.children.size(), children),
                    promoters.empty() ? "-" : promoters, extras));
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        std::ranges::sort(rows, {}, &std::pair<float, std::string>::first);
        std::string out = std::format("{} references\n", rows.size());
        for (const auto& row : rows) {
            out += row.second + "\n";
        }
        return { true, out };
    }

    // ---------------------------------------------------------------- demote (dev-only fix prototype)
    // Removes one promoter from a reference with the engine's own BGSDynamicPersistenceManager::DemoteReference
    // (next-gen Address Library ID, reused on 1.11 like TESObjectCELL::Pick). For SS2's leftover plot objects: deleted,
    // promoted only by themselves, so never freed (2026-11-26). With no promoters left, a deleted reference should stop
    // being persistent and be freed like any other.
    bool IsSelfPromoted(const Holds& a_h, std::uint32_t a_formID)
    {
        const auto self = std::format("{:08X}", a_formID);
        return std::ranges::any_of(a_h.promoters, [&](const std::string& p) { return p.starts_with(self); });
    }

    std::string HoldsLine(const Holds& a_h)
    {
        std::string promoters;
        for (const auto& p : a_h.promoters) {
            promoters += (promoters.empty() ? "" : "; ") + p;
        }
        return std::format("flags {:08X} (persistent {}), refs {}, persist {}, promoted by {}", a_h.formFlags,
            (a_h.formFlags & 0x400) != 0, a_h.engineRefs, a_h.persistCount ? std::to_string(*a_h.persistCount) : "-",
            promoters.empty() ? "-" : promoters);
    }

    Reply Demote(const std::string& a_formID, const std::string& a_owner)
    {
        const auto form = RE::TESForm::GetFormByID(static_cast<RE::TESFormID>(std::stoul(a_formID, nullptr, 16)));
        const auto refr = form ? form->As<RE::TESObjectREFR>() : nullptr;
        if (!refr) {
            return { false, "no reference " + a_formID };
        }
        RE::TESForm* owner = refr;
        if (!a_owner.empty()) {
            owner = RE::TESForm::GetFormByID(static_cast<RE::TESFormID>(std::stoul(a_owner, nullptr, 16)));
            if (!owner) {
                return { false, "no owner form " + a_owner };
            }
        }
        const auto manager = RE::BGSDynamicPersistenceManager::GetSingleton();
        if (!manager) {
            return { false, "no BGSDynamicPersistenceManager" };
        }
        const auto before = HoldsLine(ReadHolds(refr, false));
        const bool result = manager->DemoteReference(refr, owner, false);
        const auto after = HoldsLine(ReadHolds(refr, false));
        return { true, std::format("ref {}\nowner {}\nDemoteReference returned {}\nbefore: {}\nafter:  {}\n", Name(refr),
                           Name(owner), result, before, after) };
    }

    // Demotes, by itself, every deleted reference within a_radius of the player that lists itself as a promoter.
    Reply DemoteNear(float a_radius)
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        const auto tes = RE::TES::GetSingleton();
        const auto manager = RE::BGSDynamicPersistenceManager::GetSingleton();
        if (!player || !tes || !manager || a_radius <= 0.0f) {
            return { false, "no player, TES or persistence manager, or radius not above 0" };
        }
        std::vector<RE::TESObjectREFR*> targets;
        tes->ForEachReferenceInRange(player->GetPosition(), a_radius, [&](RE::TESObjectREFR* a_ref) {
            if (a_ref && a_ref->GetDelete() && IsSelfPromoted(ReadHolds(a_ref, false), a_ref->GetFormID())) {
                targets.push_back(a_ref);
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        std::size_t ok = 0;
        std::string out;
        for (const auto ref : targets) {
            const bool result = manager->DemoteReference(ref, ref, false);
            ok += result;
            out += std::format("{:08X} {} -> {}\n", ref->GetFormID(), result, HoldsLine(ReadHolds(ref, false)));
        }
        return { true, std::format("{} deleted self-promoted references, DemoteReference true for {}\n", targets.size(), ok) + out };
    }

    // demote() for every deleted reference anywhere in memory that lists itself as a promoter (persistent references
    // are always in memory, so this reaches the whole save, not just loaded cells). Collects FormIDs under the form
    // map's read lock, then demotes after releasing it (freeing changes the map); each is looked up again first.
    Reply DemoteAll()
    {
        const auto manager = RE::BGSDynamicPersistenceManager::GetSingleton();
        if (!manager) {
            return { false, "no BGSDynamicPersistenceManager" };
        }
        std::vector<std::uint32_t> ids;
        std::size_t                references = 0;
        {
            const auto& [map, lock] = RE::TESForm::GetAllForms();
            RE::BSAutoReadLock guard{ lock };
            if (!map) {
                return { false, "no form map" };
            }
            for (const auto& [id, form] : *map) {
                const auto ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
                if (!ref) {
                    continue;
                }
                ++references;
                if (ref->GetDelete() && ref->extraList && ref->extraList->GetByType(RE::EXTRA_DATA_TYPE::kPromotedRef) &&
                    IsSelfPromoted(ReadHolds(ref, false), ref->GetFormID())) {
                    ids.push_back(id);
                }
            }
        }
        std::size_t                          freed = 0, kept = 0;
        std::map<std::string, std::size_t>   byBase;
        for (const auto id : ids) {
            const auto form = RE::TESForm::GetFormByID(id);
            const auto ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!ref) {
                continue;
            }
            const auto base = Name(ref->GetObjectReference());
            manager->DemoteReference(ref, ref, false);
            const auto after = RE::TESForm::GetFormByID(id);
            const auto stillRef = after ? after->As<RE::TESObjectREFR>() : nullptr;
            if (!stillRef || (stillRef->formFlags & 0x400) == 0) {
                ++freed;
                byBase[base]++;
            } else {
                ++kept;  // another promoter still holds it (an alias, Papyrus, another reference)
            }
        }
        std::vector<std::pair<std::size_t, std::string>> top;
        for (const auto& [base, n] : byBase) {
            top.emplace_back(n, base);
        }
        std::ranges::sort(top, std::greater{});
        std::string out = std::format("{} references in memory; {} deleted and self-promoted; {} released (no longer "
                                      "persistent), {} still held by something else\nreleased by base:\n",
            references, ids.size(), freed, kept);
        for (std::size_t i = 0; i < top.size() && i < 25; ++i) {
            out += std::format("  {:7} {}\n", top[i].first, top[i].second);
        }
        return { true, out };
    }

    // ---------------------------------------------------------------- promote hook (dev-only)
    // Who promotes a reference by itself? (SS2 plot parts do, and are then never freed, 2026-11-26.) A detour on
    // BGSDynamicPersistenceManager::PromoteReference records each call where the owner is the reference itself (or
    // every call, in "all" mode) with a native backtrace, as module+offset, to map to engine functions offline.
    // The detour overwrites only the entry's first instruction, "mov [rsp+20h], rbx" (48 89 5C 24 20, 5 bytes, checked
    // before patching: the next instruction is RIP-relative and must not move). A gateway in the F4SE trampoline runs
    // that instruction and jumps back to entry+5. Installed on request only; recording can be switched off, but the
    // detour stays until the game exits.
    namespace PromoteHook
    {
        using Fn = bool (*)(RE::BGSDynamicPersistenceManager*, RE::TESObjectREFR*, RE::TESForm*);
        constexpr std::uint8_t kPrologue[5]{ 0x48, 0x89, 0x5C, 0x24, 0x20 };
        constexpr std::size_t  kMaxRecords = 3000;

        Fn                      gateway{ nullptr };
        std::atomic_bool        installed{ false };
        std::atomic_bool        recording{ false };
        std::atomic_bool        allCalls{ false };
        std::mutex              lock;
        std::deque<std::string> records;
        std::uint64_t           dropped{ 0 };

        std::string Frame(void* a_address)
        {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    static_cast<LPCWSTR>(a_address), &module) || !module) {
                return std::format("?{:X}", reinterpret_cast<std::uintptr_t>(a_address));
            }
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(module, path, MAX_PATH);
            const auto name = std::filesystem::path(path).filename().string();
            return std::format("{}+{:X}", name,
                reinterpret_cast<std::uintptr_t>(a_address) - reinterpret_cast<std::uintptr_t>(module));
        }

        bool Hook(RE::BGSDynamicPersistenceManager* a_manager, RE::TESObjectREFR* a_ref, RE::TESForm* a_owner)
        {
            if (recording && a_ref && (allCalls || static_cast<RE::TESForm*>(a_ref) == a_owner)) {
                void*      frames[24]{};
                const auto count = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
                const auto base = a_ref->GetObjectReference();
                auto line = std::format("{} t{} ref {:08X} base {:08X} owner {:08X}{} |", Clock(), GetCurrentThreadId(),
                    a_ref->GetFormID(), base ? base->GetFormID() : 0, a_owner ? a_owner->GetFormID() : 0,
                    static_cast<RE::TESForm*>(a_ref) == a_owner ? " SELF" : "");
                for (USHORT i = 0; i < count; ++i) {
                    line += " " + Frame(frames[i]);
                }
                std::scoped_lock guard{ lock };
                if (records.size() >= kMaxRecords) {
                    records.pop_front();
                    ++dropped;
                }
                records.push_back(std::move(line));
            }
            return gateway(a_manager, a_ref, a_owner);
        }

        Reply Install()
        {
            if (installed) {
                return { true, "already installed" };
            }
            REL::Relocation<std::uintptr_t> target{ RE::ID::BGSDynamicPersistenceManager::PromoteReference };
            const auto                      entry = target.address();
            std::uint8_t                    current[5]{};
            std::memcpy(current, reinterpret_cast<const void*>(entry), 5);
            if (std::memcmp(current, kPrologue, 5) != 0) {
                return { false, std::format("PromoteReference at {:X} starts with {:02X} {:02X} {:02X} {:02X} {:02X}, not the "
                                            "expected 48 89 5C 24 20; not hooking", entry, current[0], current[1], current[2],
                                    current[3], current[4]) };
            }
            auto& trampoline = REL::GetTrampoline();
            if (trampoline.empty() || trampoline.free_size() < 64) {
                return { false, "no F4SE trampoline space" };
            }
            // Gateway: the stolen instruction, then jmp [rip+0] to entry+5.
            auto code = static_cast<std::uint8_t*>(trampoline.allocate(5 + 14));
            std::memcpy(code, kPrologue, 5);
            const std::uint8_t jump[6]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
            std::memcpy(code + 5, jump, 6);
            const std::uint64_t back = entry + 5;
            std::memcpy(code + 11, &back, 8);
            gateway = reinterpret_cast<Fn>(code);
            trampoline.write_jmp<5>(entry, reinterpret_cast<std::uintptr_t>(&Hook));
            installed = true;
            return { true, std::format("hooked PromoteReference at {:X}", entry) };
        }

        Reply Command(const std::vector<std::string>& a_parts)
        {
            const auto sub = a_parts.size() >= 3 ? a_parts[2] : std::string{ "read" };
            if (sub == "on") {
                const auto reply = Install();
                if (!reply.ok) {
                    return reply;
                }
                allCalls = a_parts.size() >= 4 && a_parts[3] == "all";
                recording = true;
                return { true, reply.text + std::format("; recording {} calls", allCalls ? "all" : "self-promotion") };
            }
            if (sub == "off") {
                recording = false;
                return { true, "recording off (the detour stays until the game exits)" };
            }
            std::scoped_lock guard{ lock };
            if (sub == "clear") {
                records.clear();
                dropped = 0;
                return { true, "cleared" };
            }
            const std::size_t want = a_parts.size() >= 4 ? std::stoul(a_parts[3]) : 200;
            std::string out = std::format("installed {}, recording {}, {} records ({} dropped)\n", installed.load(),
                recording.load(), records.size(), dropped);
            const auto start = records.size() > want ? records.size() - want : 0;
            for (std::size_t i = start; i < records.size(); ++i) {
                out += records[i] + "\n";
            }
            return { true, out };
        }
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
    // the base's form type ("LIGH", "STAT", "CONT", ...); empty means all. a_enabledOnly skips disabled and deleted
    // references. Each row ends with the reference's scale and the base's bounds (OBND, unscaled, object space).
    Reply Near(float a_radius, const std::string& a_type, std::optional<RE::NiPoint3> a_origin, bool a_enabledOnly)
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
            if (base && a_ref != player && (a_type.empty() || a_type == base->GetFormTypeString()) &&
                !(a_enabledOnly && (a_ref->IsDisabled() || a_ref->GetDelete()))) {
                const auto distance = origin.GetDistance(a_ref->GetPosition());
                const auto& b = base->boundData;
                rows.emplace_back(distance, std::format("{:08X} base {} ({}) dist {:.0f} {} scale {:.2f} bounds {} {} {} {} {} {}{}{}",
                    a_ref->GetFormID(), Name(base), base->GetFormTypeString(), distance, Where(a_ref),
                    a_ref->refScale / 100.0f, b.boundMin.x, b.boundMin.y, b.boundMin.z, b.boundMax.x, b.boundMax.y,
                    b.boundMax.z, a_ref->IsDisabled() ? " DISABLED" : "", a_ref->GetDelete() ? " DELETED" : ""));
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });

        std::ranges::sort(rows, {}, &std::pair<float, std::string>::first);
        std::string out = std::format("{} references\n", rows.size());
        for (std::size_t i = 0; i < rows.size() && i < 2000; ++i) {
            out += rows[i].second + "\n";
        }
        return { true, out };
    }

    // Every reference within a_radius of a point carrying script a_script, nearest first, with the variables named in
    // a_vars (comma-separated; "X" also matches a property's "::X_var"). A form value is followed by the form's name,
    // so a plan or a quest reads as itself. One call instead of one per reference and variable.
    Reply ScriptedNear(float a_radius, std::optional<RE::NiPoint3> a_origin, const std::string& a_script,
        const std::string& a_vars, bool a_enabledOnly)
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        const auto tes = RE::TES::GetSingleton();
        const auto gameVM = RE::GameVM::GetSingleton();
        const auto vm = gameVM ? gameVM->GetVM() : nullptr;
        if (!player || !tes || !vm || a_radius <= 0.0f) {
            return { false, "no player, TES or VM, or radius not above 0" };
        }
        const auto origin = a_origin.value_or(player->GetPosition());
        const auto wanted = Split(a_vars, ',');
        auto&      policy = vm->GetObjectHandlePolicy();

        std::vector<std::pair<float, std::string>> rows;
        tes->ForEachReferenceInRange(origin, a_radius, [&](RE::TESObjectREFR* a_ref) {
            if (!a_ref || (a_enabledOnly && (a_ref->IsDisabled() || a_ref->GetDelete()))) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const auto handle = policy.GetHandleForObject(static_cast<std::uint32_t>(a_ref->GetFormType()), a_ref);
            RE::BSTSmartPointer<RE::BSScript::Object> object;
            if (!vm->FindBoundObject(handle, a_script.c_str(), true, object, false) || !object) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const auto distance = origin.GetDistance(a_ref->GetPosition());
            std::string row = std::format("{:08X} base {} dist {:.0f} {}{}{}", a_ref->GetFormID(),
                Name(a_ref->GetObjectReference()), distance, Where(a_ref), a_ref->IsDisabled() ? " DISABLED" : "",
                a_ref->GetDelete() ? " DELETED" : "");

            std::vector<RE::BSScript::ObjectTypeInfo*> chain;
            for (auto type = object->GetTypeInfo(); type; type = type->parentTypeInfo.get()) {
                chain.insert(chain.begin(), type);
            }
            for (const auto& want : wanted) {
                std::uint32_t index = 0;
                for (const auto type : chain) {
                    const auto variables = type->GetVariableIter();
                    for (std::uint32_t i = 0; i < type->GetNumVariables(); ++i, ++index) {
                        const char* name = variables[i].name.c_str();
                        if (_stricmp(name, want.c_str()) != 0 && _stricmp(name, ("::" + want + "_var").c_str()) != 0) {
                            continue;
                        }
                        RE::BSScript::Variable value;
                        if (!vm->GetVariableValue(object, index, value)) {
                            continue;
                        }
                        std::string text = Describe(value);
                        if (value.is<RE::BSScript::Object>()) {
                            const auto& obj = RE::BSScript::get<RE::BSScript::Object>(value);
                            const auto h = obj ? obj->GetHandle() : 0;
                            if (obj && (h >> 32) == 0xFFFF) {
                                text = Name(RE::TESForm::GetFormByID(static_cast<RE::TESFormID>(h)));
                            }
                        }
                        row += std::format("\n    {} = {}", want, text);
                    }
                }
            }
            rows.emplace_back(distance, row);
            return RE::BSContainer::ForEachResult::kContinue;
        });

        std::ranges::sort(rows, {}, &std::pair<float, std::string>::first);
        std::string out = std::format("{} references with {}\n", rows.size(), a_script);
        for (std::size_t i = 0; i < rows.size() && i < 500; ++i) {
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

    // ---------------------------------------------------------------- batches (mock-ups)

    // Lines a_start .. a_start+a_count-1 of a text file (blank lines and # comments skipped).
    std::vector<std::string> FileLines(const std::string& a_path, std::size_t a_start, std::size_t a_count)
    {
        std::ifstream in(a_path);
        if (!in) {
            throw std::runtime_error("can't read " + a_path);
        }
        std::vector<std::string> out;
        std::string              line;
        std::size_t              n = 0;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            if (n >= a_start && out.size() < a_count) {
                out.push_back(line);
            }
            ++n;
        }
        return out;
    }

    // Places one reference per line ("FORMID x y z rx ry rz scale", angles in degrees) in the player's worldspace or
    // interior, and appends "placed <ref>" per new reference to a_record, so undobatch can remove exactly those.
    Reply PlaceBatch(const std::string& a_list, const std::string& a_record, std::size_t a_start, std::size_t a_count)
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        const auto cell = player ? player->GetParentCell() : nullptr;
        const auto handler = RE::TESDataHandler::GetSingleton();
        if (!cell || !handler) {
            return { false, "no player cell" };
        }
        std::ofstream record(a_record, std::ios::app);
        std::size_t   placed = 0;
        std::string   errors;
        for (const auto& line : FileLines(a_list, a_start, a_count)) {
            std::istringstream ss(line);
            std::string        id;
            float              x, y, z, rx, ry, rz, scale = 1.0f;
            if (!(ss >> id >> x >> y >> z >> rx >> ry >> rz)) {
                errors += "bad line: " + line + "\n";
                continue;
            }
            ss >> scale;
            const auto form = RE::TESForm::GetFormByID(static_cast<RE::TESFormID>(std::stoul(id, nullptr, 16)));
            const auto base = form ? form->As<RE::TESBoundObject>() : nullptr;
            if (!base) {
                errors += "no base " + id + "\n";
                continue;
            }
            RE::NEW_REFR_DATA data;
            data.location = RE::NiPoint3{ x, y, z };
            constexpr float toRad = 3.14159265f / 180.0f;
            data.direction = RE::NiPoint3{ rx * toRad, ry * toRad, rz * toRad };
            data.object = base;
            if (cell->IsInterior()) {
                data.interior = cell;
            } else {
                data.world = cell->worldSpace;
            }
            const auto ref = handler->CreateReferenceAtLocation(data).get();
            if (!ref) {
                errors += "not created " + id + "\n";
                continue;
            }
            const auto refID = std::format("{:08X}", ref->GetFormID());
            if (std::abs(scale - 1.0f) > 0.001f) {
                RE::Console::ExecuteCommand(std::format("{}.setscale {:.3f}", refID, scale).c_str());
            }
            record << "placed " << refID << "\n";
            ++placed;
        }
        return { true, std::format("placed {} of lines {}..{}\n{}", placed, a_start, a_start + a_count - 1, errors) };
    }

    // Disables one existing reference per line (its load-order FormID first on the line), appending "disabled <ref>"
    // to a_record. For a mock-up's scrap list: undobatch enables them again.
    Reply DisableBatch(const std::string& a_list, const std::string& a_record, std::size_t a_start, std::size_t a_count)
    {
        std::ofstream record(a_record, std::ios::app);
        std::size_t   done = 0;
        std::string   errors;
        for (const auto& line : FileLines(a_list, a_start, a_count)) {
            const auto id = line.substr(0, line.find_first_of(" \t"));
            const auto form = RE::TESForm::GetFormByID(static_cast<RE::TESFormID>(std::stoul(id, nullptr, 16)));
            const auto ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!ref || ref->IsDisabled()) {
                errors += (ref ? "already disabled " : "no reference ") + id + "\n";
                continue;
            }
            RE::Console::ExecuteCommand(std::format("{}.disable", id).c_str());
            record << "disabled " << id << "\n";
            ++done;
        }
        return { true, std::format("disabled {}\n{}", done, errors) };
    }

    // Undoes a record in two passes, on separate frames (the console's disable and markfordelete in one frame leave
    // the reference drawn, tested 2026-11-24): pass 0 disables "placed" references and enables "disabled" ones again,
    // pass 1 marks the placed ones for delete.
    Reply UndoBatch(const std::string& a_record, std::size_t a_start, std::size_t a_count, int a_pass)
    {
        std::size_t removed = 0, enabled = 0;
        for (const auto& line : FileLines(a_record, a_start, a_count)) {
            std::istringstream ss(line);
            std::string        what, id;
            ss >> what >> id;
            const auto form = RE::TESForm::GetFormByID(static_cast<RE::TESFormID>(std::stoul(id, nullptr, 16)));
            if (!form || !form->As<RE::TESObjectREFR>()) {
                continue;
            }
            if (what == "placed") {
                RE::Console::ExecuteCommand((a_pass == 0 ? std::format("{}.disable", id) : std::format("{}.markfordelete", id)).c_str());
                ++removed;
            } else if (what == "disabled" && a_pass == 0) {
                RE::Console::ExecuteCommand(std::format("{}.enable", id).c_str());
                ++enabled;
            }
        }
        return { true, std::format("pass {}: placed {}, enabled {}", a_pass, removed, enabled) };
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

    // ---------------------------------------------------------- actor values
    // Actor values live on any reference, not only actors: workbenches keep their resources in them (food, water,
    // defense, and Sim Settlements 2's virtual resources and supplies).

    // Every workbench: a container or furniture reference with WorkshopScript bound (vanilla's are containers, base
    // 000C1AEB "Workshop"). Persistent, so found loaded or not.
    std::vector<RE::TESObjectREFR*> Workshops()
    {
        std::vector<RE::TESObjectREFR*> out;
        const auto gameVM = RE::GameVM::GetSingleton();
        const auto vm = gameVM ? gameVM->GetVM() : nullptr;
        if (!vm) {
            return out;
        }

        std::vector<RE::TESObjectREFR*> candidates;
        {
            const auto& [map, lock] = RE::TESForm::GetAllForms();
            RE::BSAutoReadLock l{ lock };
            if (map) {
                for (const auto& [id, form] : *map) {
                    const auto refr = form ? form->As<RE::TESObjectREFR>() : nullptr;
                    const auto base = refr ? refr->GetObjectReference() : nullptr;
                    if (base && (base->Is(RE::ENUM_FORM_ID::kCONT) || base->Is(RE::ENUM_FORM_ID::kFURN))) {
                        candidates.push_back(refr);
                    }
                }
            }
        }

        auto& policy = vm->GetObjectHandlePolicy();
        for (const auto refr : candidates) {
            const auto handle = policy.GetHandleForObject(static_cast<std::uint32_t>(refr->GetFormType()), refr);
            RE::BSTSmartPointer<RE::BSScript::Object> object;
            if (vm->FindBoundObject(handle, "WorkshopScript", true, object, false) && object) {
                out.push_back(refr);
            }
        }
        std::ranges::sort(out, {}, [](const RE::TESObjectREFR* a_refr) { return a_refr->GetFormID(); });
        return out;
    }

    // a_target: a reference (FormID or editor ID), or "workshops" for every workbench.
    Reply ActorValues(const std::string& a_target, const std::vector<std::string>& a_names)
    {
        std::vector<RE::ActorValueInfo*> avs;
        for (const auto& name : a_names) {
            const auto form = FindForm(name);
            const auto av = form ? form->As<RE::ActorValueInfo>() : nullptr;
            if (!av) {
                return { false, "no actor value " + name };
            }
            avs.push_back(av);
        }

        std::vector<RE::TESObjectREFR*> refs;
        if (a_target == "workshops") {
            refs = Workshops();
        } else {
            const auto form = FindForm(a_target);
            const auto refr = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!refr) {
                return { false, "no reference " + a_target };
            }
            refs.push_back(refr);
        }

        std::string out;
        for (const auto refr : refs) {
            // Current location only: CommonLibF4's zero-argument GetEditorLocation() sits at the wrong vtable slot and
            // calls the four-argument overload with garbage pointers (crashed the game, 2026-11-23)
            const auto location = refr->GetCurrentLocation();
            out += std::format("{} @ {}\n", Name(refr), location ? Name(location) : "no location");
            for (const auto av : avs) {
                out += std::format("  {} = {}\n", Name(av), refr->GetActorValue(*av));
            }
        }
        return { true, out.empty() ? "no workbenches found" : out };
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

    // ---------------------------------------------------------------- move

    // Moves and/or turns a reference: a_to is "x,y,z" (absolute) or empty; a_offset is "forward,right,up" in the
    // reference's own frame (by its rotation Z), applied after a_to; a_rotation is "x,y,z" in degrees or empty. Runs
    // "<ref>.setpos" and "<ref>.setangle", which need no console selection, in one main-thread task; the engine
    // updates the 3D and collision as for the console. Like setpos, the change is kept in the save.
    Reply Move(const std::string& a_formID, const std::string& a_to, const std::string& a_offset, const std::string& a_rotation)
    {
        const auto form = FindForm(a_formID);
        const auto ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
        if (!ref) {
            return { false, "no reference " + a_formID };
        }
        if (ref == RE::PlayerCharacter::GetSingleton()) {
            return { false, "use the console to move the player" };
        }
        const auto triple = [](const std::string& a_text, const char* a_what) {
            const auto parts = Split(a_text, ',');
            if (parts.size() != 3) {
                throw std::runtime_error(std::format("{} must be three numbers a,b,c: \"{}\"", a_what, a_text));
            }
            return RE::NiPoint3{ std::stof(parts[0]), std::stof(parts[1]), std::stof(parts[2]) };
        };

        const auto before = std::format("before: {}", Where(ref));
        RE::NiPoint3 pos = ref->GetPosition();
        if (!a_to.empty()) {
            pos = triple(a_to, "to");
        }
        float yaw = ref->data.angle.z;
        RE::NiPoint3 angles{ ref->data.angle.x / kDeg, ref->data.angle.y / kDeg, ref->data.angle.z / kDeg };
        if (!a_rotation.empty()) {
            angles = triple(a_rotation, "rotation");
            yaw = angles.z * kDeg;
        }
        if (!a_offset.empty()) {
            const auto o = triple(a_offset, "offset");  // forward, right, up
            pos.x += std::sin(yaw) * o.x + std::cos(yaw) * o.y;
            pos.y += std::cos(yaw) * o.x - std::sin(yaw) * o.y;
            pos.z += o.z;
        }

        // "<ref>.setpos" names the reference in each command: a prid in the same task only takes effect a frame later
        // (tested 2026-11-13: prid then setpos in one task moved nothing; as separate calls, or with "ref.", it works).
        const auto id = std::format("{:08X}", ref->GetFormID());
        RE::Console::ExecuteCommand(std::format("{}.setpos x {:.2f}", id, pos.x).c_str());
        RE::Console::ExecuteCommand(std::format("{}.setpos y {:.2f}", id, pos.y).c_str());
        RE::Console::ExecuteCommand(std::format("{}.setpos z {:.2f}", id, pos.z).c_str());
        if (!a_rotation.empty()) {
            RE::Console::ExecuteCommand(std::format("{}.setangle x {:.2f}", id, angles.x).c_str());
            RE::Console::ExecuteCommand(std::format("{}.setangle y {:.2f}", id, angles.y).c_str());
            RE::Console::ExecuteCommand(std::format("{}.setangle z {:.2f}", id, angles.z).c_str());
        }
        // The console applies setpos on a later frame: the caller reads "after" in a second main-thread task.
        return { true, std::format("{}\ntarget: pos {:.2f} {:.2f} {:.2f}", before, pos.x, pos.y, pos.z) };
    }

    // ---------------------------------------------------------------- crosshair

    // What the view ray hits first: casts from the camera along the view (the free camera's pose in free mode,
    // otherwise the player's aim) through the physics world, like the game's own pick, and reports the hit point,
    // distance, the 3D node it hit and the reference that owns it. Shows when collision you can't see covers an
    // activator. Calls bhkPickData and TESObjectCELL::Pick by address (non-virtual, as CreateReferenceAtLocation).
    Reply Crosshair(float a_range)
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        const auto camera = RE::PlayerCamera::GetSingleton();
        const auto cell = player ? player->GetParentCell() : nullptr;
        if (!player || !camera || !cell || !camera->cameraRoot) {
            return { false, "no player, cell or camera" };
        }

        RE::NiPoint3 start = camera->cameraRoot->world.translate;
        float        yaw = player->data.angle.z;
        float        pitch = player->data.angle.x;
        const char*  source = "player aim";
        if (const auto state = FreeCamera()) {
            const auto pose = ReadPose(state);
            start = pose.pos;
            yaw = pose.yaw;
            pitch = pose.pitch;
            source = "free camera";
        }
        const RE::NiPoint3 dir{ std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch) };

        std::string out = std::format("from {} at {:.0f} {:.0f} {:.0f}, yaw {:.1f}, pitch {:.1f}\n", source, start.x,
            start.y, start.z, yaw / kDeg, pitch / kDeg);

        // Skip the player's own collision (the camera sits inside it): restart just past any hit on the player.
        RE::NiPoint3 from = start;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const RE::NiPoint3 to = start + dir * a_range;
            RE::bhkPickData    pick;
            pick.SetStartEnd(from, to);
            const auto node = cell->Pick(pick);
            if (!pick.HasHit()) {
                return { true, out + std::format("no hit within {:.0f}", a_range) };
            }
            const auto hit = from + (to - from) * pick.GetHitFraction();
            const auto ref = node ? RE::TESObjectREFR::FindReferenceFor3D(node) : nullptr;
            if (ref && ref == player) {
                from = hit + dir * 2.0f;
                continue;
            }
            const auto distance = (hit - start).Length();
            out += std::format("hit at {:.1f} {:.1f} {:.1f}, distance {:.1f}\n", hit.x, hit.y, hit.z, distance);
            out += std::format("node \"{}\"\n", node ? node->name.c_str() : "");
            if (ref) {
                out += std::format("ref {}\nbase {}\n{}\n", Name(ref), Name(ref->GetObjectReference()), Where(ref));
            } else {
                out += "ref none (terrain, or collision with no reference)\n";
            }
            return { true, out };
        }
        return { true, out + "only the player's own collision was hit" };
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

        // Answers before game data is ready too: the MCP server polls it to wait for a save to finish loading.
        if (op == "loads") {
            std::lock_guard lock{ loadLock };
            return { true, std::format("loads {}\nlast {}\nok {}\nat {}", loadCount.load(),
                               lastLoad.empty() ? "(none)" : lastLoad, lastLoadOk, lastLoadTime.empty() ? "-" : lastLoadTime) };
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
            const auto only = a_parts.size() >= 5 ? a_parts[4] : std::string{};
            return OnMainThread([formID, script, only]() {
                try {
                    return ReadVars(formID, script, only);
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

        if (op == "holds" && a_parts.size() >= 3) {
            const auto formID = a_parts[2];
            return OnMainThread([formID]() {
                try {
                    return HoldsInfo(formID);
                } catch (const std::exception& e) {
                    return Reply{ false, e.what() };
                }
            });
        }

        if (op == "demote" && a_parts.size() >= 3) {
            const auto formID = a_parts[2];
            const auto owner = a_parts.size() >= 4 ? a_parts[3] : std::string{};
            return OnMainThread([formID, owner]() {
                try {
                    return Demote(formID, owner);
                } catch (const std::exception& e) {
                    return Reply{ false, e.what() };
                }
            });
        }

        if (op == "promotehook") {
            const auto parts = a_parts;
            const auto sub = parts.size() >= 3 ? parts[2] : std::string{};
            if (sub == "on") {
                return OnMainThread([parts]() { return PromoteHook::Command(parts); });
            }
            return PromoteHook::Command(parts);  // reading the buffer doesn't need the main thread
        }

        if (op == "demoteall") {
            return OnMainThread([]() { return DemoteAll(); });
        }

        if (op == "demotenear" && a_parts.size() >= 3) {
            const auto radius = std::stof(a_parts[2]);
            return OnMainThread([radius]() { return DemoteNear(radius); });
        }

        if (op == "holdsnear" && a_parts.size() >= 3) {
            const auto radius = std::stof(a_parts[2]);
            const bool deletedOnly = a_parts.size() < 4 || a_parts[3] != "0";
            const auto type = a_parts.size() >= 5 ? a_parts[4] : std::string{};
            return OnMainThread([radius, deletedOnly, type]() { return HoldsNear(radius, deletedOnly, type); });
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

        // placebatch|disablebatch <list> <record> <start> <count>; undobatch <record> <start> <count>
        if ((op == "placebatch" || op == "disablebatch") && a_parts.size() >= 6) {
            try {
                const auto list = a_parts[2];
                const auto record = a_parts[3];
                const auto start = static_cast<std::size_t>(std::stoul(a_parts[4]));
                const auto count = static_cast<std::size_t>(std::stoul(a_parts[5]));
                const bool place = op == "placebatch";
                return OnMainThread([list, record, start, count, place]() {
                    try {
                        return place ? PlaceBatch(list, record, start, count) : DisableBatch(list, record, start, count);
                    } catch (const std::exception& e) {
                        return Reply{ false, e.what() };
                    }
                });
            } catch (const std::exception& e) {
                return Reply{ false, e.what() };
            }
        }
        if (op == "undobatch" && a_parts.size() >= 5) {
            try {
                const auto record = a_parts[2];
                const auto start = static_cast<std::size_t>(std::stoul(a_parts[3]));
                const auto count = static_cast<std::size_t>(std::stoul(a_parts[4]));
                const int  pass = a_parts.size() >= 6 ? std::stoi(a_parts[5]) : 0;
                return OnMainThread([record, start, count, pass]() {
                    try {
                        return UndoBatch(record, start, count, pass);
                    } catch (const std::exception& e) {
                        return Reply{ false, e.what() };
                    }
                });
            } catch (const std::exception& e) {
                return Reply{ false, e.what() };
            }
        }

        // scripted <radius> <script> <var,var,...> [x y z] [enabled_only 0/1]
        if (op == "scripted" && a_parts.size() >= 5) {
            try {
                const auto radius = std::stof(a_parts[2]);
                const auto script = a_parts[3];
                const auto vars = a_parts[4];
                std::optional<RE::NiPoint3> origin;
                if (a_parts.size() >= 8 && !a_parts[5].empty()) {
                    origin = RE::NiPoint3{ std::stof(a_parts[5]), std::stof(a_parts[6]), std::stof(a_parts[7]) };
                }
                const bool enabledOnly = a_parts.size() >= 9 && a_parts[8] == "1";
                return OnMainThread([radius, origin, script, vars, enabledOnly]() {
                    return ScriptedNear(radius, origin, script, vars, enabledOnly);
                });
            } catch (const std::exception& e) {
                return Reply{ false, e.what() };
            }
        }

        if (op == "near" && a_parts.size() >= 3) {
            try {
                const auto radius = std::stof(a_parts[2]);
                const auto type = a_parts.size() >= 4 ? a_parts[3] : std::string{};
                std::optional<RE::NiPoint3> origin;
                if (a_parts.size() >= 7 && !a_parts[4].empty()) {
                    origin = RE::NiPoint3{ std::stof(a_parts[4]), std::stof(a_parts[5]), std::stof(a_parts[6]) };
                }
                const bool enabledOnly = a_parts.size() >= 8 && a_parts[7] == "1";
                return OnMainThread([radius, type, origin, enabledOnly]() { return Near(radius, type, origin, enabledOnly); });
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

        if (op == "av" && a_parts.size() >= 4) {
            const auto target = a_parts[2];
            const std::vector<std::string> names(a_parts.begin() + 3, a_parts.end());
            return OnMainThread([target, names]() {
                try {
                    return ActorValues(target, names);
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

        if (op == "move" && a_parts.size() >= 3) {
            const auto arg = [&](std::size_t i) { return i < a_parts.size() && a_parts[i] != "-" ? a_parts[i] : std::string{}; };
            const auto id = a_parts[2];
            const auto to = arg(3);
            const auto offset = arg(4);
            const auto rotation = arg(5);
            const auto moved = OnMainThread([id, to, offset, rotation]() {
                try {
                    return Move(id, to, offset, rotation);
                } catch (const std::exception& e) {
                    return Reply{ false, e.what() };
                }
            });
            if (!moved.ok) {
                return moved;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));  // a few frames: setpos lands on a later one
            const auto after = OnMainThread([id]() {
                const auto form = FindForm(id);
                const auto ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
                return ref ? Reply{ true, "after:  " + Where(ref) } : Reply{ false, "reference gone" };
            });
            return { true, moved.text + "\n" + after.text +
                               "\n(kept in the save, like setpos: don't save a game you want the plugin's position in)" };
        }

        if (op == "crosshair") {
            float range = 1000.0f;
            try {
                if (a_parts.size() >= 3) {
                    range = std::clamp(std::stof(a_parts[2]), 10.0f, 10000.0f);
                }
            } catch (const std::exception&) {
                return { false, "range must be a number" };
            }
            return OnMainThread([range]() {
                try {
                    return Crosshair(range);
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
        using Messaging = F4SE::MessagingInterface;
        if (a_message->type == Messaging::kPreLoadGame && a_message->data) {
            std::lock_guard lock{ loadLock };
            loadingSave.assign(static_cast<const char*>(a_message->data), a_message->dataLen);
            loadingSave = loadingSave.c_str();  // drop a trailing null counted in dataLen
        }
        if (a_message->type == Messaging::kPostLoadGame || a_message->type == Messaging::kNewGame) {
            {
                std::lock_guard lock{ loadLock };
                lastLoad = a_message->type == Messaging::kNewGame ? std::string{ "(new game)" } : loadingSave;
                lastLoadOk = a_message->type == Messaging::kNewGame || reinterpret_cast<std::uintptr_t>(a_message->data) != 0;
                lastLoadTime = Clock();
            }
            ++loadCount;
        }
        if (a_message->type == Messaging::kGameDataReady) {
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
            F4SE::InitInfo info;
            info.trampoline = true;  // for the dev-only promote hook (PromoteHook), installed only on request
            info.trampolineSize = 256;
            F4SE::Init(a_f4se, info);
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
