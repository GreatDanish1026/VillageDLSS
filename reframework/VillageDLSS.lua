-- VillageDLSS companion script (REFramework autorun).
-- Sets Resident Evil Village's Image Quality (render scale) to the DLSS mode chosen in the
-- VillageDLSS ReShade overlay. The add-on writes the wanted scale (0 = leave the game's slider
-- alone) to reframework/data/VillageDLSS_mode.txt; this script applies it through
-- via.render.RenderConfig.ImageQualityRate, keeps it applied if the menu changes it, and puts the
-- game's own value back when the mode goes back to the slider or DLSS is switched off. It writes
-- the current value to reframework/data/VillageDLSS_status.txt for the overlay.

local rc_type = sdk.find_type_definition("via.render.RenderConfig")
local original = nil  -- the game's value before the first override
local frame = 0

local function get_rate(rc)
    return sdk.call_native_func(rc, rc_type, "get_ImageQualityRate")
end

local function set_rate(rc, v)
    sdk.call_native_func(rc, rc_type, "set_ImageQualityRate", v)
end

re.on_frame(function()
    frame = frame + 1
    if frame % 30 ~= 0 or not rc_type then return end
    local ok, err = pcall(function()
        local rc = sdk.get_native_singleton("via.render.RenderConfig")
        if not rc then return end
        local want = tonumber(fs.read("VillageDLSS_mode.txt") or "") or 0
        local cur = get_rate(rc)
        if want > 0 then
            if original == nil then original = cur end
            if math.abs(cur - want) > 0.001 then
                set_rate(rc, want)
                cur = get_rate(rc)
            end
        elseif original ~= nil then
            set_rate(rc, original)
            original = nil
            cur = get_rate(rc)
        end
        fs.write("VillageDLSS_status.txt", string.format("%.4f %d", cur or -1, frame))
    end)
    if not ok then fs.write("VillageDLSS_status.txt", "error " .. tostring(err)) end
end)

-- Full-resolution ray traced reflections (the add-on's "Full-resolution reflections" switch).
-- The add-on writes 0 (full resolution) or -1 (leave the game's setting) to
-- reframework/data/VillageDLSS_rt.txt; this sets via.render.ExperimentalRayTrace.SpecularResolution on
-- the main camera (0 = Origin_Res, 1 = Half_Res), keeps it there, and puts the game's value back
-- on -1. The current value goes to reframework/data/VillageDLSS_rt_status.txt.
local rt_typeof = nil
local rt_original = nil
local rt_frame = 0

local function rt_component()
    local cam = sdk.get_primary_camera()
    if not cam then return nil end
    local go = cam:call("get_GameObject")
    if not go then return nil end
    if not rt_typeof then rt_typeof = sdk.typeof("via.render.ExperimentalRayTrace") end
    if not rt_typeof then return nil end
    return go:call("getComponent(System.Type)", rt_typeof)
end

re.on_frame(function()
    rt_frame = rt_frame + 1
    if rt_frame % 30 ~= 15 then return end
    local ok, err = pcall(function()
        local want = tonumber(fs.read("VillageDLSS_rt.txt") or "") or -1
        if want < 0 and rt_original == nil then return end  -- nothing asked, nothing to undo
        local c = rt_component()
        if not c then return end
        local cur = c:call("get_SpecularResolution")
        if want >= 0 then
            if rt_original == nil then rt_original = cur end
            if cur ~= want then c:call("set_SpecularResolution", want) end
        else
            c:call("set_SpecularResolution", rt_original)
            rt_original = nil
        end
        fs.write("VillageDLSS_rt_status.txt", tostring(c:call("get_SpecularResolution")))
    end)
    if not ok then fs.write("VillageDLSS_rt_status.txt", "error " .. tostring(err)) end
end)
