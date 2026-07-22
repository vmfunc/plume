// a canned provider for `just demo` and for exercising the streaming render
// without a key or a network. it emits a fixed reply token by token, with a
// short pause between tokens so the breathing caret and live layout are visible.
#include <chrono>
#include <thread>
#include <vector>

#include "plume/provider.hpp"

namespace plume {

namespace {

class mock_provider final : public provider {
   public:
	std::string_view name() const override { return "mock"; }

	result<std::vector<model_info>> list_models() override {
		model_info m;
		m.id = "mock-model";
		m.display_name = "mock model";
		m.context = 200000;
		m.max_output = 8192;
		m.caps = {true, true, true, true, true, true};
		return std::vector<model_info>{m};
	}

	result<std::int64_t> count_tokens(const request& req) override {
		std::int64_t n = 0;
		for (const auto& m : req.messages) n += static_cast<std::int64_t>(m.plain_text().size()) / 4;
		return n;
	}

	result<completion> stream(const request&,
	                          const std::function<void(const stream_delta&)>& on_delta,
	                          const std::function<bool()>& stop) override {
		static const std::vector<std::string> words = {
		    "a ", "quill ", "is ",   "an ",  "old ", "instrument:\n\n", "you ",  "cut ",
		    "a ", "feather ", "to ", "a ",   "nib, ", "dip ", "it ",     "in ",   "ink, ",
		    "and ", "the ",  "line ", "you ", "draw ", "is ", "yours. ", "plume ", "keeps ",
		    "that ", "line ", "in ",  "a ",   "tree ", "you ", "can ",   "weave ", "through."};
		completion out;
		out.model = "mock-model";
		std::string text;
		for (const auto& w : words) {
			if (stop()) break;
			text += w;
			on_delta({stream_delta::kind::text, w, {}, {}});
			std::this_thread::sleep_for(std::chrono::milliseconds(35));
		}
		out.reply = message::text(role::assistant, text);
		out.tokens.input = 24;
		out.tokens.output = static_cast<std::int64_t>(words.size());
		out.stop_reason = "end_turn";
		on_delta({stream_delta::kind::usage, {}, {}, out.tokens});
		on_delta({stream_delta::kind::done, out.stop_reason, {}, {}});
		return out;
	}
};

}  // namespace

std::unique_ptr<provider> make_mock() { return std::make_unique<mock_provider>(); }

}  // namespace plume
