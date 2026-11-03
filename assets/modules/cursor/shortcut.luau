--!nonstrict
-- Control centre tile that opens the cursor panel.

local function render()
  shortcut.setLabel(okaru.tr("title"))
  shortcut.setIcon("pointer")
end

render()

function onClick()
  okaru.togglePanel("okaru/cursor:manager")
end
