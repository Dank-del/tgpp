#include "showcase/catalog.hpp"

namespace showcase {
using namespace tgpp;
void add_media_demos(Catalog& catalog) {
    catalog.add({"photo", "Photo upload", "media", [](DemoContext& ctx) {
        SendPhotoRequest request{}; request.chat_id = ctx.chat(); request.photo = ctx.file("photo.png", "image/png");
        request.caption = "tgpp · photo upload\nAn original, bundled PNG sent with multipart/form-data.";
        request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_photo(request); });
    }});
    catalog.add({"spoiler", "Spoiler photo", "media", [](DemoContext& ctx) {
        SendPhotoRequest request{}; request.chat_id = ctx.chat(); request.photo = ctx.file("photo_two.png", "image/png");
        request.caption = "tgpp · spoiler media\nTap the photo to reveal it."; request.has_spoiler = true;
        request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_photo(request); });
    }});
    catalog.add({"album", "Mixed media album", "media", [](DemoContext& ctx) {
        InputMediaPhoto photo{}; photo.media = "attach://photo"; photo.caption = "tgpp · one album: photo + photo + video";
        InputMediaPhoto second{}; second.media = "attach://second";
        InputMediaVideo video{}; video.media = "attach://video"; video.supports_streaming = true;
        SendMediaGroupRequest request{}; request.chat_id = ctx.chat(); request.media = {photo, second, video}; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_media_group(request, {
            {"photo", ctx.file("photo.png", "image/png")}, {"second", ctx.file("photo_two.png", "image/png")},
            {"video", ctx.file("video.mp4", "video/mp4")}}); });
    }});
    catalog.add({"video", "Video", "media", [](DemoContext& ctx) {
        SendVideoRequest request{}; request.chat_id = ctx.chat(); request.video = ctx.file("video.mp4", "video/mp4");
        request.caption = "tgpp · streamable video with sound"; request.supports_streaming = true;
        request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_video(request); });
    }});
    catalog.add({"animation", "Animation", "media", [](DemoContext& ctx) {
        SendAnimationRequest request{}; request.chat_id = ctx.chat(); request.animation = ctx.file("animation.mp4", "video/mp4");
        request.caption = "tgpp · looping animation\nA silent MP4 delivered as an animation."; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_animation(request); });
    }});
    catalog.add({"document", "Document", "media", [](DemoContext& ctx) {
        SendDocumentRequest request{}; request.chat_id = ctx.chat(); request.document = ctx.file("sample.json", "application/json");
        request.caption = "tgpp · downloadable document\nOpen this original JSON fixture."; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_document(request); });
    }});
    catalog.add({"audio", "Audio player", "media", [](DemoContext& ctx) {
        SendAudioRequest request{}; request.chat_id = ctx.chat(); request.audio = ctx.file("audio.mp3", "audio/mpeg");
        request.title = "Hello from tgpp"; request.performer = "tgpp showcase";
        request.caption = "tgpp · audio with player metadata"; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_audio(request); });
    }});
    catalog.add({"voice", "Voice note", "media", [](DemoContext& ctx) {
        SendVoiceRequest request{}; request.chat_id = ctx.chat(); request.voice = ctx.file("voice.ogg", "audio/ogg");
        request.caption = "tgpp · voice note\nLocally synthesized sample speech, uploaded as Ogg/Opus."; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_voice(request); });
    }});
    catalog.add({"video_note", "Video note", "media", [](DemoContext& ctx) {
        SendVideoNoteRequest request{}; request.chat_id = ctx.chat(); request.video_note = ctx.file("video_note.mp4", "video/mp4");
        request.length = 320; request.duration = 3; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_video_note(request); });
    }});
    catalog.add({"sticker", "Sticker", "media", [](DemoContext& ctx) {
        SendStickerRequest request{}; request.chat_id = ctx.chat(); request.sticker = ctx.file("sticker.webp", "image/webp");
        request.emoji = "🚀"; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_sticker(request); });
    }});
    catalog.add({"reuse", "Reuse file ID", "media", [](DemoContext& ctx) {
        SendPhotoRequest first{}; first.chat_id = ctx.chat(); first.photo = ctx.file("photo_two.png", "image/png");
        first.caption = "tgpp · file reuse, first upload"; first.disable_notification = true;
        const auto sent = ctx.call([&] { return ctx.bot().send_photo(first); });
        if (!sent.photo || sent.photo->empty()) throw std::runtime_error("photo response has no reusable file ID");
        SendPhotoRequest second{}; second.chat_id = ctx.chat(); second.photo = sent.photo->back().file_id;
        second.caption = "tgpp · same photo sent using its file_id, without uploading bytes again"; second.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_photo(second); });
    }});
}
} // namespace showcase
