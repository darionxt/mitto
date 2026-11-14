// The events that document.createEvent() makes, as Blink's EventFactory and
// EventModulesFactory, which make_event_factory.py writes, make them.
//
// Upstream: third_party/blink/renderer/build/scripts/make_event_factory.py
// Upstream: third_party/blink/renderer/build/scripts/templates/event_factory.cc.tmpl
// Upstream: third_party/blink/renderer/core/events/event_interface_aliases.json5

#ifndef MITTO_DOM_EVENT_FACTORY_H_
#define MITTO_DOM_EVENT_FACTORY_H_

#include <string>

namespace mitto {

class Event;

// A new event of the interface that `name` names in any ASCII case, or one of
// its aliases names, such as "MouseEvents" for MouseEvent: with an empty type,
// neither bubbling nor cancelable, and not initialized until initEvent(), as
// each interface's Create() without arguments gives it. Null for a name that
// Blink's factories do not take, such as "ErrorEvent", which
// RemoveNonAllowlistedCreateEvent leaves out. TouchEvent's feature check is
// the caller's, as in Blink's Document::createEvent().
Event* CreateEventOfInterfaceName(const std::u16string& name);

}  // namespace mitto

#endif  // MITTO_DOM_EVENT_FACTORY_H_
