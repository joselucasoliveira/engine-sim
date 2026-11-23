#include "cli.hpp"

#include "build_identity.hpp"
#include "lexical.hpp"
#include "read.hpp"
#include "resource_paths.h"
#include "resources.hpp"
#include "sha256.hpp"

#include <engine_sim/config-json.hpp>

#include <algorithm>
#include <array>
#include <cfenv>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <utility>

#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif

namespace engine_sim::legacy::cli {
namespace cfg = config_v1;
namespace json_detail = cfg::json_detail;
namespace {

[[noreturn]] void usage(std::string message) {
    throw Failure(2, "selection", "CONVERT_USAGE", std::move(message));
}

bool identifier(std::string_view name) {
    const auto alpha = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    return !name.empty() && name.size() <= 64 && alpha(name.front()) &&
        std::all_of(name.begin() + 1, name.end(), [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); });
}

std::string legacy_string(std::string_view text) {
    if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f || c == '"'; }))
        usage("The legacy string lexer cannot preserve this string");
    std::size_t trailing = 0;
    for (auto i = text.rbegin(); i != text.rend() && *i == '\\'; ++i) ++trailing;
    if (trailing % 2 != 0) usage("The legacy string lexer cannot preserve a trailing unpaired backslash");
    return '"' + std::string(text) + '"';
}

std::string literal(const Json &value) {
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_string()) return legacy_string(value.get_ref<const std::string &>());
    if (value.is_number_integer()) {
        const auto low = std::numeric_limits<std::int32_t>::min();
        const auto high = std::numeric_limits<std::int32_t>::max();
        if (value.is_number_unsigned()) {
            if (value.get<std::uint64_t>() > static_cast<std::uint64_t>(high))
                usage("Integer argument exceeds the legacy signed 32-bit range");
        }
        else if (value.get<std::int64_t>() < low || value.get<std::int64_t>() > high) {
            usage("Integer argument exceeds the legacy signed 32-bit range");
        }
        const auto integer = value.get<std::int32_t>();
        return integer == low ? "0x80000000" : std::to_string(integer);
    }
    if (!value.is_number_float()) usage("An argument must be a boolean, integer, finite real or string");
    const double real = value.get<double>();
    if (!std::isfinite(real)) usage("An argument must be finite");
    std::array<char, 768> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                      std::abs(real), std::chars_format::fixed);
    if (result.ec != std::errc{}) usage("Real argument cannot be represented by a legacy literal");
    std::string printed(buffer.data(), result.ptr);
    if (printed.find('.') == std::string::npos) printed += ".0";
    std::istringstream verify(printed);
    verify.imbue(std::locale::classic());
    double decoded = 0;
    if (!(verify >> decoded) || !verify.eof() || decoded != std::abs(real))
        usage("Real argument cannot be preserved by the legacy lexer");
    return std::signbit(real) ? '-' + printed : printed;
}

Json from_tape(const json_detail::ParsedJson &parsed, std::uint32_t index) {
    using Kind = json_detail::Kind;
    const auto &node = parsed.nodes[index];
    switch (node.kind) {
        case Kind::boolean: return node.boolean;
        case Kind::string: return parsed.strings[node.text];
        case Kind::null: return nullptr;
        case Kind::number: {
            if (parsed.lexeme(node).find_first_of(".eE") != std::string_view::npos) return node.real;
            const auto integer = json_detail::integer32(parsed, node);
            if (integer.status != json_detail::IntegerStatus::exact)
                usage("Integer argument exceeds the legacy signed 32-bit range");
            return integer.value;
        }
        case Kind::object:
        case Kind::array: {
            auto result = node.kind == Kind::object ? Json::object() : Json::array();
            for (auto i = index + 1; i < node.after; i = parsed.nodes[i].after) {
                auto child = from_tape(parsed, i);
                if (node.kind == Kind::object) result[parsed.names[parsed.nodes[i].key].value] = std::move(child);
                else result.push_back(std::move(child));
            }
            return result;
        }
    }
    throw std::logic_error("Unknown JSON tape node");
}

Json strict_object(std::string_view bytes) {
    auto parsed = json_detail::parse(bytes);
    if (const auto *error = std::get_if<json_detail::ParseError>(&parsed))
        usage(error->diagnostic.code + " " + error->diagnostic.pointer + " " + error->diagnostic.message);
    return from_tape(std::get<json_detail::ParsedJson>(parsed), 0);
}

