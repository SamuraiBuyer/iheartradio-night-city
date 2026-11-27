-- iHeart Radio (CET part): the Station Manager, in the Cyber Engine Tweaks overlay, drawn to look like the app.
--
-- Everything that needs the internet or makes logos runs in the plugin (red4ext\plugins\iHeartRadio\iHeartRadio.dll),
-- which this talks to through three functions, with JSON in and out (see the plugin's Stations.hpp):
--   IHeartRadio.Call(request)  quick answers       IHeartRadio.Begin(request) -> ticket      IHeartRadio.Poll(ticket)
-- New stations and logos show up the next time the game starts (RadioExt and the game load them at startup).
--
-- Drawing: ImGui can't show web pages or pictures, so the app's look (src/manager/ui/style.css) is rebuilt with
-- ImGui's draw list: frames, cut-corner buttons, letter-spaced titles, and the logos as runs of small rectangles.

local menu = { visible = false }

-- ------------------------------------------------------------------ the app's colours (style.css)
local C = {
    bg = { 0.039, 0.031, 0.039, 1 },          -- #0a080a
    panel = { 0.086, 0.039, 0.047, 0.78 },    -- rgba(22, 10, 12, .78)
    red = { 1.0, 0.365, 0.333, 1 },           -- #ff5d55
    redDim = { 1.0, 0.365, 0.333, 0.32 },
    redFaint = { 1.0, 0.365, 0.333, 0.12 },
    cyan = { 0.369, 0.965, 1.0, 1 },          -- #5ef6ff
    cyanDim = { 0.369, 0.965, 1.0, 0.28 },
    cyanFaint = { 0.369, 0.965, 1.0, 0.08 },
    yellow = { 0.953, 0.902, 0.0, 1 },        -- #f3e600
    text = { 1.0, 0.843, 0.824, 1 },          -- #ffd7d2
    muted = { 0.639, 0.380, 0.361, 1 },       -- #a3615c
    ink = { 0.06, 0.03, 0.035, 1 },           -- text on yellow / red buttons
    field = { 0.055, 0.027, 0.031, 1 },
}

local TILE = 88            -- logo style tiles (pixels), drawn 1:1 (sharp)
local BIG = 144            -- the big preview (pixels), drawn 1:1 (sharp)
local LIST_ICON = 40       -- station list icons, drawn at 1x
local THUMB = 32           -- search result logos (colour), drawn at 2.5x
local MAX_GAP = 10

-- ------------------------------------------------------------------ state
local view = "search"       -- right panel: "search" | "setup"
local status = nil          -- plugin status, or nil when the plugin isn't there
local stations = {}         -- installed
local query = ""
local results = nil         -- search results
local searching = false
local setup = nil           -- the station being added / edited
local notice = nil          -- { text, colour }
local confirmRemoveAll = 0  -- time of the first click on REMOVE ALL STATIONS
local jobs = {}             -- ticket -> callback
local iconCache = {}        -- station id -> runs (installed logo, list size)
local thumbCache = {}       -- image url -> runs | "loading" | false
local F = 16                -- CET's font size, read every frame

local function say(text, colour) notice = { text = text, colour = colour or C.cyan } end

-- ------------------------------------------------------------------ talking to the plugin
local function call(request)
    local ok, answer = pcall(function() return IHeartRadio.Call(json.encode(request)) end)
    if not ok or type(answer) ~= "string" or answer == "" then return nil end
    local decoded, data = pcall(json.decode, answer)
    return decoded and data or nil
end

local function begin(request, onDone)
    local ok, ticket = pcall(function() return IHeartRadio.Begin(json.encode(request)) end)
    if not ok or type(ticket) ~= "number" then
        onDone({ ok = false, error = "The iHeart Radio plugin isn't running (red4ext\\plugins\\iHeartRadio)." })
        return
    end
    jobs[ticket] = onDone
end

function menu.update()
    if next(jobs) == nil then return end
    -- collect the finished ones first: their callbacks may start new jobs
    local done = {}
    for ticket, onDone in pairs(jobs) do
        local ok, answer = pcall(function() return IHeartRadio.Poll(ticket) end)
        if not ok then
            done[#done + 1] = { ticket, onDone, { ok = false, error = "lost the plugin" } }
        elseif answer ~= "" then
            local decoded, data = pcall(json.decode, answer)
            done[#done + 1] = { ticket, onDone, decoded and data or { ok = false, error = "bad answer from the plugin" } }
        end
    end
    for _, d in ipairs(done) do
        jobs[d[1]] = nil
        local ok, err = pcall(d[2], d[3])
        if not ok then say("Menu error: " .. tostring(err), C.red) end
    end
end

local function refresh()
    status = call({ op = "status" })
    local list = call({ op = "list" })
    stations = list and list.stations or {}
    iconCache = {}
end

-- ------------------------------------------------------------------ pictures as runs of rectangles
local function u32(c, a) return ImGui.GetColorU32(c[1], c[2], c[3], a or c[4]) end

-- Pictures become rectangles { x0, y0, x1, y1, colour }: runs of equal pixels along a row, and a run that repeats
-- exactly in the next row just grows taller (logos are mostly flat shapes, so this keeps the count low).
local function rects(size, pixelAt)
    local out, open = {}, {}   -- open: "x0:x1:code" -> rectangle still growing (ends at the current row)
    for y = 0, size - 1 do
        local nextOpen, x = {}, 0
        while x < size do
            local code, colour = pixelAt(x, y)
            local x1 = x + 1
            while x1 < size and pixelAt(x1, y) == code do x1 = x1 + 1 end
            if colour then
                local key = x .. ":" .. x1 .. ":" .. code
                local r = open[key]
                if r then r[4] = y + 1 else r = { x, y, x1, y + 1, colour } out[#out + 1] = r end
                nextOpen[key] = r
            end
            x = x1
        end
        open = nextOpen
    end
    return out
end

-- one hex digit per pixel (alpha / 16) -> rectangles in cyan. aShades: how many alpha steps to keep (fewer = fewer
-- rectangles along the soft edges; the small pictures don't need all 16)
local function alphaRuns(px, size, aShades)
    local n = (aShades or 16) - 1
    local levels = {}
    for q = 1, n do levels[q] = u32(C.cyan, q / n) end
    local step = {}
    for v = 0, 15 do step[string.format("%x", v)] = math.floor(v * n / 15 + 0.5) end
    return rects(size, function(x, y)
        local q = step[px:sub(y * size + x + 1, y * size + x + 1)] or 0
        return q, q > 0 and levels[q] or nil
    end)
end

-- three hex digits per pixel (R G B, 4 bits each) -> rectangles in colour, 8 steps per channel (white is the card's
-- background)
local function colourRuns(px, size)
    local colours, step = {}, {}
    for v = 0, 15 do step[string.format("%x", v)] = math.floor(v / 2 + 0.5) end
    return rects(size, function(x, y)
        local o = (y * size + x) * 3
        local r, g, b = step[px:sub(o + 1, o + 1)] or 0, step[px:sub(o + 2, o + 2)] or 0, step[px:sub(o + 3, o + 3)] or 0
        local code = r * 64 + g * 8 + b
        if code == 511 or (r >= 7 and g >= 7 and b >= 7) then return 511, nil end
        if not colours[code] then colours[code] = ImGui.GetColorU32(r / 8, g / 8, b / 8, 1) end
        return code, colours[code]
    end)
end

local function drawRuns(dl, runs, x, y, scale)
    for _, r in ipairs(runs or {}) do
        ImGui.ImDrawListAddRectFilled(dl, x + r[1] * scale, y + r[2] * scale, x + r[3] * scale, y + r[4] * scale, r[5])
    end
end

-- a soft cyan glow behind an icon, like the app's preview (only the bigger shapes)
local function drawGlow(dl, runs, x, y, scale)
    local glow = u32(C.cyan, 0.045)
    local g = 3
    for _, r in ipairs(runs or {}) do
        if (r[3] - r[1]) * (r[4] - r[2]) >= 30 then
            ImGui.ImDrawListAddRectFilled(dl, x + r[1] * scale - g, y + r[2] * scale - g, x + r[3] * scale + g, y + r[4] * scale + g, glow)
        end
    end
end

-- Preview of a logo style (or "current": the installed logo) for the station being set up, shown aSize pixels big:
-- { runs, scale }, or nil while loading. Drawn 1:1 so it's sharp; a very detailed logo (rough textures make
-- thousands of little rectangles) is fetched at half the size and drawn at 2x instead, so the menu stays light.
local BUDGET = { [BIG] = 2500, [TILE] = 900 }
local function stylePreview(key, invert, size)
    local s = setup
    if not s or s.loading then return nil end
    if key == "keep" then key, invert = "current", false end
    local id = key .. (invert and ":inv" or "") .. ":" .. s.gap .. ":" .. size
    if s.previews[id] == nil then
        local shades = size == BIG and 8 or 5
        local entry = false
        local answer = call({ op = "preview", id = s.id, key = key, invert = invert, gap = s.gap, size = size })
        if answer and answer.busy then return nil end   -- the styles are still being made: ask again next frame
        if answer and answer.ok then
            entry = { runs = alphaRuns(answer.pixels, answer.size, shades), scale = 1 }
            if #entry.runs > (BUDGET[size] or 1e9) then
                local small = call({ op = "preview", id = s.id, key = key, invert = invert, gap = s.gap, size = size / 2 })
                if small and small.ok then entry = { runs = alphaRuns(small.pixels, small.size, shades), scale = 2 } end
            end
        end
        s.previews[id] = entry
    end
    return s.previews[id] or nil
end

local function stationIcon(id)
    if iconCache[id] == nil then
        local answer = call({ op = "preview", id = id, key = "current", size = LIST_ICON })
        iconCache[id] = answer and answer.ok and alphaRuns(answer.pixels, answer.size, 5) or false
    end
    return iconCache[id] or nil
end

local function loadThumb(url)
    if not url or url == "" or thumbCache[url] ~= nil then return end
    thumbCache[url] = "loading"
    begin({ op = "thumb", url = url, size = THUMB }, function(r)
        thumbCache[url] = r.ok and colourRuns(r.pixels, r.size) or false
    end)
end

-- ------------------------------------------------------------------ drawing helpers (screen coordinates)
local dl = nil   -- the current window's draw list

local function textW(s, size)
    local w = ImGui.CalcTextSize(s)
    return w * size / F
end

local function text(x, y, size, colour, s) ImGui.ImDrawListAddText(dl, size, x, y, u32(colour), s) end

-- letter-spaced text (the app's titles); returns its width
local function spaced(x, y, size, colour, s, spacing)
    local col, cx = u32(colour), x
    for i = 1, #s do
        local ch = s:sub(i, i)
        ImGui.ImDrawListAddText(dl, size, cx, y, col, ch)
        cx = cx + textW(ch, size) + spacing
    end
    return cx - x - spacing
end

local function spacedW(s, size, spacing)
    local w = 0
    for i = 1, #s do w = w + textW(s:sub(i, i), size) + spacing end
    return w - spacing
end

-- shortened with "..." to fit maxW (spacing: the letter spacing it will be drawn with)
local function fit(s, size, maxW, spacing)
    local width = spacing and function(t) return spacedW(t, size, spacing) end or function(t) return textW(t, size) end
    if width(s) <= maxW then return s end
    while #s > 1 and width(s .. "...") > maxW do s = s:sub(1, -2) end
    return s .. "..."
end


local function frame(x0, y0, x1, y1, colour, thick)
    ImGui.ImDrawListAddRect(dl, x0, y0, x1, y1, u32(colour), 0, 0, thick or 1)
end

local function fill(x0, y0, x1, y1, colour) ImGui.ImDrawListAddRectFilled(dl, x0, y0, x1, y1, u32(colour)) end

-- a filled box with the bottom-right corner cut off (the app's buttons)
local function cutFill(x0, y0, x1, y1, colour, cut)
    local col = u32(colour)
    ImGui.ImDrawListAddRectFilled(dl, x0, y0, x1 - cut, y1, col)
    ImGui.ImDrawListAddRectFilled(dl, x1 - cut, y0, x1, y1 - cut, col)
    ImGui.ImDrawListAddTriangleFilled(dl, x1 - cut, y1 - cut, x1, y1 - cut, x1 - cut, y1, col)
end

local function bars(x, y, h, colour)   -- the little "signal" icon
    local col = u32(colour)
    local w = h / 4
    for i = 0, 2 do
        local bh = h * (0.4 + 0.3 * i)
        ImGui.ImDrawListAddRectFilled(dl, x + i * (w + 3), y + h - bh, x + i * (w + 3) + w, y + h, col)
    end
end

-- an invisible button at local position (x, y) of the current window: clicked, hovered, screen x0, y0, x1, y1
local function hit(id, x, y, w, h)
    ImGui.SetCursorPos(x, y)
    local clicked = ImGui.InvisibleButton(id, math.max(1, w), math.max(1, h))
    local hovered = ImGui.IsItemHovered()
    local x0, y0 = ImGui.GetItemRectMin()
    local x1, y1 = ImGui.GetItemRectMax()
    return clicked, hovered, x0, y0, x1, y1
end

-- the app's button kinds: "primary" (yellow), "search" (red), "danger" / "ghost" (outlined)
local function button(id, x, y, w, h, label, kind, active)
    local clicked, hovered, x0, y0, x1, y1 = hit(id, x, y, w, h)
    local size = F * (h >= 44 and 1.05 or 0.82)
    local spacing = h >= 44 and 3 or 2
    local lw = spacedW(label, size, spacing)
    local tx, ty = x0 + (w - lw) / 2, y0 + (h - size) / 2
    if kind == "primary" then
        cutFill(x0, y0, x1, y1, hovered and { 1, 0.96, 0.3, 1 } or C.yellow, 12)
        spaced(tx, ty, size, C.ink, label, spacing)
    elseif kind == "search" then
        cutFill(x0, y0, x1, y1, hovered and { 1, 0.5, 0.47, 1 } or C.red, 8)
        spaced(tx, ty, size, C.ink, label, spacing)
    elseif kind == "danger" then
        fill(x0, y0, x1, y1, hovered and C.redFaint or { 0, 0, 0, 0 })
        frame(x0, y0, x1, y1, C.red)
        spaced(tx, ty, size, C.red, label, spacing)
    else
        local col = active and C.cyan or (hovered and C.red or C.redDim)
        frame(x0, y0, x1, y1, col)
        if active then fill(x0, y0, x1, y1, C.cyanFaint) end
        spaced(tx, ty, size, active and C.cyan or C.text, label, spacing)
    end
    return clicked
end

-- the app's boxed red titles ("RADIOPORT", "FIND A STATION"); returns its width
local function titleBox(x, y, label)
    local size = F * 1.15
    local w = spacedW(label, size, 3) + 28
    fill(x, y, x + w, y + 34, C.redFaint)
    frame(x, y, x + w, y + 34, C.red)
    spaced(x + 14, y + (34 - size) / 2, size, C.red, label, 3)
    return w
end

local function label(x, y, s, note)
    local w = spaced(x, y, F * 0.78, C.red, s, 2)
    if note then text(x + w + 12, y + 1, F * 0.7, C.muted, note) end
end

-- a text field styled like the app's (dark box, red frame, cyan text)
local function styledInput(kind, id, x, y, w, value, a, b, c)
    ImGui.SetCursorPos(x, y)
    ImGui.SetNextItemWidth(w)
    ImGui.PushStyleColor(ImGuiCol.Text, u32(C.cyan))
    ImGui.PushStyleVar(ImGuiStyleVar.FramePadding, 12, 10)
    local v, changed
    if kind == "text" then v, changed = ImGui.InputText(id, value, a, b or 0)
    elseif kind == "float" then v, changed = ImGui.InputFloat(id, value, 0, 0, a)
    elseif kind == "sliderf" then v, changed = ImGui.SliderFloat(id, value, a, b, c)
    else v, changed = ImGui.SliderInt(id, value, a, b, c) end
    ImGui.PopStyleVar(1)
    ImGui.PopStyleColor(1)
    return v, changed
end

-- ------------------------------------------------------------------ actions
local function openStation(id, mode, installed)
    setup = { mode = mode, id = id, loading = true, name = "", fm = 0, volume = 1, callLetters = "", styles = {},
              choice = "none", gap = 3, invert = false, previews = {}, hasLogo = false, info = "" }
    if installed then
        setup.name, setup.fm, setup.volume = installed.displayName, installed.fm, installed.volume
        setup.choice = installed.hasLogo and "keep" or "none"
        setup.installedLogo = installed.hasLogo
    end
    view = "setup"
    notice = nil
    begin({ op = "station", id = id, gap = 3 }, function(r)
        if not setup or setup.id ~= id then return end
        setup.loading = false
        if not r.ok then
            setup.info = ""
            say(r.error or "Couldn't load the station.", C.red)
            return
        end
        setup.callLetters = r.callLetters or ""
        local parts = {}
        for _, v in ipairs({ r.callLetters, r.city }) do if v and v ~= "" then parts[#parts + 1] = v end end
        setup.info = table.concat(parts, " - ")
        setup.styles = r.styles or {}
        setup.hasLogo = r.hasLogo
        if mode == "new" then
            setup.name, setup.fm = r.displayName, r.fm
            setup.choice = (#setup.styles > 0) and setup.styles[1].key or "none"
            if not r.hasStream then say("iHeart lists no live stream for this station: it may not play.", C.red) end
        elseif r.installed then
            setup.gap = r.installed.gap or 3
            setup.invert = r.installed.invert or false
            setup.savedKey = r.installed.key
            if setup.gap ~= 3 then
                -- the styles for the line gap it was made with
                begin({ op = "styles", gap = setup.gap }, function(st)
                    if setup and setup.id == id and st.ok then setup.styles = st.styles; setup.previews = {} end
                end)
            end
        end
    end)
end

local function save()
    local s = setup
    if not s or s.loading or s.saving then return end
    local name = (s.name or ""):gsub("^%s+", ""):gsub("%s+$", "")
    if name == "" then say("Give the station a name.", C.red) return end
    s.saving = true
    local logo = (s.choice == "keep" and "keep") or (s.choice == "none" and "none") or "style"
    begin({ op = "install", id = s.id, displayName = name, fm = s.fm, volume = s.volume, callLetters = s.callLetters,
            logo = logo, key = s.choice, gap = s.gap, invert = s.invert }, function(r)
        s.saving = false
        if not r.ok then say(r.error or "Couldn't save the station.", C.red) return end
        refresh()
        if setup == s then
            s.mode = "edit"
            s.installedLogo = logo == "style" or (logo == "keep" and s.installedLogo)
            if logo == "style" then s.savedKey = s.choice; s.choice = "keep"; s.previews = {} end
        end
        say(string.upper(name) .. " SAVED - RESTART TO TUNE IN")
    end)
end

local function removeStation(id, name)
    begin({ op = "remove", id = id }, function(r)
        if not r.ok then say(r.error or "Couldn't remove the station.", C.red) return end
        refresh()
        view = "search"
        setup = nil
        say(string.upper(name) .. " REMOVED - IT'S GONE AFTER THE NEXT RESTART")
    end)
end

local function search(q)
    if q then query = q end
    if query:gsub("%s", "") == "" then return end
    searching = true
    results = nil
    notice = nil
    view = "search"
    begin({ op = "search", q = query }, function(r)
        searching = false
        if not r.ok then say(r.error or "Search failed.", C.red) results = {} return end
        results = r.stations or {}
        for _, st in ipairs(results) do loadThumb(st.image) end
    end)
end

-- ------------------------------------------------------------------ header and footer
local function drawHeader(wx, wy, W)
    -- the app's logo box
    frame(wx + 24, wy + 24, wx + 76, wy + 76, C.redDim)
    bars(wx + 36, wy + 42, 22, C.cyan)
    spaced(wx + 96, wy + 14, F * 0.62, C.muted, "NCPD_RADIO_NET // TRN_TCLAS_IHR_01", 2)
    spaced(wx + 94, wy + 28, F * 2.1, C.red, "IHEART RADIO", 4)
    local w = spaced(wx + 96, wy + 66, F * 0.9, C.text, "STATION MANAGER", 6)
    text(wx + 96 + w + 12, wy + 69, F * 0.7, C.muted, "v" .. ((status and status.version) or "?") .. " - IN GAME")

    -- status chips, right aligned
    local chips = {
        { "RADIOEXT", status and status.radioExt },
        { "BRIDGE", status ~= nil },
        { (status and status.logosWaiting) and "LOGOS AFTER RESTART" or "LOGOS UP TO DATE", not (status and status.logosWaiting), true },
    }
    local x = wx + W - 24
    for i = #chips, 1, -1 do
        local c = chips[i]
        local size = F * 0.72
        local cw = spacedW(c[1], size, 2) + 34
        x = x - cw
        fill(x, wy + 34, x + cw, wy + 62, C.field)
        frame(x, wy + 34, x + cw, wy + 62, C.redDim)
        local dot = c[2] and C.cyan or (c[3] and C.yellow or C.red)
        fill(x + 10, wy + 44, x + 18, wy + 52, dot)
        spaced(x + 24, wy + 48 - size / 2, size, C.text, c[1], 2)
        x = x - 8
    end
end

local function statusText()
    if searching then return "SEARCHING IHEARTRADIO...", C.cyan end
    if setup and setup.loading then return "TUNING IN TO STATION " .. setup.id, C.cyan end
    if setup and setup.saving then return "WRITING STATION FILES", C.cyan end
    if notice then return string.upper(notice.text), notice.colour end
    if view == "search" and results then return #results .. " STATIONS FOUND", C.cyan end
    return "READY", C.cyan
end

local function drawFooter(wx, wy, W, H)
    local y = wy + H - 40
    ImGui.ImDrawListAddLine(dl, wx + 24, y - 8, wx + W - 24, y - 8, u32(C.redDim), 1)
    -- left: the note; right: the disclaimer, and REMOVE ALL STATIONS (two clicks) left of it
    local note = "CHANGES SHOW UP AFTER A GAME RESTART"
    local nw = spaced(wx + 24, y + 7, F * 0.62, C.muted, note, 1)
    local disclaimer = "NOT AFFILIATED WITH IHEARTMEDIA OR CD PROJEKT RED"
    local dw = spacedW(disclaimer, F * 0.62, 1)
    spaced(wx + W - 24 - dw, y + 7, F * 0.62, C.muted, disclaimer, 1)
    local armed = os.clock() - confirmRemoveAll < 5
    local lab = armed and "CLICK AGAIN TO REMOVE ALL STATIONS" or "REMOVE ALL STATIONS"
    local lw = spacedW(lab, F * 0.7, 2)
    local removeX = W - 24 - dw - 28 - lw
    -- the status, centred in the space between the note and REMOVE ALL STATIONS
    local s, colour = statusText()
    local size = F * 0.78
    local left, right = 24 + nw + 24, removeX - 24
    s = fit(s, size, right - left, 2)
    spaced(wx + (left + right) / 2 - spacedW(s, size, 2) / 2, y + 6, size, colour, s, 2)
    local clicked, hovered, x0, y0 = hit("##removeAll", removeX, H - 40, lw, 24)
    spaced(x0, y0 + 6, F * 0.7, armed and C.red or (hovered and C.text or C.muted), lab, 2)
    if clicked then
        if armed then
            confirmRemoveAll = 0
            begin({ op = "removeAll" }, function(r)
                refresh()
                setup = nil
                view = "search"
                say(r.ok and "All stations removed. Do this before uninstalling the mod." or (r.error or "failed"), r.ok and C.cyan or C.red)
            end)
        else
            confirmRemoveAll = os.clock()
        end
    end
end

-- ------------------------------------------------------------------ left panel: the installed stations
local function drawLeft(w, h)
    local px, py = ImGui.GetWindowPos()
    titleBox(px + 16, py + 16, "RADIOPORT")
    local count = #stations .. (#stations == 1 and " STATION" or " STATIONS")
    spaced(px + w - 16 - spacedW(count, F * 0.72, 2), py + 26, F * 0.72, C.muted, count, 2)

    ImGui.SetCursorPos(0, 66)
    if ImGui.BeginChild("##stationList", w, h - 66 - 76, false) then
        local ok, err = pcall(function()
            dl = ImGui.GetWindowDrawList()
            local rowW = w - 24
            for i, st in ipairs(stations) do
                local y = (i - 1) * 74 + 4
                local clicked, hovered, x0, y0, x1, y1 = hit("##st" .. st.id, 12, y, rowW, 66)
                local selected = setup and setup.id == st.id
                if selected then
                    fill(x0, y0, x1, y1, C.cyanFaint)
                    frame(x0, y0, x1, y1, C.cyan)
                    bars(x0 + 14, y0 + 24, 18, C.cyan)
                else
                    if hovered then frame(x0, y0, x1, y1, C.redDim) end
                    -- the app's little data column
                    local n = tonumber(st.id) or 0
                    for k = 0, 3 do
                        text(x0 + 8, y0 + 12 + k * 10, F * 0.5, C.muted, string.format("%07d", (n * (k + 7) * 7919) % 9999999))
                    end
                end
                local icon = stationIcon(st.id)
                if icon then drawRuns(dl, icon, x0 + 62, y0 + 13, 1)
                else spaced(x0 + 66, y0 + 26, F * 0.6, C.muted, "NO LOGO", 1) end
                text(x0 + 114, y0 + 12, F * 1.15, C.cyan, fit(st.displayName, F * 1.15, rowW - 124))
                spaced(x0 + 114, y0 + 40, F * 0.66, C.muted, "IHEART " .. st.id, 2)
                if clicked then openStation(st.id, "edit", st) end
            end
            if #stations == 0 then
                local x0, y0 = ImGui.GetWindowPos()
                text(x0 + 16, y0 + 12, F * 0.85, C.muted, "No stations yet. Click ADD STATION.")
            end
            ImGui.SetCursorPos(0, #stations * 74 + 8)
            ImGui.Dummy(1, 1)
        end)
        if not ok then ImGui.EndChild() error(err, 0) end
    end
    ImGui.EndChild()
    dl = ImGui.GetWindowDrawList()

    if button("##add", 14, h - 64, w - 28, 48, "+  ADD STATION", "primary") then
        view = "search"
        setup = nil
        notice = nil
    end
end

-- ------------------------------------------------------------------ right panel: find a station
local function drawSearch(w, h)
    local px, py = ImGui.GetWindowPos()
    titleBox(px + 16, py + 16, "FIND A STATION")
    spaced(px + w - 16 - spacedW("SOURCE: IHEARTRADIO.COM", F * 0.66, 2), py + 28, F * 0.66, C.muted, "SOURCE: IHEARTRADIO.COM", 2)

    -- the search bar: red frame, magnifier, the field, the red SEARCH button
    local bx0, by0, bw = px + 16, py + 70, w - 32 - 150
    fill(bx0, by0, bx0 + bw, by0 + 52, C.field)
    frame(bx0, by0, bx0 + bw, by0 + 52, C.red)
    ImGui.ImDrawListAddCircle(dl, bx0 + 22, by0 + 23, 7, u32(C.red), 16, 2)
    ImGui.ImDrawListAddLine(dl, bx0 + 27, by0 + 28, bx0 + 33, by0 + 34, u32(C.red), 2)
    ImGui.PushStyleColor(ImGuiCol.FrameBg, 0, 0, 0, 0)
    ImGui.PushStyleVar(ImGuiStyleVar.FrameBorderSize, 0)
    ImGui.SetWindowFontScale(1.3)
    local q, enter = styledInput("text", "##query", 60, 74, bw - 60, query, 100, ImGuiInputTextFlags.EnterReturnsTrue)
    ImGui.SetWindowFontScale(1.0)
    ImGui.PopStyleVar(1)
    ImGui.PopStyleColor(1)
    if type(q) == "string" then query = q end
    if (button("##search", w - 16 - 150, 70, 150, 52, "SEARCH", "search") or enter) and not searching then search() end

    -- examples (click to search)
    local ex, ey = px + 16, py + 132
    local w0 = spaced(ex, ey, F * 0.72, C.muted, "e.g.", 1)
    local x = 16 + w0 + 10
    for i, e in ipairs({ "Z100", "102.7 KIIS FM", "NEW YORK ROCK", "POWER 105.1" }) do
        local ew = spacedW(e, F * 0.72, 2)
        local clicked, hovered = hit("##ex" .. i, x, 130, ew, 18)
        spaced(px + x, ey, F * 0.72, hovered and C.text or C.cyan, e, 2)
        if clicked then search(e) end
        x = x + ew + 18
    end

    -- results: cards in a grid
    ImGui.SetCursorPos(0, 160)
    if ImGui.BeginChild("##results", w, h - 160, false) then
        local ok, err = pcall(function()
            dl = ImGui.GetWindowDrawList()
            local cx0, cy0 = ImGui.GetWindowPos()
            if not results then
                if not searching then
                    text(cx0 + 16, cy0 + 12, F * 0.9, C.muted, "Search for a station by name, call letters or city.")
                end
                return
            end
            if #results == 0 and not searching then
                text(cx0 + 16, cy0 + 12, F * 0.9, C.muted, "No stations found. Try the call letters or a city.")
                return
            end
            local cols = w >= 720 and 3 or 2
            local gap, cardH = 14, 112
            local cardW = (w - 32 - gap * (cols - 1)) / cols
            for i, st in ipairs(results) do
                local col, row = (i - 1) % cols, math.floor((i - 1) / cols)
                local x, y = 16 + col * (cardW + gap), 4 + row * (cardH + gap)
                ImGui.SetCursorPos(x, y)
                if ImGui.IsRectVisible(cardW, cardH) then
                    local clicked, hovered, x0, y0, x1, y1 = hit("##r" .. st.id, x, y, cardW, cardH)
                    fill(x0, y0, x1, y1, hovered and C.redFaint or C.panel)
                    frame(x0, y0, x1, y1, hovered and C.red or C.redDim)
                    -- the station's own logo on white
                    fill(x0 + 12, y0 + 16, x0 + 92, y0 + 96, { 1, 1, 1, 1 })
                    local thumb = thumbCache[st.image]
                    if type(thumb) == "table" then drawRuns(dl, thumb, x0 + 12, y0 + 16, 2.5)
                    else text(x0 + 22, y0 + 46, F * 0.8, C.muted, thumb == "loading" and "..." or st.callLetters) end
                    local tx, tw = x0 + 106, cardW - 118
                    local name = st.name:upper()
                    text(tx, y0 + 16, F * 1.05, C.text, fit(name, F * 1.05, tw - (st.installed and 74 or 0)))
                    local sub = st.callLetters .. ((st.frequency ~= "") and (" - " .. st.frequency) or "")
                    spaced(tx, y0 + 44, F * 0.68, C.muted, fit(sub, F * 0.68, tw, 1), 1)
                    spaced(tx, y0 + 64, F * 0.68, C.cyan, fit((st.genre or ""):upper(), F * 0.68, tw, 1), 1)
                    if st.installed then
                        local iw = spacedW("INSTALLED", F * 0.55, 1) + 10
                        fill(x1 - 10 - iw, y0 + 10, x1 - 10, y0 + 24, C.cyanDim)
                        frame(x1 - 10 - iw, y0 + 10, x1 - 10, y0 + 24, C.cyan)
                        spaced(x1 - 5 - iw, y0 + 12, F * 0.55, C.text, "INSTALLED", 1)
                    end
                    if clicked then
                        local installed
                        for _, s in ipairs(stations) do if s.id == st.id then installed = s end end
                        openStation(st.id, installed and "edit" or "new", installed)
                    end
                end
            end
            ImGui.SetCursorPos(0, math.ceil(#results / cols) * (cardH + gap) + 8)
            ImGui.Dummy(1, 1)
        end)
        if not ok then ImGui.EndChild() error(err, 0) end
    end
    ImGui.EndChild()
    dl = ImGui.GetWindowDrawList()
end

-- ------------------------------------------------------------------ right panel: set up a station
-- the game's radioport, the way the station will look in it
local function drawRadioport(x0, y0, w)
    local s = setup
    frame(x0, y0, x0 + w, y0 + 440, C.redDim)
    fill(x0, y0, x0 + w, y0 + 440, C.panel)
    frame(x0 + 16, y0 + 16, x0 + w - 16, y0 + 58, C.red)
    ImGui.ImDrawListAddLine(dl, x0 + w - 16, y0 + 16, x0 + w - 16, y0 + 58, u32(C.red), 3)
    spaced(x0 + 30, y0 + 26, F * 1.15, C.cyan, "RADIOPORT", 3)
    ImGui.ImDrawListAddLine(dl, x0, y0 + 72, x0 + w, y0 + 72, u32(C.redDim), 1)

    -- the logo, big, with its glow
    local bx, by, bs = x0 + 16, y0 + 88, 180
    fill(bx, by, bx + bs, by + bs, { 0.03, 0.02, 0.025, 1 })
    frame(bx, by, bx + bs, by + bs, C.redDim)
    fill(bx - 6, by + 20, bx - 3, by + bs - 20, C.red)
    local preview = s.choice ~= "none" and stylePreview(s.choice, s.invert, BIG) or nil
    if preview then
        drawGlow(dl, preview.runs, bx + 18, by + 4, preview.scale)
        drawRuns(dl, preview.runs, bx + 18, by + 4, preview.scale)
    elseif s.loading then
        spaced(bx + 40, by + 84, F * 0.75, C.muted, "LOADING LOGO...", 2)
    else
        spaced(bx + 34, by + 84, F * 0.75, C.muted, "DEFAULT GAME ICON", 2)
    end
    local imageName = "IHEART_" .. string.format("%08d", tonumber(s.id) or 0)
    for k, line in ipairs({ "IMAGE NAME: " .. imageName, "IMAGE TYPE: ROOT NODE", "LOAD ADDRESS: 00000000" }) do
        text(bx + 8, by + bs - 33 + (k - 1) * 10, F * 0.48, C.muted, line)
    end

    -- now playing
    local nx, nw = bx + bs + 20, w - bs - 52
    spaced(nx, by + 4, F * 0.62, C.muted, "NOW PLAYING", 2)
    ImGui.ImDrawListAddLine(dl, nx, by + 24, nx + nw, by + 24, u32(C.redDim), 1)
    text(nx, by + 36, F * 1.2, C.red, fit((s.name ~= "" and s.name or "NEW STATION"):upper(), F * 1.2, nw))
    spaced(nx, by + 66, F * 0.66, C.red, "ARTIST - SONG ON AIR", 1)
    ImGui.ImDrawListAddLine(dl, nx, by + bs - 50, nx + nw, by + bs - 50, u32(C.redDim), 1)
    local vy = by + bs - 28
    spaced(nx, vy - 22, F * 0.7, C.red, "VOLUME", 2)
    local ax = nx
    frame(ax, vy, ax + 24, vy + 24, C.cyan)
    text(ax + 7, vy + 3, F * 0.8, C.cyan, "A")
    text(ax + 36, vy + 2, F, C.cyan, string.format("%d%%", math.floor((s.volume or 1) * 100 + 0.5)))
    frame(ax + 92, vy, ax + 116, vy + 24, C.cyan)
    text(ax + 99, vy + 3, F * 0.8, C.cyan, "D")

    -- the station list around this one, like the radioport
    ImGui.ImDrawListAddLine(dl, x0, by + bs + 16, x0 + w, by + bs + 16, u32(C.redDim), 1)
    local list, mine = {}, nil
    for _, st in ipairs(stations) do
        if st.id ~= s.id then list[#list + 1] = { fm = st.fm or 0, name = st.displayName } end
    end
    mine = { fm = s.fm or 0, name = (s.name ~= "" and s.name or "NEW STATION"), me = true }
    list[#list + 1] = mine
    table.sort(list, function(a, b) return a.fm < b.fm end)
    local at = 1
    for i, e in ipairs(list) do if e.me then at = i end end
    local first = math.max(1, math.min(at - 1, #list - 3))   -- four rows fit in the card
    local ly = by + bs + 28
    for i = first, math.min(#list, first + 3) do
        local e = list[i]
        if e.me then
            fill(x0 + 16, ly - 4, x0 + w - 16, ly + 26, C.cyanFaint)
            frame(x0 + 16, ly - 4, x0 + w - 16, ly + 26, C.cyan)
            bars(x0 + 26, ly + 3, 14, C.cyan)
            text(x0 + 50, ly + 1, F * 1.0, C.cyan, fit(e.name:upper(), F, w - 80))
        else
            text(x0 + 26, ly + 1, F * 1.0, C.cyanDim, fit(e.name:upper(), F, w - 60))
        end
        ly = ly + 32
    end
end

local function drawTile(id, x, y, key, invert, caption, badge, selected)
    local clicked, hovered, x0, y0, x1, y1 = hit(id, x, y, 96, 116)
    fill(x0, y0, x1, y1, selected and C.cyanFaint or C.panel)
    frame(x0, y0, x1, y1, selected and C.cyan or (hovered and C.red or C.redDim))
    if key then
        local preview = stylePreview(key, invert, TILE)
        if preview then drawRuns(dl, preview.runs, x0 + 4, y0 + 6, preview.scale)
        else spaced(x0 + 38, y0 + 44, F * 0.6, C.muted, "...", 1) end
    else
        spaced(x0 + 10, y0 + 44, F * 0.62, C.muted, "GAME ICON", 1)
    end
    if badge then
        local colour = badge == "AUTO" and C.yellow or C.cyan
        local bw = spacedW(badge, F * 0.5, 1) + 10
        fill(x0 + 4, y0 + 4, x0 + 4 + bw, y0 + 16, colour)
        spaced(x0 + 9, y0 + 5, F * 0.5, C.ink, badge, 1)
    end
    local cap = fit(caption:upper(), F * 0.55, 88, 1)
    spaced(x0 + (96 - spacedW(cap, F * 0.55, 1)) / 2, y0 + 99, F * 0.55, selected and C.cyan or C.muted, cap, 1)
    return clicked
end

local function drawSetup(w, h)
    local s = setup
    local px, py = ImGui.GetWindowPos()
    -- BACK, the title, where the station is
    local clicked, hovered, bx0, by0 = hit("##back", 16, 22, 70, 22)
    spaced(bx0, by0 + 2, F * 0.85, hovered and C.text or C.red, "< BACK", 2)
    if clicked then view = "search"; setup = nil; notice = nil; return end
    titleBox(px + 100, py + 16, s.mode == "new" and "NEW STATION" or "EDIT STATION")
    if s.info ~= "" then
        local info = s.info:upper()
        spaced(px + w - 16 - spacedW(info, F * 0.66, 2), py + 28, F * 0.66, C.muted, info, 2)
    end

    ImGui.SetCursorPos(0, 66)
    if ImGui.BeginChild("##setup", w, h - 66, false) then
        local ok, err = pcall(function()
            dl = ImGui.GetWindowDrawList()
            local cx0, cy0 = ImGui.GetWindowPos()
            local cw = math.max(360, math.floor(w * 0.45))
            drawRadioport(cx0 + 16, cy0 + 4, cw)

            -- the fields
            local fx, fw = cw + 40, w - cw - 56
            label(cx0 + fx, cy0 + 6, "STATION NAME", "shown in the game")
            ImGui.SetWindowFontScale(1.25)
            local name = styledInput("text", "##name", fx, 26, fw, s.name or "", 64)
            ImGui.SetWindowFontScale(1.0)
            if type(name) == "string" then s.name = name end

            label(cx0 + fx, cy0 + 88, "FREQUENCY")
            label(cx0 + fx + 170, cy0 + 88, "VOLUME", string.format("%d%%", math.floor((s.volume or 1) * 100 + 0.5)))
            ImGui.SetWindowFontScale(1.25)
            s.fm = styledInput("float", "##fm", fx, 108, 150, s.fm or 0, "%.1f")
            ImGui.SetWindowFontScale(1.0)
            s.volume = styledInput("sliderf", "##vol", fx + 170, 114, fw - 170, s.volume or 1, 0.2, 1.5, "")

            -- logo styles
            label(cx0 + fx, cy0 + 168, "LOGO STYLE", "pick the one that reads best")
            local tiles = {}
            if s.installedLogo then tiles[#tiles + 1] = { choice = "keep", key = "keep", caption = "Installed logo", badge = "CURRENT" } end
            for i, st in ipairs(s.styles) do
                tiles[#tiles + 1] = { choice = st.key, key = st.key, caption = st.label, badge = (i == 1) and "AUTO" or nil }
            end
            tiles[#tiles + 1] = { choice = "none", key = nil, caption = "No logo" }
            local perRow = math.max(1, math.floor((fw + 10) / 106))
            local ty = 190
            if s.loading and #s.styles == 0 then
                spaced(cx0 + fx, cy0 + ty + 10, F * 0.75, C.muted, "LOADING THE STATION LOGO...", 2)
                ty = ty + 40
            end
            for i, t in ipairs(tiles) do
                local col, row = (i - 1) % perRow, math.floor((i - 1) / perRow)
                local invert = (t.choice ~= "keep" and t.choice ~= "none") and s.invert or false
                if drawTile("##tile" .. t.choice, fx + col * 106, ty + row * 126, t.key, invert, t.caption, t.badge,
                    s.choice == t.choice) then
                    s.choice = t.choice
                end
            end
            ty = ty + math.ceil(#tiles / perRow) * 126 + 6

            -- line gap and invert (they change the styles, not the installed logo)
            if s.hasLogo then
                label(cx0 + fx, cy0 + ty + 8, "LINE GAP")
                s.gapShown = styledInput("sliderint", "##gap", fx + 80, ty, 140, s.gapShown or s.gap, 0, MAX_GAP, "%d")
                if ImGui.IsItemDeactivatedAfterEdit() and s.gapShown ~= s.gap then
                    local gap = s.gapShown
                    begin({ op = "styles", gap = gap }, function(r)
                        if setup == s and r.ok and r.gap == gap then s.gap = gap; s.styles = r.styles; s.previews = {} end
                    end)
                end
                -- the app's INVERT switch
                local tclicked, _, tx0, ty0 = hit("##invert", fx + 250, ty + 4, 100, 22)
                local on = s.invert
                frame(tx0, ty0 + 4, tx0 + 32, ty0 + 18, on and C.cyan or C.redDim)
                fill(on and tx0 + 18 or tx0 + 3, ty0 + 7, on and tx0 + 29 or tx0 + 14, ty0 + 15, on and C.cyan or C.red)
                spaced(tx0 + 42, ty0 + 5, F * 0.75, on and C.cyan or C.muted, "INVERT", 2)
                if tclicked then s.invert = not s.invert end
                ty = ty + 44
            end

            -- save / remove
            local busy = s.loading or s.saving
            if button("##save", fx, ty + 10, 240, 54, busy and "PLEASE WAIT" or (s.mode == "new" and "INSTALL STATION" or "SAVE CHANGES"), "primary") then
                save()
            end
            if s.mode == "edit" then
                local lab = s.confirmRemove and "CONFIRM?" or "REMOVE"
                if button("##remove", fx + 256, ty + 10, 130, 54, lab, "danger") then
                    if s.confirmRemove then removeStation(s.id, s.name) else s.confirmRemove = true end
                end
            end
            ImGui.SetCursorPos(0, math.max(ty + 80, 460))
            ImGui.Dummy(1, 1)
        end)
        if not ok then ImGui.EndChild() error(err, 0) end
    end
    ImGui.EndChild()
    dl = ImGui.GetWindowDrawList()
end

-- ------------------------------------------------------------------ the window
local function drawContent()
    F = ImGui.GetFontSize()
    dl = ImGui.GetWindowDrawList()
    local wx, wy = ImGui.GetWindowPos()
    local W, H = ImGui.GetWindowSize()

    -- the app's faint grid
    local grid = u32(C.red, 0.035)
    for gx = wx, wx + W, 48 do ImGui.ImDrawListAddLine(dl, gx, wy, gx, wy + H, grid, 1) end
    for gy = wy, wy + H, 48 do ImGui.ImDrawListAddLine(dl, wx, gy, wx + W, gy, grid, 1) end

    drawHeader(wx, wy, W)
    if not status then
        text(wx + 24, wy + 110, F * 1.1, C.red, "The iHeart Radio plugin isn't loaded.")
        text(wx + 24, wy + 136, F * 0.9, C.text, "Install the whole mod (bin and red4ext folders) and make sure RED4ext is installed.")
        return
    end
    if not status.radioExt then
        spaced(wx + 24, wy + 96, F * 0.75, C.red, "RADIOEXT IS MISSING: THE STATIONS PLAY THROUGH IT (NEXUS MOD 4591)", 2)
    end

    local top, bottom = 114, 56
    local ph = H - top - bottom
    local lw = math.max(290, math.floor(W * 0.27))
    local rx, rw = 16 + lw + 16, W - (16 + lw + 16) - 16

    -- panel frames
    fill(wx + 16, wy + top, wx + 16 + lw, wy + top + ph, C.panel)
    frame(wx + 16, wy + top, wx + 16 + lw, wy + top + ph, C.redDim)
    fill(wx + rx, wy + top, wx + rx + rw, wy + top + ph, C.panel)
    frame(wx + rx, wy + top, wx + rx + rw, wy + top + ph, C.redDim)

    ImGui.SetCursorPos(16, top)
    if ImGui.BeginChild("##left", lw, ph, false, ImGuiWindowFlags.NoScrollbar + ImGuiWindowFlags.NoScrollWithMouse) then
        local ok, err = pcall(function()
            dl = ImGui.GetWindowDrawList()
            drawLeft(lw, ph)
        end)
        if not ok then ImGui.EndChild() error(err, 0) end
    end
    ImGui.EndChild()

    ImGui.SetCursorPos(rx, top)
    if ImGui.BeginChild("##right", rw, ph, false, ImGuiWindowFlags.NoScrollbar + ImGuiWindowFlags.NoScrollWithMouse) then
        local ok, err = pcall(function()
            dl = ImGui.GetWindowDrawList()
            if view == "setup" and setup then drawSetup(rw, ph) else drawSearch(rw, ph) end
        end)
        if not ok then ImGui.EndChild() error(err, 0) end
    end
    ImGui.EndChild()

    dl = ImGui.GetWindowDrawList()
    drawFooter(wx, wy, W, H)
end

local drawError = nil
function menu.draw()
    if not menu.visible then return end
    -- the app's colours for the parts ImGui draws itself (frames, sliders, scroll bars, text fields)
    local colours = {
        { ImGuiCol.WindowBg, C.bg }, { ImGuiCol.ChildBg, { 0, 0, 0, 0 } }, { ImGuiCol.Border, C.redDim },
        { ImGuiCol.Text, C.text }, { ImGuiCol.FrameBg, C.field }, { ImGuiCol.FrameBgHovered, { 0.12, 0.05, 0.06, 1 } },
        { ImGuiCol.FrameBgActive, { 0.14, 0.06, 0.07, 1 } }, { ImGuiCol.SliderGrab, C.cyan }, { ImGuiCol.SliderGrabActive, C.cyan },
        { ImGuiCol.ScrollbarBg, { 0, 0, 0, 0 } }, { ImGuiCol.ScrollbarGrab, C.redDim },
        { ImGuiCol.ScrollbarGrabHovered, { 1.0, 0.365, 0.333, 0.5 } }, { ImGuiCol.ScrollbarGrabActive, C.red },
        { ImGuiCol.TextSelectedBg, C.cyanDim }, { ImGuiCol.ResizeGrip, C.redDim }, { ImGuiCol.ResizeGripHovered, C.red },
        { ImGuiCol.ResizeGripActive, C.red },
    }
    for _, c in ipairs(colours) do ImGui.PushStyleColor(c[1], c[2][1], c[2][2], c[2][3], c[2][4]) end
    local vars = {
        { ImGuiStyleVar.WindowRounding, 0 }, { ImGuiStyleVar.ChildRounding, 0 }, { ImGuiStyleVar.FrameRounding, 0 },
        { ImGuiStyleVar.GrabRounding, 0 }, { ImGuiStyleVar.ScrollbarRounding, 0 }, { ImGuiStyleVar.FrameBorderSize, 1 },
        { ImGuiStyleVar.WindowBorderSize, 1 }, { ImGuiStyleVar.ScrollbarSize, 6 }, { ImGuiStyleVar.GrabMinSize, 14 },
    }
    for _, v in ipairs(vars) do ImGui.PushStyleVar(v[1], v[2]) end
    ImGui.PushStyleVar(ImGuiStyleVar.WindowPadding, 0, 0)

    ImGui.SetNextWindowSize(1280, 860, ImGuiCond.FirstUseEver)
    if ImGui.Begin("iHeart Radio##stationManager", ImGuiWindowFlags.NoTitleBar + ImGuiWindowFlags.NoCollapse +
        ImGuiWindowFlags.NoScrollbar + ImGuiWindowFlags.NoScrollWithMouse) then
        if drawError then
            ImGui.SetCursorPos(24, 24)
            ImGui.TextWrapped("The iHeart Radio menu hit an error: " .. drawError)
        else
            local ok, err = pcall(drawContent)
            if not ok then
                drawError = tostring(err)
                pcall(function() spdlog.error("[iHeartRadio] menu: " .. drawError) end)
            end
        end
    end
    ImGui.End()

    ImGui.PopStyleVar(#vars + 1)
    ImGui.PopStyleColor(#colours)
end

function menu.open()
    menu.visible = true
    refresh()
end

function menu.close() menu.visible = false end

return menu
