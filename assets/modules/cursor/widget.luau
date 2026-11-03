--!nonstrict
-- Bar button for the cursor panel. Optional: the control-centre tile stays the
-- main way in; this only makes the plugin addable from the bar's widget picker.

barWidget.setGlyph("pointer")
barWidget.setTooltip(okaru.tr("title"))

function onClick()
  okaru.togglePanel("okaru/cursor:manager")
end