Json arg_value(std::string_view spelling) {
    const auto bytes = "{\"arg\":" + std::string(spelling) + '}';
    auto object = strict_object(bytes);
    if (object.size() != 1) usage("Each --arg contains exactly one JSON scalar");
    auto json = std::move(object.at("arg"));
    // Preserve integer versus real types and the spelling of real signed zero.
    (void)literal(json);
    return json;
}

fs::path absolute(std::string_view text) {
    if (text.empty() || std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
        usage("A path must be a nonempty absolute UTF-8 path");
    auto path = fs::u8path(text);
    if (!path.is_absolute()) usage("Path must be absolute: " + std::string(text));
    return path;
}

void check_item(Item &item) {
    if (item.key.empty()) usage("An invocation requires a nonempty key");
    if (!item.args.is_object()) usage("Named arguments must form an object");
    if (item.selector != "entry" && item.selector != "main" &&
        !(item.selector.compare(0, 8, "factory:") == 0 && identifier(std::string_view(item.selector).substr(8))))
        usage("Expected entry, main, or factory:IDENT");
    if (item.selector == "entry" && !item.args.empty()) usage("An entry does not accept named arguments");
    for (auto it = item.args.begin(); it != item.args.end(); ++it) {
        if (!identifier(it.key())) usage("Invalid formal name: " + it.key());
        (void)literal(it.value());
    }
    if (item.output) protect(*item.output, {item.entry});
}

Item from_json(const Json &value) {
    static const std::set<std::string> keys{
        "key","entry","library_root","asset_root","resource_root","selector","args","output","validate_only"
    };
    if (!value.is_object()) usage("Each batch item must be an object");
    for (auto it = value.begin(); it != value.end(); ++it)
        if (!keys.count(it.key())) usage("Unknown batch item property: " + it.key());
    const bool output = value.contains("output");
    const bool validation = value.contains("validate_only");
    if (output == validation || (validation && value.at("validate_only") != true))
        usage("Each item requires exactly one output or validate_only:true");
    Item item;
    item.key = value.at("key").get<std::string>();
    item.entry = absolute(value.at("entry").get<std::string>());
    item.library = absolute(value.at("library_root").get<std::string>());
    item.assets = absolute(value.at("asset_root").get<std::string>());
    item.resources = absolute(value.at("resource_root").get<std::string>());
    item.selector = value.at("selector").get<std::string>();
    item.args = value.at("args");
    if (output) item.output = destination(value.at("output").get<std::string>());
    check_item(item);
    return item;
}

Json plan_item(const Item &item) {
    return {{"key", item.key}, {"entry", item.entry.u8string()}, {"selector",item.selector}, {"args",item.args},
        {"library_root",item.library.u8string()}, {"asset_root",item.assets.u8string()},
        {"resource_root",item.resources.u8string()}, {"output",item.output ? Json(item.output->u8string()) : Json(nullptr)}};
}

fs::path canonical_input(const fs::path &path, bool directory) {
    std::error_code error;
    auto result = fs::canonical(path, error);
    if (error || (directory ? !fs::is_directory(result, error) : !fs::is_regular_file(result, error)) || error)
        throw Failure(5, "paths", "CONVERT_INPUT_PATH",
                      directory ? "Expected an existing directory" : "Expected an existing regular source file", path);
    return result;
}

Json tagged_path(const fs::path &path, const Item &item, const fs::path &work) {
    const std::array<std::pair<const char *,fs::path>,5> roots{{
        {"entry",item.entry.parent_path()}, {"library",item.library}, {"assets",item.assets},
        {"resources",item.resources}, {"work",work}
    }};
    for (const auto &[tag,root] : roots) {
        const auto relative = path.lexically_relative(root);
        if (!relative.empty() && relative.begin()->u8string() != "..")
            return {{"root",tag},{"relative",relative.generic_u8string()}};
    }
    return {{"root","absolute"},{"relative",path.generic_u8string()}};
}

std::string wrapper_text(const Item &item) {
    std::string result;
    const auto standard_library = item.library / "engine_sim.mr";
    std::error_code error;
    if (item.selector != "main" || (!item.args.empty() && fs::is_regular_file(standard_library,error)))
        result = "import " + legacy_string(standard_library.generic_u8string()) + '\n';
    result += "import " + legacy_string(item.entry.generic_u8string()) + '\n';
    const bool factory = item.selector != "main";
    result += factory ? "set_engine(" + item.selector.substr(8) + '(' : "main(";
    bool first = true;
    for (auto it = item.args.begin(); it != item.args.end(); ++it) {
        if (!std::exchange(first,false)) result += ", ";
        result += it.key() + ": " + literal(it.value());
    }
    result += factory ? "))\n" : ")\n";
    return result;
}

}

