--!nonstrict
-- Resets the icon theme when the plugin is removed from OKARU's settings.
--
-- OKARU deletes the plugin directory before it tears the runtime down, so
-- by the time onExit runs bin/iconmgr.py is gone. The data dir survives, so
-- every start keeps a copy of the engine there. A plugin removed while already
-- disabled gets no onExit at all: the panel's Reset button covers that case.

local shared = require("./shared.luau")

local function dataEngine()
  local data = okaru.pluginDataDir()
  return type(data) == "string" and data ~= "" and data .. "/iconmgr.py" or nil
end

local dir, copy = okaru.pluginDir(), dataEngine()
if type(dir) == "string" and dir ~= "" and copy ~= nil then
  okaru.runAsync(`cp -f {shared.shellQuote(dir .. "/bin/iconmgr.py")} {shared.shellQuote(copy)}`)
end

function onExit(_signal, reason)
  -- Disabling is temporary and shutdown is every logout; only a removal resets.
  if reason ~= "uninstall" or copy == nil then
    return
  end
  -- No callback: the runtime is gone before this finishes.
  okaru.runAsync(`{shared.engine(copy)} uninstall > {shared.shellQuote(copy .. ".uninstall.json")} 2>&1`)
end
