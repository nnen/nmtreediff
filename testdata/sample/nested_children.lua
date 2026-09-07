-- A format whose nodes keep their children under a wrapper element:
--
--   <node id="root">
--     <children>
--       <child id="a"/>
--       <child id="b"/>
--     </children>
--   </node>
--
-- The wrapper is not a node and should not survive as one, and the elements
-- inside it are the node's children. The five-question form cannot say that:
-- it can keep the wrapper as a property, but it cannot make it vanish. The
-- enter and exit form can, because exit sees the items already made from what
-- the wrapper held and simply passes them up.

provider "nested" {
  display_name = "Nested children",
  base = "xml",
  extensions = { ".nested" },

  -- Enter is told an element has started, before anything inside it. It never
  -- emits; it leaves something for descendants or steers the walk. Here a node
  -- leaves its id, so a child with no id of its own can be keyed by it.
  enter = function(el, frame)
    if el.name == "node" or el.name == "child" then
      frame.data = el.attr.id
    elseif el.name == "editor" then
      -- Layout data the editor writes. One property, nothing visited inside.
      frame:opaque()
    end
  end,

  exit = function(el, out)
    if el.name == "node" or el.name == "child" then
      -- The nearest element above that left an id is the owner.
      local owner = el.parent
      while owner ~= nil and owner.data == nil do
        owner = owner.parent
      end
      local key = el.attr.id or el.attr.name
      if owner ~= nil then
        key = owner.data .. "/" .. key
      end
      out:node(el.name, el)
         :identity(key, "strong")
         :title(el.attr.name or el.attr.id)
         :attributes(el)
         :adopt(el.items)
    elseif el.name == "children" then
      out:forward(el.items)
    end
    -- Anything else is not mentioned, and gets the default treatment.
  end,
}
