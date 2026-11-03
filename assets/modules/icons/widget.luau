--!nonstrict
-- Bar button for the icons panel. Optional: the control-centre tile is the
-- other way in; this makes the plugin addable from the bar's widget picker.

barWidget.setGlyph("icons")
barWidget.setTooltip(okaru.tr("title"))

function onClick()
  okaru.togglePanel("okaru/icons:manager")
end
