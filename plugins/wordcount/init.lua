-- a statusline segment with the word count of the last reply. the smallest
-- useful plugin: one hook to observe, one to display. no capabilities needed.

local words = 0

plume.on("post_receive", function(full)
	words = 0
	for _ in full:gmatch("%S+") do
		words = words + 1
	end
end)

plume.statusline("wordcount", function()
	return words .. "w"
end)
