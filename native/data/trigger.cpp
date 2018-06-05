#include "trigger.hpp"

namespace audionodes {

TriggerData::TriggerData(EventSeries events) :
    events(events)
{}
TriggerData::TriggerData() {}

TriggerData TriggerData::dummy = TriggerData();

}
