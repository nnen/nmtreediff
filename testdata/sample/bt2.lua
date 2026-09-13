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

provider "bt2-lua" {
  display_name = "Behavior tree 2 (script)",
  base = "xml",

  -- No extensions claimed. The compiled provider already owns .bt and .btree,
  -- and two formats fighting over one suffix would make which one you get
  -- depend on registration order. Ask for this one by name.
  extensions = { ".bt2" },

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
    -- One call per element. `owner` is where the element's children attach:
    -- the node it became, or, for an element that became a property, the
    -- node above it.
    local function visit(element, owner)
        -- if element.name == "id" then
        --     owner:property("id", element.text)
        --     owner:set_identity(element.text, "strong")
        --     return
        -- elseif element.name == "rttiType" then
        --     owner:property("rttiType", element.text)
        --     owner:set_title(element.text, element.parent.name)
        --     return
        -- elseif element.name == "children" then
        if element.name == "children" then
            for child in element:children() do
                out:next(visit, child, owner)
            end
            return
        end

        local rttiType = element:child("rttiType")
        local id = element:child("id")

        if rttiType then
            local node = owner:child(element):set_name("node")
            node:set_title(rttiType.text, element.name)
            if id then
                node:set_identity(id.text, "strong")
            end
            for attribute in element:properties() do
                node:property(attribute)
            end
            for child in element:children() do
                out:next(visit, child, node)
            end
        else
            local prop = owner:property(element.name, element.text)
            -- if element.attr.value then
            --     owner:set_value(element.attr.value)
            -- end
            for attribute in element:properties() do
                if attribute.name ~= "#text" then
                    prop:property(attribute)
                end
            end
            for child in element:children() do
                out:next(visit, child, prop)
            end
        end
    end

    -- The document element becomes the root node whatever it is called, so
    -- that a tree always has somewhere to hang and the version attribute
    -- stays visible.
    local root = out:root(doc.root)
    local rootSubbehavior = doc.root:child("subbehavior")
    local rootNode = rootSubbehavior and rootSubbehavior:child("root")
    visit(rootNode or doc.root, root)
    -- for child in doc.root:children() do
    --   out:next(visit, child, root)
    -- end
  end,
}
