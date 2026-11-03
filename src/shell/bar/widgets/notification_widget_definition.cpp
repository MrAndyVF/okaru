#include "shell/bar/widgets/notification_widget_definition.h"

const okaru::bar::WidgetDefinition<NotificationWidget::Options>& notificationWidgetDefinition() {
  using okaru::bar::field;
  using Options = NotificationWidget::Options;

  static const okaru::bar::WidgetDefinition<Options> definition{
      .type = "notifications",
      .fields = {
          field<&Options::hideWhenNoUnread>({
              .key = "hide_when_no_unread",
          }),
      },
  };
  return definition;
}
