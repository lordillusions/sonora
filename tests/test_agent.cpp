#include "TestFramework.h"
#include "agent/AgentProtocol.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
using namespace sonora;
namespace
{
using control::object;
bool rejects(const std::function<void()>& fn) { try { fn(); } catch (const std::exception&) { return true; } return false; }
juce::var chatCall(const char* id, const char* name, juce::var args)
{
    const auto function = object({{"name", name}, {"arguments", juce::JSON::toString(args, true)}});
    const auto call = object({{"id", id}, {"type", "function"}, {"function", function},
        {"extra_content", object({{"google", object({{"thought_signature", "opaque-signature"}})}})}});
    const auto message = object({{"role", "assistant"}, {"content", juce::var()},
        {"reasoning_content", "private provider continuation"}, {"tool_calls", juce::Array<juce::var>{call}}});
    return object({{"choices", juce::Array<juce::var>{object({{"finish_reason", "tool_calls"}, {"message", message}})}}});
}
struct NativeHarness
{
    std::unique_ptr<model::Song> song = io::parseAls(*io::createNewSetDom(), "/tmp/agent-test.als").song;
    model::EditHistory history;
    juce::String document = "native-document";
    int external = 0;
    control::Backend backend()
    {
        control::Backend b;
        b.state = [this] { control::State s; s.song = song.get(); s.documentId = document.toStdString();
            s.revision = std::to_string(history.version()) + ":" + std::to_string(external); return s; };
        b.apply = [this](model::EditCommand c) { history.perform(*song, std::move(c)); return control::ApplyResult::success(); };
        b.undo = [this] { history.undo(*song); return control::ApplyResult::success(); };
        b.parameters = [](int, int) {
            juce::Array<juce::var> entries;
            for (int i = 0; i < 1000; ++i) entries.add(object({{"parameterId", juce::String(i)}, {"name", "Oscillator " + juce::String(i)},
                {"normalized", .5}, {"display", "50 %"}, {"writable", true}}));
            return juce::var(entries);
        };
        return b;
    }
};
}
TEST_CASE("agent: provider presets, endpoint validation and credential-free settings")
{
    CHECK(agent::providers().size() == 7);
    for (const auto& p : agent::providers())
    {
        const auto c = agent::preset(p.id); CHECK(! c.enabled);
        if (c.provider == "custom") CHECK(! agent::validate(c).ok);
        else { CHECK(agent::validate(c).ok); CHECK(agent::requestUrl(c).startsWith("https://")); }
        CHECK(! agent::configJson(c).hasProperty("apiKey"));
    }
    auto c = agent::preset("custom"); c.model = "local-model";
    for (const auto* url : {"http://example.com/v1", "https://user:pass@example.com/v1", "https://api.example.com/v1?key=secret", "https://api.example.com/v1#fragment", "file:///tmp/model"})
    { c.baseUrl = url; CHECK(! agent::validate(c).ok); }
    c.baseUrl = "http://127.0.0.1:18081/v1"; CHECK(agent::validate(c).ok);
    CHECK(agent::requestUrl(c) == "http://127.0.0.1:18081/v1/chat/completions");
    c.enabled = true; auto roundtrip = agent::configFromJson(agent::configJson(c)); CHECK(roundtrip.enabled); CHECK(roundtrip.model == c.model);
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("agent-settings-" + juce::Uuid().toString() + ".json");
    agent::Settings settings(file); c.enabled = false; REQUIRE(settings.save(c, {}).ok); CHECK(! settings.load().enabled);
    CHECK(! file.loadFileAsString().containsIgnoreCase("apikey")); file.deleteFile();
}
TEST_CASE("agent: Responses framing preserves encrypted continuation and maps tool outputs")
{
    agent::Conversation c(agent::preset("openai"), "Set tempo to 128");
    const auto request = c.request(); CHECK(! static_cast<bool>(request["store"])); CHECK(request["input"][0]["role"].toString() == "user");
    REQUIRE(request["tools"].isArray()); CHECK(request["tools"].size() == 11);
    CHECK(! juce::JSON::toString(request["tools"]).contains("$ref"));
    const auto response = object({{"status", "completed"}, {"output", juce::Array<juce::var>{
        object({{"type", "reasoning"}, {"encrypted_content", "opaque-state"}}),
        object({{"type", "function_call"}, {"call_id", "call-1"}, {"name", "sonora_project_inspect"}, {"arguments", "{}"}})}}});
    const auto turn = c.receive(response); REQUIRE(turn.calls.size() == 1);
    c.addResult(turn.calls[0], object({{"ok", true}, {"revision", "1"}}));
    const auto followup = c.request(); CHECK(followup["input"][1]["encrypted_content"].toString() == "opaque-state");
    CHECK(followup["input"][3]["type"].toString() == "function_call_output"); CHECK(followup["input"][3]["call_id"].toString() == "call-1");
    CHECK(rejects([&] { c.receive(response); }));
}
TEST_CASE("agent: compatible Chat preserves reasoning and Gemini signatures without displaying them")
{
    agent::Conversation c(agent::preset("gemini"), "Inspect my project");
    const auto turn = c.receive(chatCall("chat-1", "sonora_project_inspect", object({}))); REQUIRE(turn.calls.size() == 1);
    CHECK(turn.text.isEmpty()); c.addResult(turn.calls[0], object({{"ok", true}}));
    const auto wire = juce::JSON::toString(c.request()["tools"], true);
    CHECK(! wire.contains("\"oneOf\"")); CHECK(! wire.contains("\"const\"")); CHECK(! wire.contains("\"not\"")); CHECK(wire.contains("\"anyOf\""));
    const auto messages = c.request()["messages"];
    CHECK(messages[0]["role"].toString() == "system"); CHECK(messages[2]["reasoning_content"].toString() == "private provider continuation");
    CHECK(messages[2]["tool_calls"][0]["extra_content"]["google"]["thought_signature"].toString() == "opaque-signature");
    CHECK(messages[3]["role"].toString() == "tool"); CHECK(messages[3]["tool_call_id"].toString() == "chat-1");
}
TEST_CASE("agent: Anthropic groups tool results and retains thinking signatures")
{
    agent::Conversation c(agent::preset("anthropic"), "Inspect project and capabilities");
    const auto response = object({{"stop_reason", "tool_use"}, {"content", juce::Array<juce::var>{
        object({{"type", "thinking"}, {"thinking", "private"}, {"signature", "opaque"}}),
        object({{"type", "tool_use"}, {"id", "a"}, {"name", "sonora_project_inspect"}, {"input", object({})}}),
        object({{"type", "tool_use"}, {"id", "b"}, {"name", "sonora_capabilities_get"}, {"input", object({})}})}}});
    const auto turn = c.receive(response); REQUIRE(turn.calls.size() == 2); CHECK(turn.text.isEmpty());
    for (const auto& call : turn.calls) c.addResult(call, object({{"ok", true}}));
    const auto request = c.request(); CHECK(request["system"].isString()); CHECK(static_cast<int>(request["max_tokens"]) == 4096);
    CHECK(request["messages"][1]["content"][0]["signature"].toString() == "opaque");
    CHECK(request["messages"][2]["content"].size() == 2); CHECK(request["messages"][2]["content"][1]["tool_use_id"].toString() == "b");
}
TEST_CASE("agent: malformed, unknown and incomplete model calls never become executable turns")
{
    auto config = agent::preset("deepseek");
    for (const auto* function : {"arbitrary_shell", "save_file"})
    { agent::Conversation c(config, "Hello"); CHECK(rejects([&] { c.receive(chatCall("id", function, object({}))); })); }
    agent::Conversation incomplete(config, "Hello");
    auto response = chatCall("id", "sonora_project_inspect", object({})); response["choices"][0].getDynamicObject()->setProperty("finish_reason", "length");
    CHECK(rejects([&] { incomplete.receive(response); }));
    CHECK(rejects([] { agent::parseJson(juce::String::repeatedString("[", 65) + juce::String::repeatedString("]", 65)); }));
    CHECK(rejects([] { agent::parseJson("{\"unfinished\":"); }));
    for (const auto& malformed : {juce::var(), juce::var(juce::Array<juce::var>{}), juce::var("invalid")})
    {
        agent::Conversation c(config, "Inspect");
        auto invalid = chatCall("bad", "sonora_project_inspect", object({}));
        invalid["choices"][0]["message"]["tool_calls"][0].getDynamicObject()->setProperty("function", malformed);
        CHECK(rejects([&] { c.receive(invalid); }));
    }
    agent::Conversation oversized(agent::preset("anthropic"), "Inspect");
    const auto bigCall = object({{"stop_reason", "tool_use"}, {"content", juce::Array<juce::var>{object({
        {"type", "tool_use"}, {"id", "big"}, {"name", "sonora_project_inspect"},
        {"input", object({{"extra", juce::String::repeatedString("x", 65537)}})}})}}});
    CHECK(rejects([&] { oversized.receive(bigCall); }));
}
TEST_CASE("agent: native tool adapter shares revision, target, undo and filtered paging guards")
{
    NativeHarness h; control::ControlService service(h.backend());
    auto inspect = agent::executeTool(service, {"a", "sonora_project_inspect", object({})}, h.document, "a"); REQUIRE(static_cast<bool>(inspect["ok"]));
    auto args = object({{"expectedRevision", inspect["revision"]}, {"commands", juce::Array<juce::var>{object({{"type", "tempo.set"}, {"bpm", 128}})}}});
    auto result = agent::executeTool(service, {"b", "sonora_edits_apply", args}, h.document, "b"); REQUIRE(static_cast<bool>(result["ok"]));
    CHECK_NEAR(h.song->tempo, 128, 1e-9); CHECK(static_cast<bool>(service.context()["undoAvailable"]));
    auto undo = object({{"expectedRevision", result["revision"]}, {"undoToken", result["data"]["undoToken"]}});
    REQUIRE(static_cast<bool>(agent::executeTool(service, {"c", "sonora_history_undo", undo}, h.document, "c")["ok"]));
    CHECK_NEAR(h.song->tempo, 120, 1e-9); CHECK(! static_cast<bool>(service.context()["undoAvailable"]));
    CHECK(! static_cast<bool>(agent::executeTool(service, {"d", "sonora_edits_apply", args}, h.document, "d")["ok"]));
    h.document = "another-document";
    CHECK(agent::executeTool(service, {"e", "sonora_project_inspect", object({})}, "native-document", "e")["error"]["code"].toString() == "stale_document");
    auto query = object({{"trackId", h.song->tracks[0].id}, {"deviceId", 0}, {"filter", "Oscillator 9"}, {"offset", 1}, {"limit", 2}});
    auto values = agent::executeTool(service, {"f", "sonora_parameters_get", query}, h.document, "f");
    REQUIRE(static_cast<bool>(values["ok"])); CHECK(values["data"]["parameters"].size() == 2);
    CHECK(values["data"]["parameters"][0]["parameterId"].toString() == "90"); CHECK(static_cast<bool>(values["data"]["hasMore"]));
    CHECK(agent::describeResult("MCP", "edits.apply", result).contains("Applied"));
}