Failure::Failure(int exit, std::string stage, std::string code, std::string message,
                 fs::path path, std::string pointer)
    : std::runtime_error(std::move(message)), m_exit(exit), m_stage(std::move(stage)), m_code(std::move(code)),
      m_path(std::move(path)), m_pointer(std::move(pointer)) {}

Json Failure::record() const {
    return {{"stage",m_stage},{"code",m_code},{"path",m_path.empty() ? Json(nullptr) : Json(m_path.u8string())},
            {"pointer",m_pointer},{"message",what()},{"exit",m_exit}};
}

Plan arguments(const std::vector<std::string> &argv) {
    Plan plan;
    Item one;
    std::set<std::string> seen;
    bool validate_only = false;
    for (std::size_t i=1;i<argv.size();++i) {
        const auto &key=argv[i];
        if (key!="--arg" && !seen.insert(key).second) usage("Duplicate option: "+key);
        if (key=="--force") { plan.force=true; continue; }
        if (key=="--validate-only") { validate_only=true; continue; }
        if (++i==argv.size()) usage("Missing value for "+key);
        const auto &val=argv[i];
        if (key=="--entry") one.entry=absolute(val);
        else if (key=="--manifest") plan.manifest=destination(val);
        else if (key=="--library-root") one.library=absolute(val);
        else if (key=="--asset-root") one.assets=absolute(val);
        else if (key=="--resource-root") one.resources=absolute(val);
        else if (key=="--selector") one.selector=val;
        else if (key=="--output") one.output=destination(val);
        else if (key=="--batch") plan.batch_path=absolute(val);
        else if (key=="--arg") {
            const auto separator=val.find('=');
            if (separator==std::string::npos) usage("Expected --arg NAME=JSON_SCALAR");
            const auto name=val.substr(0,separator);
            if (!identifier(name) || one.args.contains(name)) usage("Invalid or duplicate named argument: "+name);
            one.args[name]=arg_value(std::string_view(val).substr(separator+1));
        }
        else usage("Unknown option: "+key);
    }
    if (plan.manifest.empty()) usage("Missing --manifest");
    if (!plan.batch_path.empty()) {
        if (!one.args.empty() || std::any_of(seen.begin(),seen.end(),[](const auto &name) {
            return name!="--batch" && name!="--manifest" && name!="--force";
        })) usage("Use only --batch PLAN --manifest RESULT [--force] for a batch");
        const auto input=strict_object(read_file(plan.batch_path,json_detail::document_bytes,2));
        if (input.size()!=2 || !input.contains("record_version") || !input.at("record_version").is_number_integer() ||
            input.at("record_version")!=1 || !input.contains("items") ||
            !input.at("items").is_array() || input.at("items").empty())
            usage("Expected a batch object with record_version 1 and a nonempty items array");
        for (const auto &item:input.at("items")) plan.items.push_back(from_json(item));
    }
    else {
        if (one.entry.empty() || one.library.empty() || one.assets.empty() || one.resources.empty() ||
            (one.output.has_value()==validate_only)) usage("Specify one entry, its three roots, and exactly one output or --validate-only");
        one.key=one.entry.stem().u8string();
        check_item(one);
        plan.items.push_back(std::move(one));
    }
    std::set<std::string> keys;
    auto journal_path = plan.manifest;
    journal_path += ".journal";
    std::set<fs::path> destinations{plan.manifest, journal_path};
    std::vector<fs::path> inputs;
    if (!plan.batch_path.empty()) inputs.push_back(plan.batch_path);
    for (const auto &item:plan.items) inputs.push_back(item.entry);
    protect(plan.manifest,inputs);
    protect(journal_path,inputs);
    for (const auto &item:plan.items) {
        if (!keys.insert(item.key).second) usage("Duplicate key: "+item.key);
        if (item.output) {
            protect(*item.output,inputs);
            if (!destinations.insert(*item.output).second) usage("Output collides with another destination");
        }
    }
    return plan;
}

