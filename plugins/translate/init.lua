-- rewrite the outgoing message to french before it is sent, by asking a cheap
-- model. this is the whole plugin surface in one file: a pre_send hook that
-- mutates the message, plus plume.model.complete gated behind the net capability.

plume.on("pre_send", function(text)
	if text == nil or #text == 0 then
		return text
	end
	local prompt = "Translate the following to French. Reply with only the translation, no notes.\n\n" .. text
	local out = plume.model.complete({ prompt = prompt })
	-- if the model is unavailable (net not granted, no key), leave the message
	-- untouched rather than sending an error string.
	if out ~= nil and #out > 0 and out:sub(1, 6) ~= "[plume" then
		return out
	end
	return text
end)
