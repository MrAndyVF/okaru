--!nonstrict
-- Control centre tile that opens the icons panel.

shortcut.setLabel(okaru.tr("title"))
shortcut.setIcon("icons")

function onClick()
  okaru.togglePanel("okaru/icons:manager")
end
