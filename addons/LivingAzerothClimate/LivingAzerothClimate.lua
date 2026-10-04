-- Living Azeroth: Climate -- a small line under the minimap clock: temperature, humidity, weather.
-- An example for the functions wxl-seyris-living-azeroth gives addons (docs/lua-api.md). Without the
-- module (functions missing) it stays hidden.

local WEATHER = { fine = "", rain = " Rain", snow = " Snow", sand = " Sandstorm" }

local frame = CreateFrame("Frame", "LivingAzerothClimateFrame", Minimap)
frame:SetSize(120, 14)
local text = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
text:SetAllPoints()

local function Place()
    frame:ClearAllPoints()
    if TimeManagerClockButton and TimeManagerClockButton:IsShown() then
        frame:SetPoint("TOP", TimeManagerClockButton, "BOTTOM", 0, 2)
    else
        frame:SetPoint("TOP", Minimap, "BOTTOM", 0, -2)
    end
end

local function Update()
    if not LivingAzeroth_GetTemperature then frame:Hide() return end
    local ground, air = LivingAzeroth_GetTemperature()
    if not air then frame:Hide() return end
    local humidity = LivingAzeroth_GetHumidity() or 0
    local weather, intensity = LivingAzeroth_GetWeather()
    local label = WEATHER[weather] or ""
    if label ~= "" and intensity and intensity < 0.34 then label = " Light" .. label:lower() end
    text:SetFormattedText("%d\194\176C  %d%%%s", math.floor(air + 0.5), math.floor(humidity * 100 + 0.5), label)
    frame:Show()
end

-- Hover: the details.
frame:EnableMouse(true)
frame:SetScript("OnEnter", function(self)
    if not LivingAzeroth_GetTemperature then return end
    local ground, air = LivingAzeroth_GetTemperature()
    local moisture, rest = LivingAzeroth_GetGroundMoisture()
    GameTooltip:SetOwner(self, "ANCHOR_BOTTOMLEFT")
    GameTooltip:AddLine("Climate")
    if air then GameTooltip:AddDoubleLine("Air", string.format("%.1f\194\176C", air), 1, 1, 1, 1, 1, 1) end
    if ground then GameTooltip:AddDoubleLine("Ground", string.format("%.1f\194\176C", ground), 1, 1, 1, 1, 1, 1) end
    GameTooltip:AddDoubleLine("Humidity", string.format("%d%%", (LivingAzeroth_GetHumidity() or 0) * 100), 1, 1, 1, 1, 1, 1)
    if moisture then GameTooltip:AddDoubleLine("Ground moisture", string.format("%d%% (rests at %d%%)", moisture * 100, rest * 100), 1, 1, 1, 1, 1, 1) end
    GameTooltip:Show()
end)
frame:SetScript("OnLeave", function() GameTooltip:Hide() end)

-- The timer runs on a frame that's never hidden (a hidden frame gets no OnUpdate).
local events = CreateFrame("Frame")
local elapsed = 0
events:SetScript("OnUpdate", function(self, dt)
    elapsed = elapsed + dt
    if elapsed < 1 then return end
    elapsed = 0
    Update()
end)
events:RegisterEvent("PLAYER_ENTERING_WORLD")
events:SetScript("OnEvent", function() Place() Update() end)
Place()
