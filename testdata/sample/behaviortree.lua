-- The behaviour tree format, written in script rather than compiled in.
--
-- This is the same format as the built-in "bt" provider, which is the point:
-- the golden corpus already records what the compiled one produces, so this
-- has an exact answer to be judged against rather than a plausible one. If the
-- two ever disagree, either the bridge is losing something or the scripted
-- surface is missing something the compiled provider can say.
--
-- Try it with:
--   nmxmldiff --config testdata/sample/behaviortree.lua --format bt-lua \
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

provider "bt-lua" {
  display_name = "Behavior tree (script)",
  base = "xml",

  -- No extensions claimed. The compiled provider already owns .bt and .btree,
  -- and two formats fighting over one suffix would make which one you get
  -- depend on registration order. Ask for this one by name.
  extensions = {},

  -- A behaviour tree is deep and narrow, so it reads better left to right.
  graph_direction = "left_to_right",

  -- What makes a node the same node across versions comes first, then what it
  -- does, then what its author called it. Everything else keeps document order
  -- behind these. Presentation only: matching compares properties as a set
  -- whatever this says.
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

    -- Only <node> elements are nodes. What the node does is what kind of node
    -- it is: the element name is "node" for every one of them, which tells a
    -- reader nothing. The editor writes a GUID, so two nodes carrying the same
    -- one are the same node however far apart they have moved. That is worth
    -- saying out loud: "strong" means the matcher may pair them across any
    -- distance, and it is what lets a node survive a change of type that no
    -- structural heuristic could recover from.
    if el.name == "node" then
      local n = out:node(el.attr.type or "node", el)
      n:identity(el.attr.id, "strong")
      n:title(el.attr.type, el.attr.name)
      n:attributes(el)
      n:adopt(el.items)
      return
    end

    -- <property name= value=/> describes the node it sits inside, so it
    -- becomes a property of that node rather than a node of its own.
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
