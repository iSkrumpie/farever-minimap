-- ==============================================================
-- nav_arrow.lua  (v1.0.0)
--
-- Demonstrates a 3D-look navigation arrow that points toward an
-- arbitrary world (x, y, z) waypoint and tilts up or down depending
-- on whether the target is above or below the player. The arrow
-- rotates in screen space using the player's heading, so "forward"
-- is always up on the panel.
--
-- Pattern any plugin author can copy:
--
--   1. Get player position + heading from farever.player.*.
--   2. Compute the world-space delta to the target (dx, dy, dz).
--   3. Rotate (dx, dy) into the player's local frame using cos/sin
--      of the heading. forward = align with player facing,
--      right = the perpendicular.
--   4. atan2(right, forward) is the screen angle.
--      atan2(dz, horizontal_distance) is the vertical tilt.
--   5. Draw triangle + shaft using draw_triangle_filled + draw_line.
--
-- Picks one of a couple of demo waypoints from a combo, or you can
-- punch in custom (x, y, z) values with the drag_float inputs.
-- ==============================================================

-- A handful of test waypoints lifted from iSkrumpie's POI list so
-- the plugin works out of the box if you spawn near Krisomal.
local waypoints = {
    { label = "Chest @ Krisomal NE",    x = 1421.99, y = 1363.53, z = 185.07 },
    { label = "Chest @ E Ramparts",     x = 1447.65, y = 1072.49, z =  16.50 },
    { label = "Activity: Castle Top",   x = 1294.63, y = 1168.71, z = 339.75 },
    { label = "Dungeon: Krisomal SE",   x = 1490.00, y = 1551.00, z = -88.50 },
    { label = "Custom (use inputs)",    x =    0.00, y =    0.00, z =    0.00 },
}

local sel = 1
local custom_x = 0.0
local custom_y = 0.0
local custom_z = 0.0

local labels_table = {}
for i, w in ipairs(waypoints) do labels_table[i] = w.label end

function on_init()
    sel      = farever.store.get("sel", 1)
    custom_x = farever.store.get("cx",  0.0)
    custom_y = farever.store.get("cy",  0.0)
    custom_z = farever.store.get("cz",  0.0)
end

local function current_target()
    if sel == #waypoints then
        return custom_x, custom_y, custom_z, "Custom"
    end
    local w = waypoints[sel]
    return w.x, w.y, w.z, w.label
end

-- Pick an arrow color from dz. Green inside +/- 5m, blue when target
-- is well above, red when well below. Useful even on its own as a
-- "is the chest near my floor" hint.
local function color_for_dz(dz)
    if math.abs(dz) < 5 then
        return 0.4, 1.0, 0.4
    elseif dz > 0 then
        return 0.4, 0.7, 1.0
    else
        return 1.0, 0.4, 0.4
    end
end

