#include "showcase/catalog.hpp"

namespace showcase {
using namespace tgpp;
using namespace std::chrono_literals;
void add_location_demos(Catalog& catalog) {
    catalog.add({"location", "Location pin", "places", [](DemoContext& ctx) {
        ctx.text("tgpp · location pin\nDemo location: Tokyo Station. This is a fixed sample location.");
        SendLocationRequest request{}; request.chat_id = ctx.chat(); request.latitude = 35.6812; request.longitude = 139.7671; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_location(request); });
    }});
    catalog.add({"venue", "Venue", "places", [](DemoContext& ctx) {
        SendVenueRequest request{}; request.chat_id = ctx.chat(); request.latitude = 35.6812; request.longitude = 139.7671;
        request.title = "tgpp demo · Tokyo Station"; request.address = "Marunouchi, Chiyoda City, Tokyo, Japan"; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_venue(request); });
    }});
    catalog.add({"live_location", "Live location", "places", [](DemoContext& ctx) {
        ctx.text("tgpp · live location\nA fixed demo point will move slightly, then stop sharing.");
        SendLocationRequest request{}; request.chat_id = ctx.chat(); request.latitude = 35.6812; request.longitude = 139.7671; request.live_period = 60; request.disable_notification = true;
        const auto message = ctx.call([&] { return ctx.bot().send_location(request); });
        ctx.pause(2s);
        EditMessageLiveLocationRequest edit{}; edit.chat_id = ctx.chat(); edit.message_id = message.message_id; edit.latitude = 35.6815; edit.longitude = 139.7675;
        ctx.call([&] { return ctx.bot().edit_message_live_location(edit); });
        ctx.pause(2s);
        StopMessageLiveLocationRequest stop{}; stop.chat_id = ctx.chat(); stop.message_id = message.message_id;
        ctx.call([&] { return ctx.bot().stop_message_live_location(stop); });
    }});
    catalog.add({"contact", "Contact card", "places", [](DemoContext& ctx) {
        SendContactRequest request{}; request.chat_id = ctx.chat(); request.phone_number = "+12025550147";
        request.first_name = "tgpp"; request.last_name = "Fictional demo contact"; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_contact(request); });
    }});
}
} // namespace showcase
