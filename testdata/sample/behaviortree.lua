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

  -- Only <node> elements are nodes. Everything else is either folded into the
  -- node above it or walked through.
  is_node = function(element)
    return element.name == "node"
  end,

  -- <property name= value=/> describes the node it sits inside, so it becomes
  -- a property of that node rather than a node of its own. Anything else the
  -- walk passes through, which keeps a wrapper element from swallowing what is
  -- inside it.
  fold_into_parent = function(element)
    return element.name == "property"
  end,

  -- What the node does is what kind of node it is. The element name is "node"
  -- for every one of them, which tells a reader nothing.
  kind = function(element)
    return element.attr.type
  end,

  -- The editor writes a GUID, so two nodes carrying the same one are the same
  -- node however far apart they have moved. That is worth saying out loud:
  -- "strong" means the matcher may pair them across any distance, and it is
  -- what lets a node survive a change of type that no structural heuristic
  -- could recover from.
  identity = function(element)
    return element.attr.id, "strong"
  end,

  -- The behaviour first, the author's name for it second.
  title = function(element)
    return element.attr.type, element.attr.name
  end,
}
