// provider construction: resolve the credential, pick the base url, build the
// right backend. the setup wizard never writes a key to plaintext without
// saying so — that promise is upstream; here we only read.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "plume/provider.hpp"

#if defined(PLUME_HAVE_LIBSECRET)
#include <libsecret/secret.h>
#endif

namespace plume {

// defined in anthropic.cpp / openai.cpp.
std::unique_ptr<provider> make_anthropic(const std::string& base, const std::string& key);
std::unique_ptr<provider> make_openai_compatible(const std::string& kind, const std::string& base,
                                                 const std::string& key);

namespace {

std::string trim(std::string s) {
	const auto not_space = [](unsigned char c) { return !std::isspace(c); };
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
	s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
	return s;
}

// run a key command in a subprocess and take its first line. the user chose the
// command (op read, pass show, …); we do not interpret it beyond reading stdout.
result<std::string> run_key_cmd(const std::string& cmd) {
	FILE* pipe = ::popen(cmd.c_str(), "r");
	if (!pipe) return fail(errc::auth, "could not run key_cmd");
	std::string out;
	std::array<char, 512> buf{};
	while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) out += buf.data();
	const int rc = ::pclose(pipe);
	out = trim(std::move(out));
	if (rc != 0 || out.empty()) return fail(errc::auth, "key_cmd produced no key");
	return out;
}

result<std::string> keychain_lookup(const std::string& account) {
#if defined(PLUME_HAVE_LIBSECRET)
	// SecretSchema carries several reserved fields we intentionally leave zeroed.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
	static const SecretSchema schema = {
	    "sh.collar.plume",
	    SECRET_SCHEMA_NONE,
	    {{"account", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}};
#pragma clang diagnostic pop
	GError* err = nullptr;
	gchar* pw =
	    secret_password_lookup_sync(&schema, nullptr, &err, "account", account.c_str(), nullptr);
	if (err) {
		std::string m = err->message ? err->message : "keychain error";
		g_error_free(err);
		return fail(errc::auth, m);
	}
	if (!pw) return fail(errc::auth, "no keychain entry for " + account);
	std::string key = pw;
	secret_password_free(pw);
	return key;
#elif defined(__APPLE__)
	return run_key_cmd("security find-generic-password -s plume -a " + account + " -w");
#else
	(void)account;
	return fail(errc::unsupported, "keychain support not built in");
#endif
}

result<std::string> resolve_auth(const auth& a) {
	switch (a.kind) {
		case auth::source::env: {
			const char* v = std::getenv(a.value.c_str());
			if (!v || !*v) return fail(errc::auth, "env var " + a.value + " is unset");
			return std::string(v);
		}
		case auth::source::inline_key: return a.value;
		case auth::source::key_cmd: return run_key_cmd(a.value);
		case auth::source::keychain: return keychain_lookup(a.value);
	}
	return fail(errc::auth, "unknown auth source");
}

std::string default_base(const std::string& kind) {
	if (kind == "openai") return "https://api.openai.com/v1";
	if (kind == "openrouter") return "https://openrouter.ai/api/v1";
	if (kind == "ollama") return "http://localhost:11434/v1";
	return {};
}

}  // namespace

result<std::unique_ptr<provider>> make_provider(const provider_config& cfg) {
	// ollama commonly needs no key; everything else does.
	std::string key;
	if (cfg.kind != "ollama" || cfg.credential.kind != auth::source::env ||
	    std::getenv(cfg.credential.value.c_str())) {
		auto k = resolve_auth(cfg.credential);
		if (!k && cfg.kind != "ollama") return std::unexpected(k.error());
		if (k) key = *k;
	}

	if (cfg.kind == "anthropic") {
		return make_anthropic(cfg.base_url, key);
	}

	const std::string base = cfg.base_url.empty() ? default_base(cfg.kind) : cfg.base_url;
	if (base.empty()) return fail(errc::config, "provider '" + cfg.kind + "' needs a base_url");
	return make_openai_compatible(cfg.kind, base, key);
}

}  // namespace plume
