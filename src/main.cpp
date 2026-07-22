#include <cstdlib>
#include <string>

#include "plume/app.hpp"
#include "plume/cli.hpp"
#include "plume/config.hpp"

int main(int argc, char** argv) {
	// a subcommand (ask, export, import, doctor, themes, config) short-circuits
	// to the cli. run_cli returns -1 to mean "no subcommand, fall through to the
	// interactive app".
	const int cli_rc = plume::run_cli(argc, argv);
	if (cli_rc >= 0) return cli_rc;

	auto cfg = plume::load_config(plume::default_config_path());
	if (!cfg) {
		// a broken config is worth stopping for; an absent one is not (the
		// loader returns defaults for that).
		std::fputs(("plume: " + cfg.error().detail + "\n").c_str(), stderr);
		return EXIT_FAILURE;
	}

	auto a = plume::app::create(std::move(*cfg));
	if (!a) {
		std::fputs(("plume: " + a.error().detail + "\n").c_str(), stderr);
		return EXIT_FAILURE;
	}
	return a->run();
}