std::string plan_identity(const Plan &plan) {
    auto rows=Json::array();
    for (const auto &item:plan.items) rows.push_back(plan_item(item));
    return sha256(Json{{"force",plan.force},{"items",rows},{"manifest",plan.manifest.u8string()},
                      {"batch",plan.batch_path.u8string()}}.dump());
}

Json executable_identity() {
    std::string host = build_host;
#ifdef __GLIBC__
    host += std::string("; glibc ") + gnu_get_libc_version();
#endif
    return {{"revision",build_revision},{"executable_sha256",sha256_file(engine_sim::executablePath())},
        {"source_state",source_state},{"source_fingerprint",source_fingerprint},
        {"parser_revision",parser_revision},{"host",host},{"compiler",build_compiler},
        {"flags",std::string(build_flags)+"; fenv rounding "+std::to_string(std::fegetround())},{"lock_sha256",lock_sha256}};
}

Json base_record(const Item &item, const Json &identity) {
    return {{"record_version",1},{"key",item.key},{"status","evaluating"},
        {"entry",{{"path",item.entry.u8string()},{"sha256",nullptr},{"selector",item.selector},
                  {"args",item.args},{"wrapper_sha256",nullptr}}},
        {"converter",identity},{"inputs",Json::array()},{"resources",Json::array()},{"output",nullptr},
        {"original",nullptr},{"checks",Json::array()},{"ignored_inputs",Json::array()},{"failure",nullptr},
        {"rights",{{"status","private-local"},{"source_notice","Preserve the selected source and assets' original notices"},
                   {"asset_gate","pending"}}}};
}

