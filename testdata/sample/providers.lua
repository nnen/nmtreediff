-- A sample configuration. Pass it with --config, or copy it to
-- ~/.nmtreediff.lua where it is read without being asked for.

-- Point the studio's own suffixes at a format the tool already knows.
formats {
  [".bt"]        = "bt",
  [".btree"]     = "bt",
  [".leveldata"] = "json",
}

-- What to use for a file nothing else claims.
fallback "xml"

-- Which way the node graph runs. A format may still choose for itself.
graph_direction "top_down"

-- Which key closes the window. Use "none" to turn that off.
exit_key "escape"

-- A studio with many suffixes writes a loop rather than a list. This is the
-- reason configuration is a script and not a table of pairs.
local generated = {}
for _, suffix in ipairs{ ".mesh", ".anim", ".mat" } do
  generated[suffix] = "json"
end
formats(generated)
