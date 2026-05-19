-- Personal best DPS tracker. Showcases the v1.1 plugin API:
-- events, persistent store, and toast notifications.

local best_dps = 0.0
local best_skill = ""
local fights_logged = 0
local enabled = true

function on_init()
    best_dps      = farever.store.get("best_dps", 0.0)
    best_skill    = farever.store.get("best_skill", "")
    fights_logged = farever.store.get("fights_logged", 0)
    enabled       = farever.store.get("enabled", true)
    farever.log.info(string.format(
        "loaded: best %.0f DPS (%s), %d fights logged",
        best_dps, best_skill, fights_logged))
end

function on_render()
    imgui.text(string.format("Personal best: %.0f DPS", best_dps))
    if best_skill ~= "" then
        imgui.text(string.format("Top skill at PB: %s", best_skill))
    end
    imgui.text(string.format("Fights logged: %d", fights_logged))
    imgui.separator()

    local new_enabled, changed = imgui.checkbox("Track new fights", enabled)
    if changed then
        enabled = new_enabled
        farever.store.set("enabled", enabled)
    end

    if imgui.button("Reset personal best") then
        best_dps = 0.0
        best_skill = ""
        fights_logged = 0
        farever.store.set("best_dps", 0.0)
        farever.store.set("best_skill", "")
        farever.store.set("fights_logged", 0)
        farever.toast("Personal best reset")
    end
end

function on_event(name, data)
    if not enabled then return end
    if name ~= "fight_end" then return end

    fights_logged = fights_logged + 1
    farever.store.set("fights_logged", fights_logged)

    if data.dps > best_dps then
        local prev = best_dps
        best_dps   = data.dps
        best_skill = data.top_skill or ""
        farever.store.set("best_dps", best_dps)
        farever.store.set("best_skill", best_skill)
        farever.toast(
            string.format("NEW BEST: %.0f DPS (was %.0f)", best_dps, prev),
            3.5)
        farever.log.info(string.format(
            "new PB: %.0f DPS on %s (prev %.0f)",
            best_dps, best_skill, prev))
    end
end
