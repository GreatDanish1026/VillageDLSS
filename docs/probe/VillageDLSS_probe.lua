-- VillageDLSS probe (REFramework autorun): finds where Resident Evil Village keeps its
-- Image Quality (render scale) setting, so VillageDLSS can set it from a DLSS mode dropdown.
-- Reads type information only and writes reframework/data/VillageDLSS_probe.txt. Changes nothing.

local out = {}
local function add(s) out[#out + 1] = s end

local function interesting(name)
    local l = name:lower()
    return l:find("imagequality") or l:find("renderscale") or l:find("resolutionscale") or
           l:find("renderingscale") or l:find("qualityrate") or l:find("upscal") or l:find("fsr") or
           l:find("renderresolution") or l:find("interlace")
end

local function describe(td, all)
    local name = td:get_full_name()
    local lines = {}
    for _, f in ipairs(td:get_fields() or {}) do
        local ok, n = pcall(function() return f:get_name() end)
        if ok and n and (all or interesting(n)) then
            local tn = "?"
            pcall(function() tn = f:get_type():get_full_name() end)
            lines[#lines + 1] = "  field " .. n .. " : " .. tn .. (f:is_static() and " (static)" or "")
        end
    end
    for _, m in ipairs(td:get_methods() or {}) do
        local ok, n = pcall(function() return m:get_name() end)
        if ok and n and (all or interesting(n)) then
            local np = 0
            pcall(function() np = m:get_num_params() end)
            lines[#lines + 1] = "  method " .. n .. " (" .. np .. " params)"
        end
    end
    if #lines > 0 then
        add("TYPE " .. name)
        for _, l in ipairs(lines) do add(l) end
    end
end

local function run()
    -- Every type in the type database, if this REFramework build can list them.
    local ok, err = pcall(function()
        local tdb = sdk.get_tdb()
        local n = tdb:get_num_types()
        add("type database: " .. n .. " types")
        for i = 0, n - 1 do
            local td = tdb:get_type(i)
            if td then pcall(describe, td, false) end
        end
    end)
    if not ok then add("type database scan not available: " .. tostring(err)) end

    -- Likely places, listed in full.
    for _, n in ipairs({"via.render.RenderConfig", "app.GraphicSystemApp", "via.SceneView",
                        "via.render.RenderOutput", "app.PCGraphicSampleResourceManager"}) do
        local td = sdk.find_type_definition(n)
        if td then
            add("---- " .. n)
            pcall(describe, td, true)
        else
            add("missing " .. n)
        end
    end
    fs.write("VillageDLSS_probe.txt", table.concat(out, "\n"))
end

local done = false
re.on_frame(function()
    if done then return end
    done = true
    local ok, err = pcall(run)
    if not ok then fs.write("VillageDLSS_probe.txt", "probe failed: " .. tostring(err) .. "\n" .. table.concat(out, "\n")) end
end)
