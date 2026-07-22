#include "plume/cli.hpp"

#include <unistd.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "plume/codec.hpp"
#include "plume/config.hpp"
#include "plume/provider.hpp"
#include "plume/store.hpp"
#include "plume/sync.hpp"
#include "plume/terminal.hpp"
#include "plume/theme.hpp"
#include "plume/weave.hpp"

#ifndef PLUME_VERSION
#define PLUME_VERSION "0.1.0"
#endif

namespace plume {

namespace {

void err(const std::string& m) {
	std::fputs(("plume: " + m + "\n").c_str(), stderr);
}

std::string read_stdin() {
	std::string s;
	char buf[4096];
	std::size_t n = 0;
	while ((n = std::fread(buf, 1, sizeof buf, stdin)) > 0) s.append(buf, n);
	return s;
}

bool has_flag(const std::vector<std::string>& a, std::string_view f) {
	for (const auto& x : a)
		if (x == f) return true;
	return false;
}

std::string flag_value(const std::vector<std::string>& a, std::string_view f) {
	for (std::size_t i = 0; i + 1 < a.size(); ++i)
		if (a[i] == f) return a[i + 1];
	return {};
}

result<std::unique_ptr<provider>> provider_from(const config& cfg) {
	const provider_entry* pe = cfg.active_provider();
	if (!pe) return fail(errc::config, "no provider configured — run plume and finish the wizard");
	provider_config pc;
	pc.kind = pe->kind;
	pc.base_url = pe->base_url;
	pc.credential.value = pe->auth_value;
	if (pe->auth_source == "key_cmd")
		pc.credential.kind = auth::source::key_cmd;
	else if (pe->auth_source == "keychain")
		pc.credential.kind = auth::source::keychain;
	else if (pe->auth_source == "inline")
		pc.credential.kind = auth::source::inline_key;
	else
		pc.credential.kind = auth::source::env;
	return make_provider(pc);
}

std::string resolve_model(const config& cfg, provider& prov, const std::string& override_id) {
	if (!override_id.empty()) return override_id;
	const provider_entry* pe = cfg.active_provider();
	if (pe && !pe->default_model.empty()) return pe->default_model;
	if (!cfg.defaults.model.empty()) return cfg.defaults.model;
	// last resort: ask the provider and prefer an opus-class id.
	if (auto models = prov.list_models(); models && !models->empty()) {
		for (const auto& m : *models)
			if (m.id.find("opus") != std::string::npos) return m.id;
		return models->front().id;
	}
	return {};
}

int cmd_ask(const std::vector<std::string>& args) {
	auto cfg = load_config(default_config_path());
	if (!cfg) return err(cfg.error().detail), 1;

	auto prov = provider_from(*cfg);
	if (!prov) return err(prov.error().detail), 1;

	std::string question;
	for (const auto& a : args)
		if (a.rfind("--", 0) != 0 && flag_value(args, "--model") != a) question += a + " ";
	if (!isatty(STDIN_FILENO)) {
		std::string piped = read_stdin();
		if (!piped.empty()) question += "\n" + piped;
	}
	if (question.empty()) return err("nothing to ask (give a question or pipe stdin)"), 1;

	const std::string model = resolve_model(*cfg, **prov, flag_value(args, "--model"));
	if (model.empty()) return err("no model — set --model or defaults.model in config"), 1;

	request req;
	req.params = cfg->defaults;
	req.params.model = model;
	if (req.params.max_tokens < 1024) req.params.max_tokens = 4096;
	req.messages.push_back(message::text(role::user, question));

	const bool json_out = has_flag(args, "--json");
	const bool no_stream = has_flag(args, "--no-stream");

	std::string collected;
	auto on_delta = [&](const stream_delta& d) {
		if (d.type == stream_delta::kind::text) {
			collected += d.text;
			if (!no_stream && !json_out) {
				std::fputs(d.text.c_str(), stdout);
				std::fflush(stdout);
			}
		}
	};
	auto never_stop = [] { return false; };

	auto out = (*prov)->stream(req, on_delta, never_stop);
	if (!out) return err(out.error().detail), 1;

	if (json_out) {
		const nlohmann::json j = {{"model", out->model},
		                          {"stop_reason", out->stop_reason},
		                          {"text", collected},
		                          {"tokens_in", out->tokens.input},
		                          {"tokens_out", out->tokens.output}};
		std::fputs(j.dump().c_str(), stdout);
		std::fputc('\n', stdout);
	} else if (no_stream) {
		std::fputs(collected.c_str(), stdout);
		std::fputc('\n', stdout);
	} else {
		std::fputc('\n', stdout);
	}
	return 0;
}

int cmd_doctor() {
	std::printf("plume %s\n\n", PLUME_VERSION);

	const std::string cfg_path = default_config_path();
	auto cfg = load_config(cfg_path);
	std::printf("config   %s %s\n", cfg_path.c_str(),
	            cfg ? "(ok)" : ("(error: " + cfg.error().detail + ")").c_str());
	if (!cfg) return 1;

	const std::string db = cfg->data_dir + "/plume.sqlite";
	if (auto s = store::open(db)) {
		std::printf("store    %s %s\n", db.c_str(),
		            s->check_integrity().value_or(false) ? "(ok)" : "(integrity failed)");
	} else {
		std::printf("store    %s (error: %s)\n", db.c_str(), s.error().detail.c_str());
	}

	const term::capabilities caps = term::probe();
	std::printf("terminal truecolor=%d kitty=%d sixel=%d osc52=%d italics=%d dark=%d\n",
	            caps.truecolor, caps.kitty_graphics, caps.sixel, caps.osc52, caps.italics,
	            caps.dark);

	if (const provider_entry* pe = cfg->active_provider()) {
		std::printf("provider %s (%s)\n", cfg->default_provider.c_str(), pe->kind.c_str());
		if (auto prov = provider_from(*cfg)) {
			auto models = (*prov)->list_models();
			std::printf("models   %s\n",
			            models ? (std::to_string(models->size()) + " available").c_str()
			                   : ("unreachable: " + models.error().detail).c_str());
		} else {
			std::printf("auth     error: %s\n", prov.error().detail.c_str());
		}
	} else {
		std::printf("provider none configured\n");
	}

	std::printf("mcp      %zu server(s) configured\n", cfg->mcp.size());
	return 0;
}

int cmd_themes() {
	auto cfg = load_config(default_config_path());
	const std::string dir = (cfg ? cfg->config_dir : default_config_path()) + "/themes";
	std::printf("built-in: rose-pine, rose-pine-moon, rose-pine-dawn, va11\n");
	std::printf("user dir: %s\n", dir.c_str());
	return 0;
}

int cmd_config(const std::vector<std::string>& args) {
	const std::string path = default_config_path();
	if (!args.empty() && args[0] == "edit") {
		const char* editor = std::getenv("EDITOR");
		const std::string cmd = std::string(editor && *editor ? editor : "vi") + " " + path;
		return std::system(cmd.c_str()) == 0 ? 0 : 1;
	}
	std::printf("%s\n", path.c_str());
	return 0;
}

int cmd_export(const std::vector<std::string>& args) {
	if (args.empty())
		return err("usage: plume export <conversation-id> [--format md|json|html]"), 1;
	auto cfg = load_config(default_config_path());
	if (!cfg) return err(cfg.error().detail), 1;
	auto s = store::open(cfg->data_dir + "/plume.sqlite");
	if (!s) return err(s.error().detail), 1;

	const convo_id id{args[0]};
	std::string format = flag_value(args, "--format");
	if (format.empty()) format = "md";

	weave w(*s);
	if (format == "json") {
		auto j = w.to_json(id);
		if (!j) return err(j.error().detail), 1;
		std::fputs(j->c_str(), stdout);
		return 0;
	}
	auto path = w.active_path(id);
	if (!path) return err(path.error().detail), 1;

	if (format == "html") {
		std::printf("<!doctype html><meta charset=utf-8><title>plume export</title>\n");
		std::printf(
		    "<body "
		    "style=\"background:#191724;color:#e0def4;font-family:monospace;max-width:48rem;margin:"
		    "2rem auto\">\n");
		for (const auto& n : *path) {
			auto blocks = codec::decode_blocks(n.content_json);
			std::string body = blocks ? message{n.role, *blocks}.plain_text() : n.content_json;
			std::printf(
			    "<section><h3 style=\"color:#9ccfd8\">%s</h3><pre "
			    "style=\"white-space:pre-wrap\">%s</pre></section>\n",
			    std::string(to_string(n.role)).c_str(), body.c_str());
		}
		std::printf("</body>\n");
		return 0;
	}

	// markdown (default)
	for (const auto& n : *path) {
		auto blocks = codec::decode_blocks(n.content_json);
		std::string body = blocks ? message{n.role, *blocks}.plain_text() : n.content_json;
		std::printf("## %s\n\n%s\n\n", std::string(to_string(n.role)).c_str(), body.c_str());
	}
	return 0;
}

int cmd_import(const std::vector<std::string>& args) {
	if (args.empty()) return err("usage: plume import <claude-export-dir>"), 1;
	auto cfg = load_config(default_config_path());
	if (!cfg) return err(cfg.error().detail), 1;
	auto s = store::open(cfg->data_dir + "/plume.sqlite");
	if (!s) return err(s.error().detail), 1;

	auto backend = open_export(args[0]);
	if (!backend) return err(backend.error().detail), 1;
	auto stats = (*backend)->import_into(*s);
	if (!stats) return err(stats.error().detail), 1;
	std::printf("imported %d conversations, %d nodes (%d new, %d updated)\n", stats->conversations,
	            stats->nodes, stats->created, stats->updated);
	return 0;
}

void print_help() {
	std::fputs(
	    "plume — a quill for terminals\n\n"
	    "  plume                 open the tui\n"
	    "  plume ask \"...\"        stream an answer (‑‑model ‑‑json ‑‑no-stream; reads stdin)\n"
	    "  plume export <id>     export a conversation (‑‑format md|json|html)\n"
	    "  plume import <dir>    import a claude.ai data export\n"
	    "  plume themes          list themes\n"
	    "  plume doctor          check config, db, terminal, provider\n"
	    "  plume config edit     open the config in $EDITOR\n",
	    stdout);
}

}  // namespace

int run_cli(int argc, char** argv) {
	if (argc < 2) return -1;  // no subcommand: fall through to the tui
	const std::string sub = argv[1];
	const std::vector<std::string> rest(argv + 2, argv + argc);

	if (sub == "ask") return cmd_ask(rest);
	if (sub == "doctor") return cmd_doctor();
	if (sub == "themes") return cmd_themes();
	if (sub == "config") return cmd_config(rest);
	if (sub == "export") return cmd_export(rest);
	if (sub == "import") return cmd_import(rest);
	if (sub == "demo") {
		setenv("PLUME_MOCK", "1", 1);
		return -1;  // the tui handles the demo, in mock mode
	}
	if (sub == "--version" || sub == "-v") return std::printf("plume %s\n", PLUME_VERSION), 0;
	if (sub == "--help" || sub == "-h") return print_help(), 0;
	return -1;  // anything else (e.g. a conversation id to resume) goes to the tui
}

}  // namespace plume
