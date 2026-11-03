--!nonstrict
-- VPN Manager bar widget: shield glyph that reflects connection state, click
-- opens the panel.

local connected = false
local activeNames = {}

local function render()
  barWidget.setGlyph(connected and "shield-lock-filled" or "shield-lock")
  if connected then
    barWidget.setColor("primary")
  else
    barWidget.setColor("on_surface_variant")
  end
  if connected and #activeNames > 0 then
    barWidget.setTooltip(okaru.tr("tooltip.connected", { names = table.concat(activeNames, ", ") }))
  else
    barWidget.setTooltip(okaru.tr("tooltip.disconnected"))
  end
end

okaru.state.watch("vpn.status", function(status)
  if type(status) == "table" then
    connected = status.anyActive == true
    activeNames = type(status.activeNames) == "table" and status.activeNames or {}
  else
    connected = false
    activeNames = {}
  end
  render()
end)

function onClick()
  okaru.togglePanel("okaru/vpn-manager:panel")
end

render()
