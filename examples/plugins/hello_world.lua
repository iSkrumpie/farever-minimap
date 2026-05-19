-- Example plugin for farever-mod.
--
-- Place .lua files in data/plugins/ next to dinput8.dll. The mod
-- loads them at startup and reloads them automatically when you
-- save changes. Errors land in farever-mod.log and in the Plugin
-- Manager window (Filter tablet -> "Show plugin manager").

local frame_count = 0

function on_init()
    farever.log.info("hello_world plugin started")
end

function on_render()
    frame_count = frame_count + 1

    imgui.text("Hello from a Lua plugin!")
    imgui.separator()

    if farever.player.locked() then
        imgui.text(string.format("Pos: %.1f, %.1f", farever.player.x(),
                                                    farever.player.y()))
        imgui.text(string.format("Heading: %.2f rad", farever.player.rot_z()))
    else
        imgui.text("Hero not locked yet.")
    end

    imgui.separator()
    imgui.text(string.format("DPS: %.0f", farever.dps.current()))
    imgui.text(string.format("Total: %.0f", farever.dps.total()))
    imgui.text(string.format("Elapsed: %.1fs", farever.dps.elapsed()))

    imgui.separator()
    imgui.text(string.format("Frame: %d", frame_count))

    if imgui.button("Reset frame counter") then
        frame_count = 0
        farever.log.info("frame counter reset")
    end
end

function on_event(name, data)
    if name == "hero_locked" then
        farever.log.info("hero locked")
    elseif name == "fight_end" then
        farever.log.info(string.format(
            "fight #%d ended (%.1fs, %.0f total damage)",
            data.fight_id, data.duration, data.total_damage))
    end
end
