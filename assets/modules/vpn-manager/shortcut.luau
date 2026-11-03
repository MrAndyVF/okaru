--!nonstrict
-- VPN Manager control-center shortcut: toggles the default VPN connection.

local connected = false
local defaultUuid = ""

local function render()
  shortcut.setLabel(okaru.tr("title"))
  shortcut.setIcon("shield-lock-filled", "shield-lock")
  shortcut.setActive(connected)
  shortcut.setEnabled(true)
end

okaru.state.watch("vpn.status", function(status)
  connected = type(status) == "table" and status.anyActive == true
  render()
end)

okaru.state.watch("vpn.defaultVpnUuid", function(uuid)
  defaultUuid = type(uuid) == "string" and uuid or ""
end)

function onClick()
  if defaultUuid == "" then
    okaru.notifyError(okaru.tr("title"), okaru.tr("notifications.no_default"))
    okaru.togglePanel("okaru/vpn-manager:panel")
    return
  end
  if connected then
    okaru.state.set("vpn.cmd", { op = "disconnect", uuid = defaultUuid })
  else
    okaru.state.set("vpn.cmd", { op = "connect", uuid = defaultUuid })
  end
end

render()