std::string evaluate(const Item &unresolved, const fs::path &manifest, Json &record) {
    auto item=unresolved;
    item.entry=canonical_input(item.entry,false);
    item.library=canonical_input(item.library,true);
    item.assets=canonical_input(item.assets,true);
    item.resources=canonical_input(item.resources,true);
    const auto entry_hash=sha256_file(item.entry);
    record["entry"]["path"]=item.entry.u8string();
    record["entry"]["sha256"]=entry_hash;
    const auto work=manifest.parent_path();
    auto compile_entry=item.entry;
    if (item.selector!="entry") {
        const auto wrapper=wrapper_text(item);
        const auto digest=sha256(wrapper);
        compile_entry=work/fs::u8path(manifest.filename().u8string()+"."+digest.substr(0,16)+".mr");
        const auto state=fs::symlink_status(compile_entry);
        protect(compile_entry,{item.entry});
        if (fs::exists(state)) {
            if (!fs::is_regular_file(state) || read_file(compile_entry,65536,6)!=wrapper)
                throw Failure(6,"output","CONVERT_COLLISION","Wrapper path already has different bytes",compile_entry);
        }
        else publish(compile_entry,wrapper,false);
        record["entry"]["wrapper_sha256"]=digest;
        record["entry"]["wrapper_path"]=compile_entry.u8string();
    }
    bool selected_input=false;
    auto verify=[&](const es_script::ScriptInput &input) {
        const auto &path=input.resolvedPath;
        const auto digest=sha256(input.bytes);
        if (path==item.entry) {
            if (digest!=entry_hash) throw es_script::Compiler::Error({"resources",path.u8string(),"Entry bytes changed during evaluation"});
            selected_input=true;
        }
        const auto role=path==item.entry ? "entry" : path==compile_entry ? "wrapper" : "import";
        record["inputs"].push_back({{"role",role},{"path",tagged_path(path,item,work)},
            {"canonical_source",path.u8string()},{"sha256",digest},{"length",input.bytes.size()}});
    };
    Snapshot staged;
    try { staged=capture({compile_entry,item.library,item.assets},verify); }
    catch (const es_script::Compiler::Error &error) {
        const auto &problem=error.diagnostic;
        const bool selector_error=item.selector!="entry" && problem.stage=="compile" &&
                problem.message.find(compile_entry.stem().u8string()+"(")!=std::string::npos;
        const int exit=selector_error ? 2 : problem.stage=="resources" ? 5 : problem.stage=="graph" ? 4 : 3;
        throw Failure(exit,problem.stage,selector_error ? "CONVERT_SELECTOR" : "CONVERT_LEGACY",
                      problem.message,fs::u8path(problem.path));
    }
    if (!selected_input) throw Failure(3,"execute","CONVERT_INPUT","Selected source was not observed",item.entry);

    auto binding=resolve(std::move(staged),item.resources);
    if (const auto *error=std::get_if<cfg::Diagnostic>(&binding))
        throw Failure(error->stage=="resources" ? 5 : 4,error->stage,error->code,error->message,
                      error->path ? fs::u8path(*error->path) : item.entry,error->pointer);
    auto model=std::get<ResolvedSnapshot>(std::move(binding));
    const auto &definitions=model.model.values().components;
    for (std::size_t channel=0;channel<model.model.topology().exhausts.size();++channel) {
        const auto &exhaust=definitions.exhausts.at(model.model.topology().exhausts[channel]);
        const auto impulse=std::find_if(model.impulses.begin(),model.impulses.end(),[&](const auto &entry) {
            return entry.origin.impulse==exhaust.impulse_response;
        });
        if (impulse==model.impulses.end()) throw std::logic_error("Selected exhaust has no pinned impulse");
        const auto &wave=model.waves.at(impulse->wave);
        const auto &parameter=definitions.impulses.at(static_cast<std::size_t>(impulse-model.impulses.begin()));
        const auto raw=std::string_view(reinterpret_cast<const char *>(wave.bytes.data()),wave.bytes.size());
        record["resources"].push_back({{"channel",channel},{"impulse_id",impulse->origin.impulse.value},
            {"declaring_unit",tagged_path(fs::canonical(impulse->origin.declaring_unit),item,work)},
            {"declared_name",impulse->origin.evaluated_filename},{"canonical_source",wave.canonical_path.u8string()},
            {"path",wave.relative_path},{"sha256",sha256(raw)},{"length",wave.bytes.size()},
            {"rate_hz",wave.rate},{"channels",wave.channels},{"pcm_bits",wave.encoding_bits},{"frames",wave.samples},
            {"gain",parameter.volume.value}});
    }
    for (const auto &ignored:model.ignored_inputs)
        record["ignored_inputs"].push_back({{"source",ignored.component_id},{"construct","intake"},
            {"field",ignored.name},{"value",ignored.value},{"reason","Evaluated input has no R2 effect"}});

    record["harmonic_lobes"] = Json::array();
    for (const auto &[id, inputs] : model.harmonics) {
        record["harmonic_lobes"].push_back({{"id", id}, {"duration_at_50_thou_rad", inputs.duration_at_50_thou_rad},
            {"max_lift_m", inputs.lift_m}, {"gamma", inputs.gamma}, {"steps", inputs.steps}});
    }

    auto output=cfg::write_json(model.model);
    const auto decoded=cfg::read_json(output);
    if (const auto *error=std::get_if<cfg::Diagnostic>(&decoded))
        throw Failure(4,error->stage,error->code,error->message,item.entry,error->pointer);
    if (cfg::write_json(std::get<cfg::NormalizedDocument>(decoded))!=output)
        throw Failure(4,"json","CONVERT_ROUNDTRIP","Canonical model changed on strict re-read",item.entry);
    const auto hash=sha256(output);
    record["output"]={{"path",item.output ? Json(item.output->u8string()) : Json(nullptr)},
        {"format_version",1},{"schema_id","urn:engine-sim:config:1"},{"schema_sha256",json_detail::schema_digest()},
        {"writer_revision",build_revision},{"bytes_sha256",hash},{"semantic_sha256",hash}};
    for (const auto *gate:{"structure","constructed identity","behavior","runtime closure","rights"}) {
        const bool passed=std::string_view(gate)=="structure";
        record["checks"].push_back({{"gate",gate},{"status",passed ? "pass" : "pending"},
            {"evidence",passed ? Json{{"revision",build_revision},{"sha256",hash}} : Json(nullptr)}});
    }
    return output;
}

}
