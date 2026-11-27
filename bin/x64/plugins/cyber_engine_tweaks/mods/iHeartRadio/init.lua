-- iHeart Radio (CET part): shows the song on air on iHeart stations.
--
-- The bridge (red4ext\plugins\iHeartRadio\iHeartRadio.dll) reads the song titles sent inside each stream the game
-- listens to and writes them to data/nowplaying.txt, one line per station:  <id> TAB <artist> TAB <title>
-- (artist / title empty during ads and DJ talk).
--
-- RadioExt shows a stream station's "currentSong.path" as its track (radio menu NOW PLAYING, song toast in the car),
-- and for streams it is only used for that. So we put "Artist - Title" there, or the station name between songs,
-- and send the game's own "radio song changed" event so the open radio menu and the car HUD refresh.

local NOW_FILE = "data/nowplaying.txt"
local READ_EVERY = 2.0      -- seconds; the bridge itself updates every 10 s

local timer = 0
local lastRaw = nil
local shown = {}            -- station id -> text currently shown

local function readNowPlaying()
    local f = io.open(NOW_FILE, "r")
    if not f then return nil end
    local s = f:read("*a")
    f:close()
    return s
end

local function parse(s)
    local t = {}
    for line in (s or ""):gmatch("[^\r\n]+") do
        local id, artist, title = line:match("^(%d+)\t([^\t]*)\t(.*)$")
        if id then t[id] = { artist = artist, title = title } end
    end
    return t
end

local function trackText(e)
    if not e or not e.title or e.title == "" then return nil end
    if e.artist and e.artist ~= "" then return e.artist .. " - " .. e.title end
    return e.title
end

-- iHeart station id of a RadioExt station, from its bridge URL (http://127.0.0.1:8925/zc1469.mp3)
local function stationId(radio)
    local ok, url = pcall(function() return radio.metadata.streamInfo.streamURL end)
    if not ok or type(url) ~= "string" then return nil end
    return url:match("^https?://127%.0%.0%.1:%d+/zc(%d+)") or url:match("ihrhls%.com/zc(%d+)")
end

local function songChanged()
    pcall(function() Game.GetUISystem():QueueEvent(VehicleRadioSongChanged.new()) end)
end

local function update(force)
    local rx = GetMod("radioExt")
    if not rx or not rx.radioManager or not rx.radioManager.radios then return end
    local raw = readNowPlaying()
    if raw == lastRaw and not force then return end
    lastRaw = raw
    local now = parse(raw)

    for _, radio in pairs(rx.radioManager.radios) do
        local id = stationId(radio)
        if id and radio.currentSong then
            local song = trackText(now[id])
            local text = song or radio.name
            if radio.currentSong.path ~= text then
                radio.currentSong.path = text
                pcall(function() spdlog.info("[iHeartRadio] " .. tostring(radio.name) .. ": " .. text) end)
                -- heard right now in the car / on the pocket radio: refresh the menu, and show the song toast for a
                -- new song (not when it goes back to the station name for ads)
                local playing = radio.channels and radio.channels[-1]
                if playing and (song or shown[id]) then songChanged() end
            end
            shown[id] = song
        end
    end
end

registerForEvent("onInit", function()
    print("[iHeartRadio] now playing loaded" .. (GetMod("radioExt") and "" or " - RadioExt is missing!"))
    -- picking one of our stations: show what's on at once (the file may already know it)
    pcall(function()
        ObserveAfter("VehicleRadioPopupGameController", "Activate", function() update(true) end)
    end)
end)

registerForEvent("onUpdate", function(dt)
    timer = timer + dt
    if timer < READ_EVERY then return end
    timer = 0
    pcall(update, false)
end)

-- for other mods (e.g. car dashboards): what an iHeart station is playing
return {
    nowPlaying = function(stationName)
        local rx = GetMod("radioExt")
        if not rx or not rx.radioManager then return nil end
        for _, radio in pairs(rx.radioManager.radios) do
            if radio.name == stationName and stationId(radio) then return radio.currentSong and radio.currentSong.path end
        end
        return nil
    end,
}
