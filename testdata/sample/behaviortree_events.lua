-- The behaviour tree format again, this time written against the enter and
-- exit form rather than the five questions.
--
-- Same format, same answer: the tests hold this against the compiled "bt"
-- provider and the five-question "bt-lua" script, and all three must produce
-- the same tree and the same change list. The five questions are the short
-- way to say the common thing; this form is for a format the questions cannot
-- describe, and it has to be able to say everything they can. It is longer,
-- because it spells out what the questions imply.
--
-- Try it with:
--   nmxmldiff --config testdata/sample/behaviortree_events.lua --format bt-events \
--             testdata/sample/guard_before.bt testdata/sample/guard_after.bt

-- A <property> with a name is one the format folds into the node above.
local function is_folded_property(el)
  return el.name == "property" and el.attr.name ~= nil and el.attr.name ~= ""
end

-- Whether an element sits inside a folded property's content. Everything in
-- there is a part of that property, however it is named, and a wrapper the
-- format does not know is transparent on the way up.
local function inside_property(el)
  local above = el.parent
  while above ~= nil do
    if above.parent == nil or above.name == "node" then
      return false
    end
    if is_folded_property(above) then
      return true
    end
    above = above.parent
  end
  return false
end

provider "bt-events" {
  display_name = "Behavior tree (events)",
  base = "xml",
  extensions = {},
  graph_direction = "left_to_right",
  property_order = { "id", "type", "name" },

  -- Called when an element ends, after everything inside it. By then each
  -- element inside has already been turned into a node or a property, and
  -- those sit in el.items in document order.
  exit = function(el, out)
    -- The document element is the root, whatever it is called.
    if el.parent == nil then
      out:node(el.name, el):attributes(el):adopt(el.items)
      return
    end

    -- Inside a folded property, an element is a part: its attributes and
    -- the parts made from what it holds, or its text when it holds nothing.
    if inside_property(el) then
      local value = ""
      if el.child_count == 0 and next(el.attr) == nil then
        value = el.text
      end
      out:property(el.name, value, el):attributes(el):adopt(el.items):collapse()
      return
    end

    if el.name == "node" then
      local n = out:node(el.attr.type or "node", el)
      n:identity(el.attr.id, "strong")
      n:title(el.attr.type, el.attr.name)
      n:attributes(el)
      n:adopt(el.items)
      return
    end

    if is_folded_property(el) then
      if el.attr.value == nil and el.child_count > 0 then
        -- Content made of elements is a value with parts. The name and value
        -- attributes are the element's own bookkeeping, not parts of it.
        out:property(el.attr.name, "", el):attributes(el, "name", "value"):adopt(el.items):collapse()
      else
        out:property(el.attr.name, el.attr.value or el.text, el)
      end
      return
    end

    -- Anything else keeps its own name and attributes as a property of the
    -- node above, and what it holds goes up to that node too. A wrapper such
    -- as <children> neither breaks the tree nor disappears from it.
    out:property(el.name, "", el):attributes(el):collapse()
    out:forward(el.items)
  end,
}
