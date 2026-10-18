GameMenus = GameMenus or {}

local function Escape(value)
	value = tostring(value or "")
	return value:gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;"):gsub('"', "&quot;"):gsub("'", "&#39;")
end

local function ScoresHtml(rows)
	if type(rows) ~= "table" or #rows == 0 then
		return "<p>No high scores recorded for this map.</p>"
	end
	local html = {}
	for _, score in ipairs(rows) do
		local label = string.format("%s / %s / %s / %s", Escape(score.mode), Escape(score.difficulty), Escape(score.team), Escape(score.layout))
		html[#html + 1] = "<div class='scoreCategory'>" .. label .. "</div>"
		html[#html + 1] = string.format(
			"<div class='scoreEntry'><div class='scoreMeta'>#%s &nbsp; %s &nbsp; %s</div><div class='scorePlayers'>%s</div></div>",
			Escape(math.floor(tonumber(score.rank) or 0)), Escape(score.date), Escape(score.score), Rocket_QuakeToRML(score.players or ""))
	end
	return table.concat(html)
end

local function LoadScores(map, onPage)
	cgame.Request("highscores", { map = map }):Then(function(reply)
		local grouped, byId = {}, {}
		for _, row in ipairs(reply.items or {}) do
			local score = byId[row.recordId]
			if not score then
				score = {
					recordId = row.recordId, mode = row.mode, difficulty = row.difficulty,
					team = row.team, layout = row.layout, rank = row.rank,
					date = row.date, score = row.score, names = {}
				}
				byId[row.recordId] = score
				grouped[#grouped + 1] = score
			end
			if row.player and row.player ~= "No players" then
				score.names[#score.names + 1] = row.player
			end
		end
		for _, score in ipairs(grouped) do
			score.players = #score.names > 0 and table.concat(score.names, ", ") or "No players"
			score.names = nil
		end
		onPage(grouped)
	end, function(err)
		onPage(nil, err)
	end)
end

function GameMenus.LoadWelcome(document)
	document:GetElementById("welcomeError").inner_rml = ""
	cgame.Request("menu_state", {}):Then(function(state)
		GameMenus.currentMap = state.map
		document:GetElementById("welcomeMap").inner_rml = Escape(state.map) .. " - " .. Escape(state.mode)
		document:GetElementById("welcomeLevelshot").inner_rml = "<img src='/$levelshot/" .. Escape(state.map) .. "' />"
		LoadScores(state.map, function(rows, err)
			local scores = document:GetElementById("welcomeScores")
			scores.inner_rml = err and ("<p>" .. Escape(err) .. "</p>") or ScoresHtml(rows)
		end)
		GameMenus.ModeChanged(document)
	end, function(err)
		document:GetElementById("welcomeMap").inner_rml = Escape(err)
	end)
end

local function RenderLayouts(document, layouts, requestedTeam)
	local layoutSelect = Element.As.ElementFormControlSelect(document:GetElementById("welcomeLayout"))
	local seen = {}
	layoutSelect:RemoveAll()
	for _, layout in ipairs(layouts or {}) do
		local key = string.lower(layout)
		local matchesTeam = key == "builtin" or (requestedTeam ~= "" and key:sub(1, #requestedTeam + 1) == requestedTeam .. "-")
		if matchesTeam and not seen[key] then
			seen[key] = true
			layoutSelect:Add(Escape(layout), layout)
		end
	end
end

function GameMenus.LoadLayouts(document, map)
	local team = Element.As.ElementFormControlSelect(document:GetElementById("welcomeTeam")).value
	local mode = Element.As.ElementFormControlSelect(document:GetElementById("welcomeMode")).value
	-- Tower Defense has no team selector, so only its team-neutral builtin layout applies.
	local requestedTeam = (mode == "pve" or mode == "raid") and team or ""
	local cacheKey = map .. "|" .. requestedTeam
	GameMenus.layoutsByMapTeam = GameMenus.layoutsByMapTeam or {}
	local cached = GameMenus.layoutsByMapTeam[cacheKey]
	if cached then
		RenderLayouts(document, cached, requestedTeam)
		return
	end
	cgame.Request("layouts", { map = map, team = requestedTeam }):Then(function(reply)
		local layouts, seen = {}, {}
		for _, layout in ipairs(reply.layouts or {}) do
			local key = string.lower(layout)
			if not seen[key] then
				seen[key] = true
				layouts[#layouts + 1] = layout
			end
		end
		GameMenus.layoutsByMapTeam[cacheKey] = layouts
		local currentTeam = Element.As.ElementFormControlSelect(document:GetElementById("welcomeTeam")).value
		local currentMode = Element.As.ElementFormControlSelect(document:GetElementById("welcomeMode")).value
		local currentRequestedTeam = (currentMode == "pve" or currentMode == "raid") and currentTeam or ""
		if GameMenus.currentMap == map and currentRequestedTeam == requestedTeam then
			RenderLayouts(document, layouts, requestedTeam)
		end
	end, function(err)
		document:GetElementById("welcomeError").inner_rml = Escape(err)
	end)
end

function GameMenus.TeamChanged(document)
	if GameMenus.currentMap then GameMenus.LoadLayouts(document, GameMenus.currentMap) end
end

function GameMenus.ModeChanged(document)
	local mode = Element.As.ElementFormControlSelect(document:GetElementById("welcomeMode")).value
	document:GetElementById("welcomeModeSettings").style.display = "block"
	local team = document:GetElementById("welcomeTeam")
	local hasTeamSettings = mode == "pve" or mode == "raid"
	team.style.display = hasTeamSettings and "block" or "none"
	document:GetElementById("welcomeTeamLabel").style.display = hasTeamSettings and "block" or "none"
	local difficulty = document:GetElementById("welcomeDifficulty")
	difficulty.style.display = hasTeamSettings and "block" or "none"
	document:GetElementById("welcomeDifficultyLabel").style.display = hasTeamSettings and "block" or "none"
	GameMenus.TeamChanged(document)
end

function GameMenus.CallModeVote(document)
	local getValue = function(id) return Element.As.ElementFormControlSelect(document:GetElementById(id)).value end
	local mode = getValue("welcomeMode")
	local vote = mode
	if mode == "pve" or mode == "raid" then
		vote = getValue("welcomeTeam") .. mode
	end
	local args = " " .. getValue("welcomeDifficulty") .. " " .. getValue("welcomeLayout")
	Cmd.exec("callvote " .. vote .. args)
	document:Hide()
end

local function FillMapResults(document, rows, append)
	local select = Element.As.ElementFormControlSelect(document:GetElementById("mapResults"))
	GameMenus.mapItems = GameMenus.mapItems or {}
	if not append then GameMenus.mapItems = {} end
	for _, map in ipairs(rows or {}) do
		GameMenus.mapItems[#GameMenus.mapItems + 1] = map.name
	end
	select:RemoveAll()
	select:Add("Select a map", "")
	for _, name in ipairs(GameMenus.mapItems) do
		select:Add(Escape(name), name)
	end
end

function GameMenus.SearchMaps(document, query)
	if query == nil then query = Element.As.ElementFormControlInput(document:GetElementById("mapSearch")).value end
	document:GetElementById("moreMaps"):SetAttribute("cursor", "")
	document:GetElementById("moreMaps"):SetAttribute("query", query or "")
	document:GetElementById("moreMaps").style.display = "none"
	local select = document:GetElementById("mapResults")
	FillMapResults(document, {}, false)
	local progress = document:GetElementById("mapSearchProgress")
	progress.style.display = "block"
	GameMenus.mapSearchGeneration = (GameMenus.mapSearchGeneration or 0) + 1
	local generation = GameMenus.mapSearchGeneration
	cgame.Request("maps", { query = query or "", cursor = "" }):Then(function(reply)
		if generation ~= GameMenus.mapSearchGeneration then return end
		FillMapResults(document, reply.items, false)
		progress.style.display = "none"
		local more = document:GetElementById("moreMaps")
		more:SetAttribute("cursor", reply.nextCursor or "")
		more.style.display = reply.nextCursor and "block" or "none"
	end, function(err)
		if generation ~= GameMenus.mapSearchGeneration then return end
		local mapSelect = Element.As.ElementFormControlSelect(select)
		mapSelect:RemoveAll()
		mapSelect:Add(Escape(err), "")
		progress.style.display = "none"
	end)
end

function GameMenus.MoreMaps(document)
	local more = document:GetElementById("moreMaps")
	local cursor = more:GetAttribute("cursor")
	if not cursor or cursor == "" then return end
	cgame.Request("maps", { query = more:GetAttribute("query"), cursor = cursor }):Then(function(reply)
		FillMapResults(document, reply.items, true)
		more:SetAttribute("cursor", reply.nextCursor or "")
		more.style.display = reply.nextCursor and "block" or "none"
	end, function(err)
		document:GetElementById("highscoreResults").inner_rml = Escape(err)
	end)
end

function GameMenus.SelectMap(document)
	local map = Element.As.ElementFormControlSelect(document:GetElementById("mapResults")).value
	if map == "" then return end
	document:GetElementById("highscoreLevelshot").inner_rml = "<img src='/$levelshot/" .. Escape(map) .. "' />"
	document:GetElementById("highscoreResults").inner_rml = "<p>Loading scores…</p>"
	LoadScores(map, function(rows, err)
		document:GetElementById("highscoreResults").inner_rml = err and ("<p>" .. Escape(err) .. "</p>") or ScoresHtml(rows)
	end)
end