-- Draws a 3D-look arrow at (cx, cy). Returns d_h (horizontal
-- distance), dz (vertical delta), and theta_h (angle in radians,
-- 0 = directly forward) so the caller can also display them.
local function draw_nav_arrow(cx, cy, tx, ty, tz)
    local px = farever.player.x()
    local py = farever.player.y()
    local pz = farever.player.z()
    local heading = farever.player.rot_z()

    local dx = tx - px
    local dy = ty - py
    local dz = tz - pz

    -- Rotate world delta into player-local frame so "forward" matches
    -- the player's facing. Convention: heading = 0 means facing +Y,
    -- rotating clockwise. If the rotation looks reversed for your
    -- character, swap the signs on sinh.
    local cosh = math.cos(heading)
    local sinh = math.sin(heading)
    local forward = dx * cosh + dy * sinh
    local right   = -dx * sinh + dy * cosh

    local d_h = math.sqrt(forward * forward + right * right)
    local theta_h = math.atan(right, forward)        -- 0 = directly forward
    local theta_v = math.atan(dz, math.max(d_h, 1.0))

    -- Map theta_h into screen space. ImGui Y grows downward, and we
    -- want "forward" to point up on screen, so dir_x = sin(theta_h),
    -- dir_y = -cos(theta_h).
    local dir_x  =  math.sin(theta_h)
    local dir_y  = -math.cos(theta_h)
    local perp_x = -dir_y
    local perp_y =  dir_x

    -- Length and base pulled from a tiny pulse so the arrow feels
    -- alive.
    local t      = farever.now()
    local pulse  = 0.95 + 0.05 * math.sin(t * 3)
    local L      = 55.0 * pulse
    local head_l = 22.0 * pulse
    local head_w = 14.0 * pulse

    -- Tip vertical offset for the "tilts up / down" cue. Positive
    -- theta_v (target above) pulls the tip upward on screen.
    local tilt = math.sin(theta_v) * 18.0

    -- Tail behind the center, tip in front.
    local tail_x = cx - dir_x * L * 0.35
    local tail_y = cy - dir_y * L * 0.35
    local tip_x  = cx + dir_x * L
    local tip_y  = cy + dir_y * L - tilt

    -- Triangle head: tip + two base corners pulled back by head_l
    -- along the arrow's direction, then offset by ±head_w along the
    -- perpendicular.
    local back_x = tip_x - dir_x * head_l
    local back_y = tip_y - dir_y * head_l
    local hl_x   = back_x + perp_x * head_w
    local hl_y   = back_y + perp_y * head_w
    local hr_x   = back_x - perp_x * head_w
    local hr_y   = back_y - perp_y * head_w

    local r, g, b = color_for_dz(dz)

    -- Shaft
    imgui.draw_line(tail_x, tail_y, back_x, back_y, r, g, b, 1.0, 5.0)
    -- Filled head
    imgui.draw_triangle_filled(tip_x, tip_y, hl_x, hl_y, hr_x, hr_y, r, g, b, 1.0)
    -- Outline so the head reads on bright backgrounds too
    imgui.draw_triangle(tip_x, tip_y, hl_x, hl_y, hr_x, hr_y, 0, 0, 0, 0.7, 1.5)

    -- A small vertical tick on the side to underline the up/down
    -- direction (helps when the arrow is pointing nearly forward but
    -- the target is way above / below).
    if math.abs(dz) > 3 then
        local tick_x = cx + 70
        local tick_top, tick_bot = cy - 35, cy + 35
        imgui.draw_line(tick_x, tick_top, tick_x, tick_bot, 0.6, 0.6, 0.6, 0.7, 1.0)
        local mark_y = cy - math.max(-35, math.min(35, dz * 0.5))
        imgui.draw_triangle_filled(tick_x - 8, mark_y - 4,
                                   tick_x - 8, mark_y + 4,
                                   tick_x, mark_y, r, g, b, 1.0)
    end

    return d_h, dz, theta_h
end

function on_render()
    imgui.text("--- nav arrow ---")

    if not farever.player.locked() then
        imgui.text_colored(1.0, 0.6, 0.2, 1.0, "waiting for player lock...")
        return
    end

    -- Combo to pick a waypoint
    local v, c
    v, c = imgui.combo("waypoint", sel, labels_table)
    if c then sel = v; farever.store.set("sel", sel) end

    -- Custom inputs (only meaningful when "Custom" is selected, but
    -- always shown so the user can type values then switch)
    v, c = imgui.drag_float("custom x", custom_x, 1.0, -50000.0, 50000.0)
    if c then custom_x = v; farever.store.set("cx", custom_x) end
    v, c = imgui.drag_float("custom y", custom_y, 1.0, -50000.0, 50000.0)
    if c then custom_y = v; farever.store.set("cy", custom_y) end
    v, c = imgui.drag_float("custom z", custom_z, 0.5, -50000.0, 50000.0)
    if c then custom_z = v; farever.store.set("cz", custom_z) end

    imgui.separator()

    local tx, ty, tz, name = current_target()

    -- Reserve a 200 x 160 block for the arrow itself, anchored at
    -- the current cursor.
    local ax, ay = imgui.cursor_pos()
    local box_w, box_h = 200, 160
    local cx, cy = ax + box_w * 0.5, ay + box_h * 0.5

    -- Faint background box so the arrow has a visual frame
    imgui.draw_rect_filled(ax, ay, ax + box_w, ay + box_h, 0.10, 0.12, 0.15, 0.6)
    imgui.draw_rect      (ax, ay, ax + box_w, ay + box_h, 0.4, 0.4, 0.5, 0.6, 1.5)

    local d_h, dz, theta_h = draw_nav_arrow(cx, cy, tx, ty, tz)
    imgui.dummy(box_w, box_h)

    imgui.text(string.format("target: %s", name))
    imgui.text(string.format("xy dist: %.1f m   dz: %+.1f m", d_h, dz))
    local r, g, b = color_for_dz(dz)
    if dz > 5 then
        imgui.text_colored(r, g, b, 1.0, "target is above you")
    elseif dz < -5 then
        imgui.text_colored(r, g, b, 1.0, "target is below you")
    else
        imgui.text_colored(r, g, b, 1.0, "target is at your level")
    end
    imgui.text(string.format("angle from facing: %+.0f deg",
                             math.deg(theta_h)))
end
