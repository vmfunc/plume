// the cost dashboard: plume never stores per-node cost, so it is derived on open
// from token totals priced against cfg.prices. out-of-line members of app::impl.
#include "app_impl.hpp"

namespace plume {

using namespace ftxui;

namespace {

std::string usd(double v) {
	std::string s = std::to_string(v);
	const auto dot = s.find('.');
	return dot == std::string::npos ? s : s.substr(0, dot + 5);
}

}  // namespace

void app::impl::open_cost() {
	ov = overlay::cost;
	cost_by_model.clear();
	cost_convo = {};
	cost_all = {};
	if (!db) return;

	auto price_node = [&](const node& n, cost_stats& into) {
		into.in += n.tokens_in;
		into.out += n.tokens_out;
		if (auto it = cfg.prices.find(n.model); it != cfg.prices.end())
			into.usd += (n.tokens_in * it->second.input + n.tokens_out * it->second.output) / 1e6;
		auto& m = cost_by_model[n.model.empty() ? "(unknown)" : n.model];
		m.in += n.tokens_in;
		m.out += n.tokens_out;
		if (auto it = cfg.prices.find(n.model); it != cfg.prices.end())
			m.usd += (n.tokens_in * it->second.input + n.tokens_out * it->second.output) / 1e6;
	};

	for (const auto& n : transcript) {
		cost_convo.in += n.tokens_in;
		cost_convo.out += n.tokens_out;
		if (auto it = cfg.prices.find(n.model); it != cfg.prices.end())
			cost_convo.usd +=
			    (n.tokens_in * it->second.input + n.tokens_out * it->second.output) / 1e6;
	}
	if (auto list = db->conversations())
		for (const auto& c : *list)
			if (auto ns = db->nodes_of(c.id))
				for (const auto& n : *ns) price_node(n, cost_all);
}

Element app::impl::cost_view() {
	auto row = [&](const std::string& label, const cost_stats& s, rgb tint) {
		return hbox({text("  " + label) | color(col(tint)) | size(WIDTH, EQUAL, 18),
		             text("in " + std::to_string(s.in) + "  out " + std::to_string(s.out)) |
		                 color(col(th.p.foam)) | size(WIDTH, EQUAL, 26),
		             text("$" + usd(s.usd)) | color(col(th.p.gold))});
	};
	Elements rows = {
	    row("this turn", {last_usage.input, last_usage.output, cost_of(last_usage)}, th.p.iris),
	    row("this conversation", cost_convo, th.p.iris),
	    row("all conversations", cost_all, th.p.rose), text(""),
	    text("  by model") | color(col(th.p.gold)) | dim};
	for (const auto& [model, s] : cost_by_model)
		if (s.in > 0 || s.out > 0) rows.push_back(row(model, s, th.p.subtle));
	rows.push_back(text(""));
	rows.push_back(text("  prices are a snapshot; verify against your provider") |
	               color(col(th.p.muted)) | dim);
	return ui::overlay(th, "cost", vbox(std::move(rows)));
}

}  // namespace plume
