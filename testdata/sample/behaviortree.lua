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

  -- The document as XML read it comes in as `doc`, and the tree goes out
  -- through `out`. The script owns the walk. Rather than have `visit` call
  -- itself for each child, it hands the call to `out:next`, and the tool runs
  -- what was queued once the current call returns. Depth then costs memory
  -- rather than the stack, and a tree nested thousands of levels deep shapes
  -- the same as a shallow one.
  shape = function(doc, out)

    -- The node types that decorate exactly one node under them. A decorator
    -- wraps the node it sits over rather than choosing among children, so
    -- the two read as one thing and the node view draws them stacked. The
    -- same list the compiled provider keeps.
    local decorators = {
      Inverter = true, Repeater = true, Cooldown = true, Succeeder = true, Limit = true,
    }

    -- An element's attributes, as a list. The XML reading records a leaf's
    -- text under "#text", which is content rather than an attribute.
    local function attributes(element)
      local list = {}
      for attribute in element:properties() do
        if attribute.name ~= "#text" then
          list[#list + 1] = attribute
        end
      end
      return list
    end

    -- Copies an element's attributes onto a node or a property.
    local function copy_attributes(into, element)
      for _, attribute in ipairs(attributes(element)) do
        into:property(attribute)
      end
    end

    -- The value an element reads as when it is nothing but a value: its text
    -- when it has no attributes and no children, its one attribute's value
    -- when that is all it has, or its one child's value when that child is
    -- itself nothing but a value. Otherwise nil, and the element is a record.
    -- Written recursively, because nothing forbids it and a property's parts
    -- are never deep enough to matter.
    local function collapsed(element)
      local attrs = attributes(element)
      local count = #attrs + element.child_count
      if count == 0 then
        return element.text
      elseif count == 1 then
        if #attrs == 1 then
          return attrs[1].value
        end
        return collapsed(element.first_child)
      end
      return nil
    end

    -- Turns an element into a property with parts: one part per attribute
    -- and one per element inside, all the way down, except where an element
    -- collapses to a single value.
    local function fold(into, element)
      local value = collapsed(element)
      if value ~= nil then
        into:set_value(value)
        return
      end
      copy_attributes(into, element)
      for child in element:children() do
        fold(into:child(child), child)
      end
    end

    -- One call per element. `owner` is where the element's children attach:
    -- the node it became, or, for an element that became a property, the
    -- node above it.
    local function visit(element, owner)
      if element.name == "node" then
        -- What the node does is what kind of node it is. The element name is
        -- "node" for every one of them, which tells a reader nothing. The
        -- editor writes a GUID, so two nodes carrying the same one are the
        -- same node however far apart they have moved: "strong" says the
        -- matcher may pair them across any distance.
        local kind = element.attr.type
        if kind == nil or kind == "" then kind = "node" end
        local node = owner:child(element):set_name(kind)
        copy_attributes(node, element)
        node:set_identity(element.attr.id, "strong")
        if decorators[kind] then node:set_stacked(true) end
        for child in element:children() do
          out:next(visit, child, node)
        end

      elseif element.name == "property" and (element.attr.name or "") ~= "" then
        -- <property name= value=/> describes the node it sits inside, so it
        -- becomes a property of that node rather than a node of its own. One
        -- whose content is elements is a property with parts; reading it as
        -- text would find nothing there and lose everything inside. The name
        -- and value attributes are the element's own bookkeeping and are not
        -- repeated among the parts.
        local prop = owner:property(element):set_name(element.attr.name)
        if element.attr.value == nil and element.child_count > 0 then
          for _, attribute in ipairs(attributes(element)) do
            if attribute.name ~= "name" and attribute.name ~= "value" then
              prop:property(attribute)
            end
          end
          for child in element:children() do
            fold(prop:child(child), child)
          end
        else
          prop:set_value(element.attr.value or element.text)
        end

      else
        -- Not a node and not a property pair, but nothing is dropped: the
        -- element becomes a property of the node above it, and the walk
        -- carries on inside so any nodes it holds still surface where they
        -- belong. Attributes only, since the children are visited on their
        -- own account; one attribute reads as the value, more as a record.
        local kept = owner:property(element)
        local attrs = attributes(element)
        if #attrs == 1 then
          kept:set_value(attrs[1].value)
        else
          for _, attribute in ipairs(attrs) do
            kept:property(attribute)
          end
        end
        for child in element:children() do
          out:next(visit, child, owner)
        end
      end
    end

    -- The document element becomes the root node whatever it is called, so
    -- that a tree always has somewhere to hang and the version attribute
    -- stays visible.
    local root = out:root(doc.root)
    copy_attributes(root, doc.root)
    for child in doc.root:children() do
      out:next(visit, child, root)
    end
  end,
}
