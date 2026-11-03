-- Claude Code Usage — headless service (data owner).
--
-- Runs get-claude-usage (with --json) on the configured interval, decodes the
-- result, and publishes it to the plugin's shared state so the bar widget and the
-- desktop widget can mirror it without each shelling out. Mirrors the
-- service-owns-logic pattern of the screen_recorder plugin.
--
--   okaru.state.set("usage", <table>)   -- the parsed usage object
--   okaru.state.set("status", {...})     -- loading / ready / error + availability
--
-- Drive a manual refresh from the shell:
--   okaru msg plugin okaru/claudecode:service all refresh

local scriptPath = `{okaru.pluginDir()}/get-claude-usage`
local depsOk = okaru.commandExists("jq") and okaru.commandExists("curl")
-- runAsync kills the process after 5 s unless told otherwise; a large Claude Code
-- history takes longer than that to scan. 60 s is the host's maximum.
local FETCH_TIMEOUT_MS = 60000
local fetching = false
local started = false

local function publishStatus(state, message)
  okaru.state.set("status", { state = state, available = depsOk, message = message or "" })
end

-- refresh_interval is in minutes (2–15); convert to the update() cadence in ms.
local function intervalMs()
  local mins = okaru.getConfig("refresh_interval")
  if type(mins) ~= "number" or mins < 1 then
    mins = 2
  end
  return math.floor(mins * 60000)
end

local function fetch()
  if fetching then
    return
  end
  if not depsOk then
    publishStatus("error", "jq and curl are required")
    return
  end

  fetching = true
  publishStatus("loading")

  local launched = okaru.runAsync(`bash '{scriptPath}' --json`, function(result)
    fetching = false

    if type(result) ~= "table" or result.success == false or result.exitCode ~= 0 then
      okaru.log("claudecode: get-claude-usage failed")
      publishStatus("error", "Fetch failed")
      return
    end

    local data = okaru.json.decode(result.stdout or "")
    if type(data) ~= "table" then
      okaru.log("claudecode: could not decode JSON output")
      publishStatus("error", "Bad data")
      return
    end

    okaru.state.set("usage", data)
    publishStatus("ready")
  end, FETCH_TIMEOUT_MS)

  if not launched then
    fetching = false
    publishStatus("error", "Could not launch get-claude-usage")
  end
end

-- update() fires immediately at load and then on the interval — that gives us the
-- DMS "refresh on start, then every N minutes" behavior for free.
function update()
  okaru.setUpdateInterval(intervalMs())
  if not started then
    started = true
    if not depsOk then
      publishStatus("error", "jq and curl are required")
    end
  end
  fetch()
end

-- The bar widget bumps this nonce on click to force an out-of-band refresh.
okaru.state.watch("refresh_nonce", function()
  fetch()
end)

function onIpc(event)
  if event == "refresh" then
    fetch()
  end
end
